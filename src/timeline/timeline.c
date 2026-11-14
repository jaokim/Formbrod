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

static fb_ms_t clip_len(const FBClip *c)
{
    return c->out - c->in;
}

/* Moves clip `first` and the clips after it on its track by `delta`
 * (sorted by track and start, they follow it in the array). */
static void shift_from(FBTimeline *tl, int first, fb_ms_t delta)
{
    int i, track;

    if (first < 0 || first >= tl->num_clips || delta == 0)
        return;
    track = tl->clips[first].track;
    for (i = first; i < tl->num_clips && tl->clips[i].track == track; i++)
        tl->clips[i].start += delta;
}

/* The clip before / after `i` on its track, or -1. */
static int prev_on_track(const FBTimeline *tl, int i)
{
    return (i > 0 && tl->clips[i - 1].track == tl->clips[i].track) ? i - 1
                                                                   : -1;
}

static int next_on_track(const FBTimeline *tl, int i)
{
    return (i + 1 < tl->num_clips &&
            tl->clips[i + 1].track == tl->clips[i].track) ? i + 1 : -1;
}

/* How long clip `i`'s crossfade can be: what the clip before it leaves
 * after its own crossfade, and what `i` leaves before the next one's. */
static fb_ms_t xfade_room(const FBTimeline *tl, int i)
{
    int p = prev_on_track(tl, i), n = next_on_track(tl, i);
    fb_ms_t a, b;

    if (p < 0)
        return 0;
    a = clip_len(&tl->clips[p]) - tl->clips[p].xfade;
    b = clip_len(&tl->clips[i]) - (n >= 0 ? tl->clips[n].xfade : 0);
    return a < b ? a : b;
}

/* After an edit: crossfades that no longer fit are shortened (moving the
 * clip and those after it later), and the first clip of a track has none. */
static void fit_crossfades(FBTimeline *tl, int track)
{
    int i;

    for (i = 0; i < tl->num_clips; i++) {
        FBClip *c = &tl->clips[i];
        fb_ms_t room;

        if (c->track != track || c->xfade <= 0) {
            if (c->xfade < 0)
                c->xfade = 0;
            continue;
        }
        room = xfade_room(tl, i);
        if (room < 0)
            room = 0;
        if (c->xfade > room) {
            fb_ms_t cut = c->xfade - room;

            c->xfade = room;
            shift_from(tl, i, cut);
        }
    }
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
            tl->clips[i + 1].xfade = 0;    /* it follows the left half */
            tl->clips[i + 1].xfade_kind[0] = '\0';
            clip->out = cut;
            tl->num_clips++;
            fit_crossfades(tl, clip->track);
            last = ++i;                    /* don't look at the new half */
        }
    }
    return last;
}

int FB_TimelineRemoveClip(FBTimeline *tl, int clip)
{
    FBClip gone;
    fb_ms_t delta;
    int next;

    if (clip < 0 || clip >= tl->num_clips)
        return -1;
    gone = tl->clips[clip];
    next = next_on_track(tl, clip);
    /* The clips after it close up to where it took over from the one
     * before it. A crossfade into it goes with it, and so does one out of
     * it: the next clip then simply follows. */
    delta = -(clip_len(&gone) - gone.xfade);
    if (next >= 0) {
        delta += tl->clips[next].xfade;
        tl->clips[next].xfade = 0;
        tl->clips[next].xfade_kind[0] = '\0';
    }
    memmove(&tl->clips[clip], &tl->clips[clip + 1],
            (size_t)(tl->num_clips - clip - 1) * sizeof(FBClip));
    tl->num_clips--;
    if (next >= 0)
        shift_from(tl, clip, delta);
    fit_crossfades(tl, gone.track);
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
    if (next_on_track(tl, clip) >= 0)
        shift_from(tl, clip + 1, FB_ClipEnd(c) - old_end);
    fit_crossfades(tl, c->track);
    return 0;
}

