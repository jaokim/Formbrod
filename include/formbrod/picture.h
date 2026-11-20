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

/* ---- IFF ANIM -------------------------------------------------------------
 *
 * A stretch of the edit as an IFF ANIM for paint programs: 256 colours,
 * the first frame as an ILBM BODY, the others as ANIM-5 deltas (byte
 * vertical delta, op 5), each against the frame two before it, as ANIM
 * players and paint programs double-buffer.
 *
 * All frames share one palette: 6 x 7 x 6 levels of red, green and blue
 * (252 colours; the last 4 are greys), with a 4 x 4 ordered dither, which
 * stays put from frame to frame, so still parts of the picture make no
 * deltas. Frames are written as they come; Close fills in the sizes. */

typedef struct FBAnimWriter FBAnimWriter;

/* Starts an ANIM of w x h frames at fps_num/fps_den frames a second.
 * NULL if the file can't be made. */
FBAnimWriter *FB_AnimOpen(const char *path, int w, int h, int fps_num,
                          int fps_den);
/* Adds a frame (ARGB, w x h, `stride` bytes a row). 0, or -1. */
int  FB_AnimAdd(FBAnimWriter *a, const unsigned char *argb, int stride);
/* How many frames have been added. */
int  FB_AnimFrames(const FBAnimWriter *a);
/* Finishes the file. 0, or -1 (writing failed, or there were no
 * frames); the writer is freed either way. */
int  FB_AnimClose(FBAnimWriter *a);
/* Stops and deletes the file. */
void FB_AnimAbort(FBAnimWriter *a);

/* The palette (256 RGB triples) and a frame mapped to it (one index a
 * pixel), as the writer does it. */
void FB_AnimPalette(unsigned char rgb[256 * 3]);
void FB_AnimQuantize(const unsigned char *argb, int w, int h, int stride,
                     unsigned char *out);

/* Whether a file name asks for a movie rather than an ANIM: it ends in
 * a movie container's name (.mp4, .mov, .avi, .mkv, .webm, .gif, .mpg,
 * .m4v). Anything else, with no extension too, gets an ANIM. */
int  FB_IsMovieName(const char *path);

#endif
