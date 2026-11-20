#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "app.h"
#include "formbrod/module.h"
#include "formbrod/project.h"
#include "formbrod/log.h"
#include "formbrod/timeview.h"
#include "formbrod/backend.h"
#include "prefs/prefs.h"
#include "version.h"
#include "locale/catalog.h"
#include "player/audio.h"
#include "formbrod/render.h"

#include <time.h>

#ifdef __amigaos4__
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>   /* SYS_Asynch etc. */
#endif

static void cache_start(void);
static void cache_request(FBSource *src);
static void cache_stop(void);
static void thumbs_start(void);
static void thumbs_end(void);
static void still_cleanup(void);

/* Every change to the edit goes through here: it's unsaved, and what the
 * background renders (render.h) has to be looked at again. */
static void edited(void)
{
    App.modified = 1;
    App.gen++;
}
static int preview_window_done(const FBResult *r);

FBApp App;

int App_Init(void)
{
    memset(&App, 0, sizeof(App));
    FB_TimelineInit(&App.timeline);
    FB_RegisterBuiltinModules();
    /* ffmpeg's messages go to the log file, or nowhere: never the shell.
     * What matters reaches the user through the status line. */
    FB_BackendQuiet();
    App.backend = FB_CreateLocalBackend();
    if (!App.backend)
        return -1;
    /* Proxies are made locally for now. A RemoteBackend here would make
     * them on a render server, into a cache drawer both can see. */
    App.cache_backend = FB_CreateLocalBackend();
    thumbs_start();
    cache_start();
    App_SetStatus("%s", FB_Str(MSG_STATUS_WELCOME));
    return 0;
}

void App_Cleanup(void)
{
    App_PlayStop();
    Audio_Close();
    App_PreviewStop();
    still_cleanup();
    if (App.range.anim) {
        FB_AnimAbort(App.range.anim);     /* no half-made ANIM left */
        App.range.anim = NULL;
    }
    if (App.cache_backend) {
        cache_stop();   /* no half-made proxies left behind */
        App.cache_backend->Dispose(App.cache_backend);
        App.cache_backend = NULL;
    }
    if (App.backend) {
        App.backend->Dispose(App.backend);
        App.backend = NULL;
    }
    thumbs_end();
}

void App_SetStatus(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(App.status, sizeof(App.status), fmt, ap);
    va_end(ap);
    /* What the user was told, in their language. */
    FB_Log(FB_LOG_INFO, "status", "%s", App.status);
}

/* The job and an ffmpeg command line that does the same thing, so a
 * render can be tried on another computer. */
static void log_job(const FBJob *job)
{
    static char text[FB_FILTERGRAPH_LEN + 8192];

    if (!FB_LogWants(FB_LOG_INFO))
        return;
    if (FB_JobWrite(job, text, sizeof(text)) > 0) {
        FB_Log(FB_LOG_INFO, "job", "Job:");
        FB_LogText(FB_LOG_INFO, "job", text);
    }
    if (FB_JobToCommandLine(job, text, sizeof(text)) > 0) {
        FB_Log(FB_LOG_INFO, "job", "The same as an ffmpeg command:");
        FB_LogText(FB_LOG_INFO, "job", text);
    }
}

/* ---- Log --------------------------------------------------------------- */

#define LOG_FILE_DEFAULT "T:Formbrod.log"

int App_LogEnabled(void)
{
    return strcmp(Prefs_GetString("Log", "1"), "0") != 0;
}

const char *App_LogFile(void)
{
    const char *path = Prefs_GetString("LogFile", LOG_FILE_DEFAULT);

    return *path ? path : LOG_FILE_DEFAULT;
}

void App_StartLog(void)
{
    FBLogLevel level = FB_LogLevelFromName(Prefs_GetString("LogLevel",
                                                           "info"),
                                           FB_LOG_INFO);

    if (!App_LogEnabled() || FB_LogOpen(App_LogFile(), level) != 0)
        return;
    FB_Log(FB_LOG_INFO, "app", "Formbrod " FB_VERSION " (" FB_VERSION_DATE
           "), log level %s", FB_LogLevelName(level));
    FB_BackendLogStart();
}

void App_StopLog(void)
{
    if (!FB_LogIsOpen())
        return;
    FB_BackendLogStop();
    FB_Log(FB_LOG_INFO, "app", "Log closed");
    FB_LogClose();
}

void App_SetLogging(int on)
{
    Prefs_SetString("Log", on ? "1" : "0");
    Prefs_Save();
    if (on && !FB_LogIsOpen())
        App_StartLog();
    else if (!on)
        App_StopLog();
}

/* ---- Projects ---------------------------------------------------------- */

int App_NewProject(void)
{
    FB_TimelineInit(&App.timeline);
    App.project[0] = '\0';
    App.modified = 0;
    App.gen++;          /* another edit: pieces to render again */
    App_SetStatus("%s", FB_Str(MSG_STATUS_NEW_PROJECT));
    return 0;
}

/* Reads a whole file into a NUL-terminated buffer, or NULL. */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *text = NULL;
    long size;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) >= 0 &&
        size < 4L * 1024 * 1024 && fseek(f, 0, SEEK_SET) == 0 &&
        (text = malloc((size_t)size + 1)) != NULL) {
        if (fread(text, 1, (size_t)size, f) != (size_t)size) {
            free(text);
            text = NULL;
        } else {
            text[size] = '\0';
        }
    }
    fclose(f);
    return text;
}

int App_IsProjectFile(const char *path)
{
    static const char magic[] = "FORMBROD-PROJECT";
    char head[sizeof(magic)];
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f)
        return 0;
    n = fread(head, 1, sizeof(magic) - 1, f);
    fclose(f);
    return n == sizeof(magic) - 1 && memcmp(head, magic, n) == 0;
}

static const char *project_reason(FBProjectError err)
{
    switch (err) {
    case FB_PROJECT_OK:          return "";
    case FB_PROJECT_NOT_PROJECT: return FB_Str(MSG_PROJECT_NOT_PROJECT);
    case FB_PROJECT_TOO_NEW:     return FB_Str(MSG_PROJECT_TOO_NEW);
    case FB_PROJECT_BAD_LINE:    return FB_Str(MSG_PROJECT_BAD_LINE);
    case FB_PROJECT_BAD_SOURCE:  return FB_Str(MSG_PROJECT_BAD_SOURCE);
    case FB_PROJECT_TOO_MANY:    return FB_Str(MSG_PROJECT_TOO_MANY);
    }
    return FB_Str(MSG_JOB_UNKNOWN);
}

int App_OpenProject(const char *path)
{
    static FBTimeline tl;   /* large; keep it off the stack */
    char dir[FB_PATH_LEN];
    FBProjectError err;
    char *text;
    int line = 0, missing = 0, i, c;

    text = read_file(path);
    if (!text) {
        App_SetStatus(FB_Str(MSG_STATUS_CANT_OPEN_PROJECT), path);
        return -1;
    }
    FB_PathDir(path, dir, sizeof(dir));
    err = FB_ProjectParse(&tl, text, dir, &line);
    free(text);
    if (err == FB_PROJECT_NOT_PROJECT || err == FB_PROJECT_TOO_NEW) {
        App_SetStatus(FB_Str(MSG_STATUS_NOT_OPENED), path,
                      project_reason(err));
        return -1;
    }
    if (err != FB_PROJECT_OK) {
        App_SetStatus(FB_Str(MSG_STATUS_BAD_PROJECT), path, line,
                      project_reason(err));
        return -1;
    }

    /* The file only keeps paths; ask the files themselves for the rest.
     * A missing file keeps its clips, so the project can still be saved
     * once its path is fixed, and lasts as long as its longest clip. */
    for (i = 0; i < tl.num_sources; i++) {
        FBSource *src = &tl.sources[i];
        FBSource info;
        char why[128];

        if (FB_ProbeMedia(src->path, &info, why, sizeof(why)) == FB_PROBE_OK) {
            src->duration  = info.duration;
            src->has_video = info.has_video;
            src->has_audio = info.has_audio;
            src->still     = info.still;
            src->loops     = info.loops;
            src->intra     = info.intra;
            FB_SourceSetVideo(src, info.width, info.height,
                              info.sar_num, info.sar_den,
                              info.fps_num, info.fps_den);
            cache_request(src);
        } else {
            missing++;
            for (c = 0; c < tl.num_clips; c++)
                if (tl.clips[c].source == i && tl.clips[c].out > src->duration)
                    src->duration = tl.clips[c].out;
        }
    }

    App.timeline = tl;
    strncpy(App.project, path, sizeof(App.project) - 1);
    App.project[sizeof(App.project) - 1] = '\0';
    App.modified = 0;
    App.gen++;          /* another edit: pieces to render again */
    if (missing)
        App_SetStatus(FB_Str(MSG_STATUS_OPENED_MISSING), path, missing);
    else
        App_SetStatus(FB_Str(MSG_STATUS_OPENED), path);
    return 0;
}

int App_SaveProject(const char *path)
{
    char dir[FB_PATH_LEN];
    char *text = NULL;
    size_t size = 16 * 1024;
    FILE *f;
    int n = -1, ok;

    if (!path || !*path)
        path = App.project;
    if (!*path) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_NO_PROJECT_FILE));
        return -1;
    }

    FB_PathDir(path, dir, sizeof(dir));
    while (n < 0 && size <= 4 * 1024 * 1024) {
        char *bigger = realloc(text, size);

        if (!bigger)
            break;
        text = bigger;
        n = FB_ProjectWrite(&App.timeline, dir, text, size);
        size *= 2;
    }
    if (n < 0) {
        free(text);
        App_SetStatus(FB_Str(MSG_STATUS_CANT_SAVE), path);
        return -1;
    }

    f = fopen(path, "wb");
    ok = f && fwrite(text, 1, (size_t)n, f) == (size_t)n;
    if (f && fclose(f) != 0)
        ok = 0;
    free(text);
    if (!ok) {
        App_SetStatus(FB_Str(MSG_STATUS_CANT_SAVE), path);
        return -1;
    }

    if (path != App.project) {
        strncpy(App.project, path, sizeof(App.project) - 1);
        App.project[sizeof(App.project) - 1] = '\0';
    }
    App.modified = 0;
    App_SetStatus(FB_Str(MSG_STATUS_SAVED), path);
    return 0;
}

/* Probes a file and adds it as a source. Returns the source index, or -1
 * with the status line saying why. */
static int add_source(const char *path)
{
    FBSource info;
    char why[128] = "";
    int src;

    switch (FB_ProbeMedia(path, &info, why, sizeof(why))) {
    case FB_PROBE_OK:
        break;
    case FB_PROBE_NO_LIBAV:
        App_SetStatus("%s", FB_Str(MSG_STATUS_NO_LIBAV));
        return -1;
    case FB_PROBE_CANT_OPEN:
        App_SetStatus(FB_Str(MSG_STATUS_CANT_OPEN), path, why);
        return -1;
    case FB_PROBE_NO_VIDEO:
        App_SetStatus(FB_Str(MSG_STATUS_NO_VIDEO), path);
        return -1;
    case FB_PROBE_NO_DURATION:
        App_SetStatus(FB_Str(MSG_STATUS_NO_DURATION), path);
        return -1;
    default:
        App_SetStatus(FB_Str(MSG_STATUS_CANT_READ), path);
        return -1;
    }
    src = FB_TimelineAddSource(&App.timeline, path, info.duration,
                               info.has_video, info.has_audio);
    if (src < 0) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_TOO_MANY));
        return -1;
    }
    FB_SourceSetVideo(&App.timeline.sources[src], info.width, info.height,
                      info.sar_num, info.sar_den, info.fps_num, info.fps_den);
    App.timeline.sources[src].still = info.still;
    App.timeline.sources[src].loops = info.loops;
    App.timeline.sources[src].intra = info.intra;
    cache_request(&App.timeline.sources[src]);
    return src;
}

