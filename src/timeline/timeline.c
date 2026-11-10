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
    tl->format.fit = FB_FIT_FIT;
    tl->format.pad_color = 0x000000;
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

/* ---- Output format and fitting --------------------------------------- */

void FB_SourceSetVideo(FBSource *src, int width, int height,
                       int sar_num, int sar_den, int fps_num, int fps_den)
{
    src->width   = width > 0 ? width : 0;
    src->height  = height > 0 ? height : 0;
    src->sar_num = (sar_num > 0 && sar_den > 0) ? sar_num : 0;
    src->sar_den = (sar_num > 0 && sar_den > 0) ? sar_den : 0;
    src->fps_num = (fps_num > 0 && fps_den > 0) ? fps_num : 0;
    src->fps_den = (fps_num > 0 && fps_den > 0) ? fps_den : 0;
}

/* The source's size as shown, i.e. with square pixels. 0 if unknown. */
static int display_size(const FBSource *src, int *w, int *h)
{
    *w = src->width;
    *h = src->height;
    if (*w <= 0 || *h <= 0)
        return 0;
    if (src->sar_num > 0 && src->sar_den > 0 && src->sar_num != src->sar_den)
        *w = (int)(((int64_t)*w * src->sar_num + src->sar_den / 2)
                   / src->sar_den);
    return 1;
}

static int even(int v)
{
    v &= ~1;
    return v < 2 ? 2 : v;
}

static const FBSource *first_source(const FBTimeline *tl)
{
    int s;

    if (tl->num_clips <= 0)
        return NULL;
    s = tl->clips[0].source;
    if (s < 0 || s >= tl->num_sources)
        return NULL;
    return &tl->sources[s];
}

int FB_TimelineFrameSize(const FBTimeline *tl, int *width, int *height)
{
    const FBSource *src;
    int w, h;

    *width = *height = 0;
    if (tl->format.width > 0 && tl->format.height > 0) {
        *width  = even(tl->format.width);
        *height = even(tl->format.height);
        return 1;
    }
    src = first_source(tl);
    if (!src || !display_size(src, &w, &h))
        return 0;
    *width  = even(w);
    *height = even(h);
    return 1;
}

int FB_TimelineFrameRate(const FBTimeline *tl, int *num, int *den)
{
    const FBSource *src;

    *num = *den = 0;
    if (tl->format.fps_num > 0 && tl->format.fps_den > 0) {
        *num = tl->format.fps_num;
        *den = tl->format.fps_den;
        return 1;
    }
    src = first_source(tl);
    if (!src || src->fps_num <= 0 || src->fps_den <= 0)
        return 0;
    *num = src->fps_num;
    *den = src->fps_den;
    return 1;
}

FBFit FB_ClipFit(const FBTimeline *tl, const FBClip *clip)
{
    if (clip->fit > FB_FIT_DEFAULT && clip->fit < FB_FIT_COUNT)
        return clip->fit;
    if (tl->format.fit > FB_FIT_DEFAULT && tl->format.fit < FB_FIT_COUNT)
        return tl->format.fit;
    return FB_FIT_FIT;
}

FBFitResult FB_ClipFitResult(const FBTimeline *tl, const FBClip *clip,
                             int width, int height)
{
    int w, h;
    int64_t a, b, tol;

    if (clip->source < 0 || clip->source >= tl->num_sources)
        return FB_FITS_NONE;
    if (width <= 0 || height <= 0 ||
        !display_size(&tl->sources[clip->source], &w, &h))
        return FB_FITS_NONE;
    if (w == width && h == height)
        return FB_FITS_NONE;

    /* Compare aspects as w/h vs width/height, within 1% (odd sizes and
     * rounding to even shouldn't count as a different shape). */
    a = (int64_t)w * height;
    b = (int64_t)width * h;
    tol = b / 100;
    if (a - b <= tol && b - a <= tol)
        return FB_FITS_SCALE;

    switch (FB_ClipFit(tl, clip)) {
    case FB_FIT_FILL:    return FB_FITS_CROP;
    case FB_FIT_STRETCH: return FB_FITS_STRETCH;
    default:             return a > b ? FB_FITS_LETTERBOX : FB_FITS_PILLARBOX;
    }
}

static const char *const fit_names[FB_FIT_COUNT] = {
    "default", "fit", "fill", "stretch"
};

const char *FB_FitName(FBFit fit)
{
    if (fit < 0 || fit >= FB_FIT_COUNT)
        return "default";
    return fit_names[fit];
}

int FB_FitFromName(const char *name)
{
    int i;

    for (i = 0; i < FB_FIT_COUNT; i++) {
        const char *a = name, *b = fit_names[i];

        while (*a && *b) {
            char c = *a;

            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            if (c != *b)
                break;
            a++;
            b++;
        }
        if (!*a && !*b)
            return i;
    }
    return -1;
}
