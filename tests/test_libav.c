/*
 * End-to-end test of the local libav backend on the host.
 *
 *   make -f Makefile.host test-libav
 *
 * tests/test_libav.sh makes small input files with the ffmpeg CLI and runs
 * this program on them. It builds real jobs with FB_JobBuild(), renders
 * them through FB_CreateLocalBackend() exactly as the GUI does (Submit,
 * then Progress until done), and checks the output with libavformat.
 *
 * Inputs in <dir>:
 *   a.mp4  5 s, 320x240 25 fps, stereo 48 kHz
 *   b.mkv  4 s, 640x360 30 fps, mono 44.1 kHz
 *   c.mp4  3 s, 320x240, no audio
 */
#include <stdio.h>
#include <string.h>
#include <libavformat/avformat.h>
#include "formbrod/timeline.h"
#include "formbrod/job.h"
#include "formbrod/module.h"
#include "formbrod/backend.h"
#include "formbrod/cache.h"
#include "formbrod/player.h"

static int failures;
static char dir[400];

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

typedef struct Probe {
    double duration;   /* seconds, from the longest stream */
    int    video, audio, width, height;
} Probe;

static int probe(const char *path, Probe *pr)
{
    AVFormatContext *fmt = NULL;
    unsigned i;

    memset(pr, 0, sizeof(*pr));
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0)
        return -1;
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        return -1;
    }
    if (fmt->duration != AV_NOPTS_VALUE)
        pr->duration = fmt->duration / (double)AV_TIME_BASE;
    for (i = 0; i < fmt->nb_streams; i++) {
        AVCodecParameters *c = fmt->streams[i]->codecpar;

        if (c->codec_type == AVMEDIA_TYPE_VIDEO) {
            pr->video++;
            pr->width = c->width;
            pr->height = c->height;
        } else if (c->codec_type == AVMEDIA_TYPE_AUDIO) {
            pr->audio++;
        }
    }
    avformat_close_input(&fmt);
    return 0;
}

static void in_path(char *buf, const char *name)
{
    snprintf(buf, FB_PATH_LEN, "%s/%s", dir, name);
}

static void set_output(FBOutput *out, const char *name, const char *vcodec,
                       const char *acodec, int w, int h, FBQuality q)
{
    memset(out, 0, sizeof(*out));
    in_path(out->path, name);
    strcpy(out->vcodec, vcodec);
    strcpy(out->acodec, acodec);
    out->width = w;
    out->height = h;
    out->quality = q;
    remove(out->path);
}

/* Renders like the GUI loop does. Returns the final state. */
static FBRenderState render(const FBJob *job, int *calls, FBResult *res)
{
    FBBackend *be = FB_CreateLocalBackend();
    FBProgress pr;
    fb_ms_t last = -1;

    *calls = 0;
    if (be->Submit(be, job) != 0) {
        *res = be->Result(be);
        be->Dispose(be);
        return res->state;
    }
    do {
        pr = be->Progress(be);
        (*calls)++;
        if (pr.position < last) {
            printf("FAIL: progress went backwards\n");
            failures++;
        }
        last = pr.position;
    } while (pr.state == FB_RENDER_RUNNING && *calls < 100000);
    *res = be->Result(be);
    printf("  %s: %s after %d calls, %lld/%lld ms\n", job->output.path,
           res->state == FB_RENDER_DONE ? "done" : res->message, *calls,
           (long long)pr.position, (long long)pr.total);
    be->Dispose(be);
    return res->state;
}

static int near(double a, double b)
{
    return a > b - 0.15 && a < b + 0.15;
}

static void test_join(void)
{
    FBTimeline tl;
    FBOutput out;
    FBJob job;
    FBResult res;
    FBEffect *fx;
    Probe pr;
    char a[FB_PATH_LEN], b[FB_PATH_LEN];
    int calls;

    printf("join two files with fades\n");
    in_path(a, "a.mp4");
    in_path(b, "b.mkv");
    FB_TimelineInit(&tl);
    FB_TimelineAddSource(&tl, a, 5000, 1, 1);
    FB_TimelineAddSource(&tl, b, 4000, 1, 1);
    FB_TimelineAppendClip(&tl, 0, 1000, 3000);
    FB_TimelineAppendClip(&tl, 1, 500, 2500);
    fx = FB_ClipAddEffect(&tl.clips[0], "fade");
    FB_EffectSetParam(fx, "in", 0.5);
    fx = FB_ClipAddEffect(&tl.clips[1], "fade");
    FB_EffectSetParam(fx, "in", 0);
    FB_EffectSetParam(fx, "out", 0.5);
    set_output(&out, "out_join.mp4", "mpeg4", "aac", 640, 360,
               FB_QUALITY_FINAL);
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);

    CHECK(render(&job, &calls, &res) == FB_RENDER_DONE);
    CHECK(strcmp(res.message, out.path) == 0);
    CHECK(calls > 2);   /* the work is spread over several calls */
    CHECK(probe(out.path, &pr) == 0);
    CHECK(near(pr.duration, 4.0));
    CHECK(pr.video == 1 && pr.audio == 1);
    CHECK(pr.width == 640 && pr.height == 360);
}