/* How long a new clip of a source is: all of it, or for a still picture
 * FB_STILL_LENGTH (Set out or TRIM makes it longer). */
static fb_ms_t new_clip_length(int src)
{
    const FBSource *s = &App.timeline.sources[src];

    return s->still ? FB_STILL_LENGTH : s->duration;
}

/* Undoes add_source() when the clip couldn't be placed. */
static void drop_last_source(void)
{
    if (App.timeline.num_sources > 0)
        App.timeline.num_sources--;
}

int App_AddFile(const char *path)
{
    FBTimeline *tl = &App.timeline;
    int src = add_source(path);

    if (src < 0)
        return -1;
    /* Always a new track: a picture track (playing the file's sound too)
     * for a video, a sound track for music. */
    if (FB_TimelineAppendToTrack(tl, tl->num_tracks, src, 0,
                                 new_clip_length(src)) < 0) {
        drop_last_source();
        App_SetStatus("%s", FB_Str(MSG_STATUS_TOO_MANY));
        return -1;
    }
    edited();
    App_SetStatus(FB_Str(MSG_STATUS_ADDED), path);
    return tl->num_tracks - 1;
}

int App_AppendFile(int track, const char *path)
{
    FBTimeline *tl = &App.timeline;
    const FBSource *s;
    int src;

    if (track < 0 || track >= tl->num_tracks) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_NO_SUCH_TRACK));
        return -1;
    }
    if ((src = add_source(path)) < 0)
        return -1;
    s = &tl->sources[src];
    if (tl->tracks[track].kind == FB_TRACK_VIDEO ? !s->has_video
                                                 : !s->has_audio) {
        drop_last_source();
        App_SetStatus(FB_Str(MSG_STATUS_WRONG_TRACK), path);
        return -1;
    }
    if (FB_TimelineAppendToTrack(tl, track, src, 0, new_clip_length(src))
        < 0) {
        drop_last_source();
        App_SetStatus("%s", FB_Str(MSG_STATUS_TOO_MANY));
        return -1;
    }
    edited();
    App_SetStatus(FB_Str(MSG_STATUS_ADDED), path);
    return 0;
}

int App_Split(int track, fb_ms_t at)
{
    if (FB_TimelineSplit(&App.timeline, track, at) < 0) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_NOTHING_TO_SPLIT));
        return -1;
    }
    edited();
    return 0;
}

/* ---- Tracks ------------------------------------------------------------ */

static int valid_track(int track)
{
    if (track < 0 || track >= App.timeline.num_tracks) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_NO_SUCH_TRACK));
        return 0;
    }
    return 1;
}

int App_RemoveTrack(int track)
{
    if (!valid_track(track))
        return -1;
    FB_TimelineRemoveTrack(&App.timeline, track);
    edited();
    return 0;
}

int App_SetTrackMuted(int track, int muted)
{
    if (!valid_track(track))
        return -1;
    App.timeline.tracks[track].muted = muted != 0;
    edited();
    return 0;
}

int App_SetTrackHidden(int track, int hidden)
{
    if (!valid_track(track))
        return -1;
    App.timeline.tracks[track].hidden = hidden != 0;
    edited();
    return 0;
}

/* ---- Trimming --------------------------------------------------------- */

static const char *file_part(const char *path)
{
    const char *p = path, *name = path;

    for (; *p; p++)
        if (*p == '/' || *p == ':')
            name = p + 1;
    return name;
}

/* "Holiday.mp4 now runs from 0:12.4 to 0:31.0" */
static void trimmed_status(int clip)
{
    const FBClip *c = &App.timeline.clips[clip];
    char in[24], out[24];

    if (App.timeline.sources[c->source].still) {
        /* A picture has no place in a file, only a length. */
        FB_FormatTimecode(out, sizeof(out), FB_US(c->out - c->in),
                          FB_US(100), 0, Locale_DecimalPoint());
        App_SetStatus(FB_Str(MSG_STATUS_STILL_LENGTH),
                      file_part(App.timeline.sources[c->source].path), out);
        return;
    }
    FB_FormatTimecode(in, sizeof(in), FB_US(c->in), FB_US(100), 0,
                      Locale_DecimalPoint());
    FB_FormatTimecode(out, sizeof(out), FB_US(c->out), FB_US(100), 0,
                      Locale_DecimalPoint());
    App_SetStatus(FB_Str(MSG_STATUS_TRIMMED),
                  file_part(App.timeline.sources[c->source].path), in, out);
}

int App_MoveClip(int clip, fb_ms_t start)
{
    const FBTimeline *tl = &App.timeline;
    char when[24];

    if (clip < 0 || clip >= tl->num_clips) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_NO_SUCH_CLIP));
        return -1;
    }
    if (FB_TimelineMoveClip(&App.timeline, clip, start) != 0) {
        FB_FormatTimecode(when, sizeof(when),
                          FB_US(FB_TimelineEarliestStart(tl, clip)),
                          FB_US(100), 0, Locale_DecimalPoint());
        App_SetStatus(FB_Str(MSG_STATUS_TOO_EARLY), when);
        return -1;
    }
    edited();
    FB_FormatTimecode(when, sizeof(when), FB_US(start), FB_US(100), 0,
                      Locale_DecimalPoint());
    App_SetStatus(FB_Str(MSG_STATUS_MOVED),
                  file_part(tl->sources[tl->clips[clip].source].path), when);
    return 0;
}

fb_ms_t App_SetCrossfade(int clip, fb_ms_t len, const char *kind)
{
    FBTimeline *tl = &App.timeline;
    const char *name;
    char when[24];
    fb_ms_t got;

    if (clip < 0 || clip >= tl->num_clips) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_NO_SUCH_CLIP));
        return -1;
    }
    got = FB_TimelineSetCrossfade(tl, clip, len, kind);
    if (got < 0) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_CANT_CROSSFADE));
        return -1;
    }
    edited();
    name = file_part(tl->sources[tl->clips[clip].source].path);
    if (got == 0) {
        App_SetStatus(FB_Str(MSG_STATUS_NO_CROSSFADE), name);
    } else {
        FB_FormatTimecode(when, sizeof(when), FB_US(got), FB_US(100), 0,
                          Locale_DecimalPoint());
        App_SetStatus(FB_Str(MSG_STATUS_CROSSFADE), name, when);
    }
    FB_Log(FB_LOG_INFO, "edit", "crossfade into clip %d: %lld ms, %s", clip,
           (long long)got, FB_ClipCrossfadeKind(&tl->clips[clip]));
    return got;
}

int App_MoveClipTo(int clip, int track, fb_ms_t at)
{
    FBTimeline *tl = &App.timeline;
    const char *name;
    char when[24];
    int idx;

    if (clip < 0 || clip >= tl->num_clips) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_NO_SUCH_CLIP));
        return -1;
    }
    if (track < 0 || track > tl->num_tracks) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_NO_SUCH_TRACK));
        return -1;
    }
    name = file_part(tl->sources[tl->clips[clip].source].path);
    idx = FB_TimelineMoveClipTo(tl, clip, track, at);
    if (idx < 0) {
        App_SetStatus(FB_Str(MSG_STATUS_WRONG_TRACK), name);
        return -1;
    }
    edited();
    FB_FormatTimecode(when, sizeof(when), FB_US(tl->clips[idx].start),
                      FB_US(100), 0, Locale_DecimalPoint());
    App_SetStatus(FB_Str(MSG_STATUS_MOVED), name, when);
    return idx;
}

int App_TrimClip(int clip, fb_ms_t in, fb_ms_t out)
{
    if (clip < 0 || clip >= App.timeline.num_clips) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_NO_SUCH_CLIP));
        return -1;
    }
    if (FB_TimelineTrimClip(&App.timeline, clip, in, out) != 0) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_BAD_TRIM));
        return -1;
    }
    edited();
    trimmed_status(clip);
    return 0;
}

/* The clip Set in / Set out work on: `clip`, or if that is -1 the picture
 * seen at `at`. */
static int clip_for(int clip, fb_ms_t at)
{
    if (clip < 0)
        clip = FB_TimelineClipAt(&App.timeline, at, NULL);
    if (clip < 0 || clip >= App.timeline.num_clips) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_NO_CLIP_HERE));
        return -1;
    }
    return clip;
}

int App_SetIn(int clip, fb_ms_t at)
{
    const FBClip *c;

    if ((clip = clip_for(clip, at)) < 0)
        return -1;
    c = &App.timeline.clips[clip];
    if (at < c->start || at >= FB_ClipEnd(c)) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_OUTSIDE_CLIP));
        return -1;
    }
    return App_TrimClip(clip, c->in + (at - c->start), c->out);
}

int App_SetOut(int clip, fb_ms_t at)
{
    const FBClip *c;
    fb_ms_t out, dur;

    if ((clip = clip_for(clip, at)) < 0)
        return -1;
    c = &App.timeline.clips[clip];
    if (at <= c->start) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_OUTSIDE_CLIP));
        return -1;
    }
    /* Past the clip's end makes it longer, as far as the file goes (a
     * still picture or a looping animation can be any length). */
    out = c->in + (at - c->start);
    dur = FB_SourceLimit(&App.timeline.sources[c->source]);
    if (dur > 0 && out > dur)
        out = dur;
    return App_TrimClip(clip, c->in, out);
}

int App_RemoveClip(int clip)
{
    if (FB_TimelineRemoveClip(&App.timeline, clip) != 0)
        return -1;
    edited();
    return 0;
}

int App_AddEffect(int clip, const char *module)
{
    const FBModule *mod = FB_FindModule(module);
    FBEffect *fx;
    int i;

    if (clip < 0 || clip >= App.timeline.num_clips || !mod) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_NO_SUCH_CLIP));
        return -1;
    }
    fx = FB_ClipAddEffect(&App.timeline.clips[clip], module);
    if (!fx)
        return -1;
    for (i = 0; i < mod->num_params; i++)
        FB_EffectSetParam(fx, mod->params[i].name, mod->params[i].def);
    edited();
    return App.timeline.clips[clip].num_effects - 1;
}

int App_SetEffectParam(int clip, int effect, const char *name, double value)
{
    FBClip *c;

    if (clip < 0 || clip >= App.timeline.num_clips)
        return -1;
    c = &App.timeline.clips[clip];
    if (effect < 0 || effect >= c->num_effects)
        return -1;
    if (FB_EffectSetParam(&c->effects[effect], name, value) != 0)
        return -1;
    edited();
    return 0;
}

