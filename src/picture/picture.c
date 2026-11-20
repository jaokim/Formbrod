/*
 * picture.c - single pictures as PNG or 24-bit IFF ILBM (picture.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "formbrod/picture.h"

/* A growing byte buffer. `failed` sticks once memory runs out. */
typedef struct Out {
    unsigned char *buf;
    size_t         len, size;
    int            failed;
} Out;

static void put(Out *o, const void *p, size_t n)
{
    if (o->failed)
        return;
    if (o->len + n > o->size) {
        size_t size = o->size ? o->size : 4096;
        unsigned char *b;

        while (size < o->len + n)
            size *= 2;
        b = realloc(o->buf, size);
        if (!b) {
            o->failed = 1;
            return;
        }
        o->buf = b;
        o->size = size;
    }
    memcpy(o->buf + o->len, p, n);
    o->len += n;
}

static void put8(Out *o, unsigned v)
{
    unsigned char b = (unsigned char)v;

    put(o, &b, 1);
}

static void put16be(Out *o, unsigned v)
{
    put8(o, v >> 8);
    put8(o, v);
}

static void put32be(Out *o, unsigned long v)
{
    put8(o, (unsigned)(v >> 24));
    put8(o, (unsigned)(v >> 16));
    put8(o, (unsigned)(v >> 8));
    put8(o, (unsigned)v);
}

/* Writes a big-endian length at `at` (filled in after the chunk). */
static void patch32be(Out *o, size_t at, unsigned long v)
{
    if (o->failed)
        return;
    o->buf[at]     = (unsigned char)(v >> 24);
    o->buf[at + 1] = (unsigned char)(v >> 16);
    o->buf[at + 2] = (unsigned char)(v >> 8);
    o->buf[at + 3] = (unsigned char)v;
}

/* ---- PNG ------------------------------------------------------------------ */

static unsigned long crc_table[256];