/* The sizes, rates and audio of a.mp4, b.mkv and c.mp4 differ; with probed
 * sources and no output size the timeline's defaults must make them fit:
 * the frame of the first clip, b letterboxed, silence under c. */
static void test_mixed_sizes(void)
{
    FBTimeline tl;
    FBOutput out;
    FBJob job;
    FBResult res;
    Probe pr;
    char a[FB_PATH_LEN], b[FB_PATH_LEN], c[FB_PATH_LEN];
    int calls;

    printf("mixed sizes, rates and audio, default fit\n");
    in_path(a, "a.mp4");
    in_path(b, "b.mkv");
    in_path(c, "c.mp4");
    FB_TimelineInit(&tl);
    FB_TimelineAddSource(&tl, a, 5000, 1, 1);
    FB_SourceSetVideo(&tl.sources[0], 320, 240, 1, 1, 25, 1);
    FB_TimelineAddSource(&tl, b, 4000, 1, 1);
    FB_SourceSetVideo(&tl.sources[1], 640, 360, 1, 1, 30, 1);
    FB_TimelineAddSource(&tl, c, 3000, 1, 0);
    FB_SourceSetVideo(&tl.sources[2], 320, 240, 1, 1, 25, 1);
    FB_TimelineAppendClip(&tl, 0, 0, 2000);
    FB_TimelineAppendClip(&tl, 1, 0, 2000);
    FB_TimelineAppendClip(&tl, 2, 0, 1500);
    tl.clips[1].fit = FB_FIT_FILL;   /* per-clip override */
    set_output(&out, "out_mixed.mp4", "mpeg4", "aac", 0, 0,
               FB_QUALITY_FINAL);
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    CHECK(strstr(job.filtergraph, "anullsrc") != NULL);

    CHECK(render(&job, &calls, &res) == FB_RENDER_DONE);
    CHECK(probe(out.path, &pr) == 0);
    CHECK(near(pr.duration, 5.5));
    CHECK(pr.video == 1 && pr.audio == 1);
    CHECK(pr.width == 320 && pr.height == 240);
}

/* Two clips of different size and rate with a wipe between them, and an
 * upper track crossfading with transparency around it. */
static void test_crossfade(void)
{
    FBTimeline tl;
    FBOutput out;
    FBJob job;
    FBResult res;
    Probe pr;
    char a[FB_PATH_LEN], b[FB_PATH_LEN], c[FB_PATH_LEN];
    int calls;

    printf("crossfades, on the bottom track and an upper one\n");
    in_path(a, "a.mp4");
    in_path(b, "b.mkv");
    in_path(c, "c.mp4");
    FB_TimelineInit(&tl);
    FB_TimelineAddSource(&tl, a, 5000, 1, 1);
    FB_SourceSetVideo(&tl.sources[0], 320, 240, 1, 1, 25, 1);
    FB_TimelineAddSource(&tl, b, 4000, 1, 1);
    FB_SourceSetVideo(&tl.sources[1], 640, 360, 1, 1, 30, 1);
    FB_TimelineAddSource(&tl, c, 3000, 1, 0);
    FB_SourceSetVideo(&tl.sources[2], 320, 240, 1, 1, 25, 1);
    FB_TimelineAppendToTrack(&tl, 0, 0, 0, 2000);
    FB_TimelineAppendToTrack(&tl, 0, 1, 0, 2000);
    CHECK(FB_TimelineSetCrossfade(&tl, 1, 1000, "wipeleft") == 1000);
    FB_TimelineAddTrack(&tl, FB_TRACK_VIDEO);
    FB_TimelineAddClip(&tl, 1, 2, 0, 1000, 500);
    FB_TimelineAppendToTrack(&tl, 1, 2, 1000, 2000);
    CHECK(FB_TimelineSetCrossfade(&tl, 3, 500, NULL) == 500);
    set_output(&out, "out_xfade.mp4", "mpeg4", "aac", 0, 0,
               FB_QUALITY_FINAL);
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    CHECK(strstr(job.filtergraph, "xfade=transition=wipeleft") != NULL);
    CHECK(strstr(job.filtergraph, "acrossfade=d=1.000") != NULL);

    CHECK(render(&job, &calls, &res) == FB_RENDER_DONE);
    CHECK(probe(out.path, &pr) == 0);
    CHECK(near(pr.duration, 3.0));
    CHECK(pr.video == 1 && pr.audio == 1);
    CHECK(pr.width == 320 && pr.height == 240);
}

