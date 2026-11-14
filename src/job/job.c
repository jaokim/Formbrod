/*
 * job.c - turn a timeline into a render job, and (de)serialise jobs.
 *
 * Each track becomes one stream as long as the whole result. For each clip
 * we emit a trimmed video chain (picture tracks) and an audio chain (when
 * the track has sound), fit it to the timeline's frame (see FBFormat), run
 * the clip's effects through their modules, fill gaps with black (clear on
 * upper picture tracks) and silence, and concat the track. With one track
 * that is the whole job:
 *
 *   [0:v]trim=start=1:end=4,setpts=PTS-STARTPTS,<fit>,fps=25/1,fade=...[v0];
 *   [0:a]atrim=start=1:end=4,asetpts=PTS-STARTPTS,aformat=...,afade=...[a0];
 *   [1:v]...[v1];
 *   anullsrc=...,atrim=duration=2,aformat=...[a1];     (clip without audio)
 *   [v0][a0][v1][a1]concat=n=2:v=1:a=1[outv][outa]
 *
 * With more tracks, each concat ends in [tNv]/[tNa]; picture tracks are
 * then laid on top of each other with overlay, bottom first, and all sound
 * is mixed with amix:
 *
 *   [t0v][t2v]overlay=format=auto:eof_action=pass[outv];
 *   [t0a][t1a]amix=inputs=2:duration=longest:dropout_transition=0,volume=2[outa]
 *
 * A clip with a crossfade is mixed into the one before it before the
 * track is joined: xfade for the picture (its offset counts from the start
 * of what it mixes into), acrossfade for the sound:
 *
 *   [v0][v1]xfade=transition=fade:duration=1.000:offset=3.000[xv1];
 *   [a0][a1]acrossfade=d=1.000[xa1];
 *   [xv1][xa1][v2][a2]concat=n=2:v=1:a=1[outv][outa]
 *
 * <fit> is one of (W x H is the frame):
 *   fit      scale=W:H:force_original_aspect_ratio=decrease,pad=W:H:...
 *   fill     scale=W:H:force_original_aspect_ratio=increase,crop=W:H
 *   stretch  scale=W:H
 * followed by setsar=1, or just setsar=1 for a clip that already matches.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "formbrod/job.h"
#include "formbrod/module.h"

#define PREVIEW_WIDTH 320
#define FB_AUDIO_RATE 48000   /* every clip's audio is resampled to this */

typedef struct Buf {
    char  *p;
    size_t len;
    size_t used;
    int    overflow;
} Buf;

static void buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (b->overflow)
        return;
    va_start(ap, fmt);
    n = vsnprintf(b->p + b->used, b->len - b->used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->len - b->used)
        b->overflow = 1;
    else
        b->used += (size_t)n;
}

static void copy_str(char *dst, const char *src, size_t len)
{
    strncpy(dst, src, len - 1);
    dst[len - 1] = '\0';
}

static double secs(fb_ms_t ms)
{
    return ms / 1000.0;
}

/* Appends ",<filters>" for every effect on the clip. */
static FBJobError add_effects(Buf *b, const FBClip *clip, int audio)
{
    char tmp[1024];
    int i;

    for (i = 0; i < clip->num_effects; i++) {
        const FBEffect *fx = &clip->effects[i];
        const FBModule *mod = FB_FindModule(fx->module);
        int (*fn)(const FBEffect *, fb_ms_t, char *, size_t);

        if (!mod)
            return FB_JOB_UNKNOWN_MODULE;
        if (mod->flags & FB_MODF_NATIVE)
            return FB_JOB_NOT_EXPRESSIBLE;

        fn = audio ? mod->audio_filter : mod->video_filter;
        if (!fn)
            continue;
        if (fn(fx, clip->out - clip->in, tmp, sizeof(tmp)) != 0)
            return FB_JOB_OVERFLOW;
        if (tmp[0])
            buf_printf(b, ",%s", tmp);
    }
    return FB_JOB_OK;
}

/* Rounds down to an even number of at least 2; yuv420p needs even sizes. */
static int even(int v)
{
    v &= ~1;
    return v < 2 ? 2 : v;
}

