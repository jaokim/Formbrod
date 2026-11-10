/*
 * Host-side tests for the portable core (timeline, modules, jobs).
 * Build and run with:  make -f Makefile.host test
 */
#include <stdio.h>
#include <string.h>
#include "formbrod/timeline.h"
#include "formbrod/job.h"
#include "formbrod/module.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static void test_timeline(void)
{
    FBTimeline tl;
    int s, right;

    FB_TimelineInit(&tl);
    s = FB_TimelineAddSource(&tl, "Work:Video/a.mp4", 10000, 1, 1);
    CHECK(s == 0);
    CHECK(FB_TimelineAppendClip(&tl, s, 0, 10000) == 0);
    CHECK(FB_TimelineAppendClip(&tl, 5, 0, 1000) == -1);   /* bad source */
    CHECK(FB_TimelineAppendClip(&tl, s, 500, 500) == -1);  /* empty clip */

    right = FB_TimelineSplit(&tl, 4000);
    CHECK(right == 1);
    CHECK(tl.num_clips == 2);
    CHECK(tl.clips[0].out == 4000);
    CHECK(tl.clips[1].in == 4000 && tl.clips[1].out == 10000);
    CHECK(FB_TimelineDuration(&tl) == 10000);

    CHECK(FB_TimelineSplit(&tl, 4000) == -1);   /* on a boundary */
    CHECK(FB_TimelineRemoveClip(&tl, 0) == 0);
    CHECK(tl.num_clips == 1 && FB_TimelineDuration(&tl) == 6000);
}

static void test_job_build_and_roundtrip(void)
{
    FBTimeline tl;
    FBOutput out;
    FBJob job, parsed;
    FBEffect *fx;
    char text[32768], cmd[32768];
    int a, b;

    FB_TimelineInit(&tl);
    a = FB_TimelineAddSource(&tl, "Work:Video/my clip.mp4", 10000, 1, 1);
    b = FB_TimelineAddSource(&tl, "Work:Video/b.mp4", 5000, 1, 1);
    FB_TimelineAddSource(&tl, "Work:Video/unused.mp4", 5000, 1, 1);
    FB_TimelineAppendClip(&tl, a, 1000, 4000);
    FB_TimelineAppendClip(&tl, b, 0, 5000);
    fx = FB_ClipAddEffect(&tl.clips[0], "fade");
    FB_EffectSetParam(fx, "in", 0.5);

    memset(&out, 0, sizeof(out));
    strcpy(out.path, "Work:Video/out.mp4");
    strcpy(out.vcodec, "mpeg4");
    strcpy(out.acodec, "aac");
    out.quality = FB_QUALITY_FINAL;

    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    CHECK(job.num_inputs == 2);   /* unused source dropped */
    CHECK(strstr(job.filtergraph, "[0:v]trim=start=1.000:end=4.000") != NULL);
    CHECK(strstr(job.filtergraph, "fade=t=in:st=0:d=0.500[v0]") != NULL);
    CHECK(strstr(job.filtergraph, "afade=t=in:st=0:d=0.500[a0]") != NULL);
    CHECK(strstr(job.filtergraph, "concat=n=2:v=1:a=1[outv][outa]") != NULL);
    printf("filtergraph: %s\n", job.filtergraph);

    CHECK(FB_JobWrite(&job, text, sizeof(text)) > 0);
    CHECK(FB_JobParse(&parsed, text) == 0);
    CHECK(parsed.num_inputs == 2);
    CHECK(strcmp(parsed.inputs[0], "Work:Video/my clip.mp4") == 0);
    CHECK(strcmp(parsed.filtergraph, job.filtergraph) == 0);
    CHECK(strcmp(parsed.output.path, out.path) == 0);
    CHECK(strcmp(parsed.audio_out, "outa") == 0);
    CHECK(parsed.output.quality == FB_QUALITY_FINAL);

    /* 3 s of clip 0 plus 5 s of clip 1; the atrims don't count twice */
    CHECK(FB_JobEstimateLength(&job) == 8000);

    CHECK(FB_JobToCommandLine(&job, cmd, sizeof(cmd)) > 0);
    CHECK(strstr(cmd, "-i \"Work:Video/my clip.mp4\"") != NULL);
    printf("command: %s\n", cmd);
}