/* Two clips from one file, in reverse order: one stream feeds two pads. */
static void test_same_source_reversed(void)
{
    FBTimeline tl;
    FBOutput out;
    FBJob job;
    FBResult res;
    Probe pr;
    char a[FB_PATH_LEN];
    int calls;

    printf("two clips from one file, reversed, preview\n");
    in_path(a, "a.mp4");
    FB_TimelineInit(&tl);
    FB_TimelineAddSource(&tl, a, 5000, 1, 1);
    FB_TimelineAppendClip(&tl, 0, 3000, 4500);
    FB_TimelineAppendClip(&tl, 0, 500, 1500);
    set_output(&out, "out_reversed.mp4", "mpeg4", "aac", 0, 0,
               FB_QUALITY_PREVIEW);
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    CHECK(job.num_inputs == 1);

    CHECK(render(&job, &calls, &res) == FB_RENDER_DONE);
    CHECK(probe(out.path, &pr) == 0);
    CHECK(near(pr.duration, 2.5));
    CHECK(pr.video == 1 && pr.audio == 1);
    CHECK(pr.width == 320);
}

/* No audio, an unknown codec name (falls back), and a different
 * container. */
static void test_video_only_fallback(void)
{
    FBTimeline tl;
    FBOutput out;
    FBJob job;
    FBResult res;
    Probe pr;
    char c[FB_PATH_LEN];
    int calls;

    printf("video only, unknown codec name, mkv\n");
    in_path(c, "c.mp4");
    FB_TimelineInit(&tl);
    FB_TimelineAddSource(&tl, c, 3000, 1, 0);
    FB_TimelineAppendClip(&tl, 0, 0, 3000);
    set_output(&out, "out_video_only.mkv", "nosuchcodec", "aac", 0, 0,
               FB_QUALITY_FINAL);
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    CHECK(job.audio_out[0] == '\0');

    CHECK(render(&job, &calls, &res) == FB_RENDER_DONE);
    CHECK(probe(out.path, &pr) == 0);
    CHECK(near(pr.duration, 3.0));
    CHECK(pr.video == 1 && pr.audio == 0);
}

/* A codec that takes crf, if this ffmpeg has it (else the fallback). */
static void test_h264(void)
{
    FBTimeline tl;
    FBOutput out;
    FBJob job;
    FBResult res;
    Probe pr;
    char a[FB_PATH_LEN], b[FB_PATH_LEN];
    int calls;

    printf("libx264 + mp2 into mkv\n");
    in_path(a, "a.mp4");
    in_path(b, "b.mkv");
    FB_TimelineInit(&tl);
    FB_TimelineAddSource(&tl, b, 4000, 1, 1);
    FB_TimelineAddSource(&tl, a, 5000, 1, 1);
    FB_TimelineAppendClip(&tl, 0, 0, 4000);
    FB_TimelineAppendClip(&tl, 1, 0, 5000);
    /* These sources aren't probed (no FB_SourceSetVideo), so the timeline
     * doesn't know their size; an output size makes them fit anyway. */
    set_output(&out, "out_h264.mkv", "libx264", "mp2", 320, 180,
               FB_QUALITY_PREVIEW);
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);

    CHECK(render(&job, &calls, &res) == FB_RENDER_DONE);
    CHECK(probe(out.path, &pr) == 0);
    CHECK(near(pr.duration, 9.0));
    CHECK(pr.video == 1 && pr.audio == 1);
}

