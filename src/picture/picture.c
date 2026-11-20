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

/* ---- IFF ANIM ------------------------------------------------------------- */

#define ANIM_R 6
#define ANIM_G 7
#define ANIM_B 6
#define ANIM_COLOURS (ANIM_R * ANIM_G * ANIM_B)   /* 252 */

struct FBAnimWriter {
    FILE          *f;
    char          *path;
    int            w, h, rowbytes;
    int            jiffies;          /* each frame's time, 1/60 s */
    int            frames;
    long           dpan_at;          /* where DPAN's frame count is */
    unsigned char *chunky[3];        /* this frame, and the two before */
    unsigned char *planes[2];        /* this one's and its reference's */
    int            failed;
};

void FB_AnimPalette(unsigned char rgb[256 * 3])
{
    int i;

    memset(rgb, 0, 256 * 3);
    for (i = 0; i < ANIM_COLOURS; i++) {
        rgb[3 * i]     = (unsigned char)((i / (ANIM_G * ANIM_B)) * 255
                                         / (ANIM_R - 1));
        rgb[3 * i + 1] = (unsigned char)((i / ANIM_B % ANIM_G) * 255
                                         / (ANIM_G - 1));
        rgb[3 * i + 2] = (unsigned char)((i % ANIM_B) * 255 / (ANIM_B - 1));
    }
    for (i = ANIM_COLOURS; i < 256; i++)
        rgb[3 * i] = rgb[3 * i + 1] = rgb[3 * i + 2] =
            (unsigned char)((i - ANIM_COLOURS + 1) * 255 / 5);
}

/* v (0..255) to one of `levels`, dithered by b (0..15). */
static int dither(int v, int levels, int b)
{
    int l = (v * (levels - 1) * 32 + (2 * b + 1) * 255) / (255 * 32);

    return l > levels - 1 ? levels - 1 : l;
}

void FB_AnimQuantize(const unsigned char *argb, int w, int h, int stride,
                     unsigned char *out)
{
    static const unsigned char bayer[4][4] = {
        { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 }
    };
    int x, y;

    for (y = 0; y < h; y++) {
        const unsigned char *p = argb + (size_t)y * stride;

        for (x = 0; x < w; x++) {
            int b = bayer[y & 3][x & 3];

            *out++ = (unsigned char)(dither(p[4 * x + 1], ANIM_R, b)
                                     * ANIM_G * ANIM_B
                                     + dither(p[4 * x + 2], ANIM_G, b) * ANIM_B
                                     + dither(p[4 * x + 3], ANIM_B, b));
        }
    }
}

int FB_IsMovieName(const char *path)
{
    static const char *const ext[] = {
        "mp4", "mov", "avi", "mkv", "webm", "gif", "mpg", "mpeg", "m4v", NULL
    };
    const char *dot = path ? strrchr(path, '.') : NULL;
    int i, k;

    if (!dot || strchr(dot, '/') || strchr(dot, ':'))
        return 0;
    for (i = 0; ext[i]; i++) {
        for (k = 0; ext[i][k] && dot[1 + k]
             && tolower((unsigned char)dot[1 + k]) == ext[i][k]; k++)
            ;
        if (!ext[i][k] && !dot[1 + k])
            return 1;
    }
    return 0;
}

/* One frame's indices to 8 planes, plane after plane, rowbytes a row. */
static void to_planes(const FBAnimWriter *a, const unsigned char *chunky,
                      unsigned char *planes)
{
    size_t plane = (size_t)a->rowbytes * a->h;
    int x, y, bit;

    memset(planes, 0, plane * 8);
    for (y = 0; y < a->h; y++)
        for (x = 0; x < a->w; x++) {
            unsigned v = chunky[(size_t)y * a->w + x];
            size_t at = (size_t)y * a->rowbytes + (x >> 3);
            unsigned char m = (unsigned char)(0x80 >> (x & 7));

            for (bit = 0; bit < 8; bit++)
                if (v & (1u << bit))
                    planes[bit * plane + at] |= m;
        }
}

static int write_out(FBAnimWriter *a, Out *o)
{
    if (o->failed || fwrite(o->buf, 1, o->len, a->f) != o->len)
        a->failed = 1;
    free(o->buf);
    memset(o, 0, sizeof(*o));
    return a->failed ? -1 : 0;
}

/* Ends an IFF chunk whose size field is at `at`, padding it to even. */
static void end_chunk(Out *o, size_t at)
{
    size_t n = o->len - at - 4;

    patch32be(o, at, (unsigned long)n);
    if (n & 1)
        put8(o, 0);
}

static void put_bmhd(Out *o, int w, int h, int planes)
{
    put(o, "BMHD", 4);
    put32be(o, 20);
    put16be(o, (unsigned)w);
    put16be(o, (unsigned)h);
    put16be(o, 0);
    put16be(o, 0);
    put8(o, (unsigned)planes);
    put8(o, 0);             /* no mask */
    put8(o, 1);             /* ByteRun1 */
    put8(o, 0);
    put16be(o, 0);
    put8(o, 10);            /* x aspect: the usual lowres 10:11 */
    put8(o, 11);
    put16be(o, (unsigned)w);
    put16be(o, (unsigned)h);
}