int App_RemoveEffect(int clip, int effect)
{
    if (clip < 0 || clip >= App.timeline.num_clips ||
        FB_ClipRemoveEffect(&App.timeline.clips[clip], effect) != 0)
        return -1;
    edited();
    return 0;
}

void App_GetFades(int clip, fb_ms_t *in, fb_ms_t *out)
{
    const FBClip *c;
    int fx;

    *in = *out = 0;
    if (clip < 0 || clip >= App.timeline.num_clips)
        return;
    c = &App.timeline.clips[clip];
    if ((fx = FB_ClipFindEffect(c, "fade")) < 0)
        return;
    /* The module's defaults when a parameter isn't set. */
    *in  = (fb_ms_t)(FB_EffectGetParam(&c->effects[fx], "in", 1.0) * 1000.0
                     + 0.5);
    *out = (fb_ms_t)(FB_EffectGetParam(&c->effects[fx], "out", 0.0) * 1000.0
                     + 0.5);
}

int App_SetFades(int clip, fb_ms_t in, fb_ms_t out)
{
    FBClip *c;
    fb_ms_t old_in, old_out, len;
    int fx;

    if (clip < 0 || clip >= App.timeline.num_clips) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_NO_SUCH_CLIP));
        return -1;
    }
    c = &App.timeline.clips[clip];
    App_GetFades(clip, &old_in, &old_out);
    if (in < 0)
        in = old_in;
    if (out < 0)
        out = old_out;
    len = c->out - c->in;
    if (in > len)
        in = len;
    if (out > len - in)
        out = len - in;
    fx = FB_ClipFindEffect(c, "fade");
    if (in == 0 && out == 0) {
        if (fx >= 0)
            FB_ClipRemoveEffect(c, fx);
    } else {
        if (fx < 0) {
            if (!FB_ClipAddEffect(c, "fade"))
                return -1;
            fx = c->num_effects - 1;
        }
        FB_EffectSetParam(&c->effects[fx], "in", in / 1000.0);
        FB_EffectSetParam(&c->effects[fx], "out", out / 1000.0);
    }
    edited();
    return 0;
}

int App_SetVolume(int clip, double volume)
{
    if (clip < 0 || clip >= App.timeline.num_clips) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_NO_SUCH_CLIP));
        return -1;
    }
    if (volume < 0.0)
        volume = 0.0;
    if (volume > FB_MAX_VOLUME)
        volume = FB_MAX_VOLUME;
    App.timeline.clips[clip].volume = volume;
    edited();
    return 0;
}

int App_SetClipFit(int clip, int fit)
{
    if (clip < 0 || clip >= App.timeline.num_clips) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_NO_SUCH_CLIP));
        return -1;
    }
    if (fit < FB_FIT_DEFAULT || fit >= FB_FIT_COUNT)
        return -1;
    App.timeline.clips[clip].fit = (FBFit)fit;
    edited();
    return 0;
}

int App_SetDefaultFit(int fit)
{
    if (fit <= FB_FIT_DEFAULT || fit >= FB_FIT_COUNT)
        return -1;
    App.timeline.format.fit = (FBFit)fit;
    edited();
    return 0;
}

int App_SetFrameSize(int width, int height)
{
    if (width == 0 && height == 0) {
        App.timeline.format.width = App.timeline.format.height = 0;
        edited();
        return 0;
    }
    if (width < 2 || height < 2 || width > 16384 || height > 16384)
        return -1;
    /* stored even, as the encoders need */
    App.timeline.format.width  = width & ~1;
    App.timeline.format.height = height & ~1;
    edited();
    return 0;
}

int App_SetFrameRate(int num, int den)
{
    if (num == 0 && den == 0) {
        App.timeline.format.fps_num = App.timeline.format.fps_den = 0;
        edited();
        return 0;
    }
    if (num <= 0 || den <= 0 || num / den > 1000)
        return -1;
    App.timeline.format.fps_num = num;
    App.timeline.format.fps_den = den;
    edited();
    return 0;
}

int App_SetPadColor(unsigned long rgb)
{
    if (rgb > 0xFFFFFFUL)
        return -1;
    App.timeline.format.pad_color = (uint32_t)rgb;
    edited();
    return 0;
}

int App_SetExport(const FBExport *e)
{
    if (!FB_ExportValid(e))
        return -1;
    if (memcmp(e, &App.timeline.format.encode, sizeof(*e)) != 0) {
        App.timeline.format.encode = *e;
        edited();
    }
    return 0;
}

void App_ExportFileName(char *buf, size_t len)
{
    const char *type = App.timeline.format.encode.filetype;
    const char *name = "Formbrod";
    char base[FB_PATH_LEN];
    char *dot;

    if (App.project[0]) {
        const char *p = App.project, *s;

        /* The last part of the path, without its suffix. */
        for (s = p; *s; s++)
            if (*s == '/' || *s == ':')
                p = s + 1;
        snprintf(base, sizeof(base), "%s", p);
        dot = strrchr(base, '.');
        if (dot && dot != base)
            *dot = '\0';
        if (base[0])
            name = base;
    }
    snprintf(buf, len, "%s.%s", name, type);
}

/* FB_JobErrorString() stays in English for logs and render servers; this is
 * the translated version for the window. */
const char *App_JobErrorString(FBJobError err)
{
    switch (err) {
    case FB_JOB_OK:              return "";
    case FB_JOB_EMPTY:           return FB_Str(MSG_JOB_EMPTY);
    case FB_JOB_BAD_SOURCE:      return FB_Str(MSG_JOB_BAD_SOURCE);
    case FB_JOB_UNKNOWN_MODULE:  return FB_Str(MSG_JOB_UNKNOWN_MODULE);
    case FB_JOB_NOT_EXPRESSIBLE: return FB_Str(MSG_JOB_NOT_EXPRESSIBLE);
    case FB_JOB_OVERFLOW:        return FB_Str(MSG_JOB_OVERFLOW);
    case FB_JOB_NEED_SIZE:       return FB_Str(MSG_JOB_NEED_SIZE);
    }
    return FB_Str(MSG_JOB_UNKNOWN);
}

/* The timeline with each source that has a proxy ready reading that
 * instead. The probed info stays the original's: the frame size and shape
 * come from it, and the job scales whatever it reads to that frame. The
 * proxy has the same frames and timing, so clip times stay valid. */
static const FBTimeline *with_proxies(void)
{
    static FBTimeline tl;   /* large; keep it off the stack */
    char path[FB_PATH_LEN];
    int i, used = 0;

    tl = App.timeline;
    for (i = 0; i < tl.num_sources; i++) {
        FBSource *s = &tl.sources[i];

        if (FB_CacheFind(&App.cache, s->key, FB_CACHE_PROXY,
                         path, sizeof(path))) {
            FB_Log(FB_LOG_INFO, "cache", "Preview reads %s for %s",
                   path, s->path);
            strcpy(s->path, path);
            used++;
        }
    }
    if (used < tl.num_sources)
        FB_Log(FB_LOG_INFO, "cache", "%d of %d files have a proxy",
               used, tl.num_sources);
    return &tl;
}

int App_Export(const char *path, int preview)
{
    static FBJob job;   /* large; keep it off the stack */
    FBOutput out;
    FBJobError err;

    App_PlayStop();     /* the backend can do one thing at a time */
    if (App.rendering) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_ALREADY_RENDERING));
        return -1;
    }

    App.range.failed = 0;
    memset(&out, 0, sizeof(out));
    strncpy(out.path, path, sizeof(out.path) - 1);
    out.quality = preview ? FB_QUALITY_PREVIEW : FB_QUALITY_FINAL;
    if (preview) {
        strcpy(out.vcodec, "mpeg4");
        strcpy(out.acodec, "aac");
    } else {
        /* The project's settings (Project > Settings). */
        const FBExport *e = &App.timeline.format.encode;

        strcpy(out.vcodec, e->vcodec);
        strcpy(out.acodec, e->acodec);
        out.vquality = e->vquality;
        out.abitrate = e->abitrate;
        out.arate = e->arate;
        FB_Log(FB_LOG_INFO, "job", "Export: %s (%s), %s %d kbit/s %d Hz",
               e->vcodec, FB_VideoQualityName(e->vquality), e->acodec,
               e->abitrate, e->arate);
    }

    err = FB_JobBuild(&job, preview ? with_proxies() : &App.timeline, &out);
    if (err != FB_JOB_OK) {
        FB_Log(FB_LOG_ERROR, "job", "Can't build the job: %s",
               FB_JobErrorString(err));
        App_SetStatus("%s", App_JobErrorString(err));
        return -1;
    }
    log_job(&job);
    if (App.backend->Submit(App.backend, &job) != 0) {
        App_SetStatus("%s", App.backend->Result(App.backend).message);
        return -1;
    }
    App.rendering = 1;
    App_SetStatus(FB_Str(MSG_STATUS_RENDERING), path);
    return 0;
}

/* ---- The range between the marks ------------------------------------------ */

#define RANGE_ANIM_FPS 10

void App_SetRange(fb_ms_t from, fb_ms_t to)
{
    FB_TimelineSetRange(&App.timeline, from, to);
    App.modified = 1;   /* saved with the project; no need to render again */
}

/* Each frame of an ANIM export goes into the file as it comes. */
static int range_video(void *ud, const unsigned char *argb, int width,
                       int height, int stride, fb_ms_t pts)
{
    (void)ud;
    (void)pts;
    if (App.range.failed)
        return 0;
    if (!App.range.anim) {
        App.range.anim = FB_AnimOpen(App.range.path, width, height,
                                     App.range.fps_num, App.range.fps_den);
        if (!App.range.anim) {
            FB_Log(FB_LOG_ERROR, "range", "Can't make %s", App.range.path);
            App.range.failed = 1;    /* the rest is thrown away */
            return 0;
        }
        FB_Log(FB_LOG_INFO, "range", "ANIM %s: %dx%d", App.range.path,
               width, height);
    }
    if (FB_AnimAdd(App.range.anim, argb, stride) != 0) {
        FB_Log(FB_LOG_ERROR, "range", "Can't write frame %d of %s",
               FB_AnimFrames(App.range.anim), App.range.path);
        App.range.failed = 1;
    }
    return 0;
}

static int range_audio(void *ud, const short *samples, int frames,
                       fb_ms_t pts)
{
    (void)ud;
    (void)samples;
    (void)frames;
    (void)pts;
    return 0;
}