static void test_cancel(void)
{
    FBTimeline tl;
    FBOutput out;
    FBJob job;
    FBBackend *be;
    FBProgress pr;
    FILE *f;
    char a[FB_PATH_LEN];
    int i;

    printf("cancel\n");
    in_path(a, "a.mp4");
    FB_TimelineInit(&tl);
    FB_TimelineAddSource(&tl, a, 5000, 1, 1);
    FB_TimelineAppendClip(&tl, 0, 0, 5000);
    set_output(&out, "out_cancel.mp4", "mpeg4", "aac", 0, 0,
               FB_QUALITY_FINAL);
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);

    be = FB_CreateLocalBackend();
    CHECK(be->Submit(be, &job) == 0);
    CHECK(be->Submit(be, &job) != 0);   /* already running */
    for (i = 0; i < 3; i++)
        pr = be->Progress(be);
    CHECK(pr.state == FB_RENDER_RUNNING);
    CHECK(pr.total == 5000);
    f = fopen(out.path, "rb");
    CHECK(f != NULL);   /* it is being written */
    if (f)
        fclose(f);
    be->Cancel(be);
    CHECK(be->Progress(be).state == FB_RENDER_CANCELLED);
    CHECK(be->Result(be).state == FB_RENDER_CANCELLED);
    f = fopen(out.path, "rb");
    CHECK(f == NULL);   /* partial file deleted */
    if (f)
        fclose(f);

    /* The backend can be reused after a cancel. */
    CHECK(be->Submit(be, &job) == 0);
    be->Dispose(be);   /* disposing while running cancels too */
    f = fopen(out.path, "rb");
    CHECK(f == NULL);
    if (f)
        fclose(f);
}

static void test_errors(void)
{
    FBTimeline tl;
    FBOutput out;
    FBJob job;
    FBResult res;
    FILE *f;
    char a[FB_PATH_LEN];
    int calls;

    printf("errors\n");
    FB_TimelineInit(&tl);
    FB_TimelineAddSource(&tl, "/nonexistent/x.mp4", 5000, 1, 1);
    FB_TimelineAppendClip(&tl, 0, 0, 1000);
    set_output(&out, "out_err.mp4", "mpeg4", "aac", 0, 0, FB_QUALITY_FINAL);
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    CHECK(render(&job, &calls, &res) == FB_RENDER_FAILED);
    printf("  missing input: %s\n", res.message);
    CHECK(strstr(res.message, "/nonexistent/x.mp4") != NULL);

    /* A hand-written job with a filter that doesn't exist. */
    in_path(a, "a.mp4");
    FB_TimelineInit(&tl);
    FB_TimelineAddSource(&tl, a, 5000, 1, 1);
    FB_TimelineAppendClip(&tl, 0, 0, 1000);
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    strcpy(job.filtergraph, "[0:v]nosuchfilter[outv]");
    job.audio_out[0] = '\0';
    CHECK(render(&job, &calls, &res) == FB_RENDER_FAILED);
    printf("  bad filter: %s\n", res.message);

    /* A pad that names an input the job doesn't have. */
    strcpy(job.filtergraph, "[3:v]null[outv]");
    CHECK(render(&job, &calls, &res) == FB_RENDER_FAILED);
    printf("  bad pad: %s\n", res.message);
    CHECK(strstr(res.message, "[3:v]") != NULL);

    /* Audio asked for from a file without any. */
    in_path(job.inputs[0], "c.mp4");
    strcpy(job.filtergraph, "[0:v]null[outv];[0:a]anull[outa]");
    strcpy(job.audio_out, "outa");
    CHECK(render(&job, &calls, &res) == FB_RENDER_FAILED);
    printf("  no audio: %s\n", res.message);

    f = fopen(out.path, "rb");
    CHECK(f == NULL);   /* nothing left behind */
    if (f)
        fclose(f);
}