/* The first frame: BMHD, CMAP, DPAN, BODY. */
static void first_frame(FBAnimWriter *a, Out *o, const unsigned char *planes)
{
    unsigned char pal[256 * 3];
    size_t form, body, plane = (size_t)a->rowbytes * a->h;
    int y, p;

    put(o, "FORM", 4);
    form = o->len;
    put32be(o, 0);
    put(o, "ILBM", 4);
    put_bmhd(o, a->w, a->h, 8);
    FB_AnimPalette(pal);
    put(o, "CMAP", 4);
    put32be(o, sizeof(pal));
    put(o, pal, sizeof(pal));
    /* DPaint's DPAN: how many frames (filled in at the end). */
    put(o, "DPAN", 4);
    put32be(o, 8);
    put16be(o, 4);          /* version */
    a->dpan_at = (long)o->len;   /* relative to the frame; see AnimClose */
    put16be(o, 0);
    put32be(o, 0);
    put(o, "BODY", 4);
    body = o->len;
    put32be(o, 0);
    for (y = 0; y < a->h; y++)
        for (p = 0; p < 8; p++)
            byterun1(o, planes + p * plane + (size_t)y * a->rowbytes,
                     a->rowbytes);
    end_chunk(o, body);
    patch32be(o, form, (unsigned long)(o->len - form - 4));
}

/* One column of a plane, op 5: ops for the rows that differ from the
 * reference. Returns the op count, written before the ops at `count`. */
static void delta_column(Out *o, const unsigned char *cur,
                         const unsigned char *ref, int h, int rowbytes)
{
    size_t count = o->len;
    int ops = 0, y = 0;

    put8(o, 0);
#define AT(p, row) ((p)[(size_t)(row) * rowbytes])
    while (y < h) {
        int n;

        /* Room left: an op count is one byte, so near the limit the rest
         * goes out as literals (127 rows each). */
        if (ops + (h - y + 126) / 127 >= 254) {
            while (y < h) {
                n = h - y > 127 ? 127 : h - y;
                put8(o, 0x80 | (unsigned)n);
                for (; n > 0; n--, y++)
                    put8(o, AT(cur, y));
                ops++;
            }
            break;
        }
        for (n = 0; y + n < h && n < 127 && AT(cur, y + n) == AT(ref, y + n);
             n++)
            ;
        if (n > 0) {                       /* skip unchanged rows */
            if (y + n < h) {               /* a skip at the end isn't needed */
                put8(o, (unsigned)n);
                ops++;
            }
            y += n;
            continue;
        }
        for (n = 1; y + n < h && n < 255 && AT(cur, y + n) == AT(cur, y); n++)
            ;
        if (n >= 3) {                      /* the same byte n times */
            put8(o, 0);
            put8(o, (unsigned)n);
            put8(o, AT(cur, y));
            ops++;
            y += n;
            continue;
        }
        /* Literals until rows match again (2 or more) or a run of 3. */
        for (n = 0; y + n < h && n < 127; n++) {
            if (y + n + 1 < h && AT(cur, y + n) == AT(ref, y + n)
                && AT(cur, y + n + 1) == AT(ref, y + n + 1))
                break;
            if (y + n + 2 < h && AT(cur, y + n) == AT(cur, y + n + 1)
                && AT(cur, y + n) == AT(cur, y + n + 2))
                break;
        }
        if (n == 0)
            n = 1;
        put8(o, 0x80 | (unsigned)n);
        for (; n > 0; n--, y++)
            put8(o, AT(cur, y));
        ops++;
    }
#undef AT
    if (!o->failed)
        o->buf[count] = (unsigned char)ops;
}

/* A later frame: ANHD and DLTA against `ref` (two frames back). */
static void delta_frame(FBAnimWriter *a, Out *o, const unsigned char *cur,
                        const unsigned char *ref)
{
    size_t form, dlta, ptrs, plane = (size_t)a->rowbytes * a->h;
    int p, x, i;

    put(o, "FORM", 4);
    form = o->len;
    put32be(o, 0);
    put(o, "ILBM", 4);
    put(o, "ANHD", 4);
    put32be(o, 40);
    put8(o, 5);                         /* byte vertical delta */
    put8(o, 0);
    put16be(o, (unsigned)a->w);
    put16be(o, (unsigned)a->h);
    put16be(o, 0);
    put16be(o, 0);
    put32be(o, 0);                      /* abstime */
    put32be(o, (unsigned long)a->jiffies);
    put8(o, 0);                         /* interleave: two frames back */
    put8(o, 0);
    put32be(o, 0);                      /* bits: store, not XOR */
    for (i = 0; i < 16; i++)
        put8(o, 0);

    put(o, "DLTA", 4);
    dlta = o->len;
    put32be(o, 0);
    ptrs = o->len;
    for (i = 0; i < 16; i++)
        put32be(o, 0);
    for (p = 0; p < 8; p++) {
        const unsigned char *c = cur + p * plane, *r = ref + p * plane;

        if (memcmp(c, r, plane) == 0)
            continue;                   /* pointer 0: plane unchanged */
        patch32be(o, ptrs + 4 * p, (unsigned long)(o->len - ptrs));
        for (x = 0; x < a->rowbytes; x++)
            delta_column(o, c + x, r + x, a->h, a->rowbytes);
    }
    end_chunk(o, dlta);
    patch32be(o, form, (unsigned long)(o->len - form - 4));
}

