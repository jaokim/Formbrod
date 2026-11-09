#include <string.h>
#include "formbrod/timeline.h"

static void copy_str(char *dst, const char *src, size_t len)
{
    strncpy(dst, src, len - 1);
    dst[len - 1] = '\0';
}

void FB_TimelineInit(FBTimeline *tl)
{
    memset(tl, 0, sizeof(*tl));
}

int FB_TimelineAddSource(FBTimeline *tl, const char *path, fb_ms_t duration,
                         int has_video, int has_audio)
{
    FBSource *src;

    if (tl->num_sources >= FB_MAX_SOURCES)
        return -1;

    src = &tl->sources[tl->num_sources];
    memset(src, 0, sizeof(*src));
    copy_str(src->path, path, sizeof(src->path));
    src->duration  = duration;
    src->has_video = has_video;
    src->has_audio = has_audio;
    return tl->num_sources++;
}

int FB_TimelineAppendClip(FBTimeline *tl, int source, fb_ms_t in, fb_ms_t out)
{
    FBClip *clip;

    if (tl->num_clips >= FB_MAX_CLIPS)
        return -1;
    if (source < 0 || source >= tl->num_sources)
        return -1;
    if (in < 0 || out <= in)
        return -1;

    clip = &tl->clips[tl->num_clips];
    memset(clip, 0, sizeof(*clip));
    clip->source = source;
    clip->in     = in;
    clip->out    = out;
    return tl->num_clips++;
}

int FB_TimelineSplit(FBTimeline *tl, fb_ms_t at)
{
    fb_ms_t start = 0;
    int i;

    if (tl->num_clips >= FB_MAX_CLIPS)
        return -1;

    for (i = 0; i < tl->num_clips; i++) {
        FBClip *clip = &tl->clips[i];
        fb_ms_t len = clip->out - clip->in;

        if (at > start && at < start + len) {
            fb_ms_t cut = clip->in + (at - start);

            memmove(&tl->clips[i + 2], &tl->clips[i + 1],
                    (size_t)(tl->num_clips - i - 1) * sizeof(FBClip));
            tl->clips[i + 1] = *clip;      /* right half keeps the effects */
            tl->clips[i + 1].in = cut;
            clip->out = cut;
            tl->num_clips++;
            return i + 1;
        }
        start += len;
    }
    return -1;
}

int FB_TimelineRemoveClip(FBTimeline *tl, int clip)
{
    if (clip < 0 || clip >= tl->num_clips)
        return -1;
    memmove(&tl->clips[clip], &tl->clips[clip + 1],
            (size_t)(tl->num_clips - clip - 1) * sizeof(FBClip));
    tl->num_clips--;
    return 0;
}

FBEffect *FB_ClipAddEffect(FBClip *clip, const char *module)
{
    FBEffect *fx;

    if (clip->num_effects >= FB_MAX_EFFECTS)
        return NULL;
    fx = &clip->effects[clip->num_effects++];
    memset(fx, 0, sizeof(*fx));
    copy_str(fx->module, module, sizeof(fx->module));
    return fx;
}

int FB_EffectSetParam(FBEffect *fx, const char *name, double value)
{
    int i;

    for (i = 0; i < fx->num_params; i++) {
        if (strcmp(fx->params[i].name, name) == 0) {
            fx->params[i].value = value;
            return 0;
        }
    }
    if (fx->num_params >= FB_MAX_PARAMS)
        return -1;
    copy_str(fx->params[fx->num_params].name, name, FB_NAME_LEN);
    fx->params[fx->num_params].value = value;
    fx->num_params++;
    return 0;
}

double FB_EffectGetParam(const FBEffect *fx, const char *name, double def)
{
    int i;

    for (i = 0; i < fx->num_params; i++)
        if (strcmp(fx->params[i].name, name) == 0)
            return fx->params[i].value;
    return def;
}

fb_ms_t FB_TimelineDuration(const FBTimeline *tl)
{
    fb_ms_t total = 0;
    int i;

    for (i = 0; i < tl->num_clips; i++)
        total += tl->clips[i].out - tl->clips[i].in;
    return total;
}