/* A preview copy: MJPEG (full-range colour) and PCM, smaller. */
static void test_proxy(void)
{
    static FBCache c;
    static FBJob job;
    FBSource src;
    FBResult res;
    Probe pr;
    int calls, t;

    printf("proxy copy, MJPEG + PCM\n");
    memset(&src, 0, sizeof(src));
    in_path(src.path, "a.mp4");
    strcpy(src.key, "0000000000proxy1");
    src.duration = 5000;
    src.has_video = 1;
    src.has_audio = 1;
    FB_SourceSetVideo(&src, 320, 240, 1, 1, 25, 1);
    FB_CacheInit(&c, dir, 160);
    {
        char old[FB_PATH_LEN];   /* from an earlier run */

        FB_CachePath(&c, src.key, FB_CACHE_PROXY, 0, old, sizeof(old));
        remove(old);
    }
    CHECK(FB_CacheRequest(&c, &src) == FB_CACHE_QUEUED);
    t = FB_CacheNextJob(&c, &job);
    CHECK(t == 0);
    CHECK(render(&job, &calls, &res) == FB_RENDER_DONE);
    CHECK(FB_CacheFinish(&c, 1) == FB_CACHE_READY);
    {
        char path[FB_PATH_LEN];

        CHECK(FB_CacheFind(&c, src.key, FB_CACHE_PROXY, path, sizeof(path)));
        CHECK(probe(path, &pr) == 0);
        CHECK(near(pr.duration, 5.0));
        CHECK(pr.width == 160 && pr.height == 120);
        CHECK(pr.video == 1 && pr.audio == 1);
    }
}

/* A window from the middle: inputs jump to their first frame used. */
static void test_window_seek(void)
{
    static FBTimeline tl, w;
    static FBJob job;
    FBOutput out;
    FBResult res;
    Probe pr;
    char a[FB_PATH_LEN], b[FB_PATH_LEN];
    int calls;

    printf("a window from the middle, with jumps\n");
    in_path(a, "a.mp4");
    in_path(b, "b.mkv");
    FB_TimelineInit(&tl);
    FB_TimelineAddSource(&tl, a, 5000, 1, 1);
    FB_TimelineAddSource(&tl, b, 4000, 1, 1);
    FB_SourceSetVideo(&tl.sources[0], 320, 240, 1, 1, 25, 1);
    FB_SourceSetVideo(&tl.sources[1], 640, 360, 1, 1, 30, 1);
    FB_TimelineAppendClip(&tl, 0, 0, 5000);
    FB_TimelineAppendClip(&tl, 1, 0, 4000);
    CHECK(FB_TimelineWindow(&w, &tl, 3500, 3000) == 2);
    set_output(&out, "out_window.mp4", "mpeg4", "aac", 0, 0,
               FB_QUALITY_PREVIEW);
    CHECK(FB_JobBuild(&job, &w, &out) == FB_JOB_OK);
    CHECK(job.seek[0] == 3500 && job.seek[1] == 0);
    CHECK(render(&job, &calls, &res) == FB_RENDER_DONE);
    CHECK(probe(out.path, &pr) == 0);
    CHECK(near(pr.duration, 3.0));
}

/* Playing to a sink that takes only a few frames at a time. */
typedef struct TestSink {
    int  room;            /* frames it takes before it's full */
    int  video, audio_frames, refused;
    int  width, height, stride;
    fb_ms_t last_v, last_a;
    int  backwards;
} TestSink;

static int sink_video(void *ud, const unsigned char *argb, int w, int h,
                      int stride, fb_ms_t pts)
{
    TestSink *t = ud;

    if (t->room <= 0) {
        t->refused++;
        return 1;
    }
    t->room--;
    if (pts < t->last_v)
        t->backwards++;
    t->last_v = pts;
    t->video++;
    t->width = w;
    t->height = h;
    t->stride = stride;
    /* Touch the first and last pixels, for the sanitizers. */
    (void)(argb[0] + argb[(h - 1) * stride + w * 4 - 1]);
    return 0;
}

static int sink_audio(void *ud, const short *s, int frames, fb_ms_t pts)
{
    TestSink *t = ud;

    if (pts < t->last_a)
        t->backwards++;
    t->last_a = pts;
    t->audio_frames += frames;
    (void)(s[0] + s[frames * 2 - 1]);
    return 0;
}