FBAnimWriter *FB_AnimOpen(const char *path, int w, int h, int fps_num,
                          int fps_den)
{
    FBAnimWriter *a;
    size_t n = (size_t)w * h, plane;
    int i;

    if (!path || w <= 0 || h <= 0 || w > 0xffff || h > 0xffff
        || fps_num <= 0 || fps_den <= 0)
        return NULL;
    a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    a->w = w;
    a->h = h;
    a->rowbytes = ((w + 15) / 16) * 2;
    a->jiffies = (int)((60L * fps_den + fps_num / 2) / fps_num);
    if (a->jiffies < 1)
        a->jiffies = 1;
    plane = (size_t)a->rowbytes * h;
    a->path = malloc(strlen(path) + 1);
    for (i = 0; i < 3; i++)
        a->chunky[i] = malloc(n);
    a->planes[0] = malloc(plane * 8);
    a->planes[1] = malloc(plane * 8);
    if (a->path)
        strcpy(a->path, path);
    if (!a->path || !a->chunky[0] || !a->chunky[1] || !a->chunky[2]
        || !a->planes[0] || !a->planes[1]
        || !(a->f = fopen(path, "wb"))) {
        FB_AnimAbort(a);
        return NULL;
    }
    if (fwrite("FORM\0\0\0\0ANIM", 1, 12, a->f) != 12)
        a->failed = 1;
    return a;
}

int FB_AnimFrames(const FBAnimWriter *a)
{
    return a ? a->frames : 0;
}

int FB_AnimAdd(FBAnimWriter *a, const unsigned char *argb, int stride)
{
    Out o;
    unsigned char *t;

    if (!a || a->failed)
        return -1;
    memset(&o, 0, sizeof(o));
    /* chunky[0] is this frame; [1] and [2] the ones before. */
    t = a->chunky[2];
    a->chunky[2] = a->chunky[1];
    a->chunky[1] = a->chunky[0];
    a->chunky[0] = t;
    FB_AnimQuantize(argb, a->w, a->h, stride, a->chunky[0]);
    to_planes(a, a->chunky[0], a->planes[0]);
    if (a->frames == 0) {
        long at = ftell(a->f);

        first_frame(a, &o, a->planes[0]);
        a->dpan_at += at;
    } else {
        /* Two frames back; before frame 2 both buffers hold frame 0. */
        to_planes(a, a->chunky[a->frames == 1 ? 1 : 2], a->planes[1]);
        delta_frame(a, &o, a->planes[0], a->planes[1]);
    }
    if (write_out(a, &o) != 0)
        return -1;
    a->frames++;
    return 0;
}

static void anim_free(FBAnimWriter *a)
{
    int i;

    for (i = 0; i < 3; i++)
        free(a->chunky[i]);
    free(a->planes[0]);
    free(a->planes[1]);
    free(a->path);
    free(a);
}

int FB_AnimClose(FBAnimWriter *a)
{
    unsigned char b[4];
    long end;
    int rc;

    if (!a)
        return -1;
    end = ftell(a->f);
    if (a->frames == 0 || end < 12)
        a->failed = 1;
    if (!a->failed) {
        b[0] = (unsigned char)((end - 8) >> 24);
        b[1] = (unsigned char)((end - 8) >> 16);
        b[2] = (unsigned char)((end - 8) >> 8);
        b[3] = (unsigned char)(end - 8);
        if (fseek(a->f, 4, SEEK_SET) != 0 || fwrite(b, 1, 4, a->f) != 4)
            a->failed = 1;
        b[0] = (unsigned char)(a->frames >> 8);
        b[1] = (unsigned char)a->frames;
        if (fseek(a->f, a->dpan_at, SEEK_SET) != 0
            || fwrite(b, 1, 2, a->f) != 2)
            a->failed = 1;
    }
    if (fclose(a->f) != 0)
        a->failed = 1;
    a->f = NULL;
    rc = a->failed ? -1 : 0;
    if (rc != 0)
        remove(a->path);
    anim_free(a);
    return rc;
}

void FB_AnimAbort(FBAnimWriter *a)
{
    if (!a)
        return;
    if (a->f) {
        fclose(a->f);
        remove(a->path);
    }
    anim_free(a);
}
