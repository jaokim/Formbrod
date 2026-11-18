/*
 * picture.h - writing single pictures for paint programs.
 *
 * A frame of the edit (ARGB, as the backend's sink hands it out, see
 * backend.h) is written either as a PNG or as a 24-bit IFF ILBM, chosen
 * by the file name: ".png" gives a PNG, anything else an ILBM, which
 * every Amiga paint program and the datatypes read.
 *
 * The PNG is stored without compression (deflate's "stored" blocks), so
 * no zlib is needed; the ILBM rows are packed with ByteRun1. Alpha is
 * left out: frames of the edit are opaque.
 *
 * Plain C99 and stdio.
 */
#ifndef FORMBROD_PICTURE_H
#define FORMBROD_PICTURE_H

#include <stddef.h>

typedef enum {
    FB_PICTURE_ILBM = 0,
    FB_PICTURE_PNG
} FBPictureFormat;

/* The format a file name asks for: PNG for ".png" (any case), else
 * ILBM. */
FBPictureFormat FB_PictureFormatFor(const char *path);

/* Writes w x h ARGB pixels (`stride` bytes a row) to `path`, in the
 * format its name asks for. 0, or -1 if the file can't be written. */
int FB_PictureWrite(const char *path, const unsigned char *argb, int w,
                    int h, int stride);

/* The same into memory, for tests: returns the bytes written, or the
 * size needed if `buf` is NULL or too small (nothing is written then). */
size_t FB_PictureEncode(FBPictureFormat fmt, const unsigned char *argb,
                        int w, int h, int stride, unsigned char *buf,
                        size_t len);

#endif