static void test_play(void)
{
    static FBTimeline tl, w;
    static FBJob job;
    FBBackend *be;
    FBFrameSink sink;
    FBOutput out;
    FBProgress pr;
    TestSink t;
    char a[FB_PATH_LEN];
    int calls = 0;

    printf("play to a sink, from 1 s\n");
    in_path(a, "a.mp4");
    FB_TimelineInit(&tl);
    FB_TimelineAddSource(&tl, a, 5000, 1, 1);
    FB_SourceSetVideo(&tl.sources[0], 320, 240, 1, 1, 25, 1);
    FB_TimelineAppendClip(&tl, 0, 0, 5000);
    CHECK(FB_TimelineWindow(&w, &tl, 1000, 4000) == 1);
    memset(&out, 0, sizeof(out));
    out.quality = FB_QUALITY_PREVIEW;
    CHECK(FB_JobBuild(&job, &w, &out) == FB_JOB_OK);

    memset(&t, 0, sizeof(t));
    sink.Video = sink_video;
    sink.Audio = sink_audio;
    sink.ud = &t;
    be = FB_CreateLocalBackend();
    CHECK(be->Play != NULL);
    CHECK(be->Play(be, &job, &sink) == 0);
    do {
        t.room = 3;            /* "playback" takes 3 frames per call */
        pr = be->Progress(be);
        calls++;
    } while (pr.state == FB_RENDER_RUNNING && calls < 100000);
    printf("  %d calls: %d frames %dx%d, %d audio frames, %d refused\n",
           calls, t.video, t.width, t.height, t.audio_frames, t.refused);
    CHECK(pr.state == FB_RENDER_DONE);
    CHECK(t.video >= 99 && t.video <= 101);          /* 4 s at 25 fps */
    CHECK(t.width == 320 && t.height == 240);
    CHECK(t.stride >= t.width * 4);
    CHECK(t.audio_frames > 4 * FB_PLAY_RATE - 2000 &&
          t.audio_frames < 4 * FB_PLAY_RATE + 2000);
    CHECK(t.refused > 0);                          /* it was paced */
    CHECK(t.backwards == 0);
    CHECK(t.last_v >= 3900 && t.last_v <= 4000);

    /* Cancelling mid-way. */
    memset(&t, 0, sizeof(t));
    t.room = 5;
    CHECK(be->Play(be, &job, &sink) == 0);
    be->Progress(be);
    be->Cancel(be);
    CHECK(be->Result(be).state == FB_RENDER_CANCELLED);
    be->Dispose(be);
}

/* The backend into the player, on a made-up clock: `step_ms` a turn,
 * the sound output taking that much sound each turn. 10 ms is a machine
 * that keeps up; 100 ms one that manages ten turns a second. */
static void test_play_player(int step_ms)
{
    static FBTimeline tl, w;
    static FBJob job;
    static FBPlayer pl;
    static short out[FB_PLAY_RATE / 10 * 2];
    long chunk = (long)FB_PLAY_RATE * step_ms / 1000;
    FBBackend *be = FB_CreateLocalBackend();
    FBFrameSink sink;
    FBOutput o;
    char a[FB_PATH_LEN], b[FB_PATH_LEN];
    long long now = 1000000;
    long real = 0, turns = 0;
    int ended = 0;

    printf("play into the player, %d ms a turn\n", step_ms);
    in_path(a, "a.mp4");
    in_path(b, "b.mkv");
    FB_TimelineInit(&tl);
    FB_TimelineAddSource(&tl, a, 5000, 1, 1);
    FB_TimelineAddSource(&tl, b, 4000, 1, 1);
    FB_SourceSetVideo(&tl.sources[0], 320, 240, 1, 1, 25, 1);
    FB_SourceSetVideo(&tl.sources[1], 640, 360, 1, 1, 30, 1);
    FB_TimelineAppendClip(&tl, 0, 0, 5000);
    FB_TimelineAppendClip(&tl, 1, 0, 4000);
    CHECK(FB_TimelineWindow(&w, &tl, 2000, 7000) == 2);   /* 2 s to the end */
    memset(&o, 0, sizeof(o));
    o.quality = FB_QUALITY_PREVIEW;
    CHECK(FB_JobBuild(&job, &w, &o) == FB_JOB_OK);
    CHECK(FB_PlayerInit(&pl, job.audio_out[0] != 0) == 0);
    sink = FB_PlayerSink(&pl);
    CHECK(be->Play(be, &job, &sink) == 0);

    while (!FB_PlayerFinished(&pl, now) && turns < 5000) {
        if (!ended) {
            FBProgress p = be->Progress(be);

            if (p.state != FB_RENDER_RUNNING) {
                CHECK(p.state == FB_RENDER_DONE);
                FB_PlayerSetEnded(&pl);
                ended = 1;
            }
        }
        if (!FB_PlayerStarted(&pl) && FB_PlayerReady(&pl))
            FB_PlayerStart(&pl, now);
        if (FB_PlayerStarted(&pl)) {
            FB_PlayerFrame(&pl, now);
            real += FB_PlayerTakeAudio(&pl, out, chunk);
        }
        now += step_ms * 1000LL;
        turns++;
    }
    printf("  %ld turns: %ld shown, %ld dropped, %ld ms of sound\n", turns,
           pl.num_shown, pl.num_dropped, real * 1000 / FB_PLAY_RATE);
    CHECK(FB_PlayerFinished(&pl, now));
    /* 7 s of preview at 25 fps (the first clip's rate). */
    CHECK(pl.num_shown + pl.num_dropped >= 174 &&
          pl.num_shown + pl.num_dropped <= 176);
    if (step_ms <= 40)
        CHECK(pl.num_dropped == 0);  /* a picture every turn or more */
    else
        CHECK(pl.num_dropped > 0 && pl.num_shown < 100);   /* skips */
    CHECK(real * 1000 / FB_PLAY_RATE > 6900);
    /* About 7 s of clock either way: it keeps time. */
    CHECK(turns * step_ms < 7600);
    FB_PlayerFree(&pl);
    be->Dispose(be);
}

