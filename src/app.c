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

#ifdef __amigaos4__
#include <proto/exec.h>
#include <proto/dos.h>
#endif

static void cache_start(void);
static void cache_request(FBSource *src);
static void cache_stop(void);

FBApp App;

int App_Init(void)
{
    memset(&App, 0, sizeof(App));
    FB_TimelineInit(&App.timeline);
    FB_RegisterBuiltinModules();
    App.backend = FB_CreateLocalBackend();
    if (!App.backend)
        return -1;
    /* Proxies are made locally for now. A RemoteBackend here would make
     * them on a render server, into a cache drawer both can see. */
    App.cache_backend = FB_CreateLocalBackend();
    cache_start();
    App_SetStatus("%s", FB_Str(MSG_STATUS_WELCOME));
    return 0;
}

void App_Cleanup(void)
{
    if (App.cache_backend) {
        cache_stop();   /* no half-made proxies left behind */
        App.cache_backend->Dispose(App.cache_backend);
        App.cache_backend = NULL;
    }
    if (App.backend) {
        App.backend->Dispose(App.backend);
        App.backend = NULL;
    }
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
    App.modified = 1;
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
    App.modified = 1;
    App_SetStatus(FB_Str(MSG_STATUS_ADDED), path);
    return 0;
}

int App_Split(int track, fb_ms_t at)
{
    if (FB_TimelineSplit(&App.timeline, track, at) < 0) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_NOTHING_TO_SPLIT));
        return -1;
    }
    App.modified = 1;
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
    App.modified = 1;
    return 0;
}

int App_SetTrackMuted(int track, int muted)
{
    if (!valid_track(track))
        return -1;
    App.timeline.tracks[track].muted = muted != 0;
    App.modified = 1;
    return 0;
}

int App_SetTrackHidden(int track, int hidden)
{
    if (!valid_track(track))
        return -1;
    App.timeline.tracks[track].hidden = hidden != 0;
    App.modified = 1;
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
    App.modified = 1;
    FB_FormatTimecode(when, sizeof(when), FB_US(start), FB_US(100), 0,
                      Locale_DecimalPoint());
    App_SetStatus(FB_Str(MSG_STATUS_MOVED),
                  file_part(tl->sources[tl->clips[clip].source].path), when);
    return 0;
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
    App.modified = 1;
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
     * still picture can be any length). */
    out = c->in + (at - c->start);
    dur = App.timeline.sources[c->source].duration;
    if (dur > 0 && out > dur)
        out = dur;
    return App_TrimClip(clip, c->in, out);
}

int App_RemoveClip(int clip)
{
    if (FB_TimelineRemoveClip(&App.timeline, clip) != 0)
        return -1;
    App.modified = 1;
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
    App.modified = 1;
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
    App.modified = 1;
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
    App.modified = 1;
    return 0;
}

int App_SetDefaultFit(int fit)
{
    if (fit <= FB_FIT_DEFAULT || fit >= FB_FIT_COUNT)
        return -1;
    App.timeline.format.fit = (FBFit)fit;
    App.modified = 1;
    return 0;
}

int App_SetFrameSize(int width, int height)
{
    if (width == 0 && height == 0) {
        App.timeline.format.width = App.timeline.format.height = 0;
        App.modified = 1;
        return 0;
    }
    if (width < 2 || height < 2 || width > 16384 || height > 16384)
        return -1;
    /* stored even, as the encoders need */
    App.timeline.format.width  = width & ~1;
    App.timeline.format.height = height & ~1;
    App.modified = 1;
    return 0;
}

int App_SetFrameRate(int num, int den)
{
    if (num == 0 && den == 0) {
        App.timeline.format.fps_num = App.timeline.format.fps_den = 0;
        App.modified = 1;
        return 0;
    }
    if (num <= 0 || den <= 0 || num / den > 1000)
        return -1;
    App.timeline.format.fps_num = num;
    App.timeline.format.fps_den = den;
    App.modified = 1;
    return 0;
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

    if (App.rendering) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_ALREADY_RENDERING));
        return -1;
    }

    memset(&out, 0, sizeof(out));
    strncpy(out.path, path, sizeof(out.path) - 1);
    strcpy(out.vcodec, "mpeg4");
    strcpy(out.acodec, "aac");
    out.quality = preview ? FB_QUALITY_PREVIEW : FB_QUALITY_FINAL;

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

        App.rendering = 0;
        if (r.state == FB_RENDER_DONE)
            App_SetStatus(FB_Str(MSG_STATUS_DONE), r.message);
        else if (r.state == FB_RENDER_CANCELLED)
            App_SetStatus("%s", FB_Str(MSG_STATUS_CANCELLED));
        else
            App_SetStatus(FB_Str(MSG_STATUS_FAILED), r.message);
    }
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
    FB_Log(FB_LOG_INFO, "cache", "Cache in %s, proxies %d wide",
           dir, App.cache.proxy_width);
}

/* Reads the file's fingerprint if needed and asks for its proxy. */
static void cache_request(FBSource *src)
{
    FBCacheState st;

    if (!App.cache_backend || !App_ProxiesEnabled())
        return;
    if (!src->key[0] && src->has_video && !src->still &&
        FB_CacheKey(src->path, src->key) != 0) {
        FB_Log(FB_LOG_WARNING, "cache", "%s: can't read its fingerprint",
               src->path);
        return;
    }
    st = FB_CacheRequest(&App.cache, src);
    if (st == FB_CACHE_QUEUED)
        FB_Log(FB_LOG_INFO, "cache", "%s (%s): proxy queued",
               src->path, src->key);
    else if (st == FB_CACHE_READY)
        FB_Log(FB_LOG_INFO, "cache", "%s (%s): proxy ready",
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
    FB_CacheAbort(&App.cache);
    App.caching = 0;
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
        App.cache.num_tasks = 0;
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
           (App.cache_backend && FB_CachePending(&App.cache) > 0);
}

int App_PollCache(void)
{
    static FBJob job;   /* large; keep it off the stack */
    FBProgress p;

    /* An export has the machine to itself; the cache job waits. */
    if (!App.cache_backend || App.rendering)
        return 0;

    if (!App.caching) {
        int t = FB_CacheNextJob(&App.cache, &job);

        if (t < 0)
            return 0;
        FB_Log(FB_LOG_INFO, "cache", "Making a proxy of %s",
               App.cache.tasks[t].source.path);
        log_job(&job);
        if (App.cache_backend->Submit(App.cache_backend, &job) != 0) {
            FB_Log(FB_LOG_WARNING, "cache", "Can't start it: %s",
                   App.cache_backend->Result(App.cache_backend).message);
            FB_CacheFinish(&App.cache, 0);   /* not tried again */
            return 0;
        }
        App.caching = 1;
        return 0;
    }

    p = App.cache_backend->Progress(App.cache_backend);
    if (p.state == FB_RENDER_RUNNING)
        return 0;
    {
        FBResult r = App.cache_backend->Result(App.cache_backend);
        int t = App.cache.current;
        char name[FB_PATH_LEN] = "";

        if (t >= 0)
            strcpy(name, App.cache.tasks[t].source.path);
        App.caching = 0;
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