/* Appends the chain that makes a clip exactly fw x fh with square pixels,
 * according to its fit mode. */
static void add_fit(Buf *b, const FBTimeline *tl, const FBClip *c,
                    int fw, int fh)
{
    const FBSource *src = &tl->sources[c->source];
    int known = src->width > 0 && src->height > 0;

    /* Already the right size and shape: nothing to do. */
    if (known && FB_ClipFitResult(tl, c, fw, fh) == FB_FITS_NONE) {
        buf_printf(b, ",setsar=1");
        return;
    }
    /* Non-square pixels (DV, anamorphic DVD): make them square first, so
     * the aspect-keeping modes see the picture as it is shown. */
    if (src->sar_num > 0 && src->sar_den > 0 && src->sar_num != src->sar_den)
        buf_printf(b, ",scale=iw*sar:ih,setsar=1");

    switch (FB_ClipFit(tl, c)) {
    case FB_FIT_FILL:
        buf_printf(b, ",scale=%d:%d:force_original_aspect_ratio=increase,"
                   "crop=%d:%d", fw, fh, fw, fh);
        break;
    case FB_FIT_STRETCH:
        buf_printf(b, ",scale=%d:%d", fw, fh);
        break;
    default:
        buf_printf(b, ",scale=%d:%d:force_original_aspect_ratio=decrease,"
                   "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=0x%06lX",
                   fw, fh, fw, fh,
                   (unsigned long)(tl->format.pad_color & 0xFFFFFF));
        break;
    }
    buf_printf(b, ",setsar=1");
}

/* What a track contributes to the job. */
typedef struct TrackUse {
    int video;     /* picture: a picture track that isn't hidden */
    int audio;     /* sound: not muted, and some clip has sound */
    int clips;     /* clips on it */
} TrackUse;

typedef struct Frame {
    int fw, fh;            /* frame size, or 0 if unknown */
    int legacy_w;          /* size unknown: scale=W:-2 per clip */
    int fps_num, fps_den;  /* 0 if unknown */
} Frame;

/* One clip's picture: trimmed, fitted, at the frame rate, with effects.
 * Clips on upper tracks get an alpha channel so gaps can be clear. */
static FBJobError clip_video(Buf *b, const FBTimeline *tl, const FBClip *c,
                             int in, const Frame *f, int upper, int seg)
{
    FBJobError err;

    /* A still picture is one frame: repeat it at the frame rate, then
     * take the clip's part like any other. */
    if (tl->sources[c->source].still)
        buf_printf(b, "[%d:v]loop=loop=-1:size=1:start=0,"
                   "setpts=N*%d/(%d*TB),", in,
                   f->fps_num > 0 ? f->fps_den : 1,
                   f->fps_num > 0 ? f->fps_num : 25);
    else
        buf_printf(b, "[%d:v]", in);
    buf_printf(b, "trim=start=%.3f:end=%.3f,setpts=PTS-STARTPTS",
               secs(c->in), secs(c->out));
    /* concat needs one frame size and pixel shape for all clips */
    if (f->fw > 0)
        add_fit(b, tl, c, f->fw, f->fh);
    else if (f->legacy_w > 0)
        buf_printf(b, ",scale=%d:-2,setsar=1", f->legacy_w);
    if (f->fps_num > 0)
        buf_printf(b, ",fps=%d/%d", f->fps_num, f->fps_den);
    if ((err = add_effects(b, c, 0)) != FB_JOB_OK)
        return err;
    if (upper)
        buf_printf(b, ",format=yuva420p");
    buf_printf(b, "[v%d];", seg);
    return FB_JOB_OK;
}