int App_ExportRange(const char *path, int width, int fps)
{
    static FBTimeline win;   /* large; keep them off the stack */
    static FBJob job;
    FBOutput out;
    FBJobError err;
    fb_ms_t from, to;
    int anim = !FB_IsMovieName(path), i, silent;
    size_t plen = strlen(path);

    App_PlayStop();     /* the backend can do one thing at a time */
    if (App.rendering) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_ALREADY_RENDERING));
        return -1;
    }
    if (FB_TimelineRange(&App.timeline, &from, &to) <= 0) {
        App_SetStatus("%s", App_JobErrorString(FB_JOB_EMPTY));
        return -1;
    }
    FB_TimelineWindow(&win, with_proxies(), from, to - from);
    if (fps <= 0 && anim)
        fps = RANGE_ANIM_FPS;
    if (fps > 0) {
        win.format.fps_num = fps;
        win.format.fps_den = 1;
    }
    memset(&out, 0, sizeof(out));
    strncpy(out.path, path, sizeof(out.path) - 1);
    strcpy(out.vcodec, "mpeg4");
    strcpy(out.acodec, "aac");
    out.quality = FB_QUALITY_PREVIEW;
    out.width = width > 0 ? width : FB_PREVIEW_WIDTH;
    /* An ANIM or a GIF has no sound. */
    silent = anim || (plen >= 4 && (strcmp(path + plen - 4, ".gif") == 0
                                    || strcmp(path + plen - 4, ".GIF") == 0));
    if (silent)
        for (i = 0; i < win.num_tracks; i++)
            win.tracks[i].muted = 1;
    err = FB_JobBuild(&job, &win, &out);
    if (err == FB_JOB_EMPTY && silent) {   /* only sound there: black */
        for (i = 0; i < win.num_tracks; i++)
            win.tracks[i].muted = App.timeline.tracks[i].muted;
        err = FB_JobBuild(&job, &win, &out);
    }
    if (err != FB_JOB_OK) {
        FB_Log(FB_LOG_ERROR, "range", "Can't build the job: %s",
               FB_JobErrorString(err));
        App_SetStatus("%s", App_JobErrorString(err));
        return -1;
    }
    FB_Log(FB_LOG_INFO, "range", "%s %lld to %lld ms as %s, %d wide",
           anim ? "ANIM" : "Movie", (long long)from, (long long)to, path,
           out.width);
    log_job(&job);
    if (anim) {
        FBFrameSink sink;

        memset(&App.range, 0, sizeof(App.range));
        strncpy(App.range.path, path, sizeof(App.range.path) - 1);
        if (!FB_TimelineFrameRate(&win, &App.range.fps_num,
                                  &App.range.fps_den)) {
            App.range.fps_num = fps > 0 ? fps : 25;
            App.range.fps_den = 1;
        }
        sink.Video = range_video;
        sink.Audio = range_audio;
        sink.ud = NULL;
        if (App.backend->Play(App.backend, &job, &sink) != 0) {
            App_SetStatus("%s", App.backend->Result(App.backend).message);
            return -1;
        }
        App.rendering = 2;
    } else if (App.backend->Submit(App.backend, &job) != 0) {
        App_SetStatus("%s", App.backend->Result(App.backend).message);
        return -1;
    } else {
        App.rendering = 1;
    }
    App_SetStatus(FB_Str(MSG_STATUS_RENDERING), path);
    return 0;
}

/* The ANIM export ended: finish the file, or remove it. */
static void range_done(FBResult *r)
{
    int ok = r->state == FB_RENDER_DONE && !App.range.failed;

    if (App.range.anim) {
        int frames = FB_AnimFrames(App.range.anim);

        if (ok) {
            ok = FB_AnimClose(App.range.anim) == 0;
            FB_Log(ok ? FB_LOG_INFO : FB_LOG_ERROR, "range",
                   "%s: %d frames%s", App.range.path, frames,
                   ok ? "" : ", but it couldn't be finished");
        } else {
            FB_AnimAbort(App.range.anim);
        }
        App.range.anim = NULL;
    } else if (ok) {
        ok = 0;                         /* not a single frame came */
    }
    if (r->state == FB_RENDER_DONE) {
        r->state = ok ? FB_RENDER_DONE : FB_RENDER_FAILED;
        snprintf(r->message, sizeof(r->message), "%.*s",
                 (int)sizeof(r->message) - 1, App.range.path);
    }
}

void App_CancelRender(void)
{
    if (App.rendering)
        App.backend->Cancel(App.backend);
}

FBProgress App_PollRender(void)
{
    FBProgress p = App.backend->Progress(App.backend);

    if (App.rendering && p.state != FB_RENDER_RUNNING) {
        FBResult r = App.backend->Result(App.backend);

        if (App.rendering == 2) {
            range_done(&r);
            App.range.failed = r.state == FB_RENDER_FAILED;
        }
        App.rendering = 0;
        if (App.preview.rendering)
            preview_window_done(&r);
        else if (r.state == FB_RENDER_DONE)
            App_SetStatus(FB_Str(MSG_STATUS_DONE), r.message);
        else if (r.state == FB_RENDER_CANCELLED)
            App_SetStatus("%s", FB_Str(MSG_STATUS_CANCELLED));
        else
            App_SetStatus(FB_Str(MSG_STATUS_FAILED), r.message);
    }
    /* An ANIM whose file couldn't be finished failed, whatever the
     * backend says (for ARexx RENDERSTATUS). */
    if (!App.rendering && App.range.failed && p.state == FB_RENDER_DONE)
        p.state = FB_RENDER_FAILED;
    return p;
}

/* ---- Media cache ---------------------------------------------------------- */

int App_ProxiesEnabled(void)
{
    return strcmp(Prefs_GetString("MakeProxies", "1"), "0") != 0;
}

int App_ProxyWidthSetting(void)
{
    const char *v = Prefs_GetString("ProxyWidth", "auto");

    return atoi(v) > 0 ? atoi(v) : 0;
}

/* The CPU's clock in MHz and its cores, 0 if unknown. */
static void cpu_info(int *mhz, int *cores)
{
    *mhz = 0;
    *cores = 0;
#ifdef __amigaos4__
    {
        uint64 speed = 0;
        uint32 n = 0;

        /* GCIT_NumberOfCPUs is newer than GCIT_ProcessorSpeed; an exec
         * without it leaves n at 0, which counts as one core. */
        IExec->GetCPUInfoTags(GCIT_ProcessorSpeed, &speed,
                              GCIT_NumberOfCPUs, &n, TAG_DONE);
        *mhz = (int)(speed / 1000000);
        *cores = (int)n;
    }
#endif
}

int App_ProxyWidth(void)
{
    return App.cache.proxy_width;
}

static void cache_start(void)
{
    const char *dir = Prefs_GetString("CacheDir", "PROGDIR:Cache");
    int width = App_ProxyWidthSetting();
    int mhz, cores;

    if (width <= 0) {
        cpu_info(&mhz, &cores);
        width = FB_CacheAutoWidth(mhz, cores);
        FB_Log(FB_LOG_INFO, "cache",
               "CPU at %d MHz with %d core(s): proxies %d wide",
               mhz, cores, width);
    }
#ifdef __amigaos4__
    {
        /* Make the drawer if it isn't there; fails harmlessly if it is. */
        BPTR lock = IDOS->CreateDir(dir);

        if (lock)
            IDOS->UnLock(lock);
    }
#endif
    FB_CacheInit(&App.cache, dir, width);
    if (App.thumbs.w > 0)
        FB_CacheSetThumbSize(&App.cache, App.thumbs.w, App.thumbs.h);
    FB_Log(FB_LOG_INFO, "cache", "Cache in %s, proxies %d wide",
           dir, App.cache.proxy_width);
}

/* Reads the file's fingerprint if needed and asks for its proxy, its
 * thumbnail strip and its sound peaks, as the settings say. */
static void cache_request(FBSource *src)
{
    int proxy = App_ProxiesEnabled() && src->has_video && !src->still;
    int thumbs = App_ThumbsEnabled();
    FBCacheState st;

    if (!App.cache_backend || (!proxy && !thumbs) ||
        (!src->has_video && !src->has_audio))
        return;
    if (!src->key[0] &&
        FB_CacheKey(src->path, src->key) != 0) {
        FB_Log(FB_LOG_WARNING, "cache", "%s: can't read its fingerprint",
               src->path);
        return;
    }
    if (proxy) {
        st = FB_CacheRequest(&App.cache, src);
        if (st == FB_CACHE_QUEUED)
            FB_Log(FB_LOG_INFO, "cache", "%s (%s): proxy queued",
                   src->path, src->key);
        else if (st == FB_CACHE_READY)
            FB_Log(FB_LOG_INFO, "cache", "%s (%s): proxy ready",
                   src->path, src->key);
    }
    if (thumbs &&
        FB_CacheRequestThumbs(&App.cache, src) == FB_CACHE_QUEUED)
        FB_Log(FB_LOG_INFO, "cache", "%s (%s): thumbnails queued",
               src->path, src->key);
    if (thumbs &&
        FB_CacheRequestWave(&App.cache, src) == FB_CACHE_QUEUED)
        FB_Log(FB_LOG_INFO, "cache", "%s (%s): waveform queued",
               src->path, src->key);
}

/* Asks again for every source, after the settings change. */
static void cache_request_all(void)
{
    int i;

    for (i = 0; i < App.timeline.num_sources; i++)
        cache_request(&App.timeline.sources[i]);
}

/* Stops the cache job that's running, putting it back in the queue. */
static void cache_stop(void)
{
    if (!App.caching)
        return;
    /* Cancel closes the files at once, so the partial one can go. */
    App.cache_backend->Cancel(App.cache_backend);
    if (App.caching == 2)
        FB_CacheCommit(&App.cache, App.bg.key, FB_CACHE_RENDER, 0);
    else
        FB_CacheAbort(&App.cache);
    App.caching = 0;
}

/* ---- Background rendering ------------------------------------------------ */

#define MAX_PIECES 720                      /* two hours of edit */

static char BgKeys[MAX_PIECES][FB_KEY_LEN]; /* each piece's key */
static char BgOld[MAX_PIECES][FB_KEY_LEN];  /* before the last edit */
static unsigned char BgState[MAX_PIECES];   /* 0 to do, 1 there, 2 failed */
static FBPlayStep PlaySteps[MAX_PIECES];

int App_BackgroundRender(void)
{
    return strcmp(Prefs_GetString("BackgroundRender", "1"), "0") != 0;
}

void App_SetBackgroundRender(int on)
{
    Prefs_SetString("BackgroundRender", on ? "1" : "0");
    Prefs_Save();
    if (!on && App.caching == 2)
        cache_stop();
    App.bg.done_valid = 0;
}

/* The pieces' keys and which are rendered, for the edit as it is now.
 * Worked out again only after an edit. */
static void bg_keys(void)
{
    int i, n;

    if (App.bg.keys_valid && App.bg.keys_gen == App.gen)
        return;
    n = FB_RenderPieces(&App.timeline);
    if (n > MAX_PIECES)
        n = MAX_PIECES;
    memcpy(BgOld, BgKeys, sizeof(BgKeys));
    for (i = 0; i < n; i++) {
        if (FB_RenderPieceKey(&App.timeline, i, BgKeys[i]) != 0)
            BgKeys[i][0] = '\0';
        BgState[i] = !BgKeys[i][0] ? 2 :
                     FB_CacheFind(&App.cache, BgKeys[i], FB_CACHE_RENDER,
                                  NULL, 0) ? 1 : 0;
    }
    for (; i < MAX_PIECES; i++)
        BgKeys[i][0] = '\0';
    App.bg.pieces = n;
    App.bg.keys_gen = App.gen;
    App.bg.keys_valid = 1;
}

