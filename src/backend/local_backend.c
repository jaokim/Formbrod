/*
 * local_backend.c - render on the Amiga with the linked libav* libraries.
 *
 * Progress() is called from the GUI loop and does a bounded slice of work
 * (decode -> filtergraph -> encode for a few frames) each time, so the GUI
 * stays responsive without a separate render task. If this turns out to be
 * too choppy, move the loop into a child process and keep the same API.
 *
 * Built without HAVE_LIBAV, Submit() fails with a clear message so the rest
 * of the app can be developed and tested without the libraries.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "formbrod/backend.h"

typedef struct LocalPriv {
    FBJob      job;
    FBProgress progress;
    FBResult   result;
#ifdef HAVE_LIBAV
    /* TODO: AVFormatContext *in[FB_MAX_SOURCES], *out;
     *       AVFilterGraph *graph; buffersrc/buffersink contexts;
     *       decoder and encoder contexts. */
#endif
} LocalPriv;

static void finish(LocalPriv *p, FBRenderState state, const char *msg)
{
    p->progress.state = state;
    p->result.state = state;
    strncpy(p->result.message, msg, sizeof(p->result.message) - 1);
    p->result.message[sizeof(p->result.message) - 1] = '\0';
}

static int local_submit(FBBackend *be, const FBJob *job)
{
    LocalPriv *p = be->priv;

    if (p->progress.state == FB_RENDER_RUNNING)
        return -1;
    memset(&p->progress, 0, sizeof(p->progress));
    memset(&p->result, 0, sizeof(p->result));
    p->job = *job;

#ifdef HAVE_LIBAV
    /* TODO (phase 1):
     *  1. avformat_open_input() + avformat_find_stream_info() per input
     *  2. open a decoder per used stream
     *  3. avfilter_graph_parse_ptr() on job->filtergraph with one
     *     buffer/abuffer source per "[n:v]"/"[n:a]" pad and sinks on
     *     job->video_out / job->audio_out
     *  4. avformat_alloc_output_context2() + encoders from job->output
     *  5. set p->progress.total from the timeline length
     */
    finish(p, FB_RENDER_FAILED, "Local rendering is not implemented yet");
    return -1;
#else
    finish(p, FB_RENDER_FAILED, "Built without libav (HAVE_LIBAV not set)");
    return -1;
#endif
}

static FBProgress local_progress(FBBackend *be)
{
    LocalPriv *p = be->priv;

#ifdef HAVE_LIBAV
    if (p->progress.state == FB_RENDER_RUNNING) {
        /* TODO: pump up to N frames through decode/filter/encode, update
         * p->progress.position, call finish() at EOF or on error. */
    }
#endif
    return p->progress;
}

static void local_cancel(FBBackend *be)
{
    LocalPriv *p = be->priv;

    if (p->progress.state == FB_RENDER_RUNNING) {
        /* TODO: close contexts and delete the partial output file */
        finish(p, FB_RENDER_CANCELLED, "Cancelled");
    }
}

static FBResult local_result(FBBackend *be)
{
    return ((LocalPriv *)be->priv)->result;
}

static void local_dispose(FBBackend *be)
{
    local_cancel(be);
    free(be->priv);
    free(be);
}

FBBackend *FB_CreateLocalBackend(void)
{
    FBBackend *be = calloc(1, sizeof(*be));
    LocalPriv *p = calloc(1, sizeof(*p));

    if (!be || !p) {
        free(be);
        free(p);
        return NULL;
    }
    be->name     = "local";
    be->Submit   = local_submit;
    be->Progress = local_progress;
    be->Cancel   = local_cancel;
    be->Result   = local_result;
    be->Dispose  = local_dispose;
    be->priv     = p;
    return be;
}
