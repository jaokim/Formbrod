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

typedef struct FBBackend FBBackend;

struct FBBackend {
    const char *name;     /* "local", "remote" */

    int        (*Submit)(FBBackend *be, const FBJob *job);
    /* Do a slice of work (local) or poll the socket (remote), then report. */
    FBProgress (*Progress)(FBBackend *be);
    void       (*Cancel)(FBBackend *be);
    FBResult   (*Result)(FBBackend *be);
    void       (*Dispose)(FBBackend *be);

    void       *priv;
};

FBBackend *FB_CreateLocalBackend(void);

#endif
