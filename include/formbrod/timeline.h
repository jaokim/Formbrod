/*
 * timeline.h - the editing model.
 *
 * A project has a list of sources (video files) and a single track of clips.
 * Each clip is a [in, out) window into one source, plus an optional chain of
 * effects. This file is plain C99 with no AmigaOS dependencies so it can be
 * unit-tested on any host.
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
} FBSource;

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
    int      num_effects;
    FBEffect effects[FB_MAX_EFFECTS];
} FBClip;

typedef struct FBTimeline {
    int      num_sources;
    FBSource sources[FB_MAX_SOURCES];
    int      num_clips;
    FBClip   clips[FB_MAX_CLIPS];
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

#endif