/* Pictures from one file and the sound from another (music on its own
 * track): the sound must keep up with the pictures. */
static void test_play_separate_sound(void)
{
    static FBTimeline tl;
    static FBJob job;
    static FBPlayer pl;
    static short out[441 * 2];
    FBBackend *be = FB_CreateLocalBackend();
    FBFrameSink sink;
    FBOutput o;
    char c[FB_PATH_LEN], d[FB_PATH_LEN];
    long long now = 1000000;
    long real = 0, turns = 0;
    int ended = 0, tr;

    printf("play pictures from one file, sound from another\n");
    in_path(c, "c.mp4");
    in_path(d, "d.wav");
    FB_TimelineInit(&tl);
    FB_TimelineAddSource(&tl, c, 3000, 1, 0);
    FB_SourceSetVideo(&tl.sources[0], 320, 240, 1, 1, 25, 1);
    FB_TimelineAddSource(&tl, d, 6000, 0, 1);
    FB_TimelineAppendToTrack(&tl, 0, 0, 0, 3000);
    tr = FB_TimelineAddTrack(&tl, FB_TRACK_AUDIO);
    FB_TimelineAppendToTrack(&tl, tr, 1, 0, 3000);
    memset(&o, 0, sizeof(o));
    o.quality = FB_QUALITY_PREVIEW;
    CHECK(FB_JobBuild(&job, &tl, &o) == FB_JOB_OK);
    CHECK(job.audio_out[0] != 0);
    CHECK(FB_PlayerInit(&pl, 1) == 0);
    sink = FB_PlayerSink(&pl);
    CHECK(be->Play(be, &job, &sink) == 0);
    while (!FB_PlayerFinished(&pl, now) && turns < 5000) {
        if (!ended) {
            FBProgress p = be->Progress(be);

            if (p.state != FB_RENDER_RUNNING) {
                CHECK(p.state == FB_RENDER_DONE);
                FB_PlayerSetEnded(&pl);
                ended = 1;
            }
        }
        if (!FB_PlayerStarted(&pl) && FB_PlayerReady(&pl))
            FB_PlayerStart(&pl, now);
        if (FB_PlayerStarted(&pl)) {
            FB_PlayerFrame(&pl, now);
            real += FB_PlayerTakeAudio(&pl, out, 441);
        }
        now += 10000;
        turns++;
    }
    printf("  %ld turns: %ld shown, %ld dropped, %ld ms of sound\n", turns,
           pl.num_shown, pl.num_dropped, real * 1000 / FB_PLAY_RATE);
    CHECK(real * 1000 / FB_PLAY_RATE > 2900);
    FB_PlayerFree(&pl);
    be->Dispose(be);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <dir with a.mp4 b.mkv c.mp4>\n", argv[0]);
        return 2;
    }
    snprintf(dir, sizeof(dir), "%s", argv[1]);
    av_log_set_level(AV_LOG_ERROR);
    FB_RegisterBuiltinModules();

    test_join();
    test_same_source_reversed();
    test_mixed_sizes();
    test_crossfade();
    test_video_only_fallback();
    test_h264();
    test_cancel();
    test_errors();
    test_proxy();
    test_window_seek();
    test_play();
    test_play_player(10);
    test_play_player(100);
    test_play_separate_sound();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("All libav tests passed\n");
    return 0;
}
