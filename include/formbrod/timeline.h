/*
 * timeline.h - the editing model.
 *
 * A project has a list of sources (video and sound files) and a list of
 * tracks. Each clip sits on one track at a start time, and is a [in, out)
 * window into one source plus an optional chain of effects. This file is
 * plain C99 with no AmigaOS dependencies so it can be unit-tested on any
 * host.
 *
 * Tracks:
 *   - A picture track (FB_TRACK_VIDEO) plays its clips' picture and, unless
 *     muted, their sound. The window shows that sound on a lane under the
 *     picture, linked to it. (Unlinking will later move the sound into its
 *     own clip on a sound track.)
 *   - A sound track (FB_TRACK_AUDIO) plays only sound: music, voice-over.
 *   - Track 0 is the bottom one. Higher picture tracks are drawn on top of
 *     lower ones; all sound is mixed together.
 *
 * Clips on a track never overlap, and the array is kept sorted by track and
 * then start time. Cutting closes gaps: removing or trimming a clip moves
 * the clips after it on the same track. A gap only appears where a clip was
 * placed with a later start (a project file's AT line, later the detail
 * editor); gaps play as black (or transparent, on upper picture tracks)
 * and silence.
 *
 * The timeline also has an output format (frame size, frame rate and how
 * clips of another shape are fitted). Everything in it may be left at 0,
 * meaning "take it from the first clip", so a single file or files of one
 * size need no setup. Each clip can override the fit mode.
 */
#ifndef FORMBROD_TIMELINE_H
#define FORMBROD_TIMELINE_H

#include <stddef.h>
#include <stdint.h>

#define FB_MAX_SOURCES   32
#define FB_MAX_TRACKS    16
#define FB_MAX_CLIPS     256

/* How long a still picture is when it's added. */
#define FB_STILL_LENGTH  5000
#define FB_MAX_EFFECTS   8
#define FB_MAX_PARAMS    8
#define FB_PATH_LEN      512
#define FB_NAME_LEN      32

typedef int64_t fb_ms_t; /* time in milliseconds */

typedef struct FBSource {
    char    path[FB_PATH_LEN];
    fb_ms_t duration;       /* 0 if not probed yet, or a still picture */
    int     has_video;
    int     has_audio;
    int     still;          /* a single picture: clips can be any length */
    /* Video stream info from probing; 0 = unknown. */
    int     width, height;
    int     sar_num, sar_den;   /* pixel aspect, 0/0 or 1/1 = square */
    int     fps_num, fps_den;
} FBSource;

/* How a clip whose shape differs from the output frame is fitted into it. */
typedef enum {
    FB_FIT_DEFAULT = 0,   /* clips only: use the timeline's default */
    FB_FIT_FIT,           /* scale to fit, pad with bars (letter/pillarbox) */
    FB_FIT_FILL,          /* scale to cover, crop what sticks out */
    FB_FIT_STRETCH,       /* scale to the frame, ignoring aspect */
    FB_FIT_COUNT
} FBFit;

typedef struct FBFormat {
    int      width, height;       /* 0 = size of the first clip's source */
    int      fps_num, fps_den;    /* 0 = rate of the first clip's source */
    FBFit    fit;                 /* default for clips; DEFAULT means FIT */
    uint32_t pad_color;           /* 0xRRGGBB for the bars */
} FBFormat;

typedef struct FBParam {
    char   name[FB_NAME_LEN];
    double value;
} FBParam;

typedef struct FBEffect {
    char    module[FB_NAME_LEN];   /* module name, e.g. "fade" */
    int     num_params;
    FBParam params[FB_MAX_PARAMS];
} FBEffect;

typedef enum {
    FB_TRACK_VIDEO = 0,   /* picture, and the clips' own sound */
    FB_TRACK_AUDIO        /* sound only */
} FBTrackKind;

typedef struct FBTrack {
    FBTrackKind kind;
    int         muted;     /* its sound is left out */
    int         hidden;    /* its picture is left out (picture tracks) */
} FBTrack;

typedef struct FBClip {
    int      source;               /* index into FBTimeline.sources */
    int      track;                /* index into FBTimeline.tracks */
    fb_ms_t  start;                /* where it starts on the timeline */
    fb_ms_t  in;
    fb_ms_t  out;
    double   volume;               /* 1.0 = as recorded */
    FBFit    fit;                  /* FB_FIT_DEFAULT = timeline's */
    int      num_effects;
    FBEffect effects[FB_MAX_EFFECTS];
} FBClip;

typedef struct FBTimeline {
    int      num_sources;
    FBSource sources[FB_MAX_SOURCES];
    int      num_tracks;
    FBTrack  tracks[FB_MAX_TRACKS];
    int      num_clips;
    FBClip   clips[FB_MAX_CLIPS];
    FBFormat format;
} FBTimeline;

void    FB_TimelineInit(FBTimeline *tl);

/* Returns the new source index, or -1 if full. */
int     FB_TimelineAddSource(FBTimeline *tl, const char *path, fb_ms_t duration,
                             int has_video, int has_audio);

/* ---- Tracks ---- */