/* One clip's sound, or silence as long as the clip if its source has none. */
static FBJobError clip_audio(Buf *b, const FBTimeline *tl, const FBClip *c,
                             int in, int seg)
{
    FBJobError err;

    if (tl->sources[c->source].has_audio)
        buf_printf(b, "[%d:a]atrim=start=%.3f:end=%.3f,asetpts=PTS-STARTPTS,",
                   in, secs(c->in), secs(c->out));
    else
        buf_printf(b, "anullsrc=r=%d:cl=stereo,atrim=duration=%.3f,",
                   FB_AUDIO_RATE, secs(c->out - c->in));
    /* one rate and layout for concat; aresample is auto-inserted */
    buf_printf(b, "aformat=sample_rates=%d:channel_layouts=stereo",
               FB_AUDIO_RATE);
    if (c->volume != 1.0 && c->volume >= 0.0)
        buf_printf(b, ",volume=%.3f", c->volume);
    if ((err = add_effects(b, c, 1)) != FB_JOB_OK)
        return err;
    buf_printf(b, "[a%d];", seg);
    return FB_JOB_OK;
}

/* A gap: black (clear on upper tracks) and silence. */
static void gap(Buf *b, const FBTimeline *tl, const Frame *f, fb_ms_t len,
                int video, int audio, int upper, int seg)
{
    int num = f->fps_num > 0 ? f->fps_num : 25;
    int den = f->fps_num > 0 ? f->fps_den : 1;

    if (video) {
        if (upper)
            buf_printf(b, "color=c=black@0:s=%dx%d:r=%d/%d:d=%.3f,"
                       "format=yuva420p", f->fw, f->fh, num, den, secs(len));
        else
            buf_printf(b, "color=c=0x%06lX:s=%dx%d:r=%d/%d:d=%.3f",
                       (unsigned long)(tl->format.pad_color & 0xFFFFFF),
                       f->fw, f->fh, num, den, secs(len));
        buf_printf(b, ",setsar=1[v%d];", seg);
    }
    if (audio)
        buf_printf(b, "anullsrc=r=%d:cl=stereo,atrim=duration=%.3f,"
                   "aformat=sample_rates=%d:channel_layouts=stereo[a%d];",
                   FB_AUDIO_RATE, secs(len), FB_AUDIO_RATE, seg);
}

/* The pieces a track is joined from: a clip, a gap, or clips mixed by
 * crossfades. Labels of their picture and sound, and how long they are. */
typedef struct TrackPart {
    char    v[12], a[12];
    fb_ms_t len;
} TrackPart;

typedef struct TrackParts {
    TrackPart part[FB_MAX_CLIPS * 2 + 1];   /* clips and the gaps between */
} TrackParts;

static void part_set(TrackParts *tp, int n, int seg, fb_ms_t len)
{
    snprintf(tp->part[n].v, sizeof(tp->part[n].v), "v%d", seg);
    snprintf(tp->part[n].a, sizeof(tp->part[n].a), "a%d", seg);
    tp->part[n].len = len;
}