static void test_job_errors(void)
{
    FBTimeline tl;
    FBOutput out;
    FBJob job;

    memset(&out, 0, sizeof(out));
    FB_TimelineInit(&tl);
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_EMPTY);

    FB_TimelineAddSource(&tl, "a.mp4", 1000, 1, 0);   /* no audio */
    FB_TimelineAppendClip(&tl, 0, 0, 1000);
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    CHECK(job.audio_out[0] == '\0');
    CHECK(strstr(job.filtergraph, "concat=n=1:v=1:a=0[outv]") != NULL);
    /* preview quality scales down by default */
    CHECK(strstr(job.filtergraph, "scale=320:-2") != NULL);

    CHECK(FB_JobEstimateLength(&job) == 1000);
    strcpy(job.filtergraph, "[0:v]trim=start=2[outv]");   /* no end */
    CHECK(FB_JobEstimateLength(&job) == 0);
    strcpy(job.filtergraph, "[0:v]trim=duration=1.5,fps=25[outv]");
    CHECK(FB_JobEstimateLength(&job) == 1500);
    strcpy(job.filtergraph, "[0:v]null[outv]");
    CHECK(FB_JobEstimateLength(&job) == 0);

    FB_ClipAddEffect(&tl.clips[0], "nosuchmodule");
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_UNKNOWN_MODULE);

    CHECK(FB_JobParse(&job, "garbage\n") != 0);
}

/* Mixed sizes: 320x240 4:3 25 fps with audio, 640x360 16:9 30 fps without,
 * and 720x576 PAL with 16:15 pixels (shown as 768x576, also 4:3). */
static void make_mixed(FBTimeline *tl)
{
    FB_TimelineInit(tl);
    FB_TimelineAddSource(tl, "a.mp4", 5000, 1, 1);
    FB_SourceSetVideo(&tl->sources[0], 320, 240, 1, 1, 25, 1);
    FB_TimelineAddSource(tl, "b.mkv", 4000, 1, 0);
    FB_SourceSetVideo(&tl->sources[1], 640, 360, 0, 0, 30, 1);
    FB_TimelineAddSource(tl, "dv.avi", 3000, 1, 1);
    FB_SourceSetVideo(&tl->sources[2], 720, 576, 16, 15, 25, 1);
    FB_TimelineAppendClip(tl, 0, 0, 2000);
    FB_TimelineAppendClip(tl, 1, 0, 2000);
    FB_TimelineAppendClip(tl, 2, 0, 1000);
}