static void crc_init(void)
{
    unsigned long c;
    int n, k;

    if (crc_table[1])
        return;
    for (n = 0; n < 256; n++) {
        c = (unsigned long)n;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? 0xedb88320UL ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static unsigned long crc(const unsigned char *p, size_t n)
{
    unsigned long c = 0xffffffffUL;

    crc_init();
    while (n--)
        c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return (c ^ 0xffffffffUL) & 0xffffffffUL;
}

/* Ends a PNG chunk that started at `start` (its length field): fills in
 * the length and adds the CRC over the type and data. */
static void png_end_chunk(Out *o, size_t start)
{
    size_t data = o->len - start - 8;

    patch32be(o, start, (unsigned long)data);
    if (!o->failed)
        put32be(o, crc(o->buf + start + 4, data + 4));
}

static void png_begin_chunk(Out *o, size_t *start, const char *type)
{
    *start = o->len;
    put32be(o, 0);
    put(o, type, 4);
}

static void encode_png(Out *o, const unsigned char *argb, int w, int h,
                       int stride)
{
    static const unsigned char sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    size_t chunk, raw = (size_t)(1 + 3 * w) * h, left, block;
    unsigned long a = 1, b = 0;    /* adler32 */
    unsigned char *row;
    size_t in_row;
    int y = 0, x;

    put(o, sig, 8);
    png_begin_chunk(o, &chunk, "IHDR");
    put32be(o, (unsigned long)w);
    put32be(o, (unsigned long)h);
    put8(o, 8);             /* bits per sample */
    put8(o, 2);             /* RGB */
    put8(o, 0);             /* deflate */
    put8(o, 0);             /* adaptive filters (all "none" here) */
    put8(o, 0);             /* not interlaced */
    png_end_chunk(o, chunk);

    /* The pixels as one zlib stream of stored blocks, up to 64 KB each.
     * Each row is a filter byte (0) and its RGB. */
    row = malloc((size_t)1 + 3 * w);
    if (!row) {
        o->failed = 1;
        return;
    }
    png_begin_chunk(o, &chunk, "IDAT");
    put8(o, 0x78);
    put8(o, 0x01);
    left = raw;
    in_row = 0;
    row[0] = 0;
    while (left > 0) {
        block = left > 65535 ? 65535 : left;
        left -= block;
        put8(o, left == 0 ? 1 : 0);             /* last block? */
        put8(o, (unsigned)(block & 0xff));
        put8(o, (unsigned)(block >> 8));
        put8(o, (unsigned)(~block & 0xff));
        put8(o, (unsigned)((~block >> 8) & 0xff));
        while (block > 0) {
            size_t n;

            if (in_row == 0) {
                const unsigned char *p = argb + (size_t)y * stride;

                for (x = 0; x < w; x++) {
                    row[1 + 3 * x]     = p[4 * x + 1];
                    row[1 + 3 * x + 1] = p[4 * x + 2];
                    row[1 + 3 * x + 2] = p[4 * x + 3];
                }
            }
            n = (size_t)(1 + 3 * w) - in_row;
            if (n > block)
                n = block;
            put(o, row + in_row, n);
            {
                size_t i;

                for (i = 0; i < n; i++) {
                    a = (a + row[in_row + i]) % 65521;
                    b = (b + a) % 65521;
                }
            }
            in_row += n;
            block -= n;
            if (in_row == (size_t)(1 + 3 * w)) {
                in_row = 0;
                y++;
            }
        }
    }
    put32be(o, (b << 16) | a);
    png_end_chunk(o, chunk);
    free(row);

    png_begin_chunk(o, &chunk, "IEND");
    png_end_chunk(o, chunk);
}

/* ---- ILBM ----------------------------------------------------------------- */

/* ByteRun1 (PackBits) of one plane row. */
static void byterun1(Out *o, const unsigned char *p, int n)
{
    int i = 0;

    while (i < n) {
        int run = 1;

        while (i + run < n && run < 128 && p[i + run] == p[i])
            run++;
        if (run >= 3) {
            put8(o, (unsigned)(257 - run));     /* -(run - 1) */
            put8(o, p[i]);
            i += run;
        } else {
            /* Literals up to the next run of 3 or more. */
            int lit = 0;

            while (i + lit < n && lit < 128) {
                if (i + lit + 2 < n && p[i + lit] == p[i + lit + 1]
                    && p[i + lit] == p[i + lit + 2])
                    break;
                lit++;
            }
            put8(o, (unsigned)(lit - 1));
            put(o, p + i, (size_t)lit);
            i += lit;
        }
    }
}

static void encode_ilbm(Out *o, const unsigned char *argb, int w, int h,
                        int stride)
{
    int rowbytes = ((w + 15) / 16) * 2;
    unsigned char *plane = calloc((size_t)rowbytes, 1);
    size_t form, body;
    int y, c, bit, x;

    if (!plane) {
        o->failed = 1;
        return;
    }
    put(o, "FORM", 4);
    form = o->len;
    put32be(o, 0);
    put(o, "ILBM", 4);

    put(o, "BMHD", 4);
    put32be(o, 20);
    put16be(o, (unsigned)w);
    put16be(o, (unsigned)h);
    put16be(o, 0);          /* x */
    put16be(o, 0);          /* y */
    put8(o, 24);            /* planes */
    put8(o, 0);             /* no mask */
    put8(o, 1);             /* ByteRun1 */
    put8(o, 0);             /* pad */
    put16be(o, 0);          /* transparent colour */
    put8(o, 1);             /* x aspect */
    put8(o, 1);             /* y aspect */
    put16be(o, (unsigned)w);
    put16be(o, (unsigned)h);

    put(o, "BODY", 4);
    body = o->len;
    put32be(o, 0);
    /* Each row: the red planes (lowest bit first), then green, then blue. */
    for (y = 0; y < h; y++) {
        const unsigned char *p = argb + (size_t)y * stride;

        for (c = 1; c <= 3; c++)
            for (bit = 0; bit < 8; bit++) {
                memset(plane, 0, (size_t)rowbytes);
                for (x = 0; x < w; x++)
                    if (p[4 * x + c] & (1 << bit))
                        plane[x >> 3] |= (unsigned char)(0x80 >> (x & 7));
                byterun1(o, plane, rowbytes);
            }
    }
    {
        unsigned long n = (unsigned long)(o->len - body - 4);

        patch32be(o, body, n);
        if (n & 1)
            put8(o, 0);
    }
    patch32be(o, form, (unsigned long)(o->len - form - 4));
    free(plane);
}

/* ---- Entry points --------------------------------------------------------- */

FBPictureFormat FB_PictureFormatFor(const char *path)
{
    size_t n = path ? strlen(path) : 0;

    if (n >= 4 && path[n - 4] == '.'
        && tolower((unsigned char)path[n - 3]) == 'p'
        && tolower((unsigned char)path[n - 2]) == 'n'
        && tolower((unsigned char)path[n - 1]) == 'g')
        return FB_PICTURE_PNG;
    return FB_PICTURE_ILBM;
}

static int encode(Out *o, FBPictureFormat fmt, const unsigned char *argb,
                  int w, int h, int stride)
{
    memset(o, 0, sizeof(*o));
    if (!argb || w <= 0 || h <= 0 || w > 0xffff || h > 0xffff
        || stride < 4 * w)
        return -1;
    if (fmt == FB_PICTURE_PNG)
        encode_png(o, argb, w, h, stride);
    else
        encode_ilbm(o, argb, w, h, stride);
    if (o->failed) {
        free(o->buf);
        o->buf = NULL;
        return -1;
    }
    return 0;
}

size_t FB_PictureEncode(FBPictureFormat fmt, const unsigned char *argb,
                        int w, int h, int stride, unsigned char *buf,
                        size_t len)
{
    Out o;
    size_t n;

    if (encode(&o, fmt, argb, w, h, stride) != 0)
        return 0;
    n = o.len;
    if (buf && len >= n)
        memcpy(buf, o.buf, n);
    free(o.buf);
    return n;
}

int FB_PictureWrite(const char *path, const unsigned char *argb, int w,
                    int h, int stride)
{
    Out o;
    FILE *f;
    int rc = 0;

    if (encode(&o, FB_PictureFormatFor(path), argb, w, h, stride) != 0)
        return -1;
    f = fopen(path, "wb");
    if (!f || fwrite(o.buf, 1, o.len, f) != o.len)
        rc = -1;
    if (f && fclose(f) != 0)
        rc = -1;
    free(o.buf);
    return rc;
}