FBJobError FB_JobBuild(FBJob *job, const FBTimeline *tl, const FBOutput *out)
{
    static TrackParts tp;
    int input_of[FB_MAX_SOURCES];
    TrackUse use[FB_MAX_TRACKS];
    int vtracks = 0, atracks = 0, bottom = -1;
    int need_size = 0;
    int out_w = out->width, out_h = out->height;
    fb_ms_t length = 0;
    Frame f;
    Buf b;
    int i, t, seg = 0, vdone = 0, adone = 0;


    memset(job, 0, sizeof(*job));
    job->version = FB_JOB_VERSION;
    job->output = *out;
    memset(&f, 0, sizeof(f));
    memset(use, 0, sizeof(use));

    for (i = 0; i < FB_MAX_SOURCES; i++)
        input_of[i] = -1;

    /* Which tracks give picture and sound, how long the result is, and
     * which sources become inputs. */
    for (i = 0; i < tl->num_clips; i++) {
        const FBClip *c = &tl->clips[i];
        const FBTrack *tr;
        int s = c->source;

        if (c->track < 0 || c->track >= tl->num_tracks)
            return FB_JOB_BAD_SOURCE;
        if (s < 0 || s >= tl->num_sources)
            return FB_JOB_BAD_SOURCE;
        /* It goes into the filtergraph as it is. */
        if (!FB_CrossfadeKindValid(c->xfade_kind))
            return FB_JOB_UNKNOWN_MODULE;
        tr = &tl->tracks[c->track];
        if (tr->kind == FB_TRACK_VIDEO && !tr->hidden)
            use[c->track].video = 1;
        if (!tr->muted && tl->sources[s].has_audio)
            use[c->track].audio = 1;
        use[c->track].clips++;
    }
    for (t = 0; t < tl->num_tracks; t++) {
        if (!use[t].video && !use[t].audio)
            continue;
        if (use[t].video) {
            if (bottom < 0)
                bottom = t;
            vtracks++;
        }
        if (use[t].audio)
            atracks++;
    }
    if (vtracks == 0)
        return FB_JOB_EMPTY;      /* nothing to see */

    for (i = 0; i < tl->num_clips; i++) {
        const FBClip *c = &tl->clips[i];
        int s = c->source;

        if (!use[c->track].video && !use[c->track].audio)
            continue;
        if (FB_ClipEnd(c) > length)
            length = FB_ClipEnd(c);
        if (input_of[s] < 0) {
            input_of[s] = job->num_inputs;
            copy_str(job->inputs[job->num_inputs++], tl->sources[s].path,
                     FB_PATH_LEN);
        }
    }

    /* Black, silence, a second picture track or a crossfade (both pictures
     * must be one size) all need the frame size. */
    if (vtracks > 1)
        need_size = 1;
    for (i = 0; i < tl->num_clips; i++)
        if (tl->clips[i].xfade > 0 && use[tl->clips[i].track].video)
            need_size = 1;
    for (t = 0; t < tl->num_tracks && !need_size; t++) {
        fb_ms_t cursor = 0;

        if (!use[t].video && !use[t].audio)
            continue;
        for (i = 0; i < tl->num_clips; i++) {
            if (tl->clips[i].track != t)
                continue;
            if (tl->clips[i].start != cursor)
                need_size = 1;
            cursor = FB_ClipEnd(&tl->clips[i]);
        }
        if (cursor != length)
            need_size = 1;
    }

    if (out->quality == FB_QUALITY_PREVIEW && out_w <= 0) {
        out_w = PREVIEW_WIDTH;
        out_h = 0;
    }

    /* The frame size: the output's if given, else the timeline's. A width
     * alone keeps the timeline's aspect. If the timeline's size isn't
     * known (sources not probed) we can only scale each clip on its own,
     * which is what older versions did. */
    if (FB_TimelineFrameSize(tl, &f.fw, &f.fh)) {
        if (out_w > 0 && out_h > 0) {
            f.fw = even(out_w);
            f.fh = even(out_h);
        } else if (out_w > 0) {
            f.fh = even((int)(((int64_t)out_w * f.fh + f.fw / 2) / f.fw));
            f.fw = even(out_w);
        }
    } else if (out_w > 0 && out_h > 0) {
        f.fw = even(out_w);
        f.fh = even(out_h);
    } else if (out_w > 0) {
        f.legacy_w = out_w;
    }
    if (need_size && f.fw <= 0)
        return FB_JOB_NEED_SIZE;
    FB_TimelineFrameRate(tl, &f.fps_num, &f.fps_den);

    b.p = job->filtergraph;
    b.len = sizeof(job->filtergraph);
    b.used = 0;
    b.overflow = 0;
    b.p[0] = '\0';

    /* Each track becomes one picture and/or sound stream as long as the
     * whole result: its clips, with black/clear and silence in the gaps,
     * joined with concat. With a single track this is the plain join of
     * earlier versions, ending in [outv][outa]. */
    for (t = 0; t < tl->num_tracks; t++) {
        int pv = use[t].video, pa = use[t].audio;
        int upper = pv && t != bottom;
        int parts = 0;
        fb_ms_t cursor = 0;
        FBJobError err;

        if (!pv && !pa)
            continue;
        for (i = 0; i < tl->num_clips; i++) {
            const FBClip *c = &tl->clips[i];
            int xf = c->xfade > 0 && parts > 0;

            if (c->track != t)
                continue;
            if (c->start > cursor) {
                gap(&b, tl, &f, c->start - cursor, pv, pa, upper, seg);
                part_set(&tp, parts++, seg++, c->start - cursor);
            }
            if (pv &&
                (err = clip_video(&b, tl, c, input_of[c->source], &f, upper,
                                  seg)) != FB_JOB_OK)
                return err;
            if (pa &&
                (err = clip_audio(&b, tl, c, input_of[c->source], seg))
                != FB_JOB_OK)
                return err;
            if (xf) {
                /* Mixed into what came before it: the result replaces
                 * the last part. */
                TrackPart *last = &tp.part[parts - 1];
                fb_ms_t d = c->xfade;

                if (pv)
                    buf_printf(&b, "[%s][v%d]xfade=transition=%s:"
                               "duration=%.3f:offset=%.3f[xv%d];",
                               last->v, seg, FB_ClipCrossfadeKind(c),
                               secs(d), secs(last->len - d), seg);
                if (pa)
                    buf_printf(&b, "[%s][a%d]acrossfade=d=%.3f[xa%d];",
                               last->a, seg, secs(d), seg);
                snprintf(last->v, sizeof(last->v), "xv%d", seg);
                snprintf(last->a, sizeof(last->a), "xa%d", seg);
                last->len += (c->out - c->in) - d;
                seg++;
            } else {
                part_set(&tp, parts++, seg++, c->out - c->in);
            }
            cursor = FB_ClipEnd(c);
        }
        if (cursor < length) {
            gap(&b, tl, &f, length - cursor, pv, pa, upper, seg);
            part_set(&tp, parts++, seg++, length - cursor);
        }

        for (i = 0; i < parts; i++) {
            if (pv)
                buf_printf(&b, "[%s]", tp.part[i].v);
            if (pa)
                buf_printf(&b, "[%s]", tp.part[i].a);
        }
        buf_printf(&b, "concat=n=%d:v=%d:a=%d", parts, pv, pa);
        if (pv) {
            if (vtracks == 1)
                buf_printf(&b, "[outv]");
            else
                buf_printf(&b, "[t%dv]", t);
        }
        if (pa) {
            if (atracks == 1)
                buf_printf(&b, "[outa]");
            else
                buf_printf(&b, "[t%da]", t);
        }
        buf_printf(&b, ";");
    }

    /* Picture tracks on top of each other, bottom first. */
    if (vtracks > 1) {
        for (t = 0; t < tl->num_tracks; t++) {
            if (!use[t].video)
                continue;
            if (t == bottom) {
                vdone = 1;
                continue;
            }
            /* The pile so far: the bottom track, or the overlay before,
             * which wrote [c<vdone - 2>]. */
            if (vdone == 1)
                buf_printf(&b, "[t%dv]", bottom);
            else
                buf_printf(&b, "[c%d]", vdone - 2);
            buf_printf(&b, "[t%dv]overlay=format=auto:eof_action=pass", t);
            if (++vdone == vtracks)
                buf_printf(&b, "[outv];");
            else
                buf_printf(&b, "[c%d];", vdone - 2);
        }
    }

    /* All sound mixed. amix divides by the number of inputs; every track
     * lasts the whole length, so multiplying back gives the plain sum. */
    if (atracks > 1) {
        for (t = 0; t < tl->num_tracks; t++)
            if (use[t].audio) {
                buf_printf(&b, "[t%da]", t);
                adone++;
            }
        buf_printf(&b, "amix=inputs=%d:duration=longest:dropout_transition=0,"
                   "volume=%d[outa];", atracks, atracks);
    }

    /* No trailing ';' */
    if (!b.overflow && b.used > 0 && b.p[b.used - 1] == ';')
        b.p[--b.used] = '\0';
    if (b.overflow)
        return FB_JOB_OVERFLOW;

    job->length = length;
    copy_str(job->video_out, "outv", FB_NAME_LEN);
    copy_str(job->audio_out, atracks > 0 ? "outa" : "", FB_NAME_LEN);
    return FB_JOB_OK;
}

