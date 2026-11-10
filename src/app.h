/*
 * app.h - application state and the actions shared by the GUI and ARexx.
 *
 * Every user-visible action is a function here. The GUI and the ARexx port
 * both call these, so anything you can click you can also script.
 */
#ifndef FORMBROD_APP_H
#define FORMBROD_APP_H

#include "formbrod/timeline.h"
#include "formbrod/backend.h"

typedef struct FBApp {
    FBTimeline  timeline;
    FBBackend  *backend;
    int         rendering;
    int         quit;
    char        status[256];   /* last status line, shown in the window */
} FBApp;

extern FBApp App;

int  App_Init(void);
void App_Cleanup(void);

/* Adds a file as a source and appends it as one clip. 0 on success. */
int  App_AddFile(const char *path);
int  App_Split(fb_ms_t at);
int  App_RemoveClip(int clip);
int  App_AddEffect(int clip, const char *module);
int  App_SetEffectParam(int clip, int effect, const char *name, double value);

/* How clips of another size or shape are fitted (see FBFormat). */
int  App_SetClipFit(int clip, int fit);          /* FB_FIT_DEFAULT = global */
int  App_SetDefaultFit(int fit);                 /* not FB_FIT_DEFAULT */
int  App_SetFrameSize(int width, int height);    /* 0 0 = first clip's */
int  App_SetFrameRate(int num, int den);         /* 0 0 = first clip's */
int  App_Export(const char *path, int preview);
void App_CancelRender(void);

/* Called from the main loop while rendering. Returns progress. */
FBProgress App_PollRender(void);

void App_SetStatus(const char *fmt, ...);

/* Translated text for a job error, for the status line. */
const char *App_JobErrorString(FBJobError err);

/* Media probing (src/backend/media.c). Fills duration, stream flags and
 * video size/aspect/rate of `info` (not its path). */
typedef enum {
    FB_PROBE_OK = 0,
    FB_PROBE_NO_LIBAV,      /* built without HAVE_LIBAV */
    FB_PROBE_CANT_OPEN,     /* libavformat failed; reason in `why` */
    FB_PROBE_NO_VIDEO,
    FB_PROBE_NO_DURATION
} FBProbeResult;

/* `why` receives ffmpeg's error text for FB_PROBE_CANT_OPEN (English). */
FBProbeResult FB_ProbeMedia(const char *path, FBSource *info,
                            char *why, size_t why_len);

#endif
