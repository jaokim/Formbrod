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
    char        project[FB_PATH_LEN]; /* project file, "" = never saved */
    int         modified;      /* changed since it was opened or saved */
} FBApp;

extern FBApp App;

int  App_Init(void);
void App_Cleanup(void);

/* Projects (see formbrod/project.h for the file format). New and Open
 * replace the timeline without asking; the GUI asks about unsaved changes
 * first. Save with path NULL saves to App.project. 0 on success. */
int  App_NewProject(void);
int  App_OpenProject(const char *path);
int  App_SaveProject(const char *path);

/* Whether the file starts like a project file, so a file given on the
 * command line or dropped on the window can be opened rather than added. */
int  App_IsProjectFile(const char *path);

/* Adds a file as a source and appends it as one clip. 0 on success. */
/* Adds a file on a new track of its own; returns the track or -1.
 * AppendFile puts it after the last clip on an existing track. */
int  App_AddFile(const char *path);
int  App_AppendFile(int track, const char *path);
/* Splits at `at` on one track, or on all of them when track is -1. */
int  App_Split(int track, fb_ms_t at);
/* Tracks: remove one (with its clips; the tracks above move down), or
 * turn off its sound or picture. */
int  App_RemoveTrack(int track);
int  App_SetTrackMuted(int track, int muted);
int  App_SetTrackHidden(int track, int hidden);

/* Trimming. TrimClip sets a clip's in and out points in its file; the
 * clips after it on its track move along. SetIn / SetOut trim at timeline
 * time `at` (the playhead): SetIn drops what's before it, SetOut what's
 * after it, or makes the clip longer if `at` is past its end. With clip
 * -1 they work on the picture seen at `at`. */
int  App_TrimClip(int clip, fb_ms_t in, fb_ms_t out);
int  App_SetIn(int clip, fb_ms_t at);
int  App_SetOut(int clip, fb_ms_t at);
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
    FB_PROBE_NO_VIDEO,      /* neither picture nor sound */
    FB_PROBE_NO_DURATION
} FBProbeResult;

/* `why` receives ffmpeg's error text for FB_PROBE_CANT_OPEN (English). */
FBProbeResult FB_ProbeMedia(const char *path, FBSource *info,
                            char *why, size_t why_len);

/* The log file (formbrod/log.h), from the settings "Log" (1/0, default
 * 1), "LogFile" (default T:Formbrod.log) and "LogLevel" (error, warning,
 * info, debug). StartLog starts a new file; SetLogging turns it on or off
 * and saves that. */
int         App_LogEnabled(void);
const char *App_LogFile(void);
void        App_StartLog(void);
void        App_StopLog(void);
void        App_SetLogging(int on);

#endif
