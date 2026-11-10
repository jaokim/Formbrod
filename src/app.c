#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "app.h"
#include "formbrod/module.h"
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
}

int App_AddFile(const char *path)
{
    fb_ms_t dur = 0;
    int has_v = 0, has_a = 0;
    int src;

    if (FB_ProbeMedia(path, &dur, &has_v, &has_a) != 0 || !has_v || dur <= 0) {
        App_SetStatus(FB_Str(MSG_STATUS_CANT_READ), path);
        return -1;
    }
    src = FB_TimelineAddSource(&App.timeline, path, dur, has_v, has_a);
    if (src < 0 || FB_TimelineAppendClip(&App.timeline, src, 0, dur) < 0) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_TOO_MANY));
        return -1;
    }
    App_SetStatus(FB_Str(MSG_STATUS_ADDED), path);
    return 0;
}

int App_Split(fb_ms_t at)
{
    if (FB_TimelineSplit(&App.timeline, at) < 0) {
        App_SetStatus("%s", FB_Str(MSG_STATUS_NOTHING_TO_SPLIT));
        return -1;
    }
    return 0;
}

int App_RemoveClip(int clip)
{
    return FB_TimelineRemoveClip(&App.timeline, clip);
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
    return FB_EffectSetParam(&c->effects[effect], name, value);
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
        App_SetStatus("%s", App_JobErrorString(err));
        return -1;
    }
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
