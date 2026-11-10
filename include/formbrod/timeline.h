/*
 * timeline.h - the editing model.
 *
 * A project has a list of sources (video files) and a single track of clips.
 * Each clip is a [in, out) window into one source, plus an optional chain of
 * effects. This file is plain C99 with no AmigaOS dependencies so it can be
 * unit-tested on any host.
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
#define FB_MAX_CLIPS     256
#define FB_MAX_EFFECTS   8
#define FB_MAX_PARAMS    8
#define FB_PATH_LEN      512
#define FB_NAME_LEN      32

typedef int64_t fb_ms_t; /* time in milliseconds */

typedef struct FBSource {
    char    path[FB_PATH_LEN];
    fb_ms_t duration;       /* 0 if not probed yet */
    int     has_video;
    int     has_audio;
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

typedef struct FBClip {
    int      source;               /* index into FBTimeline.sources */
    fb_ms_t  in;
    fb_ms_t  out;
    FBFit    fit;                  /* FB_FIT_DEFAULT = timeline's */
    int      num_effects;
    FBEffect effects[FB_MAX_EFFECTS];
} FBClip;

typedef struct FBTimeline {
    int      num_sources;
    FBSource sources[FB_MAX_SOURCES];
    int      num_clips;
    FBClip   clips[FB_MAX_CLIPS];
    FBFormat format;
} FBTimeline;

void    FB_TimelineInit(FBTimeline *tl);

/* Returns the new source index, or -1 if full. */
int     FB_TimelineAddSource(FBTimeline *tl, const char *path, fb_ms_t duration,
                             int has_video, int has_audio);

/* Appends a clip at the end of the track. Returns clip index or -1. */
int     FB_TimelineAppendClip(FBTimeline *tl, int source, fb_ms_t in, fb_ms_t out);

/* Splits clip `clip` at time `at` (relative to the timeline). Returns the
 * index of the new right-hand clip, or -1 if `at` is not inside the clip. */
int     FB_TimelineSplit(FBTimeline *tl, fb_ms_t at);

int     FB_TimelineRemoveClip(FBTimeline *tl, int clip);

/* Adds an effect to a clip. Returns pointer to it so params can be set. */
FBEffect *FB_ClipAddEffect(FBClip *clip, const char *module);
int     FB_EffectSetParam(FBEffect *fx, const char *name, double value);
double  FB_EffectGetParam(const FBEffect *fx, const char *name, double def);

fb_ms_t FB_TimelineDuration(const FBTimeline *tl);

/* ---- Output format and fitting ---- */

/* Sets the video info of a source (from probing). */
void    FB_SourceSetVideo(FBSource *src, int width, int height,
                          int sar_num, int sar_den, int fps_num, int fps_den);

/* The frame size the timeline renders at: the format's size, else the first
 * clip's source shown with square pixels, rounded to even numbers. Returns 0
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