/* Adds an empty track on top. Returns its index, or -1 if full. */
int     FB_TimelineAddTrack(FBTimeline *tl, FBTrackKind kind);

/* Removes a track and its clips; the tracks above move down one. */
int     FB_TimelineRemoveTrack(FBTimeline *tl, int track);

/* Where the last clip on a track ends (0 for an empty track). */
fb_ms_t FB_TimelineTrackEnd(const FBTimeline *tl, int track);

/* The kind of track a source belongs on: picture if it has video. */
FBTrackKind FB_SourceTrackKind(const FBSource *src);

/* Names for scripts and project files: "video", "audio". FromName is
 * case-insensitive and returns -1 if unknown. */
const char *FB_TrackKindName(FBTrackKind kind);
int     FB_TrackKindFromName(const char *name);

/* ---- Clips ---- */

/* Puts a clip on `track` at `start`. Fails (-1) if the clip would overlap
 * another clip on the track. Returns the clip's index; indices of clips
 * after it move up one. */
int     FB_TimelineAddClip(FBTimeline *tl, int track, int source,
                           fb_ms_t in, fb_ms_t out, fb_ms_t start);

/* Puts a clip right after the last one on `track`. `track` may be
 * tl->num_tracks: then a new track is added for it, of the kind that
 * suits the source. Returns the clip's index or -1. */
int     FB_TimelineAppendToTrack(FBTimeline *tl, int track, int source,
                                 fb_ms_t in, fb_ms_t out);

/* Shorthand for FB_TimelineAppendToTrack(tl, 0, ...): appends to the
 * bottom track, adding it if there are no tracks yet. */
int     FB_TimelineAppendClip(FBTimeline *tl, int source, fb_ms_t in, fb_ms_t out);

/* Splits the clip that `at` falls inside on `track`, or on every track if
 * `track` is -1. Returns the index of the (last) new right-hand clip, or
 * -1 if `at` isn't inside any clip. */
int     FB_TimelineSplit(FBTimeline *tl, int track, fb_ms_t at);

/* Removes a clip. The clips after it on its track move up to close the
 * gap. */
int     FB_TimelineRemoveClip(FBTimeline *tl, int clip);

/* Changes a clip's in and out points. The clips after it on its track move
 * to follow its new end. `out` can't go past the source's end when its
 * duration is known. */
int     FB_TimelineTrimClip(FBTimeline *tl, int clip, fb_ms_t in, fb_ms_t out);

/* Where a clip ends on the timeline. */
fb_ms_t FB_ClipEnd(const FBClip *clip);

/* The picture seen at time t: the clip on the highest picture track that
 * isn't hidden, or -1. When source_t isn't NULL it gets the matching time
 * in that clip's source. */
int     FB_TimelineClipAt(const FBTimeline *tl, fb_ms_t t, fb_ms_t *source_t);

/* Adds an effect to a clip. Returns pointer to it so params can be set. */
FBEffect *FB_ClipAddEffect(FBClip *clip, const char *module);
int     FB_EffectSetParam(FBEffect *fx, const char *name, double value);
double  FB_EffectGetParam(const FBEffect *fx, const char *name, double def);

/* Where the last clip on any track ends. */
fb_ms_t FB_TimelineDuration(const FBTimeline *tl);

/* ---- Output format and fitting ---- */

/* Sets the video info of a source (from probing). */
void    FB_SourceSetVideo(FBSource *src, int width, int height,
                          int sar_num, int sar_den, int fps_num, int fps_den);

/* The frame size the timeline renders at: the format's size, else the source
 * of the first clip on the lowest picture track ("the first clip" below;
 * for the frame rate, the first one that isn't a still picture),
 * shown with square pixels, rounded to even numbers. Returns 0
 * and sets 0x0 if it isn't known (sources not probed). */
int     FB_TimelineFrameSize(const FBTimeline *tl, int *width, int *height);

/* The frame rate: the format's, else the first clip's source. 0 if unknown. */
int     FB_TimelineFrameRate(const FBTimeline *tl, int *num, int *den);

/* The fit mode that applies to a clip: its own, else the timeline's, else
 * FB_FIT_FIT. Never returns FB_FIT_DEFAULT. */
FBFit   FB_ClipFit(const FBTimeline *tl, const FBClip *clip);

/* What fitting a clip into a w x h frame does to it, for display. */
typedef enum {
    FB_FITS_NONE = 0,     /* same shape and size, or unknown: untouched */
    FB_FITS_SCALE,        /* same aspect, other size: just scaled */
    FB_FITS_LETTERBOX,    /* bars above and below */
    FB_FITS_PILLARBOX,    /* bars left and right */
    FB_FITS_CROP,         /* edges cut off */
    FB_FITS_STRETCH       /* distorted */
} FBFitResult;

FBFitResult FB_ClipFitResult(const FBTimeline *tl, const FBClip *clip,
                             int width, int height);

/* Names for scripts and job files: "default", "fit", "fill", "stretch".
 * FromName is case-insensitive and returns -1 if unknown. */
const char *FB_FitName(FBFit fit);
int     FB_FitFromName(const char *name);

#endif
