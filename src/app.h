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
#include "formbrod/cache.h"
#include "formbrod/thumbs.h"
#include "formbrod/waves.h"
#include "formbrod/player.h"

typedef struct FBApp {
    FBTimeline  timeline;
    FBBackend  *backend;
    int         rendering;
    int         quit;
    char        status[256];   /* last status line, shown in the window */
    char        project[FB_PATH_LEN]; /* project file, "" = never saved */
    int         modified;      /* changed since it was opened or saved */
    /* The media cache: proxies made in the background (cache.h). Its jobs
     * run on their own backend, so they can later go to a render server
     * while exports stay local, or the other way round. */
    FBCache     cache;
    FBBackend  *cache_backend;
    int         caching;       /* a cache job is running: 1 a proxy or a
                                * thumbnail strip, 2 a piece rendered in
                                * the background */
    /* Film-strip thumbnails in memory, read from the strips the cache
     * makes (thumbs.h). The GUI draws from it; App_LoadThumbs() fills it. */
    FBThumbStore thumbs;
    /* Sound peaks for the lanes' waveforms (waves.h), the same way. */
    FBWaveStore  waves;
    unsigned    gen;           /* counts edits (render.h keys follow it) */
    /* Background rendering (render.h). */
    struct {
        int      piece;        /* the piece being rendered */
        char     key[FB_KEY_LEN];
        unsigned gen;          /* the edit it was started for */
        unsigned keys_gen;     /* the edit the piece keys are for */
        int      keys_valid;
        int      pieces;
        unsigned done_gen;     /* the edit everything was rendered for */
        int      done_valid;
        fb_ms_t  near;         /* render from here first (last played) */
    } bg;
    /* A rolling preview from the playhead (App_PreviewStart). */
    struct {
        int     on;
        int     rendering;     /* App.backend is rendering a window */
        fb_ms_t from, end;     /* in the edit */
        int     count;         /* windows from `from` to `end` */
        int     next;          /* the next window to render */
        int     ready;         /* windows rendered so far */
        int     gone;          /* windows deleted behind playback */
        long    clock0;        /* when window 0 was ready (ms), or -1 */
        long    started;       /* when the preview was asked for (ms) */
    } preview;
    /* Playing in the preview window (App_PlayStart). */
    struct {
        int      on;
        int      ended;        /* the backend has given everything */
        int      sound;        /* sound goes out through AHI */
        fb_ms_t  from, end;    /* in the edit */
        FBPlayer pl;
        long long shown_at;    /* clock of the last time text */
        int      step, steps;  /* the plan's step playing, and how many */
        int      job_audio;    /* the edit has sound */
        FBPlayerFeed feed;     /* the step's frames into the player */
        long     dry_ms;       /* sound that wasn't there in time */
        fb_ms_t  first_dry;    /* where it first ran dry, or -1 */
    } play;
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
/* Moves a clip to start at `start` in the edit; the clips after it on its
 * track move along. Moving it later leaves a gap in front of it. */
int  App_MoveClip(int clip, fb_ms_t start);
/* Moves a clip in between the clips of a track (its own, another, or a
 * new one if `track` is the number of tracks), where a clip starting at
 * `at` has its middle; see FB_TimelineMoveClipTo(). Its old place closes
 * up. Returns its new index, or -1. */
int  App_MoveClipTo(int clip, int track, fb_ms_t at);
/* Gives a clip a crossfade `len` ms long from the clip before it on its
 * track, or takes it away (0); it and the clips after it move earlier by
 * the overlap. A longer one than fits is shortened. `kind` is an ffmpeg
 * xfade name ("fade", "wipeleft"...), NULL to keep it. Returns the length
 * set, or -1. */
fb_ms_t App_SetCrossfade(int clip, fb_ms_t len, const char *kind);
int  App_SetIn(int clip, fb_ms_t at);
int  App_SetOut(int clip, fb_ms_t at);
int  App_RemoveClip(int clip);
int  App_AddEffect(int clip, const char *module);
int  App_SetEffectParam(int clip, int effect, const char *name, double value);
int  App_RemoveEffect(int clip, int effect);
/* A clip's fade in and out (the "fade" effect), in ms; -1 keeps one as it
 * is. Both 0 takes the effect away. Each is kept within the clip. */
int  App_SetFades(int clip, fb_ms_t in, fb_ms_t out);
/* A clip's fade in and out in ms, 0 when it has none. */
void App_GetFades(int clip, fb_ms_t *in, fb_ms_t *out);
/* A clip's sound level: 1.0 = as recorded, up to FB_MAX_VOLUME. */
int  App_SetVolume(int clip, double volume);

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

/* The media cache. Video files get a small proxy, made in the background
 * when they're added or a project is opened, once per file; previews are
 * rendered from proxies that are ready (the export always uses the
 * originals). Settings: "MakeProxies" (1/0, default 1), "CacheDir"
 * (default PROGDIR:Cache) and "ProxyWidth" ("auto" or a width; auto picks
 * one from the CPU, see FB_CacheAutoWidth()).
 *
 * PollCache does a slice of the current cache job; the main loop calls it
 * whenever CacheBusy is true. It pauses while an export runs, and returns
 * 1 when a proxy has just been finished (the status line changed). */
int  App_CacheBusy(void);
int  App_PollCache(void);
int  App_ProxiesEnabled(void);
void App_SetProxies(int on);
/* 0 = auto. ProxyWidth returns the width in use. Changing it starts
 * proxies of the new size (the old ones stay, and are used again if it's
 * changed back). */
int  App_ProxyWidthSetting(void);
int  App_ProxyWidth(void);
void App_SetProxyWidth(int width);

/* Thumbnails for the film strip and waveforms for the sound lanes. Every
 * source with a picture gets a thumbnail strip, and every one with sound
 * a peak file, in the cache drawer, made in the background like the
 * proxies (after its proxy, and from it), while the setting
 * "ShowPictures" (1/0, default 1) is on. SetThumbnails changes and saves
 * it.
 *
 * ThumbSize sets the size of a thumbnail (the film strip's cell) and
 * makes the store; until then nothing is drawn from it. A lookup that
 * misses (FB_ThumbsLock() on App.thumbs, or FB_WavesLock() on App.waves,
 * from any task) sets the signal in ThumbsSigMask; the main loop then
 * calls LoadThumbs and LoadWaves, which read them and return how many
 * came, so the film strip or the lanes can be drawn again. */
int  App_ThumbsEnabled(void);
void App_SetThumbnails(int on);
int  App_ThumbSize(int w, int h);
unsigned long App_ThumbsSigMask(void);
int  App_LoadThumbs(void);
int  App_LoadWaves(void);

/* A preview from the playhead that goes on until it's stopped or the edit
 * ends. It is rendered as 10-second windows (FB_TimelineWindow), each its
 * own file T:formbrod-preview-NNN.mp4, read from proxies where they're
 * ready, and each input jumps straight to its first frame used.
 * T:formbrod-preview.m3u lists every window from the start, so a player
 * can be started at once on the playlist; rendering stays at most two
 * windows ahead of where a player started then would be, and old windows
 * are deleted behind it. The setting "PreviewPlayer" (a command, the
 * playlist's path is added after it) starts a player when the first
 * window is ready; empty = none.
 *
 * PollPreview starts the next window when it's time; the main loop calls
 * it while PreviewWaiting is true. */
int  App_PreviewStart(fb_ms_t from);
void App_PreviewStop(void);
int  App_Previewing(void);
int  App_PreviewWaiting(void);
void App_PollPreview(void);
const char *App_PreviewPlaylist(void);

/* Playing the edit in the preview window, from `from` (the playhead) to
 * the end: the job's frames go straight to the window and the sound to
 * AHI, read from proxies where they're ready. Nothing is written. Late
 * pictures are skipped to keep time. Stop at any point.
 *
 * PollPlay does a turn: some backend work, the picture due now (or NULL;
 * the main loop shows it in the window, and it stays valid until the
 * next call), sound for AHI. `sleep_ms` says how long the loop may wait
 * before the next turn. PlayPosition is where playback is in the edit,
 * for the playhead. */
int  App_PlayStart(fb_ms_t from);
void App_PlayStop(void);
int  App_Playing(void);
const FBPlayFrame *App_PollPlay(long *sleep_ms);
fb_ms_t App_PlayPosition(void);
/* "0:12,3 of 1:05,0", for under the picture. */
void App_PlayTimeText(char *buf, size_t len);

/* Background rendering: while the window is left alone and the proxies
 * are made, the edit is rendered in pieces into the cache drawer
 * (render.h), from where it was last played on; playing then reads
 * rendered pieces instead of rendering them live. Setting
 * "BackgroundRender" (1/0, default 1). */
int  App_BackgroundRender(void);
void App_SetBackgroundRender(int on);
/* How many pieces the edit has, and how many are rendered. */
void App_RenderedPieces(int *ready, int *total);

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