void App_RenderedPieces(int *ready, int *total)
{
    int i;

    bg_keys();
    *total = App.bg.pieces;
    *ready = 0;
    for (i = 0; i < App.bg.pieces; i++)
        if (BgState[i] == 1)
            (*ready)++;
}

/* Whether there may be pieces to render (cheap: no keys worked out). */
static int bg_wanted(void)
{
    return App.cache_backend && App_BackgroundRender() &&
           FB_RenderPieces(&App.timeline) > 0 &&
           !(App.bg.done_valid && App.bg.done_gen == App.gen);
}

/* The next piece to render, nearest after where it was last played
 * first; its job into `job`. 0 if every piece is rendered. */
static int bg_next(FBJob *job)
{
    char part[FB_PATH_LEN];
    int k, i, n, start;

    if (!bg_wanted())
        return 0;
    bg_keys();
    n = App.bg.pieces;
    start = FB_RenderPieceAt(App.bg.near);
    if (start >= n)
        start = 0;
    for (k = 0; k < n; k++) {
        i = (start + k) % n;
        if (BgState[i] != 0)
            continue;
        if (FB_CacheFind(&App.cache, BgKeys[i], FB_CACHE_RENDER, NULL, 0)) {
            BgState[i] = 1;                 /* made meanwhile */
            continue;
        }
        if (FB_CachePath(&App.cache, BgKeys[i], FB_CACHE_RENDER, 1, part,
                         sizeof(part)) != 0 ||
            FB_RenderPieceJob(job, with_proxies(), i, part) != FB_JOB_OK) {
            BgState[i] = 2;
            continue;
        }
        App.bg.piece = i;
        strcpy(App.bg.key, BgKeys[i]);
        App.bg.gen = App.gen;
        return 1;
    }
    App.bg.done_gen = App.gen;              /* all there, for this edit */
    App.bg.done_valid = 1;
    FB_Log(FB_LOG_INFO, "render", "Every piece of the edit is rendered");
    return 0;
}

/* A piece is done (or failed): keep it, and remove the one it replaces. */
static void bg_finished(int ok)
{
    int i = App.bg.piece, j;

    ok = ok && FB_CacheCommit(&App.cache, App.bg.key, FB_CACHE_RENDER, 1)
               == 0;
    if (!ok)
        FB_CacheCommit(&App.cache, App.bg.key, FB_CACHE_RENDER, 0);
    if (App.bg.keys_valid && App.bg.keys_gen == App.gen && i < App.bg.pieces
        && strcmp(BgKeys[i], App.bg.key) == 0)
        BgState[i] = ok ? 1 : 2;
    FB_Log(ok ? FB_LOG_INFO : FB_LOG_WARNING, "render",
           ok ? "Piece %d (from %lld ms) rendered" : "Piece %d (from %lld ms) "
           "couldn't be rendered", i, (long long)FB_RenderPieceStart(i));

    /* What this piece was before the last edit, unless another piece
     * still has that key: it would only fill the cache drawer. */
    if (ok && i < MAX_PIECES && BgOld[i][0] && strcmp(BgOld[i], App.bg.key)) {
        char old[FB_PATH_LEN];

        for (j = 0; j < App.bg.pieces; j++)
            if (strcmp(BgKeys[j], BgOld[i]) == 0)
                break;
        if (j == App.bg.pieces &&
            FB_CachePath(&App.cache, BgOld[i], FB_CACHE_RENDER, 0, old,
                         sizeof(old)) == 0)
            remove(old);
        BgOld[i][0] = '\0';
    }
}

void App_SetProxies(int on)
{
    Prefs_SetString("MakeProxies", on ? "1" : "0");
    Prefs_Save();
    if (on) {
        cache_request_all();
    } else {
        /* Forget what was waiting; turning it on again asks anew. */
        cache_stop();
        FB_CacheDrop(&App.cache, FB_CACHE_PROXY);
    }
}

void App_SetProxyWidth(int width)
{
    char v[16];

    if (width > 0)
        snprintf(v, sizeof(v), "%d", width);
    else
        strcpy(v, "auto");
    Prefs_SetString("ProxyWidth", v);
    Prefs_Save();
    cache_stop();
    cache_start();          /* a new, empty queue for the new size */
    cache_request_all();
}

int App_CacheBusy(void)
{
    return App.caching ||
           (App.cache_backend && FB_CachePending(&App.cache) > 0) ||
           bg_wanted();
}

int App_PollCache(void)
{
    static FBJob job;   /* large; keep it off the stack */
    FBProgress p;

    /* An export has the machine to itself; the cache job waits. */
    if (!App.cache_backend || App.rendering || App.play.on)
        return 0;

    if (!App.caching) {
        int t = FB_CacheNextJob(&App.cache, &job);

        /* Proxies first; then the edit, piece by piece. */
        if (t < 0) {
            if (!bg_next(&job))
                return 0;
            FB_Log(FB_LOG_INFO, "render", "Rendering piece %d (from %lld "
                   "ms) in the background", App.bg.piece,
                   (long long)FB_RenderPieceStart(App.bg.piece));
            log_job(&job);
            if (App.cache_backend->Submit(App.cache_backend, &job) != 0) {
                bg_finished(0);
                return 0;
            }
            App.caching = 2;
            return 0;
        }
        FB_Log(FB_LOG_INFO, "cache", "Making %s of %s",
               App.cache.tasks[t].kind == FB_CACHE_THUMBS ? "thumbnails" :
               App.cache.tasks[t].kind == FB_CACHE_WAVE ? "the waveform"
                                                        : "a proxy",
               App.cache.tasks[t].source.path);
        log_job(&job);
        if (App.cache_backend->Submit(App.cache_backend, &job) != 0) {
            FB_Log(FB_LOG_WARNING, "cache", "Can't start it: %s",
                   App.cache_backend->Result(App.cache_backend).message);
            if (App.cache.tasks[t].kind == FB_CACHE_THUMBS)
                FB_ThumbsSetNone(&App.thumbs, App.cache.tasks[t].key);
            else if (App.cache.tasks[t].kind == FB_CACHE_WAVE)
                FB_WavesSetNone(&App.waves, App.cache.tasks[t].key);
            FB_CacheFinish(&App.cache, 0);   /* not tried again */
            return 0;
        }
        App.caching = 1;
        return 0;
    }

    /* An edit since this piece was started: if it changed the piece,
     * what's being rendered is already out of date. Looked at once per
     * edit. */
    if (App.caching == 2 && App.bg.gen != App.gen) {
        char k[FB_KEY_LEN];

        App.bg.gen = App.gen;
        if (FB_RenderPieceKey(&App.timeline, App.bg.piece, k) != 0 ||
            strcmp(k, App.bg.key) != 0) {
            FB_Log(FB_LOG_INFO, "render", "Piece %d was changed: started "
                   "again later", App.bg.piece);
            cache_stop();
            return 0;
        }
    }

    p = App.cache_backend->Progress(App.cache_backend);
    if (p.state == FB_RENDER_RUNNING)
        return 0;
    if (App.caching == 2) {
        FBResult r = App.cache_backend->Result(App.cache_backend);
        int ready, total;

        App.caching = 0;
        bg_finished(r.state == FB_RENDER_DONE);
        App_RenderedPieces(&ready, &total);
        App_SetStatus(FB_Str(MSG_STATUS_RENDERED_AHEAD), (long)ready,
                      (long)total);
        return 1;
    }
    {
        FBResult r = App.cache_backend->Result(App.cache_backend);
        int t = App.cache.current;
        char name[FB_PATH_LEN] = "", key[FB_KEY_LEN] = "";
        FBCacheKind kind = FB_CACHE_PROXY;

        if (t >= 0) {
            strcpy(name, App.cache.tasks[t].source.path);
            strcpy(key, App.cache.tasks[t].key);
            kind = App.cache.tasks[t].kind;
        }
        App.caching = 0;
        if (kind == FB_CACHE_THUMBS) {
            /* Quietly: the film strip showing them says enough. */
            if (FB_CacheFinish(&App.cache, r.state == FB_RENDER_DONE)
                == FB_CACHE_READY) {
                FB_Log(FB_LOG_INFO, "cache", "Thumbnails of %s are ready",
                       name);
                FB_ThumbsReset(&App.thumbs, key);
                return 1;
            }
            FB_Log(FB_LOG_WARNING, "cache", "No thumbnails of %s: %s",
                   name, r.message);
            FB_ThumbsSetNone(&App.thumbs, key);
            return 1;
        }
        if (kind == FB_CACHE_WAVE) {
            if (FB_CacheFinish(&App.cache, r.state == FB_RENDER_DONE)
                == FB_CACHE_READY) {
                FB_Log(FB_LOG_INFO, "cache", "Waveform of %s is ready",
                       name);
                FB_WavesReset(&App.waves, key);
                return 1;
            }
            FB_Log(FB_LOG_WARNING, "cache", "No waveform of %s: %s",
                   name, r.message);
            FB_WavesSetNone(&App.waves, key);
            return 1;
        }
        if (FB_CacheFinish(&App.cache, r.state == FB_RENDER_DONE)
            == FB_CACHE_READY) {
            FB_Log(FB_LOG_INFO, "cache", "Proxy of %s is ready", name);
            App_SetStatus(FB_Str(MSG_STATUS_PROXY_READY), file_part(name));
            return 1;
        }
        FB_Log(FB_LOG_WARNING, "cache", "No proxy of %s: %s", name,
               r.message);
    }
    return 0;
}

/* ---- Thumbnails ------------------------------------------------------------ */

/* The store is used from Intuition's input task too (the film strip is
 * drawn there while it's dragged), so it's guarded by a semaphore, and a
 * miss wakes the main loop with a signal of its own. */
#ifdef __amigaos4__
static struct SignalSemaphore ThumbSem;
static struct Task *ThumbTask;
static int8 ThumbSig = -1;

static void thumb_lock(void *d)
{
    (void)d;
    IExec->ObtainSemaphore(&ThumbSem);
}

static void thumb_unlock(void *d)
{
    (void)d;
    IExec->ReleaseSemaphore(&ThumbSem);
}

static void thumb_wake(void *d)
{
    (void)d;
    if (ThumbSig >= 0)
        IExec->Signal(ThumbTask, 1UL << ThumbSig);
}
#endif

static void thumbs_start(void)
{
#ifdef __amigaos4__
    IExec->InitSemaphore(&ThumbSem);
    ThumbTask = IExec->FindTask(NULL);
    ThumbSig = IExec->AllocSignal(-1);
    App.thumbs.lock = thumb_lock;
    App.thumbs.unlock = thumb_unlock;
    App.thumbs.wake = thumb_wake;
    App.waves.lock = thumb_lock;
    App.waves.unlock = thumb_unlock;
    App.waves.wake = thumb_wake;
#endif
}

static void thumbs_end(void)
{
    FB_ThumbsFree(&App.thumbs);
    FB_WavesFree(&App.waves);
#ifdef __amigaos4__
    if (ThumbSig >= 0)
        IExec->FreeSignal(ThumbSig);
    ThumbSig = -1;
#endif
}

unsigned long App_ThumbsSigMask(void)
{
#ifdef __amigaos4__
    return ThumbSig >= 0 ? 1UL << ThumbSig : 0;
#else
    return 0;
#endif
}

