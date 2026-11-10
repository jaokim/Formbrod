/*
 * Host-side tests for the portable core (timeline, modules, jobs).
 * Build and run with:  make -f Makefile.host test
 */
#include <stdio.h>
#include <string.h>
#include "formbrod/timeline.h"
#include "formbrod/job.h"
#include "formbrod/module.h"
#include "formbrod/timeview.h"

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

static void test_timeview(void)
{
    FBTimeView v;
    FBTimeline tl;
    FBCell cells[64];
    fb_us_t major, minor, st;
    char buf[32];
    int n, i, levels;

    /* 400 px wide, 25 fps, 48 px cells: most zoomed in is one frame per
     * cell, 40000 / 48 = 833 us per pixel. */
    FB_ViewInit(&v, 400);
    FB_ViewSetFrame(&v, 40000, 48);
    CHECK(v.min_per_px == 833);
    FB_ViewSetLength(&v, FB_US(60000));             /* one minute */
    CHECK(v.max_per_px == 150000);                  /* 60 s in 400 px */
    CHECK(v.per_px == 833 && v.start == 0);

    FB_ViewZoomToFit(&v);
    CHECK(v.per_px == 150000);
    CHECK(FB_ViewTimeToX(&v, FB_US(30000)) == 200);
    CHECK(FB_ViewXToTime(&v, 200) == FB_US(30000));
    CHECK(FB_ViewXToTime(&v, -5) == 0);
    CHECK(FB_ViewXToTime(&v, 5000) == FB_US(60000));
    CHECK(FB_ViewTotalPx(&v) == 400);

    /* Zooming in around the middle keeps 30 s under x = 200. */
    FB_ViewSetZoom(&v, 10000, 200);
    CHECK(v.per_px == 10000);
    CHECK(v.start == FB_US(28000));
    CHECK(FB_ViewTimeToX(&v, FB_US(30000)) == 200);
    CHECK(FB_ViewTimeToX(&v, FB_US(27990)) == -1);  /* rounds down */

    /* Can't scroll past either end. */
    FB_ViewSetStart(&v, -1000);
    CHECK(v.start == 0);
    FB_ViewSetStart(&v, FB_US(99000));
    CHECK(v.start == FB_US(56000));                 /* 60 s - 400 * 10 ms */
    FB_ViewScrollPx(&v, -100);
    CHECK(v.start == FB_US(55000));

    /* Following the playhead scrolls whole pages. */
    FB_ViewSetStart(&v, 0);
    CHECK(FB_ViewEnsureVisible(&v, FB_US(3999)) == 0);
    CHECK(FB_ViewEnsureVisible(&v, FB_US(4500)) == 1);
    CHECK(v.start == FB_US(4000));
    CHECK(FB_ViewEnsureVisible(&v, FB_US(100)) == 1 && v.start == 0);

    /* Zoom levels run from min to max and round-trip. */
    levels = FB_ViewZoomLevels(&v);
    CHECK(levels > 10 && levels < 40);
    FB_ViewSetZoomLevel(&v, 0, 0);
    CHECK(v.per_px == v.min_per_px && FB_ViewZoomLevel(&v) == 0);
    FB_ViewSetZoomLevel(&v, levels - 1, 0);
    CHECK(v.per_px == v.max_per_px);
    CHECK(FB_ViewZoomLevel(&v) == levels - 1);
    for (i = 0; i < levels; i++) {
        FB_ViewSetZoomLevel(&v, i, 0);
        CHECK(FB_ViewZoomLevel(&v) == i);
    }
    FB_ViewSetZoomLevel(&v, levels + 5, 0);
    CHECK(v.per_px == v.max_per_px);

    /* A shorter length lowers the most zoomed-out level and clamps. */
    FB_ViewSetLength(&v, FB_US(4000));
    CHECK(v.max_per_px == 10000 && v.per_px == 10000 && v.start == 0);
    /* Empty: nothing to zoom out to. */
    FB_ViewSetLength(&v, 0);
    CHECK(v.max_per_px == v.min_per_px && FB_ViewZoomLevels(&v) == 1);

    /* Ruler steps: labels 60 px apart. */
    FB_ViewSetLength(&v, FB_US(3600000));
    FB_ViewSetZoom(&v, 833, 0);                     /* frame level */
    FB_ViewRulerSteps(&v, 60, 40000, &major, &minor);
    CHECK(major == 80000 && minor == 40000);        /* 2 frames, 1 frame */
    FB_ViewSetZoom(&v, 10000, 0);                   /* 10 ms per px */
    FB_ViewRulerSteps(&v, 60, 40000, &major, &minor);
    CHECK(major == FB_US(1000) && minor == FB_US(200));
    FB_ViewRulerSteps(&v, 60, 0, &major, &minor);
    CHECK(major == FB_US(1000));
    FB_ViewSetZoom(&v, 5000, 0);
    FB_ViewRulerSteps(&v, 60, 0, &major, &minor);
    CHECK(major == FB_US(500) && minor == FB_US(100));
    FB_ViewSetZoom(&v, 2000000, 0);                 /* 2 s per px */
    FB_ViewRulerSteps(&v, 60, 40000, &major, &minor);
    CHECK(major == FB_US(120000) && minor == FB_US(30000));   /* 2 min */

    /* Timecodes. */
    CHECK(strcmp(FB_FormatTimecode(buf, sizeof(buf), FB_US(65000),
                                   FB_US(1000), 40000, '.'), "1:05") == 0);
    CHECK(strcmp(FB_FormatTimecode(buf, sizeof(buf), FB_US(65480),
                                   80000, 40000, '.'), "1:05:12") == 0);
    CHECK(strcmp(FB_FormatTimecode(buf, sizeof(buf), FB_US(65500),
                                   FB_US(500), 0, ','), "1:05,5") == 0);
    CHECK(strcmp(FB_FormatTimecode(buf, sizeof(buf), FB_US(3725000),
                                   FB_US(5000), 40000, '.'), "1:02:05") == 0);
    CHECK(strcmp(FB_FormatTimecode(buf, sizeof(buf), -5,
                                   FB_US(1000), 0, '.'), "0:00") == 0);

    /* Cells sit on a grid from time 0 and show their middle frame. */
    FB_ViewInit(&v, 100);
    FB_ViewSetFrame(&v, 40000, 48);
    FB_ViewSetLength(&v, FB_US(10000));
    FB_ViewSetZoom(&v, 1000, 0);                    /* 48 ms per cell */
    FB_ViewSetStart(&v, 20000);                     /* 20 ms in */
    n = FB_ViewCells(&v, 48, cells, 64);
    CHECK(n == 3);
    CHECK(cells[0].index == 0 && cells[0].x == -20);
    CHECK(cells[0].t0 == 0 && cells[0].t1 == 48000 && cells[0].mid == 24000);
    CHECK(cells[1].x == 28 && cells[2].x == 76);
    /* At the end, the last cell's middle is the middle of what's left. */
    FB_ViewSetLength(&v, 110000);
    FB_ViewSetStart(&v, 20000);
    n = FB_ViewCells(&v, 48, cells, 64);
    CHECK(n == 3 && cells[2].t0 == 96000 && cells[2].mid == 103000);
    CHECK(FB_ViewCells(&v, 48, cells, 2) == 2);

    /* Timeline time to clip and source time. */
    FB_TimelineInit(&tl);
    FB_TimelineAddSource(&tl, "a.mp4", 10000, 1, 1);
    FB_TimelineAddSource(&tl, "b.mp4", 10000, 1, 1);
    FB_TimelineAppendClip(&tl, 0, 2000, 5000);      /* 0-3 s on the timeline */
    FB_TimelineAppendClip(&tl, 1, 1000, 2000);      /* 3-4 s */
    CHECK(FB_TimelineClipStart(&tl, 1) == FB_US(3000));
    CHECK(FB_TimelineLocate(&tl, FB_US(500), &st) == 0 && st == FB_US(2500));
    CHECK(FB_TimelineLocate(&tl, FB_US(3000), &st) == 1 && st == FB_US(1000));
    CHECK(FB_TimelineLocate(&tl, FB_US(3999), NULL) == 1);
    CHECK(FB_TimelineLocate(&tl, FB_US(4000), NULL) == -1);
    CHECK(FB_TimelineLocate(&tl, -1, NULL) == -1);
}

int main(void)
{
    FB_RegisterBuiltinModules();
    test_timeline();
    test_job_build_and_roundtrip();
    test_job_errors();
    test_fit();
    test_timeview();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("All tests passed\n");
    return 0;
}
