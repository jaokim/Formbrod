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
int  App_Export(const char *path, int preview);
void App_CancelRender(void);

/* Called from the main loop while rendering. Returns progress. */
FBProgress App_PollRender(void);

void App_SetStatus(const char *fmt, ...);

/* Translated text for a job error, for the status line. */
const char *App_JobErrorString(FBJobError err);

/* Media probing (src/backend/media.c). Fills duration and stream flags. */
int  FB_ProbeMedia(const char *path, fb_ms_t *duration, int *has_video,
                   int *has_audio);

#endif