int App_ThumbsEnabled(void)
{
    return strcmp(Prefs_GetString("ShowPictures", "1"), "0") != 0;
}

void App_SetThumbnails(int on)
{
    Prefs_SetString("ShowPictures", on ? "1" : "0");
    Prefs_Save();
    if (on) {
        cache_request_all();
    } else {
        /* Nothing more is decoded for them; turning it on asks anew. */
        cache_stop();
        FB_CacheDrop(&App.cache, FB_CACHE_THUMBS);
        FB_CacheDrop(&App.cache, FB_CACHE_WAVE);
    }
}

int App_ThumbSize(int w, int h)
{
    if (App.thumbs.pixels && App.thumbs.w == w && App.thumbs.h == h)
        return 0;
    FB_ThumbsFree(&App.thumbs);
    if (FB_ThumbsInit(&App.thumbs, w, h, 0) != 0) {
        FB_Log(FB_LOG_WARNING, "cache", "No memory for thumbnails");
        return -1;
    }
    if (App.cache.thumb_w != w || App.cache.thumb_h != h) {
        /* Strips of another size: the queued ones are for the old one. */
        cache_stop();
        FB_CacheDrop(&App.cache, FB_CACHE_THUMBS);
        FB_CacheSetThumbSize(&App.cache, w, h);
        cache_request_all();
    }
    FB_Log(FB_LOG_INFO, "cache", "Thumbnails %d x %d", w, h);
    return 0;
}

int App_LoadThumbs(void)
{
    return FB_ThumbsLoad(&App.thumbs, &App.cache);
}

int App_LoadWaves(void)
{
    return FB_WavesLoad(&App.waves, &App.cache);
}

/* ---- Preview from the playhead -------------------------------------------- */

#define PREVIEW_WINDOW   10000   /* ms per window */
#define PREVIEW_AHEAD    2       /* windows rendered ahead of playback */
#define PREVIEW_BEHIND   3       /* windows kept behind it */
#define PREVIEW_PLAYLIST "T:formbrod-preview.m3u"

/* A clock in ms; only differences are used. */
static long now_ms(void)
{
#ifdef __amigaos4__
    struct DateStamp ds;

    IDOS->DateStamp(&ds);
    return ((long)ds.ds_Days * 1440 + ds.ds_Minute) * 60000L +
           ds.ds_Tick * (1000 / TICKS_PER_SECOND);
#else
    return (long)(clock() / (CLOCKS_PER_SEC / 1000));
#endif
}

static void preview_path(int i, char *out, size_t len)
{
    snprintf(out, len, "T:formbrod-preview-%03d.mp4", i);
}

const char *App_PreviewPlaylist(void)
{
    return PREVIEW_PLAYLIST;
}

int App_Previewing(void)
{
    return App.preview.on;
}

/* Where a player started on the playlist when window 0 was ready would
 * be now, as a window number. */
static int preview_play_window(void)
{
    if (App.preview.clock0 < 0)
        return 0;
    return (int)((now_ms() - App.preview.clock0) / PREVIEW_WINDOW);
}

int App_PreviewWaiting(void)
{
    return App.preview.on && !App.preview.rendering;
}

static void preview_delete(int from, int to)
{
    char path[64];
    int i;

    for (i = from; i < to; i++) {
        preview_path(i, path, sizeof(path));
        remove(path);
    }
}

static int preview_write_playlist(void)
{
    char path[64];
    FILE *f = fopen(PREVIEW_PLAYLIST, "w");
    int i;

    if (!f)
        return -1;
    for (i = 0; i < App.preview.count; i++) {
        preview_path(i, path, sizeof(path));
        fprintf(f, "%s\n", path);
    }
    fclose(f);
    return 0;
}

static int preview_render(int i)
{
    static FBTimeline win;   /* large; keep it off the stack */
    static FBJob job;
    fb_ms_t from = App.preview.from + (fb_ms_t)i * PREVIEW_WINDOW;
    fb_ms_t len = App.preview.end - from;
    FBOutput out;
    FBJobError err;

    if (len > PREVIEW_WINDOW)
        len = PREVIEW_WINDOW;
    FB_TimelineWindow(&win, with_proxies(), from, len);
    memset(&out, 0, sizeof(out));
    preview_path(i, out.path, sizeof(out.path));
    strcpy(out.vcodec, "mpeg4");
    strcpy(out.acodec, "aac");
    out.quality = FB_QUALITY_PREVIEW;
    err = FB_JobBuild(&job, &win, &out);
    if (err != FB_JOB_OK) {
        FB_Log(FB_LOG_ERROR, "preview", "Window %d (%lld ms): %s", i,
               (long long)from, FB_JobErrorString(err));
        App_SetStatus("%s", App_JobErrorString(err));
        return -1;
    }
    FB_Log(FB_LOG_INFO, "preview", "Window %d: %lld ms from %lld ms", i,
           (long long)len, (long long)from);
    /* Gaps between clips are filled with black and silence, but one at
     * the end of the window isn't (the job ends with its last clip), so
     * a player would get there early. Only typed Starts make such gaps. */
    if (FB_TimelineDuration(&win) < len)
        FB_Log(FB_LOG_WARNING, "preview",
               "Window %d ends in a gap: %lld ms instead of %lld", i,
               (long long)FB_TimelineDuration(&win), (long long)len);
    log_job(&job);
    if (App.backend->Submit(App.backend, &job) != 0) {
        App_SetStatus("%s", App.backend->Result(App.backend).message);
        return -1;
    }
    App.rendering = 1;
    App.preview.rendering = 1;
    App.preview.next = i + 1;
    return 0;
}

static void preview_start_player(void)
{
    const char *player = Prefs_GetString("PreviewPlayer", "");
    char cmd[FB_PATH_LEN + 64];

    if (!player[0])
        return;
    snprintf(cmd, sizeof(cmd), "%s \"%s\"", player, PREVIEW_PLAYLIST);
    FB_Log(FB_LOG_INFO, "preview", "Starting the player: %s", cmd);
#ifdef __amigaos4__
    {
        BPTR in = IDOS->Open("NIL:", MODE_OLDFILE);
        BPTR out = IDOS->Open("NIL:", MODE_NEWFILE);

        /* Asynchronous: the handles are closed when the command ends. */
        if (IDOS->SystemTags(cmd, SYS_Asynch, TRUE, SYS_Input, in,
                             SYS_Output, out, TAG_END) != 0) {
            FB_Log(FB_LOG_WARNING, "preview", "Can't start the player");
            if (in)
                IDOS->Close(in);
            if (out)
                IDOS->Close(out);
        }
    }
#endif
}

int App_PreviewStart(fb_ms_t from)
{
    char at[32];

    App_PreviewStop();
    App_PlayStop();
    if (App.rendering) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_ALREADY_RENDERING));
        return -1;
    }
    App.preview.end = FB_TimelineDuration(&App.timeline);
    if (App.preview.end <= 0)
        return -1;
    if (from < 0 || from >= App.preview.end)
        from = 0;                    /* at the end: from the start */
    App.preview.from = from;
    App.preview.count = (int)((App.preview.end - from + PREVIEW_WINDOW - 1) /
                              PREVIEW_WINDOW);
    App.preview.next = 0;
    App.preview.ready = 0;
    App.preview.gone = 0;
    App.preview.clock0 = -1;
    App.preview.started = now_ms();
    preview_delete(0, 1000);         /* windows left from earlier runs */
    if (preview_write_playlist() != 0)
        FB_Log(FB_LOG_WARNING, "preview", "Can't write %s", PREVIEW_PLAYLIST);
    if (preview_render(0) != 0)
        return -1;
    App.preview.on = 1;
    FB_FormatTimeMs(at, sizeof(at), from, Locale_DecimalPoint());
    App_SetStatus(FB_Str(MSG_STATUS_PREVIEW_FROM), at);
    return 0;
}

void App_PreviewStop(void)
{
    if (!App.preview.on)
        return;
    App.preview.on = 0;
    if (App.preview.rendering) {
        App.backend->Cancel(App.backend);   /* closes and deletes the file */
        App.rendering = 0;
        App.preview.rendering = 0;
    }
    App_SetStatus("%s", FB_Str(MSG_STATUS_PREVIEW_STOPPED));
}

/* A window has been rendered (or failed). Returns 1 if the preview goes
 * on. */
static int preview_window_done(const FBResult *r)
{
    char upto[32], total[32], speed[16];
    long took;
    int i = App.preview.next - 1;

    App.preview.rendering = 0;
    if (!App.preview.on)
        return 0;
    if (r->state != FB_RENDER_DONE) {
        App.preview.on = 0;
        if (r->state == FB_RENDER_CANCELLED)
            App_SetStatus("%s", FB_Str(MSG_STATUS_PREVIEW_STOPPED));
        else
            App_SetStatus(FB_Str(MSG_STATUS_FAILED), r->message);
        return 0;
    }
    App.preview.ready = i + 1;
    if (i == 0) {
        App.preview.clock0 = now_ms();
        preview_start_player();
    }

    /* How fast it renders compared with playing: the edit rendered so
     * far against the time it took. */
    took = now_ms() - App.preview.started;
    {
        fb_ms_t done = (fb_ms_t)App.preview.ready * PREVIEW_WINDOW;
        long tenths = took > 0 ? (long)(done * 10 / took) : 0;

        snprintf(speed, sizeof(speed), "%ld%c%ld", tenths / 10,
                 Locale_DecimalPoint(), tenths % 10);
        FB_Log(FB_LOG_INFO, "preview",
               "Window %d ready after %ld ms: %s times real time", i, took,
               speed);
    }

    if (App.preview.ready >= App.preview.count) {
        App.preview.on = 0;
        App_SetStatus(FB_Str(MSG_STATUS_PREVIEW_END), PREVIEW_PLAYLIST);
        return 0;
    }
    {
        fb_ms_t at = App.preview.from + (fb_ms_t)App.preview.ready *
                     PREVIEW_WINDOW;

        FB_FormatTimeMs(upto, sizeof(upto), at, Locale_DecimalPoint());
        FB_FormatTimeMs(total, sizeof(total), App.preview.end,
                        Locale_DecimalPoint());
        App_SetStatus(FB_Str(MSG_STATUS_PREVIEW_AHEAD), upto, total, speed);
    }
    return 1;
}

void App_PollPreview(void)
{
    int play;

    if (!App.preview.on || App.preview.rendering || App.rendering)
        return;
    play = preview_play_window();
    /* Windows a player has finished with go, so T: doesn't fill up. */
    if (play - PREVIEW_BEHIND > App.preview.gone) {
        preview_delete(App.preview.gone, play - PREVIEW_BEHIND);
        App.preview.gone = play - PREVIEW_BEHIND;
    }
    /* Stay a little ahead of playback; render on at once if behind. */
    if (App.preview.next - play > PREVIEW_AHEAD)
        return;
    if (preview_render(App.preview.next) != 0)
        App.preview.on = 0;
}

/* ---- Playing in the preview window ------------------------------------- */

int App_Playing(void)
{
    return App.play.on;
}

/* Sound for AHI. Where there was none in time (and the edit isn't
 * over), it's counted, for the log: on a machine that can't keep up the
 * sound runs dry, and this says where and how much. */
