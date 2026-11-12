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

/* ---- Tracks ----------------------------------------------------------- */

int FB_TimelineAddTrack(FBTimeline *tl, FBTrackKind kind)
{
    FBTrack *t;

    if (tl->num_tracks >= FB_MAX_TRACKS)
        return -1;
    t = &tl->tracks[tl->num_tracks];
    memset(t, 0, sizeof(*t));
    t->kind = kind == FB_TRACK_AUDIO ? FB_TRACK_AUDIO : FB_TRACK_VIDEO;
    return tl->num_tracks++;
}

int FB_TimelineRemoveTrack(FBTimeline *tl, int track)
{
    int i, n = 0;

    if (track < 0 || track >= tl->num_tracks)
        return -1;
    for (i = 0; i < tl->num_clips; i++) {
        if (tl->clips[i].track == track)
            continue;
        tl->clips[n] = tl->clips[i];
        if (tl->clips[n].track > track)
            tl->clips[n].track--;
        n++;
    }
    tl->num_clips = n;
    memmove(&tl->tracks[track], &tl->tracks[track + 1],
            (size_t)(tl->num_tracks - track - 1) * sizeof(FBTrack));
    tl->num_tracks--;
    return 0;
}

fb_ms_t FB_TimelineTrackEnd(const FBTimeline *tl, int track)
{
    fb_ms_t end = 0;
    int i;

    for (i = 0; i < tl->num_clips; i++)
        if (tl->clips[i].track == track && FB_ClipEnd(&tl->clips[i]) > end)
            end = FB_ClipEnd(&tl->clips[i]);
    return end;
}

FBTrackKind FB_SourceTrackKind(const FBSource *src)
{
    return (src->has_video || !src->has_audio) ? FB_TRACK_VIDEO
                                               : FB_TRACK_AUDIO;
}

const char *FB_TrackKindName(FBTrackKind kind)
{
    return kind == FB_TRACK_AUDIO ? "audio" : "video";
}

static int same_word(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;

        if (ca != cb)
            return 0;
    }
    return *a == *b;
}

int FB_TrackKindFromName(const char *name)
{
    if (same_word(name, "video"))
        return FB_TRACK_VIDEO;
    if (same_word(name, "audio"))
        return FB_TRACK_AUDIO;
    return -1;
}

/* ---- Clips ------------------------------------------------------------ */

fb_ms_t FB_ClipEnd(const FBClip *clip)
{
    return clip->start + (clip->out - clip->in);
}

/* Moves every clip on `track` that starts at or after `from` by `delta`. */
static void shift_track(FBTimeline *tl, int track, fb_ms_t from,
                        fb_ms_t delta, int except)
{
    int i;

    for (i = 0; i < tl->num_clips; i++)
        if (i != except && tl->clips[i].track == track &&
            tl->clips[i].start >= from)
            tl->clips[i].start += delta;
}

int FB_TimelineAddClip(FBTimeline *tl, int track, int source,
                       fb_ms_t in, fb_ms_t out, fb_ms_t start)
{
    FBClip *clip;
    fb_ms_t end = start + (out - in);
    int pos;

    if (tl->num_clips >= FB_MAX_CLIPS)
        return -1;
    if (track < 0 || track >= tl->num_tracks)
        return -1;
    if (source < 0 || source >= tl->num_sources)
        return -1;
    if (in < 0 || out <= in || start < 0)
        return -1;

    /* Sorted by track, then start. */
    for (pos = 0; pos < tl->num_clips; pos++) {
        const FBClip *c = &tl->clips[pos];

        if (c->track > track || (c->track == track && c->start >= start))
            break;
    }
    if (pos > 0 && tl->clips[pos - 1].track == track &&
        FB_ClipEnd(&tl->clips[pos - 1]) > start)
        return -1;
    if (pos < tl->num_clips && tl->clips[pos].track == track &&
        tl->clips[pos].start < end)
        return -1;

    memmove(&tl->clips[pos + 1], &tl->clips[pos],
            (size_t)(tl->num_clips - pos) * sizeof(FBClip));
    clip = &tl->clips[pos];
    memset(clip, 0, sizeof(*clip));
    clip->source = source;
    clip->track  = track;
    clip->start  = start;
    clip->in     = in;
    clip->out    = out;
    clip->volume = 1.0;
    tl->num_clips++;
    return pos;
}

int FB_TimelineAppendToTrack(FBTimeline *tl, int track, int source,
                             fb_ms_t in, fb_ms_t out)
{
    int added = 0, idx;

    if (source < 0 || source >= tl->num_sources)
        return -1;
    if (in < 0 || out <= in || tl->num_clips >= FB_MAX_CLIPS)
        return -1;
    if (track == tl->num_tracks) {
        if (FB_TimelineAddTrack(tl, FB_SourceTrackKind(&tl->sources[source]))
            < 0)
            return -1;
        added = 1;
    }
    idx = FB_TimelineAddClip(tl, track, source, in, out,
                             FB_TimelineTrackEnd(tl, track));
    if (idx < 0 && added)
        tl->num_tracks--;
    return idx;
}

