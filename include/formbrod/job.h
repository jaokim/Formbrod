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
/* How wide previews are (FB_QUALITY_PREVIEW with no width given). */
#define FB_PREVIEW_WIDTH     320
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
    /* 0 = from `quality` (a preview's or a final's usual): */
    int       vquality;              /* FBVideoQuality */
    int       abitrate;              /* kbit/s */
    int       arate;                 /* Hz */
} FBOutput;

/* What a picture quality means to the encoders: a constant quantiser
 * (MPEG-4 Part 2, MPEG-1/2, MJPEG), a crf (x264, x265) or, for others, a
 * bit rate of width x height x bits_per_pixel bit/s. */
typedef struct FBVideoLevel {
    int qscale, crf, bits_per_pixel;
} FBVideoLevel;

/* The level for an output: its vquality, else high for a final render
 * and the preview's own (quick and small) for a preview. */
FBVideoLevel FB_VideoLevel(const FBOutput *out);
/* Its sound bit rate in bit/s: abitrate, else 160 kbit/s for a final
 * render and 96 for a preview. */
long FB_AudioBitRate(const FBOutput *out);
/* Its sample rate: arate, else 48 kHz (44.1 for a preview). */
int  FB_AudioRate(const FBOutput *out);
/* Whether an encoder is one of those that take a quantiser, a crf, or
 * neither (a bit rate). Used by the command line and by the backend. */
int  FB_EncoderTakesQScale(const char *name);
int  FB_EncoderTakesCRF(const char *name);
/* Whether a sound encoder has no bit rate to set (PCM, FLAC). */
int  FB_EncoderLossless(const char *name);

typedef struct FBJob {
    int      version;
    int      num_inputs;
    char     inputs[FB_MAX_SOURCES][FB_PATH_LEN];
    char     filtergraph[FB_FILTERGRAPH_LEN];
    char     video_out[FB_NAME_LEN];   /* filtergraph label, e.g. "outv" */
    char     audio_out[FB_NAME_LEN];   /* "" if no audio */
    FBOutput output;
    fb_ms_t  length;                   /* result's length, 0 if unknown */
    /* Where each input's first used frame is (ms in the file, 0 = from
     * the start): nothing before it is used, so a backend may jump there
     * (to the keyframe before it) instead of decoding up to it. Times in
     * the filtergraph stay relative to the file's start either way. */
    fb_ms_t  seek[FB_MAX_SOURCES];
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
