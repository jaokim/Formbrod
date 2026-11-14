/*
 * job.h - a render job, the unit of work handed to a backend.
 *
 * The editor turns the timeline into an FBJob: the list of input files, one
 * ffmpeg filtergraph, and output settings. Local and remote backends receive
 * exactly the same job. Jobs serialise to a simple line-based text format so
 * they can be saved, sent over a socket, or inspected by hand.
 */
#ifndef FORMBROD_JOB_H
#define FORMBROD_JOB_H

#include "timeline.h"

#define FB_JOB_VERSION       1
#define FB_FILTERGRAPH_LEN   16384

typedef enum {
    FB_QUALITY_PREVIEW = 0,   /* low-res, fast */
    FB_QUALITY_FINAL   = 1
} FBQuality;

typedef struct FBOutput {
    char      path[FB_PATH_LEN];
    char      vcodec[FB_NAME_LEN];   /* e.g. "libx264", "mpeg4" */
    char      acodec[FB_NAME_LEN];   /* e.g. "aac", "mp2" */
    int       width, height;         /* 0 = keep source size */
    FBQuality quality;
} FBOutput;

typedef struct FBJob {
    int      version;
    int      num_inputs;
    char     inputs[FB_MAX_SOURCES][FB_PATH_LEN];
    char     filtergraph[FB_FILTERGRAPH_LEN];
    char     video_out[FB_NAME_LEN];   /* filtergraph label, e.g. "outv" */
    char     audio_out[FB_NAME_LEN];   /* "" if no audio */
    FBOutput output;
    fb_ms_t  length;                   /* result's length, 0 if unknown */
} FBJob;

typedef enum {
    FB_JOB_OK = 0,
    FB_JOB_EMPTY,            /* no clips */
    FB_JOB_BAD_SOURCE,       /* clip refers to a missing source */
    FB_JOB_UNKNOWN_MODULE,   /* effect module not registered */
    FB_JOB_NOT_EXPRESSIBLE,  /* module can't be expressed as ffmpeg filters */
    FB_JOB_OVERFLOW,         /* filtergraph too long */
    FB_JOB_NEED_SIZE         /* gaps or tracks, but frame size unknown */
} FBJobError;

/* Builds a job from the timeline. Effects are turned into filters by their
 * modules (see module.h). This runs on the Amiga, so module logic lives in
 * one place and render servers stay simple job runners. */
FBJobError FB_JobBuild(FBJob *job, const FBTimeline *tl, const FBOutput *out);

const char *FB_JobErrorString(FBJobError err);

/* Serialise to / parse from text. Write returns bytes written (excluding
 * NUL) or -1 if buf is too small. Parse returns 0 on success. */
int FB_JobWrite(const FBJob *job, char *buf, size_t len);
int FB_JobParse(FBJob *job, const char *text);

/* Builds an equivalent ffmpeg command line, for logging and for render
 * servers that shell out to the stock ffmpeg CLI. */
int FB_JobToCommandLine(const FBJob *job, char *buf, size_t len);

/* The output length, for progress bars: the job's length if it has one
 * (FB_JobBuild() sets it), else the sum of every video "trim" in the
 * filtergraph, which is what a single concat plays. Returns 0 if there are
 * no video trims or one of them has no end. */
fb_ms_t FB_JobEstimateLength(const FBJob *job);

#endif