int FB_TrackFits(const FBTimeline *tl, int track, const FBSource *src)
{
    if (track < 0 || track >= tl->num_tracks)
        return 0;
    return tl->tracks[track].kind == FB_TRACK_VIDEO ? src->has_video
                                                    : src->has_audio;
}

fb_ms_t FB_TimelineStartWithout(const FBTimeline *tl, int i, int skip)
{
    const FBClip *c = &tl->clips[i];

    if (skip >= 0 && skip < tl->num_clips && i > skip &&
        c->track == tl->clips[skip].track) {
        const FBClip *k = &tl->clips[skip];
        fb_ms_t s = c->start - (clip_len(k) - k->xfade);

        if (i == skip + 1)
            s += c->xfade;     /* its crossfade goes (see RemoveClip) */
        return s;
    }
    return c->start;
}

/* Clip `i`'s crossfade once clip `skip` is out of its place. */
static fb_ms_t xfade_without(const FBTimeline *tl, int i, int skip)
{
    if (skip >= 0 && i == skip + 1 && tl->clips[i].track == tl->clips[skip].track)
        return 0;
    return tl->clips[i].xfade;
}

fb_ms_t FB_TimelineDropPoint(const FBTimeline *tl, int track, fb_ms_t at,
                             fb_ms_t len, int skip, int *inserted)
{
    fb_ms_t middle, end = -1;
    int i, free = 1;

    if (at < 0)
        at = 0;
    *inserted = 0;
    for (i = 0; i < tl->num_clips; i++) {
        const FBClip *c = &tl->clips[i];
        fb_ms_t s;

        if (i == skip || c->track != track)
            continue;
        s = FB_TimelineStartWithout(tl, i, skip);
        if (s < at + len && s + (c->out - c->in) > at)
            free = 0;
    }
    if (free)
        return at;

    /* In the way: in between clips instead. */
    *inserted = 1;
    middle = at + len / 2;
    for (i = 0; i < tl->num_clips; i++) {
        const FBClip *c = &tl->clips[i];
        fb_ms_t s;

        if (i == skip || c->track != track)
            continue;
        s = FB_TimelineStartWithout(tl, i, skip);
        if (s + (c->out - c->in) / 2 > middle)
            return s + xfade_without(tl, i, skip);  /* after its crossfade's
                                                       partner: at the cut */
        end = s + (c->out - c->in);
    }
    *inserted = 0;          /* after the last clip: nothing moves */
    return end;
}

int FB_TimelineMoveClipTo(FBTimeline *tl, int clip, int track, fb_ms_t at)
{
    FBClip moved;
    fb_ms_t len, start;
    int idx, inserted;

    if (clip < 0 || clip >= tl->num_clips || track < 0 ||
        track > tl->num_tracks)
        return -1;
    moved = tl->clips[clip];
    len = moved.out - moved.in;
    if (track == tl->num_tracks) {
        if (FB_TimelineAddTrack(tl,
                FB_SourceTrackKind(&tl->sources[moved.source])) < 0)
            return -1;
    } else if (!FB_TrackFits(tl, track, &tl->sources[moved.source])) {
        return -1;
    }

    /* Out of its old place, which closes up; then where it goes. */
    FB_TimelineRemoveClip(tl, clip);
    start = FB_TimelineDropPoint(tl, track, at, len, -1, &inserted);
    if (inserted) {
        int i;

        /* Going in at a crossfade: the clip it led into now follows the
         * moved one, with a plain cut. */
        for (i = 0; i < tl->num_clips; i++) {
            FBClip *c = &tl->clips[i];

            if (c->track == track && c->xfade > 0 &&
                c->start + c->xfade == start) {
                fb_ms_t xf = c->xfade;

                c->xfade = 0;
                c->xfade_kind[0] = '\0';
                shift_from(tl, i, xf);
                break;
            }
        }
        shift_track(tl, track, start, len, -1);
    }

    idx = FB_TimelineAddClip(tl, track, moved.source, moved.in, moved.out,
                             start);
    if (idx < 0)
        return -1;
    moved.track = track;
    moved.start = start;
    moved.xfade = 0;             /* a cut, until the user adds one */
    moved.xfade_kind[0] = '\0';
    tl->clips[idx] = moved;      /* with its effects, fit and volume */
    return idx;
}