const char *FB_JobErrorString(FBJobError err)
{
    switch (err) {
    case FB_JOB_OK:              return "OK";
    case FB_JOB_EMPTY:           return "The timeline is empty";
    case FB_JOB_BAD_SOURCE:      return "A clip refers to a missing source";
    case FB_JOB_UNKNOWN_MODULE:  return "An effect uses an unknown module";
    case FB_JOB_NOT_EXPRESSIBLE: return "An effect can't be rendered with ffmpeg filters";
    case FB_JOB_OVERFLOW:        return "The edit is too complex (filtergraph too long)";
    case FB_JOB_NEED_SIZE:       return "The frame size isn't known (files not probed)";
    }
    return "Unknown error";
}

/* ---- Text format ------------------------------------------------------
 *
 *   FORMBROD-JOB 1
 *   INPUT <path>             (repeated)
 *   FILTERGRAPH <graph>
 *   VOUT <label>
 *   AOUT <label>             (omitted if no audio)
 *   OUTPUT <path>
 *   VCODEC <name>
 *   ACODEC <name>
 *   SIZE <w> <h>
 *   QUALITY <0|1>
 *   LENGTH <ms>              (how long the result is; for progress)
 *   END
 *
 * Values run to end of line, so paths may contain spaces.
 */

int FB_JobWrite(const FBJob *job, char *out, size_t len)
{
    Buf b;
    int i;

    b.p = out;
    b.len = len;
    b.used = 0;
    b.overflow = 0;

    buf_printf(&b, "FORMBROD-JOB %d\n", job->version);
    for (i = 0; i < job->num_inputs; i++)
        buf_printf(&b, "INPUT %s\n", job->inputs[i]);
    buf_printf(&b, "FILTERGRAPH %s\n", job->filtergraph);
    buf_printf(&b, "VOUT %s\n", job->video_out);
    if (job->audio_out[0])
        buf_printf(&b, "AOUT %s\n", job->audio_out);
    buf_printf(&b, "OUTPUT %s\n", job->output.path);
    if (job->output.vcodec[0])
        buf_printf(&b, "VCODEC %s\n", job->output.vcodec);
    if (job->output.acodec[0])
        buf_printf(&b, "ACODEC %s\n", job->output.acodec);
    buf_printf(&b, "SIZE %d %d\n", job->output.width, job->output.height);
    buf_printf(&b, "QUALITY %d\n", (int)job->output.quality);
    if (job->length > 0)
        buf_printf(&b, "LENGTH %lld\n", (long long)job->length);
    buf_printf(&b, "END\n");

    return b.overflow ? -1 : (int)b.used;
}

