#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "app.h"
#include "formbrod/module.h"
#include "formbrod/project.h"
#include "formbrod/log.h"
#include "formbrod/backend.h"
#include "prefs/prefs.h"
#include "version.h"
#include "locale/catalog.h"

FBApp App;

int App_Init(void)
{
    memset(&App, 0, sizeof(App));
    FB_TimelineInit(&App.timeline);
    FB_RegisterBuiltinModules();
    App.backend = FB_CreateLocalBackend();
    if (!App.backend)
        return -1;
    App_SetStatus("%s", FB_Str(MSG_STATUS_WELCOME));
    return 0;
}

void App_Cleanup(void)
{
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
            FB_SourceSetVideo(src, info.width, info.height,
                              info.sar_num, info.sar_den,
                              info.fps_num, info.fps_den);
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

int App_AddFile(const char *path)
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
    if (src >= 0)
        FB_SourceSetVideo(&App.timeline.sources[src], info.width, info.height,
                          info.sar_num, info.sar_den,
                          info.fps_num, info.fps_den);
    if (src < 0 ||
        FB_TimelineAppendClip(&App.timeline, src, 0, info.duration) < 0) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_TOO_MANY));
        return -1;
    }
    App.modified = 1;
    App_SetStatus(FB_Str(MSG_STATUS_ADDED), path);
    return 0;
}

int App_Split(fb_ms_t at)
{
    if (FB_TimelineSplit(&App.timeline, at) < 0) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_NOTHING_TO_SPLIT));
        return -1;
    }
    App.modified = 1;
    return 0;
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
    }
    return FB_Str(MSG_JOB_UNKNOWN);
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

    err = FB_JobBuild(&job, &App.timeline, &out);
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