/* Where the clip before `clip` on its track ends, or 0. */
static fb_ms_t prev_end(const FBTimeline *tl, int clip)
{
    int p = prev_on_track(tl, clip);

    return p >= 0 ? FB_ClipEnd(&tl->clips[p]) : 0;
}

fb_ms_t FB_TimelineEarliestStart(const FBTimeline *tl, int clip)
{
    if (clip < 0 || clip >= tl->num_clips)
        return 0;
    if (tl->clips[clip].xfade > 0)
        return prev_end(tl, clip) - xfade_room(tl, clip);
    return prev_end(tl, clip);
}

int FB_TimelineMoveClip(FBTimeline *tl, int clip, fb_ms_t start)
{
    FBClip *c;
    fb_ms_t pe;

    if (clip < 0 || clip >= tl->num_clips)
        return -1;
    c = &tl->clips[clip];
    pe = prev_end(tl, clip);
    if (start == c->start)
        return 0;
    /* Into the clip before: only a crossfade may overlap it. */
    if (start < pe) {
        if (c->xfade <= 0 || start < FB_TimelineEarliestStart(tl, clip))
            return -1;
        return FB_TimelineSetCrossfade(tl, clip, pe - start, NULL) < 0 ? -1
                                                                       : 0;
    }
    c->xfade = 0;
    c->xfade_kind[0] = '\0';
    /* Sorted by track and start: the clips after it on its track follow
     * it in the array. */
    shift_from(tl, clip, start - c->start);
    return 0;
}

/* ---- Crossfades ------------------------------------------------------- */

fb_ms_t FB_TimelineMaxCrossfade(const FBTimeline *tl, int clip)
{
    const FBClip *c;
    fb_ms_t room;

    if (clip < 0 || clip >= tl->num_clips)
        return 0;
    c = &tl->clips[clip];
    if (prev_on_track(tl, clip) < 0)
        return 0;
    /* Straight after the clip before it, or overlapping it already. */
    if (c->xfade <= 0 && c->start != prev_end(tl, clip))
        return 0;
    room = xfade_room(tl, clip);
    return room > 0 ? room : 0;
}

int FB_CrossfadeKindValid(const char *kind)
{
    size_t n = 0;

    for (; kind[n]; n++) {
        char ch = kind[n];

        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              ch == '_'))
            return 0;
    }
    return n < FB_NAME_LEN;
}

const char *FB_ClipCrossfadeKind(const FBClip *clip)
{
    return clip->xfade_kind[0] ? clip->xfade_kind : "fade";
}

fb_ms_t FB_TimelineSetCrossfade(FBTimeline *tl, int clip, fb_ms_t len,
                                const char *kind)
{
    FBClip *c;
    fb_ms_t room;

    if (clip < 0 || clip >= tl->num_clips || len < 0)
        return -1;
    if (kind && !FB_CrossfadeKindValid(kind))
        return -1;
    c = &tl->clips[clip];
    if (len > 0) {
        room = FB_TimelineMaxCrossfade(tl, clip);
        if (room <= 0)
            return -1;
        if (len > room)
            len = room;
    }
    /* Longer pulls it and the clips after it earlier, shorter pushes. */
    shift_from(tl, clip, c->xfade - len);
    c->xfade = len;
    if (len == 0)
        c->xfade_kind[0] = '\0';
    else if (kind)
        copy_str(c->xfade_kind, strcmp(kind, "fade") == 0 ? "" : kind,
                 sizeof(c->xfade_kind));
    return len;
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