static long take_audio(void *ud, short *out, long frames)
{
    long real = FB_PlayerTakeAudio(ud, out, frames);

    if (real < frames && App_PlayPosition() < App.play.end - 200) {
        if (App.play.first_dry < 0)
            App.play.first_dry = App_PlayPosition();
        App.play.dry_ms += (frames - real) * 1000 / FB_PLAY_RATE;
    }
    return real;
}

/* Starts step i of the play plan: a rendered piece from its file, or a
 * stretch rendered live. Its frames go to the player at the step's
 * place. 0, or -1 if it can't be started. */
static int play_step(int i)
{
    static FBTimeline win;   /* large; keep them off the stack */
    static FBJob job;
    const FBPlayStep *st = &PlaySteps[i];
    FBFrameSink sink;
    char path[FB_PATH_LEN];
    int rc;

    FB_ENTER("play");
    FB_TRACE("play", "step %d of %d: piece %d, %lld ms from %lld ms "
             "(offset %lld)", i, App.play.steps, st->piece,
             (long long)st->len, (long long)st->from,
             (long long)st->offset);

    if (st->piece >= 0 &&
        FB_CacheFind(&App.cache, BgKeys[st->piece], FB_CACHE_RENDER, path,
                     sizeof(path))) {
        FB_RenderPlayJob(&job, path, st->offset, st->len, App.play.job_audio);
        FB_Log(FB_LOG_INFO, "play", "%lld ms from %lld ms: rendered piece %d",
               (long long)st->len, (long long)st->from, st->piece);
    } else {
        FBOutput out;
        FBJobError err;

        FB_TimelineWindow(&win, with_proxies(), st->from, st->len);
        memset(&out, 0, sizeof(out));
        out.quality = FB_QUALITY_PREVIEW;
        err = FB_JobBuild(&job, &win, &out);
        if (err != FB_JOB_OK) {
            FB_Log(FB_LOG_ERROR, "play", "Can't build the job: %s",
                   FB_JobErrorString(err));
            FB_LEAVE("play", "-1 (no job)");
            return -1;
        }
        FB_Log(FB_LOG_INFO, "play", "%lld ms from %lld ms: live",
               (long long)st->len, (long long)st->from);
        log_job(&job);
    }
    App.play.step = i;
    App.play.feed.pl = &App.play.pl;
    App.play.feed.offset = st->from - App.play.from;
    sink = FB_PlayerFeedSink(&App.play.feed);
    FB_TRACE("play", "backend Play");
    rc = App.backend->Play(App.backend, &job, &sink);
    FB_LEAVE("play", "%d%s%s", rc, rc ? ", " : "",
             rc ? App.backend->Result(App.backend).message : "");
    return rc;
}

static int play_start(fb_ms_t from);

void App_PlaySize(int *width, int *height)
{
    int w, h;

    *width = *height = 0;
    if (!FB_TimelineFrameSize(&App.timeline, &w, &h) || w <= 0 || h <= 0)
        return;
    *width = FB_PREVIEW_WIDTH;
    *height = (int)(((long long)FB_PREVIEW_WIDTH * h + w / 2) / w) & ~1;
}

int App_PlayStart(fb_ms_t from)
{
    int rc;

    FB_ENTER("play");
    FB_TRACE("play", "from %lld ms; rendering %d, previewing %d, caching %d",
             (long long)from, App.rendering, App_Previewing(), App.caching);
    rc = play_start(from);
    FB_LEAVE("play", "%d", rc);
    return rc;
}

static int play_start(fb_ms_t from)
{
    static FBTimeline win;   /* large; keep them off the stack */
    static FBJob job;
    static unsigned char rendered[MAX_PIECES];
    FBOutput out;
    FBJobError err;
    char at[32];
    int i, ready = 0;

    App_PlayStop();
    App_PreviewStop();
    if (App.rendering) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_ALREADY_RENDERING));
        return -1;
    }
    if (!App.backend->Play)
        return -1;
    App.play.end = FB_TimelineDuration(&App.timeline);
    if (App.play.end <= 0)
        return -1;
    if (from < 0 || from >= App.play.end)
        from = 0;                       /* at the end: from the start */
    App.play.from = from;

    FB_TimelineWindow(&win, with_proxies(), from, App.play.end - from);
    memset(&out, 0, sizeof(out));
    out.quality = FB_QUALITY_PREVIEW;
    err = FB_JobBuild(&job, &win, &out);
    if (err != FB_JOB_OK) {
        FB_Log(FB_LOG_ERROR, "play", "Can't build the job: %s",
               FB_JobErrorString(err));
        App_SetStatus("%s", App_JobErrorString(err));
        return -1;
    }
    App.play.job_audio = job.audio_out[0] != 0;

    /* The plan: rendered pieces from their files, the rest live. */
    bg_keys();
    for (i = 0; i < App.bg.pieces; i++) {
        rendered[i] = BgState[i] == 1;
        ready += rendered[i];
    }
    App.play.steps = FB_RenderPlan(&App.timeline, from,
                                   App.bg.pieces ? rendered : NULL,
                                   PlaySteps, MAX_PIECES);
    FB_Log(FB_LOG_INFO, "play", "%d of %d pieces rendered; %d steps", ready,
           App.bg.pieces, App.play.steps);
    if (App.play.steps <= 0)
        return -1;

    /* Sound through AHI, on the unit in the settings (AHIUnit). If it
     * can't be had, it plays silently and says so. */
    FB_TRACE("play", "opening the sound (job has sound: %d)",
             job.audio_out[0] != 0);
    App.play.sound = job.audio_out[0] &&
                     Audio_Open(atoi(Prefs_GetString("AHIUnit", "0"))) == 0;
    FB_TRACE("play", "sound %s", App.play.sound ? "on" : "off");
    if (FB_PlayerInit(&App.play.pl, App.play.sound) != 0) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_NO_MEMORY));
        return -1;
    }
    if (play_step(0) != 0) {
        App_SetStatus(FB_Str(MSG_STATUS_FAILED),
                      App.backend->Result(App.backend).message);
        FB_PlayerFree(&App.play.pl);
        return -1;
    }
    App.play.on = 1;
    App.play.ended = 0;
    App.play.shown_at = 0;
    App.play.dry_ms = 0;
    App.play.first_dry = -1;
    FB_FormatTimeMs(at, sizeof(at), from, Locale_DecimalPoint());
    if (job.audio_out[0] && !App.play.sound)
        App_SetStatus(FB_Str(MSG_STATUS_PLAYING_SILENT), at);
    else
        App_SetStatus(FB_Str(MSG_STATUS_PLAYING), at);
    return 0;
}

/* Stops; `why` is the status line (NULL = say where it stopped). */
static void play_stop(const char *why)
{
    char at[32];

    if (!App.play.on)
        return;
    FB_TRACE("play", "stopping: step %d of %d, backend %s; queue %d "
             "pictures, %ld ms sound, clock %s, ended %d",
             App.play.step, App.play.steps,
             App.play.ended ? "done" : "running", App.play.pl.count,
             App.play.pl.audio_count * 1000 / FB_PLAY_RATE,
             FB_PlayerStarted(&App.play.pl) ? "running" : "not started",
             App.play.pl.ended);
    FB_FormatTimeMs(at, sizeof(at), App_PlayPosition(), Locale_DecimalPoint());
    App.bg.near = App_PlayPosition();       /* render from here on next */
    App.play.on = 0;
    if (!App.play.ended)
        App.backend->Cancel(App.backend);
    Audio_Stop();
    FB_Log(FB_LOG_INFO, "play", "Stopped at %s: %ld pictures shown, %ld "
           "skipped", at, App.play.pl.num_shown, App.play.pl.num_dropped);
    if (App.play.dry_ms > 0)
        FB_Log(FB_LOG_WARNING, "play", "The sound wasn't ready in time for "
               "%ld ms in all, first at %lld ms in the edit (rendering "
               "slower than playing)", App.play.dry_ms,
               (long long)App.play.first_dry);
    FB_PlayerFree(&App.play.pl);
    if (why)
        App_SetStatus("%s", why);
    else
        App_SetStatus(FB_Str(MSG_STATUS_PLAY_STOPPED), at);
}

void App_PlayStop(void)
{
    play_stop(NULL);
}

fb_ms_t App_PlayPosition(void)
{
    fb_ms_t t;

    if (!App.play.on)
        return 0;
    t = App.play.from + FB_PlayerTime(&App.play.pl, FB_BackendClockUs());
    return t < App.play.end ? t : App.play.end;
}

void App_PlayTimeText(char *buf, size_t len)
{
    char at[32], total[32];

    FB_FormatTimeMs(at, sizeof(at), App_PlayPosition(), Locale_DecimalPoint());
    FB_FormatTimeMs(total, sizeof(total), App.play.end, Locale_DecimalPoint());
    snprintf(buf, len, FB_Str(MSG_PLAY_TIME), at, total);
}

/* While tracing: how playing is going, once a second, so a stall shows
 * where it is (nothing queued = the backend gives nothing; queue full but
 * the clock not started = waiting for sound; and so on). */
static fb_ms_t PlayBackendAt;      /* the backend's last position */

static void trace_play(const FBPlayer *pl, long long now)
{
    static long long last;
    static long polls;

    polls++;
    if (!FB_LogWants(FB_LOG_TRACE) || now - last < 1000000)
        return;
    last = now;
    FB_TRACE("play", "%ld polls; step %d, backend at %lld ms%s; queue %d "
             "pictures (last %lld ms), %ld ms sound; clock %s at %lld ms; "
             "%ld shown, %ld skipped", polls, App.play.step,
             (long long)PlayBackendAt,
             App.play.ended ? " (ended)" : "", pl->count,
             (long long)pl->last_pts,
             pl->audio_count * 1000 / FB_PLAY_RATE,
             FB_PlayerStarted(pl) ? "running" : "not started",
             (long long)FB_PlayerTime(pl, now), pl->num_shown,
             pl->num_dropped);
    polls = 0;
}