int FB_TimelineAppendClip(FBTimeline *tl, int source, fb_ms_t in, fb_ms_t out)
{
    return FB_TimelineAppendToTrack(tl, 0, source, in, out);
}

int FB_TimelineSplit(FBTimeline *tl, int track, fb_ms_t at)
{
    int i, last = -1;

    for (i = 0; i < tl->num_clips; i++) {
        FBClip *clip = &tl->clips[i];

        if (track >= 0 && clip->track != track)
            continue;
        if (at > clip->start && at < FB_ClipEnd(clip)) {
            fb_ms_t cut = clip->in + (at - clip->start);

            if (tl->num_clips >= FB_MAX_CLIPS)
                return last;
            memmove(&tl->clips[i + 2], &tl->clips[i + 1],
                    (size_t)(tl->num_clips - i - 1) * sizeof(FBClip));
            tl->clips[i + 1] = *clip;      /* right half keeps the effects */
            tl->clips[i + 1].in = cut;
            tl->clips[i + 1].start = at;
            clip->out = cut;
            tl->num_clips++;
            last = ++i;                    /* don't look at the new half */
        }
    }
    return last;
}

int FB_TimelineRemoveClip(FBTimeline *tl, int clip)
{
    FBClip gone;

    if (clip < 0 || clip >= tl->num_clips)
        return -1;
    gone = tl->clips[clip];
    memmove(&tl->clips[clip], &tl->clips[clip + 1],
            (size_t)(tl->num_clips - clip - 1) * sizeof(FBClip));
    tl->num_clips--;
    shift_track(tl, gone.track, FB_ClipEnd(&gone), -(gone.out - gone.in), -1);
    return 0;
}

int FB_TimelineTrimClip(FBTimeline *tl, int clip, fb_ms_t in, fb_ms_t out)
{
    FBClip *c;
    fb_ms_t old_end, dur;

    if (clip < 0 || clip >= tl->num_clips)
        return -1;
    c = &tl->clips[clip];
    dur = tl->sources[c->source].duration;
    if (in < 0 || out <= in || (dur > 0 && out > dur))
        return -1;
    old_end = FB_ClipEnd(c);
    c->in = in;
    c->out = out;
    shift_track(tl, c->track, old_end, FB_ClipEnd(c) - old_end, clip);
    return 0;
}

fb_ms_t FB_TimelineEarliestStart(const FBTimeline *tl, int clip)
{
    if (clip > 0 && clip < tl->num_clips &&
        tl->clips[clip - 1].track == tl->clips[clip].track)
        return FB_ClipEnd(&tl->clips[clip - 1]);
    return 0;
}

int FB_TimelineMoveClip(FBTimeline *tl, int clip, fb_ms_t start)
{
    fb_ms_t delta;
    int i;

    if (clip < 0 || clip >= tl->num_clips ||
        start < FB_TimelineEarliestStart(tl, clip))
        return -1;
    /* Sorted by track and start: the clips after it on its track follow
     * it in the array. */
    delta = start - tl->clips[clip].start;
    for (i = clip; i < tl->num_clips &&
                   tl->clips[i].track == tl->clips[clip].track; i++)
        tl->clips[i].start += delta;
    return 0;
}

int FB_TimelineClipAt(const FBTimeline *tl, fb_ms_t t, fb_ms_t *source_t)
{
    int i;

    /* Sorted by track, so going backwards finds the highest track first. */
    for (i = tl->num_clips - 1; i >= 0; i--) {
        const FBClip *c = &tl->clips[i];
        const FBTrack *tr = &tl->tracks[c->track];

        if (tr->kind != FB_TRACK_VIDEO || tr->hidden)
            continue;
        if (t >= c->start && t < FB_ClipEnd(c)) {
            if (source_t)
                *source_t = c->in + (t - c->start);
            return i;
        }
    }
    return -1;
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
        if (FB_ClipEnd(&tl->clips[i]) > total)
            total = FB_ClipEnd(&tl->clips[i]);
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

/* The source of the first clip on the lowest picture track, or with
 * `moving`, the first that isn't a still picture (a picture's "frame rate"
 * means nothing). */
static const FBSource *first_picture(const FBTimeline *tl, int moving)
{
    int i, s;

    for (i = 0; i < tl->num_clips; i++) {
        if (tl->tracks[tl->clips[i].track].kind != FB_TRACK_VIDEO)
            continue;
        s = tl->clips[i].source;
        if (s < 0 || s >= tl->num_sources)
            return NULL;
        if (moving && tl->sources[s].still)
            continue;
        return &tl->sources[s];
    }
    return NULL;
}

static const FBSource *first_source(const FBTimeline *tl)
{
    return first_picture(tl, 0);
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
    src = first_picture(tl, 1);
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