static void test_fit(void)
{
    FBTimeline tl;
    FBOutput out;
    FBJob job;
    int w, h, n, d;

    make_mixed(&tl);

    /* Defaults come from the first clip. */
    CHECK(FB_TimelineFrameSize(&tl, &w, &h) && w == 320 && h == 240);
    CHECK(FB_TimelineFrameRate(&tl, &n, &d) && n == 25 && d == 1);
    CHECK(FB_ClipFit(&tl, &tl.clips[1]) == FB_FIT_FIT);
    CHECK(FB_ClipFitResult(&tl, &tl.clips[0], w, h) == FB_FITS_NONE);
    CHECK(FB_ClipFitResult(&tl, &tl.clips[1], w, h) == FB_FITS_LETTERBOX);
    CHECK(FB_ClipFitResult(&tl, &tl.clips[2], w, h) == FB_FITS_SCALE);

    memset(&out, 0, sizeof(out));
    strcpy(out.path, "out.mp4");
    out.quality = FB_QUALITY_FINAL;
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    printf("mixed: %s\n", job.filtergraph);
    /* clip 0 already fits */
    CHECK(strstr(job.filtergraph, "setpts=PTS-STARTPTS,setsar=1,fps=25/1[v0]"));
    CHECK(strstr(job.filtergraph, "scale=320:240:force_original_aspect_ratio="
                 "decrease,pad=320:240:(ow-iw)/2:(oh-ih)/2:color=0x000000,"
                 "setsar=1,fps=25/1[v1]"));
    /* square pixels first for PAL DV */
    CHECK(strstr(job.filtergraph, "scale=iw*sar:ih,setsar=1,scale=320:240"));
    /* clip 1 has no audio: silence of the clip's length */
    CHECK(strstr(job.filtergraph, "anullsrc=r=48000:cl=stereo,"
                 "atrim=duration=2.000,"));
    CHECK(strstr(job.filtergraph, "concat=n=3:v=1:a=1[outv][outa]"));
    CHECK(FB_JobEstimateLength(&job) == 5000);   /* atrims don't count */

    /* Per-clip override beats the default; changing the default moves
     * every clip left at DEFAULT. */
    tl.clips[1].fit = FB_FIT_FILL;
    tl.format.fit = FB_FIT_STRETCH;
    CHECK(FB_ClipFitResult(&tl, &tl.clips[1], w, h) == FB_FITS_CROP);
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    CHECK(strstr(job.filtergraph, "force_original_aspect_ratio=increase,"
                 "crop=320:240,setsar=1,fps=25/1[v1]"));
    CHECK(strstr(job.filtergraph, "scale=320:240,setsar=1,fps=25/1[v2]"));
    tl.clips[1].fit = FB_FIT_DEFAULT;
    CHECK(FB_ClipFitResult(&tl, &tl.clips[1], w, h) == FB_FITS_STRETCH);

    /* Global format set, and split keeps the clip's mode. */
    tl.format.width = 641;           /* rounded down to even */
    tl.format.height = 360;
    tl.format.fps_num = 30000;
    tl.format.fps_den = 1001;
    tl.format.fit = FB_FIT_FIT;
    tl.format.pad_color = 0x102030;
    tl.clips[0].fit = FB_FIT_FILL;
    CHECK(FB_TimelineFrameSize(&tl, &w, &h) && w == 640 && h == 360);
    CHECK(FB_ClipFitResult(&tl, &tl.clips[0], w, h) == FB_FITS_CROP);
    CHECK(FB_ClipFitResult(&tl, &tl.clips[1], w, h) == FB_FITS_NONE);
    CHECK(FB_ClipFitResult(&tl, &tl.clips[2], w, h) == FB_FITS_PILLARBOX);
    CHECK(FB_TimelineSplit(&tl, 1000) == 1);
    CHECK(tl.clips[1].fit == FB_FIT_FILL);
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    CHECK(strstr(job.filtergraph, "color=0x102030"));
    CHECK(strstr(job.filtergraph, "fps=30000/1001"));

    /* Preview: 320 wide, height from the frame's aspect. */
    out.quality = FB_QUALITY_PREVIEW;
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    CHECK(strstr(job.filtergraph, "scale=320:180:"));
    /* An output size wins over the timeline's. */
    out.width = 400;
    out.height = 300;
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    CHECK(strstr(job.filtergraph, "pad=400:300:"));

    CHECK(FB_FitFromName("Stretch") == FB_FIT_STRETCH);
    CHECK(FB_FitFromName("DEFAULT") == FB_FIT_DEFAULT);
    CHECK(FB_FitFromName("fitt") == -1);
    CHECK(strcmp(FB_FitName(FB_FIT_FILL), "fill") == 0);
}

int main(void)
{
    FB_RegisterBuiltinModules();
    test_timeline();
    test_job_build_and_roundtrip();
    test_job_errors();
    test_fit();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("All tests passed\n");
    return 0;
}