/* Copies the rest of the line (after "KEY ") into dst. */
static const char *take_line(const char *p, char *dst, size_t len)
{
    size_t n = 0;

    while (*p && *p != '\n' && *p != '\r') {
        if (n < len - 1)
            dst[n++] = *p;
        p++;
    }
    dst[n] = '\0';
    while (*p == '\r' || *p == '\n')
        p++;
    return p;
}

int FB_JobParse(FBJob *job, const char *text)
{
    static char line[FB_FILTERGRAPH_LEN + 32];
    const char *p = text;
    int seen_header = 0, seen_end = 0;

    memset(job, 0, sizeof(*job));

    while (*p && !seen_end) {
        char *val;

        p = take_line(p, line, sizeof(line));
        if (!line[0])
            continue;
        val = strchr(line, ' ');
        if (val)
            *val++ = '\0';
        else
            val = line + strlen(line);

        if (!seen_header) {
            if (strcmp(line, "FORMBROD-JOB") != 0)
                return -1;
            job->version = atoi(val);
            if (job->version != FB_JOB_VERSION)
                return -1;
            seen_header = 1;
        } else if (strcmp(line, "INPUT") == 0) {
            if (job->num_inputs >= FB_MAX_SOURCES)
                return -1;
            copy_str(job->inputs[job->num_inputs++], val, FB_PATH_LEN);
        } else if (strcmp(line, "FILTERGRAPH") == 0) {
            copy_str(job->filtergraph, val, sizeof(job->filtergraph));
        } else if (strcmp(line, "VOUT") == 0) {
            copy_str(job->video_out, val, FB_NAME_LEN);
        } else if (strcmp(line, "AOUT") == 0) {
            copy_str(job->audio_out, val, FB_NAME_LEN);
        } else if (strcmp(line, "OUTPUT") == 0) {
            copy_str(job->output.path, val, FB_PATH_LEN);
        } else if (strcmp(line, "VCODEC") == 0) {
            copy_str(job->output.vcodec, val, FB_NAME_LEN);
        } else if (strcmp(line, "ACODEC") == 0) {
            copy_str(job->output.acodec, val, FB_NAME_LEN);
        } else if (strcmp(line, "SIZE") == 0) {
            if (sscanf(val, "%d %d", &job->output.width,
                       &job->output.height) != 2)
                return -1;
        } else if (strcmp(line, "QUALITY") == 0) {
            job->output.quality = atoi(val) ? FB_QUALITY_FINAL
                                            : FB_QUALITY_PREVIEW;
        } else if (strcmp(line, "LENGTH") == 0) {
            job->length = (fb_ms_t)strtoll(val, NULL, 10);
            if (job->length < 0)
                job->length = 0;
        } else if (strcmp(line, "END") == 0) {
            seen_end = 1;
        }
        /* Unknown keys are ignored so newer editors can talk to older
         * servers. */
    }
    return (seen_header && seen_end) ? 0 : -1;
}