const FBPlayFrame *App_PollPlay(long *sleep_ms)
{
    FBPlayer *pl = &App.play.pl;
    const FBPlayFrame *f = NULL;
    long long now;
    long due;

    *sleep_ms = 0;
    if (!App.play.on)
        return NULL;

    /* Some work, unless the queues are full (then it returns at once). */
    if (!App.play.ended) {
        FBProgress p = App.backend->Progress(App.backend);

        PlayBackendAt = p.position;
        if (p.state != FB_RENDER_RUNNING) {
            FBResult r = App.backend->Result(App.backend);

            FB_TRACE("play", "step %d ended: state %d, %s", App.play.step,
                     (int)r.state, r.message);
            /* That step is done: the next, if there is one. */
            if (r.state == FB_RENDER_DONE &&
                App.play.step + 1 < App.play.steps) {
                if (play_step(App.play.step + 1) == 0)
                    goto stepped;
                r = App.backend->Result(App.backend);
                r.state = FB_RENDER_FAILED;
            }
            App.play.ended = 1;
            FB_PlayerSetEnded(pl);
            if (r.state == FB_RENDER_FAILED) {
                char msg[300];

                snprintf(msg, sizeof(msg), FB_Str(MSG_STATUS_FAILED),
                         r.message);
                play_stop(msg);
                return NULL;
            }
        }
    }
stepped:

    now = FB_BackendClockUs();
    trace_play(pl, now);
    if (!FB_PlayerStarted(pl) && FB_PlayerReady(pl)) {
        FB_PlayerStart(pl, now);
        FB_Log(FB_LOG_INFO, "play", "Playing: %d pictures buffered", pl->count);
    }
    if (FB_PlayerStarted(pl)) {
        f = FB_PlayerFrame(pl, now);
        if (App.play.sound)
            Audio_Feed(take_audio, pl);
        if (f && pl->num_shown == 1)
            FB_TRACE("play", "first picture due: %dx%d at %lld ms",
                     f->width, f->height, (long long)f->pts);
    }
    if (!f && FB_PlayerFinished(pl, now)) {
        play_stop(FB_Str(MSG_STATUS_PLAY_END));
        return NULL;
    }

    /* The backend wants a turn while there's room in the picture queue;
     * else nothing is due until the next picture (but AHI wants sound
     * every 100 ms, so not long). */
    if (App.play.ended || pl->count >= FB_PLAY_FRAMES) {
        due = FB_PlayerNextDue(pl, now);
        *sleep_ms = due < 0 || due > 40 ? 40 : due;
    }
    return f;
}

/* ---- Single frames -------------------------------------------------------- */

/* A frame of the edit, made on a backend of its own, so it neither waits
 * for nor stops an export, a preview or the proxies. The job is a short
 * window from the frame on (FB_TimelineWindow), played into a sink that
 * keeps the first picture. Preview frames come from a piece rendered in
 * the background where there is one, else from proxies, at the preview's
 * size; full frames from the original files at the edit's size. Sound is
 * left out where it can be. */

#define STILL_WINDOW_MS 200     /* enough for a frame at any rate */

static int still_video(void *ud, const unsigned char *argb, int width,
                       int height, int stride, fb_ms_t pts)
{
    FBPlayFrame *f = &App.still.frame;
    size_t need = (size_t)width * 4 * height;
    int y;

    (void)ud;
    (void)pts;
    if (App.still.have)
        return 1;                       /* one is all we want */
    if (f->size < need) {
        unsigned char *p = realloc(f->pixels, need);

        if (!p)
            return 1;
        f->pixels = p;
        f->size = need;
    }
    for (y = 0; y < height; y++)
        memcpy(f->pixels + (size_t)y * width * 4,
               argb + (size_t)y * stride, (size_t)width * 4);
    f->width = width;
    f->height = height;
    f->stride = width * 4;
    f->pts = App.still.at;
    App.still.have = 1;
    return 0;
}

static int still_audio(void *ud, const short *samples, int frames,
                       fb_ms_t pts)
{
    (void)ud;
    (void)samples;
    (void)frames;
    (void)pts;
    return 0;                           /* not wanted: thrown away */
}

/* Where a frame at `at` can come from: inside the edit, a little before
 * its end so the window has a frame in it. */
static fb_ms_t still_clamp(fb_ms_t at)
{
    fb_ms_t end = FB_TimelineDuration(&App.timeline);

    if (at > end - 40)
        at = end - 40;
    return at < 0 ? 0 : at;
}

/* Starts making the frame at `at`. 0, or -1 (status line set). */
static int still_begin(fb_ms_t at, int full)
{
    static FBTimeline win;      /* large; keep them off the stack */
    static FBJob job;
    FBFrameSink sink;
    FBOutput out;
    FBJobError err = FB_JOB_EMPTY;
    char path[FB_PATH_LEN];
    int piece, i;

    if (!App.still.backend)
        App.still.backend = FB_CreateLocalBackend();
    if (!App.still.backend)
        return -1;
    if (App.timeline.num_clips == 0) {
        App_SetStatus("%s", App_JobErrorString(FB_JOB_EMPTY));
        return -1;
    }
    at = still_clamp(at);
    App.still.at = at;
    App.still.full = full;
    App.still.gen = App.gen;
    App.still.have = 0;

    /* A preview frame from a piece rendered in the background is the
     * cheapest: one MJPEG frame, already composed. */
    piece = full ? -1 : FB_RenderPieceAt(at);
    if (piece >= 0) {
        bg_keys();
        if (piece < App.bg.pieces && BgKeys[piece][0]
            && FB_CacheFind(&App.cache, BgKeys[piece], FB_CACHE_RENDER,
                            path, sizeof(path))) {
            FB_RenderPlayJob(&job, path, at - FB_RenderPieceStart(piece),
                             STILL_WINDOW_MS, 0);
            err = FB_JOB_OK;
            FB_Log(FB_LOG_INFO, "still", "Frame at %lld ms from rendered "
                   "piece %d", (long long)at, piece);
        }
    }
    if (err != FB_JOB_OK) {
        FB_TimelineWindow(&win, full ? &App.timeline : with_proxies(), at,
                          STILL_WINDOW_MS);
        memset(&out, 0, sizeof(out));
        out.quality = full ? FB_QUALITY_FINAL : FB_QUALITY_PREVIEW;
        /* Without sound, unless that leaves nothing (a stretch with only
         * sound in it is black, but needs its sound to be a job). */
        for (i = 0; i < win.num_tracks; i++)
            win.tracks[i].muted = 1;
        err = FB_JobBuild(&job, &win, &out);
        if (err == FB_JOB_EMPTY) {
            FB_TimelineWindow(&win, full ? &App.timeline : with_proxies(),
                              at, STILL_WINDOW_MS);
            err = FB_JobBuild(&job, &win, &out);
        }
        if (err != FB_JOB_OK) {
            FB_Log(FB_LOG_ERROR, "still", "Frame at %lld ms: %s",
                   (long long)at, FB_JobErrorString(err));
            App_SetStatus("%s", App_JobErrorString(err));
            return -1;
        }
        FB_Log(FB_LOG_INFO, "still", "%s frame at %lld ms", full ? "Full"
               : "Preview", (long long)at);
        log_job(&job);
    }
    sink.Video = still_video;
    sink.Audio = still_audio;
    sink.ud = NULL;
    if (App.still.backend->Play(App.still.backend, &job, &sink) != 0) {
        App_SetStatus("%s",
                      App.still.backend->Result(App.still.backend).message);
        return -1;
    }
    App.still.busy = 1;
    return 0;
}

int App_StillBusy(void)
{
    return App.still.busy || App.still.want;
}

int App_StillShow(fb_ms_t at)
{
    if (App.still.busy) {
        /* Finish the one under way, then this: while the playhead is
         * dragged, the window keeps up as well as it can. */
        App.still.want = 1;
        App.still.want_at = at;
        return 0;
    }
    App.still.want = 0;
    return still_begin(at, 0);
}

/* A turn of work on the frame under way. 1 when it's there, -1 when it
 * failed, 0 while it's being made. */
static int still_turn(void)
{
    FBProgress p;

    if (!App.still.busy)
        return -1;
    p = App.still.backend->Progress(App.still.backend);
    if (App.still.have || p.state != FB_RENDER_RUNNING) {
        if (p.state == FB_RENDER_RUNNING)
            App.still.backend->Cancel(App.still.backend);
        App.still.busy = 0;
        if (!App.still.have) {
            FBResult r = App.still.backend->Result(App.still.backend);

            FB_Log(FB_LOG_ERROR, "still", "No frame at %lld ms: %s",
                   (long long)App.still.at, r.message);
            return -1;
        }
        return 1;
    }
    return 0;
}

const FBPlayFrame *App_PollStill(void)
{
    int rc;

    if (!App.still.busy) {
        if (App.still.want)
            App_StillShow(App.still.want_at);
        return NULL;
    }
    rc = still_turn();
    if (rc == 0)
        return NULL;
    if (App.still.want) {
        /* The playhead moved on meanwhile: show this one, start that. */
        App.still.want = 0;
        if (App.still.want_at != App.still.at || App.still.gen != App.gen) {
            static FBPlayFrame shown;
            size_t need = (size_t)App.still.frame.stride
                          * App.still.frame.height;

            if (rc < 0) {
                App_StillShow(App.still.want_at);
                return NULL;
            }
            /* Copy it out: the next one is made into App.still.frame. */
            if (shown.size < need) {
                unsigned char *p = realloc(shown.pixels, need);

                if (!p)
                    return NULL;
                shown.pixels = p;
                shown.size = need;
            }
            memcpy(shown.pixels, App.still.frame.pixels, need);
            shown.width = App.still.frame.width;
            shown.height = App.still.frame.height;
            shown.stride = App.still.frame.stride;
            shown.pts = App.still.frame.pts;
            App_StillShow(App.still.want_at);
            return &shown;
        }
    }
    return rc > 0 ? &App.still.frame : NULL;
}

fb_ms_t App_GrabFrame(fb_ms_t at, int full, const char *path)
{
    int want = App.still.want, rc;
    fb_ms_t want_at = App.still.want_at;

    /* One the window asked for gives way, and is made again after. */
    if (App.still.busy) {
        App.still.backend->Cancel(App.still.backend);
        while (App.still.backend->Progress(App.still.backend).state
               == FB_RENDER_RUNNING)
            ;
        App.still.busy = 0;
        want = 1;
        want_at = App.still.at;
    }
    App.still.want = 0;
    if (still_begin(at, full) != 0)
        return -1;
    while ((rc = still_turn()) == 0)
        ;
    at = App.still.at;
    if (rc < 0) {
        App_SetStatus(FB_Str(MSG_STATUS_FRAME_FAILED), path);
    } else if (FB_PictureWrite(path, App.still.frame.pixels,
                               App.still.frame.width, App.still.frame.height,
                               App.still.frame.stride) != 0) {
        FB_Log(FB_LOG_ERROR, "still", "Can't write %s", path);
        App_SetStatus(FB_Str(MSG_STATUS_FRAME_FAILED), path);
        rc = -1;
    } else {
        FB_Log(FB_LOG_INFO, "still", "Wrote %s: %dx%d at %lld ms", path,
               App.still.frame.width, App.still.frame.height,
               (long long)at);
        App_SetStatus(FB_Str(MSG_STATUS_FRAME_SAVED), path);
    }
    if (want) {
        App.still.want = 1;
        App.still.want_at = want_at;
    }
    return rc < 0 ? -1 : at;
}

void App_StillTimeText(char *buf, size_t len)
{
    char at[32], total[32];

    FB_FormatTimeMs(at, sizeof(at), App.still.frame.pts,
                    Locale_DecimalPoint());
    FB_FormatTimeMs(total, sizeof(total), FB_TimelineDuration(&App.timeline),
                    Locale_DecimalPoint());
    snprintf(buf, len, FB_Str(MSG_PLAY_TIME), at, total);
}

static void still_cleanup(void)
{
    if (App.still.backend) {
        if (App.still.busy)
            App.still.backend->Cancel(App.still.backend);
        App.still.backend->Dispose(App.still.backend);
        App.still.backend = NULL;
    }
    free(App.still.frame.pixels);
    memset(&App.still, 0, sizeof(App.still));
}
