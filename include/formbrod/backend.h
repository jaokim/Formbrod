/*
 * backend.h - render backends.
 *
 * Rendering is pluggable. The editor builds an FBJob and gives it to the
 * current backend. LocalBackend renders with the linked libav* libraries;
 * a RemoteBackend (later) sends the job to a render server over TCP.
 *
 * Backends are polled from the GUI's event loop: Submit() starts work and
 * returns quickly, Progress() is called periodically, Result() is read once
 * the state is DONE or FAILED.
 */
#ifndef FORMBROD_BACKEND_H
#define FORMBROD_BACKEND_H

#include "job.h"

typedef enum {
    FB_RENDER_IDLE = 0,
    FB_RENDER_RUNNING,
    FB_RENDER_DONE,
    FB_RENDER_FAILED,
    FB_RENDER_CANCELLED
} FBRenderState;

typedef struct FBProgress {
    FBRenderState state;
    fb_ms_t       position;   /* output time rendered so far */
    fb_ms_t       total;      /* expected output length, 0 if unknown */
} FBProgress;

typedef struct FBResult {
    FBRenderState state;
    char          message[256];   /* error text or output path */
} FBResult;

/* Playing instead of writing a file (FBBackend.Play): the frames the job
 * makes are handed to a sink as they come, picture as ARGB (8 bits each,
 * in that byte order) and sound as signed 16-bit stereo at FB_PLAY_RATE.
 * Times are the frame's place in the job's result, in ms. A callback
 * returns 0 when it took the frame and 1 when it's full: the backend then
 * keeps that frame, stops its slice, and offers it again next time. So
 * playback paces the work. */
#define FB_PLAY_RATE 44100

typedef struct FBFrameSink {
    int  (*Video)(void *ud, const unsigned char *argb, int width,
                  int height, int stride, fb_ms_t pts);
    int  (*Audio)(void *ud, const short *samples, int frames, fb_ms_t pts);
    void *ud;
} FBFrameSink;

typedef struct FBBackend FBBackend;

struct FBBackend {
    const char *name;     /* "local", "remote" */

    int        (*Submit)(FBBackend *be, const FBJob *job);
    /* Do a slice of work (local) or poll the socket (remote), then report. */
    FBProgress (*Progress)(FBBackend *be);
    void       (*Cancel)(FBBackend *be);
    FBResult   (*Result)(FBBackend *be);
    void       (*Dispose)(FBBackend *be);
    /* Like Submit, but to a sink (see FBFrameSink); the job's output path
     * and codecs are not used. NULL where a backend can't play (a remote
     * one). Progress, Cancel and Result work as for Submit. */
    int        (*Play)(FBBackend *be, const FBJob *job,
                       const FBFrameSink *sink);

    void       *priv;
};

FBBackend *FB_CreateLocalBackend(void);

/* A clock in microseconds for playback timing; only differences count. */
long long FB_BackendClockUs(void);

/* Logs the libav versions and, if the log (formbrod/log.h) is open, sends
 * ffmpeg's own messages there instead of to the shell. Stop puts them
 * back. Call after FB_LogOpen() and before FB_LogClose(). */
void FB_BackendLogStart(void);
void FB_BackendLogStop(void);

#endif