static void buf_quoted(Buf *b, const char *s)
{
    buf_printf(b, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            buf_printf(b, "\\%c", *s);
        else
            buf_printf(b, "%c", *s);
    }
    buf_printf(b, "\"");
}

int FB_JobToCommandLine(const FBJob *job, char *out, size_t len)
{
    Buf b;
    int i;

    b.p = out;
    b.len = len;
    b.used = 0;
    b.overflow = 0;

    buf_printf(&b, "ffmpeg -y");
    for (i = 0; i < job->num_inputs; i++) {
        buf_printf(&b, " -i ");
        buf_quoted(&b, job->inputs[i]);
    }
    buf_printf(&b, " -filter_complex ");
    buf_quoted(&b, job->filtergraph);
    buf_printf(&b, " -map \"[%s]\"", job->video_out);
    if (job->audio_out[0])
        buf_printf(&b, " -map \"[%s]\"", job->audio_out);
    if (job->output.vcodec[0])
        buf_printf(&b, " -c:v %s", job->output.vcodec);
    if (job->output.acodec[0])
        buf_printf(&b, " -c:a %s", job->output.acodec);
    buf_printf(&b, " ");
    buf_quoted(&b, job->output.path);

    return b.overflow ? -1 : (int)b.used;
}

/* Reads "key=value" options of one filter (up to ',', ';' or '[') and
 * returns the value of `key` in ms, or -1 if absent. Plain seconds only,
 * which is what FB_JobBuild() writes. */
static fb_ms_t trim_option(const char *args, const char *key)
{
    size_t klen = strlen(key);
    const char *p = args;

    while (*p && *p != ',' && *p != ';' && *p != '[') {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=')
            return (fb_ms_t)(atof(p + klen + 1) * 1000.0 + 0.5);
        while (*p && *p != ':' && *p != ',' && *p != ';' && *p != '[')
            p++;
        if (*p == ':')
            p++;
    }
    return -1;
}

fb_ms_t FB_JobEstimateLength(const FBJob *job)
{
    const char *g = job->filtergraph;
    const char *p = g;
    fb_ms_t total = 0;
    int found = 0;

    if (job->length > 0)
        return job->length;

    while ((p = strstr(p, "trim=")) != NULL) {
        /* "trim" as a whole filter name, not "atrim" */
        int whole = (p == g || p[-1] == ',' || p[-1] == ';' || p[-1] == ']'
                     || p[-1] == ' ');
        const char *args = p + 5;

        p = args;
        if (whole) {
            fb_ms_t start = trim_option(args, "start");
            fb_ms_t end   = trim_option(args, "end");
            fb_ms_t dur   = trim_option(args, "duration");

            if (start < 0)
                start = 0;
            if (dur >= 0)
                total += dur;
            else if (end > start)
                total += end - start;
            else
                return 0;
            found = 1;
        }
    }
    return found ? total : 0;
}
