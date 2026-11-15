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
#include "formbrod/log.h"
#include "formbrod/project.h"
#include "formbrod/cache.h"
#include "formbrod/player.h"
#include "formbrod/render.h"
#include "formbrod/thumbs.h"

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

    right = FB_TimelineSplit(&tl, -1, 4000);
    CHECK(right == 1);
    CHECK(tl.num_clips == 2);
    CHECK(tl.clips[0].out == 4000);
    CHECK(tl.clips[1].in == 4000 && tl.clips[1].out == 10000);
    CHECK(FB_TimelineDuration(&tl) == 10000);

    CHECK(FB_TimelineSplit(&tl, -1, 4000) == -1);   /* on a boundary */
    CHECK(FB_TimelineRemoveClip(&tl, 0) == 0);
    CHECK(tl.num_clips == 1 && FB_TimelineDuration(&tl) == 6000);
}

static void test_tracks(void)
{
    FBTimeline tl;
    fb_ms_t st;
    int a, b, m, i;

    FB_TimelineInit(&tl);
    a = FB_TimelineAddSource(&tl, "a.mp4", 10000, 1, 1);
    b = FB_TimelineAddSource(&tl, "b.mp4", 8000, 1, 0);
    m = FB_TimelineAddSource(&tl, "music.mp3", 30000, 0, 1);
    CHECK(FB_SourceTrackKind(&tl.sources[a]) == FB_TRACK_VIDEO);
    CHECK(FB_SourceTrackKind(&tl.sources[m]) == FB_TRACK_AUDIO);

    /* Appending to track num_tracks adds a track of the right kind. */
    CHECK(FB_TimelineAppendToTrack(&tl, 0, a, 0, 4000) == 0);
    CHECK(tl.num_tracks == 1 && tl.tracks[0].kind == FB_TRACK_VIDEO);
    CHECK(FB_TimelineAppendToTrack(&tl, 2, b, 0, 1000) == -1);  /* no gap */
    CHECK(FB_TimelineAppendToTrack(&tl, 1, m, 0, 30000) == 1);
    CHECK(tl.num_tracks == 2 && tl.tracks[1].kind == FB_TRACK_AUDIO);
    CHECK(tl.clips[1].start == 0 && tl.clips[1].volume == 1.0);
    /* Appending to track 0 puts the clip after its last one, and before
     * the track-1 clip in the array (sorted by track). */
    CHECK(FB_TimelineAppendToTrack(&tl, 0, b, 1000, 3000) == 1);
    CHECK(tl.clips[1].track == 0 && tl.clips[1].start == 4000);
    CHECK(tl.clips[2].track == 1);
    CHECK(FB_TimelineTrackEnd(&tl, 0) == 6000);
    CHECK(FB_TimelineDuration(&tl) == 30000);

    /* An overlay track with a clip at 2 s; overlaps are refused. */
    CHECK(FB_TimelineAddTrack(&tl, FB_TRACK_VIDEO) == 2);
    CHECK(FB_TimelineAddClip(&tl, 2, b, 0, 1000, 2000) == 3);
    CHECK(FB_TimelineAddClip(&tl, 2, b, 0, 1000, 2500) == -1);
    CHECK(FB_TimelineAddClip(&tl, 2, b, 0, 1000, 1500) == -1);
    CHECK(FB_TimelineAddClip(&tl, 2, b, 0, 500, 1500) == 3);    /* fits */
    CHECK(tl.clips[3].start == 1500 && tl.clips[4].start == 2000);
    CHECK(FB_TimelineAddClip(&tl, 7, b, 0, 500, 0) == -1);      /* no track */

    /* What's seen: the highest picture track that has something. */
    CHECK(FB_TimelineClipAt(&tl, 2500, &st) == 4 && st == 500);
    CHECK(FB_TimelineClipAt(&tl, 3500, &st) == 0 && st == 3500);
    CHECK(FB_TimelineClipAt(&tl, 5000, &st) == 1 && st == 2000);
    CHECK(FB_TimelineClipAt(&tl, 6000, NULL) == -1);   /* only music */
    tl.tracks[2].hidden = 1;
    CHECK(FB_TimelineClipAt(&tl, 2500, NULL) == 0);
    tl.tracks[2].hidden = 0;

    /* Splitting one track, then all tracks. */
    CHECK(FB_TimelineSplit(&tl, 0, 2000) == 1);
    CHECK(tl.clips[0].out == 2000 && tl.clips[1].start == 2000 &&
          tl.clips[1].in == 2000 && tl.clips[1].out == 4000);
    CHECK(tl.num_clips == 6);
    i = FB_TimelineSplit(&tl, -1, 5000);     /* track 0 and the music */
    CHECK(i >= 0 && tl.num_clips == 8);
    for (i = 1; i < tl.num_clips; i++)       /* still sorted */
        CHECK(tl.clips[i - 1].track < tl.clips[i].track ||
              (tl.clips[i - 1].track == tl.clips[i].track &&
               FB_ClipEnd(&tl.clips[i - 1]) <= tl.clips[i].start));
    CHECK(FB_TimelineSplit(&tl, 2, 9000) == -1);

    /* Removing closes the gap on that track only. Track 0 is now
     * 0-2 a, 2-4 a, 4-5 b, 5-6 b. */
    CHECK(FB_TimelineRemoveClip(&tl, 1) == 0);
    CHECK(tl.clips[1].start == 2000 && tl.clips[2].start == 3000);
    CHECK(FB_TimelineTrackEnd(&tl, 0) == 4000);
    CHECK(tl.clips[3].track == 1 && tl.clips[3].start == 0);

    /* Trimming moves what follows; out can't pass the source's end. */
    CHECK(FB_TimelineTrimClip(&tl, 0, 500, 2000) == 0);
    CHECK(tl.clips[1].start == 1500 && tl.clips[2].start == 2500);
    CHECK(FB_TimelineTrimClip(&tl, 0, 500, 3000) == 0);
    CHECK(tl.clips[1].start == 2500);
    CHECK(FB_TimelineTrimClip(&tl, 0, 500, 10001) == -1);
    CHECK(FB_TimelineTrimClip(&tl, 0, 3000, 3000) == -1);

    /* Moving a clip takes the ones after it along; not into the one in
     * front of it. Track 0 is now 0-2.5, 2.5-3.5, 3.5-4.5 s. */
    CHECK(FB_TimelineEarliestStart(&tl, 1) == 2500);
    CHECK(FB_TimelineEarliestStart(&tl, 0) == 0);
    CHECK(FB_TimelineMoveClip(&tl, 1, 2000) == -1);
    CHECK(FB_TimelineMoveClip(&tl, 1, 3500) == 0);       /* a 1 s gap */
    CHECK(tl.clips[1].start == 3500 && tl.clips[2].start == 4500);
    CHECK(tl.clips[3].track == 1 && tl.clips[3].start == 0);  /* untouched */
    CHECK(FB_TimelineMoveClip(&tl, 1, 2500) == 0);       /* gap closed */
    CHECK(tl.clips[2].start == 3500);
    CHECK(FB_TimelineMoveClip(&tl, 0, 1000) == 0);       /* the whole track */
    CHECK(tl.clips[0].start == 1000 && tl.clips[2].start == 4500);
    CHECK(FB_TimelineMoveClip(&tl, 0, 0) == 0);
    CHECK(FB_TimelineMoveClip(&tl, 99, 0) == -1);

    /* Removing a track takes its clips; the ones above move down. */
    i = tl.num_clips;
    CHECK(FB_TimelineRemoveTrack(&tl, 1) == 0);
    CHECK(tl.num_tracks == 2 && tl.num_clips == i - 2);
    CHECK(tl.clips[tl.num_clips - 1].track == 1);
    CHECK(tl.tracks[1].kind == FB_TRACK_VIDEO);
    CHECK(FB_TimelineRemoveTrack(&tl, 5) == -1);

    CHECK(FB_TrackKindFromName("Audio") == FB_TRACK_AUDIO);
    CHECK(FB_TrackKindFromName("VIDEO") == FB_TRACK_VIDEO);
    CHECK(FB_TrackKindFromName("vid") == -1);
    CHECK(strcmp(FB_TrackKindName(FB_TRACK_AUDIO), "audio") == 0);

    /* Frame size and rate come from the lowest picture track. */
    FB_TimelineInit(&tl);
    m = FB_TimelineAddSource(&tl, "music.mp3", 30000, 0, 1);
    a = FB_TimelineAddSource(&tl, "a.mp4", 10000, 1, 1);
    FB_SourceSetVideo(&tl.sources[a], 640, 480, 1, 1, 25, 1);
    FB_TimelineAppendToTrack(&tl, 0, m, 0, 30000);
    FB_TimelineAppendToTrack(&tl, 1, a, 0, 10000);
    {
        int w, h, num, den;

        CHECK(FB_TimelineFrameSize(&tl, &w, &h) && w == 640 && h == 480);
        CHECK(FB_TimelineFrameRate(&tl, &num, &den) && num == 25);
    }
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

    CHECK(job.length == 1000 && FB_JobEstimateLength(&job) == 1000);
    /* Without a length (an older job), it's worked out from the trims. */
    job.length = 0;
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
    CHECK(FB_TimelineSplit(&tl, -1, 1000) == 1);
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

static void test_paths(void)
{
    char buf[FB_PATH_LEN];

    FB_PathDir("Work:Proj/edit.fbp", buf, sizeof(buf));
    CHECK(strcmp(buf, "Work:Proj") == 0);
    FB_PathDir("Work:edit.fbp", buf, sizeof(buf));
    CHECK(strcmp(buf, "Work:") == 0);
    FB_PathDir("edit.fbp", buf, sizeof(buf));
    CHECK(strcmp(buf, "") == 0);

    CHECK(FB_PathJoin("Work:Proj", "a.mp4", buf, sizeof(buf)) == 0);
    CHECK(strcmp(buf, "Work:Proj/a.mp4") == 0);
    CHECK(FB_PathJoin("Work:", "a.mp4", buf, sizeof(buf)) == 0);
    CHECK(strcmp(buf, "Work:a.mp4") == 0);
    CHECK(FB_PathJoin("Work:Proj", "Video:b.mp4", buf, sizeof(buf)) == 0);
    CHECK(strcmp(buf, "Video:b.mp4") == 0);
    CHECK(FB_PathJoin("Work:Proj", "x", buf, 8) == -1);

    CHECK(FB_PathRelative("Work:Proj", "work:proj/clips/a.mp4",
                          buf, sizeof(buf)) == 1);
    CHECK(strcmp(buf, "clips/a.mp4") == 0);
    CHECK(FB_PathRelative("Work:", "Work:a.mp4", buf, sizeof(buf)) == 1);
    CHECK(strcmp(buf, "a.mp4") == 0);
    CHECK(FB_PathRelative("Work:Proj", "Work:Project2/a.mp4",
                          buf, sizeof(buf)) == 0);
    CHECK(strcmp(buf, "Work:Project2/a.mp4") == 0);
    CHECK(FB_PathRelative("", "Work:a.mp4", buf, sizeof(buf)) == 0);
    CHECK(strcmp(buf, "Work:a.mp4") == 0);
}

static void test_project_roundtrip(void)
{
    static FBTimeline tl, back;
    static char text[16384];
    FBEffect *fx;
    int n, line;

    FB_TimelineInit(&tl);
    FB_TimelineAddSource(&tl, "Work:Proj/clips/intro.mp4", 8000, 1, 1);
    FB_TimelineAddSource(&tl, "Video:Holiday 2026/beach.mov", 20000, 1, 0);
    FB_TimelineAppendClip(&tl, 0, 1000, 5000);
    FB_TimelineAppendClip(&tl, 1, 0, 20000);
    tl.clips[1].fit = FB_FIT_FILL;
    fx = FB_ClipAddEffect(&tl.clips[1], "fade");
    FB_EffectSetParam(fx, "in", 0.5);
    FB_EffectSetParam(fx, "out", 1.25);
    tl.format.width = 1280;
    tl.format.height = 720;
    tl.format.fps_num = 30000;
    tl.format.fps_den = 1001;
    tl.format.fit = FB_FIT_STRETCH;
    tl.format.pad_color = 0x102030;

    n = FB_ProjectWrite(&tl, "Work:Proj", text, sizeof(text));
    CHECK(n > 0);
    CHECK(strstr(text, "SOURCE 1 clips/intro.mp4\n") != NULL);
    CHECK(strstr(text, "SOURCE 2 Video:Holiday 2026/beach.mov\n") != NULL);
    CHECK(FB_ProjectWrite(&tl, "Work:Proj", text, 64) == -1);

    n = FB_ProjectWrite(&tl, "Work:Proj", text, sizeof(text));
    CHECK(FB_ProjectParse(&back, text, "Work:Proj", &line) == FB_PROJECT_OK);
    CHECK(back.num_sources == 2 && back.num_clips == 2);
    CHECK(strcmp(back.sources[0].path, "Work:Proj/clips/intro.mp4") == 0);
    CHECK(strcmp(back.sources[1].path, "Video:Holiday 2026/beach.mov") == 0);
    CHECK(back.clips[0].source == 0 && back.clips[0].in == 1000 &&
          back.clips[0].out == 5000 && back.clips[0].fit == FB_FIT_DEFAULT);
    CHECK(back.clips[1].source == 1 && back.clips[1].fit == FB_FIT_FILL);
    CHECK(back.clips[1].num_effects == 1);
    CHECK(strcmp(back.clips[1].effects[0].module, "fade") == 0);
    CHECK(FB_EffectGetParam(&back.clips[1].effects[0], "in", 0) == 0.5);
    CHECK(FB_EffectGetParam(&back.clips[1].effects[0], "out", 0) == 1.25);
    CHECK(back.format.width == 1280 && back.format.height == 720);
    CHECK(back.format.fps_num == 30000 && back.format.fps_den == 1001);
    CHECK(back.format.fit == FB_FIT_STRETCH);
    CHECK(back.format.pad_color == 0x102030);

    /* Saved somewhere else, the whole project moves with its drawer. */
    CHECK(FB_ProjectParse(&back, text, "DH1:Moved", NULL) == FB_PROJECT_OK);
    CHECK(strcmp(back.sources[0].path, "DH1:Moved/clips/intro.mp4") == 0);
}

static void test_project_hand_edited(void)
{
    static FBTimeline tl;
    int line;

    /* Comments, indentation, CRLF, an unknown key and a path with spaces. */
    CHECK(FB_ProjectParse(&tl,
        "; my project\r\n"
        "FORMBROD-PROJECT 1\r\n"
        "\r\n"
        "  SOURCE 1   Work:My Videos/a.mp4  \r\n"
        "SHINY-NEW-KEY whatever\r\n"
        "# CLIP 1 0 10\r\n"
        "CLIP 1 0 3000\r\n"
        "\tEFFECT fade\r\n"
        "\t\tPARAM in 1\r\n", NULL, &line) == FB_PROJECT_OK);
    CHECK(tl.num_sources == 1 && tl.num_clips == 1);
    CHECK(strcmp(tl.sources[0].path, "Work:My Videos/a.mp4") == 0);
    CHECK(tl.clips[0].num_effects == 1);

    CHECK(FB_ProjectParse(&tl, "FORMBROD-JOB 1\n", NULL, &line)
          == FB_PROJECT_NOT_PROJECT);
    CHECK(FB_ProjectParse(&tl, "", NULL, &line) == FB_PROJECT_NOT_PROJECT);
    CHECK(FB_ProjectParse(&tl, "FORMBROD-PROJECT 99\n", NULL, &line)
          == FB_PROJECT_TOO_NEW);
    CHECK(FB_ProjectParse(&tl,
        "FORMBROD-PROJECT 1\nSOURCE 1 a.mp4\nCLIP 2 0 100\n", "Work:", &line)
          == FB_PROJECT_BAD_SOURCE);
    CHECK(line == 3);
    CHECK(FB_ProjectParse(&tl,
        "FORMBROD-PROJECT 1\nSOURCE 1 a.mp4\nCLIP 1 500 100\n", "", &line)
          == FB_PROJECT_BAD_LINE);
    CHECK(FB_ProjectParse(&tl,
        "FORMBROD-PROJECT 1\nSOURCE 2 a.mp4\n", "", &line)
          == FB_PROJECT_BAD_LINE);
    CHECK(line == 2);
    CHECK(FB_ProjectParse(&tl,
        "FORMBROD-PROJECT 1\nEFFECT fade\n", "", &line)
          == FB_PROJECT_BAD_LINE);
    CHECK(FB_ProjectParse(&tl,
        "FORMBROD-PROJECT 1\nSOURCE 1 a\nCLIP 1 0 9\nPARAM in x\n", "", &line)
          == FB_PROJECT_BAD_LINE);
}

static void test_job_tracks(void)
{
    FBTimeline tl;
    FBOutput out;
    FBJob job, parsed;
    char text[FB_FILTERGRAPH_LEN + 4096];
    int a, b, m;

    memset(&out, 0, sizeof(out));
    strcpy(out.path, "out.mp4");
    out.quality = FB_QUALITY_FINAL;

    FB_TimelineInit(&tl);
    a = FB_TimelineAddSource(&tl, "a.mp4", 10000, 1, 1);
    b = FB_TimelineAddSource(&tl, "logo.mp4", 10000, 1, 0);
    m = FB_TimelineAddSource(&tl, "music.mp3", 60000, 0, 1);
    FB_SourceSetVideo(&tl.sources[a], 320, 240, 1, 1, 25, 1);
    FB_SourceSetVideo(&tl.sources[b], 320, 240, 1, 1, 25, 1);

    /* 0: a 0-4 s.  1: music from 1 s to 7 s.  2: logo at 2-3 s. */
    FB_TimelineAppendToTrack(&tl, 0, a, 0, 4000);
    FB_TimelineAddTrack(&tl, FB_TRACK_AUDIO);
    FB_TimelineAddClip(&tl, 1, m, 0, 6000, 1000);
    tl.clips[1].volume = 0.5;
    FB_TimelineAddTrack(&tl, FB_TRACK_VIDEO);
    FB_TimelineAddClip(&tl, 2, b, 0, 1000, 2000);

    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    printf("tracks: %s\n", job.filtergraph);
    CHECK(job.num_inputs == 3 && job.length == 7000);
    /* Track 0 runs to 7 s with black after the clip. */
    CHECK(strstr(job.filtergraph, "color=c=0x000000:s=320x240:r=25/1:d=3.000,"
                                  "setsar=1[v1]") != NULL);
    CHECK(strstr(job.filtergraph, "[v0][a0][v1][a1]concat=n=2:v=1:a=1"
                                  "[t0v][t0a]") != NULL);
    /* Music: silence first, at half volume, sound only. */
    CHECK(strstr(job.filtergraph, "anullsrc=r=48000:cl=stereo,atrim=duration="
                                  "1.000,") != NULL);
    CHECK(strstr(job.filtergraph, "channel_layouts=stereo,volume=0.500[a3]"));
    CHECK(strstr(job.filtergraph, "[a2][a3]concat=n=2:v=0:a=1[t1a]"));
    /* The logo track is clear around the clip, and has no sound. */
    CHECK(strstr(job.filtergraph, "color=c=black@0:s=320x240:r=25/1:d=2.000,"
                                  "format=yuva420p,setsar=1[v4]") != NULL);
    CHECK(strstr(job.filtergraph, "fps=25/1,format=yuva420p[v5]") != NULL);
    CHECK(strstr(job.filtergraph, "[v4][v5][v6]concat=n=3:v=1:a=0[t2v]"));
    CHECK(strstr(job.filtergraph, "[t0v][t2v]overlay=format=auto:"
                                  "eof_action=pass[outv]") != NULL);
    CHECK(strstr(job.filtergraph, "[t0a][t1a]amix=inputs=2:duration=longest:"
                                  "dropout_transition=0,volume=2[outa]"));
    CHECK(job.filtergraph[strlen(job.filtergraph) - 1] != ';');
    CHECK(strcmp(job.audio_out, "outa") == 0);

    /* LENGTH survives the text format. */
    CHECK(FB_JobWrite(&job, text, sizeof(text)) > 0);
    CHECK(strstr(text, "LENGTH 7000\n") != NULL);
    CHECK(FB_JobParse(&parsed, text) == 0 && parsed.length == 7000);
    CHECK(FB_JobEstimateLength(&parsed) == 7000);

    /* Muting the music leaves only track 0's sound: no amix. */
    tl.tracks[1].muted = 1;
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    CHECK(strstr(job.filtergraph, "amix") == NULL);
    CHECK(strstr(job.filtergraph, "concat=n=1:v=1:a=1[t0v][outa]") != NULL);
    CHECK(job.length == 4000 && job.num_inputs == 2);

    /* Hiding the logo leaves one picture track: no overlay. */
    tl.tracks[2].hidden = 1;
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    CHECK(strstr(job.filtergraph, "overlay") == NULL);
    CHECK(strstr(job.filtergraph, "concat=n=1:v=1:a=1[outv][outa]") != NULL);

    /* Only sound left: black with the sound, like a gap. */
    tl.tracks[0].hidden = 1;
    tl.tracks[1].muted = 0;
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    CHECK(strstr(job.filtergraph, "color=c=") != NULL);
    CHECK(strstr(job.filtergraph, "overlay") == NULL);
    /* Nothing to see or hear: empty. */
    tl.tracks[0].muted = tl.tracks[1].muted = 1;
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_EMPTY);
    tl.tracks[0].muted = tl.tracks[1].muted = 0;
    tl.tracks[0].hidden = 0;
    tl.tracks[2].hidden = 0;

    /* Gaps need the frame size; unprobed sources don't have it. */
    tl.sources[a].width = tl.sources[a].height = 0;
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_NEED_SIZE);
    /* ...unless the output gives it. */
    out.width = 160;
    out.height = 120;
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    CHECK(strstr(job.filtergraph, "s=160x120") != NULL);
}

static void test_project_tracks(void)
{
    static FBTimeline tl, back;
    static char text[16384];
    int line;

    FB_TimelineInit(&tl);
    FB_TimelineAddSource(&tl, "a.mp4", 10000, 1, 1);
    FB_TimelineAddSource(&tl, "music.mp3", 60000, 0, 1);
    FB_TimelineAppendToTrack(&tl, 0, 0, 0, 4000);
    FB_TimelineAppendToTrack(&tl, 0, 0, 6000, 8000);
    FB_TimelineAppendToTrack(&tl, 1, 1, 0, 30000);
    tl.clips[2].start = 1500;                     /* a gap before it */
    tl.clips[2].volume = 0.25;
    tl.tracks[1].muted = 1;
    FB_TimelineAddTrack(&tl, FB_TRACK_VIDEO);
    tl.tracks[2].hidden = 1;
    FB_TimelineAddClip(&tl, 2, 0, 0, 1000, 2000);

    CHECK(FB_ProjectWrite(&tl, "", text, sizeof(text)) > 0);
    CHECK(strncmp(text, "FORMBROD-PROJECT 2\n", 19) == 0);
    CHECK(strstr(text, "TRACK video\nCLIP 1 0 4000\nCLIP 1 6000 8000\n"));
    CHECK(strstr(text, "TRACK audio\n    MUTED\nCLIP 2 0 30000\n"
                       "    AT 1500\n    VOLUME 0.25\n") != NULL);
    CHECK(strstr(text, "TRACK video\n    HIDDEN\nCLIP 1 0 1000\n    AT 2000\n"));

    CHECK(FB_ProjectParse(&back, text, "", &line) == FB_PROJECT_OK);
    CHECK(back.num_tracks == 3 && back.num_clips == 4);
    CHECK(back.tracks[1].kind == FB_TRACK_AUDIO && back.tracks[1].muted);
    CHECK(back.tracks[2].hidden && !back.tracks[0].muted);
    CHECK(back.clips[1].start == 4000 && back.clips[1].track == 0);
    CHECK(back.clips[2].track == 1 && back.clips[2].start == 1500 &&
          back.clips[2].volume == 0.25);
    CHECK(back.clips[3].track == 2 && back.clips[3].start == 2000);
    CHECK(back.clips[0].volume == 1.0);

    /* A length changed by hand moves the clips after it along. */
    CHECK(FB_ProjectParse(&back,
        "FORMBROD-PROJECT 2\nSOURCE 1 a.mp4\nTRACK video\n"
        "CLIP 1 0 2500\nCLIP 1 6000 8000\n", "", &line) == FB_PROJECT_OK);
    CHECK(back.clips[1].start == 2500);

    /* Version 1: one picture track. */
    CHECK(FB_ProjectParse(&back,
        "FORMBROD-PROJECT 1\nSOURCE 1 a.mp4\nCLIP 1 0 100\nCLIP 1 0 50\n",
        "", &line) == FB_PROJECT_OK);
    CHECK(back.num_tracks == 1 && back.tracks[0].kind == FB_TRACK_VIDEO);
    CHECK(back.num_clips == 2 && back.clips[1].start == 100);

    /* Mistakes are reported with their line. */
    CHECK(FB_ProjectParse(&back, "FORMBROD-PROJECT 2\nTRACK sideways\n", "",
                          &line) == FB_PROJECT_BAD_LINE && line == 2);
    CHECK(FB_ProjectParse(&back, "FORMBROD-PROJECT 2\nMUTED\n", "", &line)
          == FB_PROJECT_BAD_LINE);
    CHECK(FB_ProjectParse(&back,
        "FORMBROD-PROJECT 2\nSOURCE 1 a\nTRACK video\nCLIP 1 0 100\n"
        "CLIP 1 0 100\nAT 50\n", "", &line) == FB_PROJECT_BAD_LINE &&
        line == 6);                               /* would overlap */
    CHECK(FB_ProjectParse(&back,
        "FORMBROD-PROJECT 2\nSOURCE 1 a\nTRACK video\nCLIP 1 0 100\n"
        "VOLUME -1\n", "", &line) == FB_PROJECT_BAD_LINE);
}

/* Reads the log back without the time stamps (the first 9 characters). */
static void read_log(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    char line[1200];
    size_t used = 0;

    buf[0] = '\0';
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);

        if (n > 9 && used + n - 9 < len) {
            memcpy(buf + used, line + 9, n - 9);
            used += n - 9;
            buf[used] = '\0';
        }
    }
    fclose(f);
}

static void test_log(void)
{
    const char *path = "build/host/test.log";
    static char text[8192];
    char graph[400];
    int i;

    CHECK(FB_LogOpen("", FB_LOG_INFO) == -1 && !FB_LogIsOpen());
    FB_Log(FB_LOG_ERROR, "app", "nowhere");          /* no log: ignored */

    CHECK(FB_LogOpen(path, FB_LOG_INFO) == 0 && FB_LogIsOpen());
    CHECK(FB_LogWants(FB_LOG_WARNING) && !FB_LogWants(FB_LOG_DEBUG));
    FB_Log(FB_LOG_INFO, "app", "Started %d\n", 1);
    FB_Log(FB_LOG_DEBUG, "app", "too detailed");
    for (i = 0; i < 250; i++)
        FB_Log(FB_LOG_WARNING, "ffmpeg", "[swscaler] unknown range");
    FB_Log(FB_LOG_ERROR, "render", "Failed");
    FB_Log(FB_LOG_ERROR, "render", "Failed");
    FB_LogText(FB_LOG_INFO, "job", "INPUT 0 a.mp4\nOUTPUT b.mp4\n");
    /* A long filtergraph is broken after each ';'. */
    strcpy(graph, "[0:v]trim=start=0:end=1,setpts=PTS-STARTPTS,"
                  "scale=320:240:force_original_aspect_ratio=decrease[v0];"
                  "[0:a]atrim=start=0:end=1,asetpts=PTS-STARTPTS[a0];"
                  "[v0][a0]concat=n=1:v=1:a=1[outv][outa]");
    FB_LogText(FB_LOG_INFO, "job", graph);
    FB_LogClose();
    CHECK(!FB_LogIsOpen());

    read_log(path, text, sizeof(text));
    CHECK(strstr(text, "info    app     Started 1\n") == text);
    CHECK(strstr(text, "too detailed") == NULL);
    CHECK(strstr(text, "warning ffmpeg  [swscaler] unknown range\n"
                       "warning ffmpeg  (the line above repeated 249 more "
                       "times)\n") != NULL);
    CHECK(strstr(text, "error   render  Failed\nerror   render  (the line "
                       "above repeated 1 more time)\n") != NULL);
    CHECK(strstr(text, "info    job     INPUT 0 a.mp4\n"
                       "info    job     OUTPUT b.mp4\n") != NULL);
    CHECK(strstr(text, "info    job     [0:v]trim=start=0:end=1,setpts=PTS-"
                       "STARTPTS,scale=320:240:force_original_aspect_ratio="
                       "decrease[v0];\n") != NULL);
    CHECK(strstr(text, "info    job     [v0][a0]concat=n=1:v=1:a=1[outv]"
                       "[outa]\n") != NULL);

    /* Opening again starts a new file. */
    CHECK(FB_LogOpen(path, FB_LOG_DEBUG) == 0);
    FB_Log(FB_LOG_DEBUG, "app", "fresh");
    FB_LogClose();
    read_log(path, text, sizeof(text));
    CHECK(strcmp(text, "debug   app     fresh\n") == 0);

    CHECK(FB_LogLevelFromName("warning", FB_LOG_INFO) == FB_LOG_WARNING);
    CHECK(FB_LogLevelFromName("loud", FB_LOG_INFO) == FB_LOG_INFO);
    CHECK(strcmp(FB_LogLevelName(FB_LOG_DEBUG), "debug") == 0);
}

static void test_still(void)
{
    FBTimeline tl;
    FBOutput out;
    FBJob job;
    int pic, a, num, den;

    memset(&out, 0, sizeof(out));
    strcpy(out.path, "out.mp4");
    out.quality = FB_QUALITY_FINAL;

    FB_TimelineInit(&tl);
    pic = FB_TimelineAddSource(&tl, "pic.png", 0, 1, 0);
    a = FB_TimelineAddSource(&tl, "a.mp4", 4000, 1, 1);
    tl.sources[pic].still = 1;
    FB_SourceSetVideo(&tl.sources[pic], 400, 300, 1, 1, 25, 1);
    FB_SourceSetVideo(&tl.sources[a], 320, 240, 1, 1, 30, 1);
    CHECK(FB_TimelineAppendToTrack(&tl, 0, pic, 0, FB_STILL_LENGTH) == 0);
    CHECK(FB_TimelineAppendToTrack(&tl, 0, a, 0, 4000) == 1);

    /* The size is the first clip's, the rate the first moving one's. */
    CHECK(FB_TimelineFrameRate(&tl, &num, &den) && num == 30 && den == 1);

    /* A picture has no end: it can be made as long as wanted, and the
     * video after it moves along. A video can't go past its file. */
    CHECK(FB_TimelineTrimClip(&tl, 0, 0, 60000) == 0);
    CHECK(tl.clips[1].start == 60000);
    CHECK(FB_TimelineTrimClip(&tl, 0, 0, 3000) == 0);
    CHECK(tl.clips[1].start == 3000);
    CHECK(FB_TimelineTrimClip(&tl, 1, 0, 4001) == -1);

    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    printf("still: %s\n", job.filtergraph);
    /* Fitted and converted once, then repeated (not scaled for every
     * frame). Here it's the frame's size already. */
    CHECK(strstr(job.filtergraph, "[0:v]null,setsar=1,format=yuv420p,"
                                  "loop=loop=-1:size=1:start=0,"
                                  "setpts=N*1/(30*TB),trim=start=0.000:"
                                  "end=3.000,setpts=PTS-STARTPTS,fps=30/1[v0]")
          != NULL);
    CHECK(strstr(job.filtergraph, "[1:v]trim=start=0.000:end=4.000") != NULL);
    /* The picture has no sound: silence while it's shown. */
    CHECK(strstr(job.filtergraph, "anullsrc=r=48000:cl=stereo,atrim="
                                  "duration=3.000") != NULL);
    CHECK(job.length == 7000);

    /* A picture of another size: scaled before it's repeated, and not
     * again after. */
    FB_SourceSetVideo(&tl.sources[pic], 621, 610, 1, 1, 25, 1);
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    {
        const char *loop = strstr(job.filtergraph, "loop=loop=-1:size=1");
        const char *scale = strstr(job.filtergraph, "[0:v]null,scale=");
        const char *end = strstr(job.filtergraph, "[v0];");

        const char *again = loop ? strstr(loop, "scale=") : NULL;

        CHECK(scale && loop && end && scale < loop);
        CHECK(!again || again > end);     /* no scaling after the loop */
    }
}

static void test_time_text(void)
{
    char buf[32];
    fb_ms_t ms;

    CHECK(strcmp(FB_FormatTimeMs(buf, sizeof(buf), 65000, '.'), "1:05") == 0);
    CHECK(strcmp(FB_FormatTimeMs(buf, sizeof(buf), 65500, ','), "1:05,5") == 0);
    CHECK(strcmp(FB_FormatTimeMs(buf, sizeof(buf), 65250, '.'), "1:05.25") == 0);
    CHECK(strcmp(FB_FormatTimeMs(buf, sizeof(buf), 65004, '.'), "1:05.004") == 0);
    CHECK(strcmp(FB_FormatTimeMs(buf, sizeof(buf), 3725125, '.'),
                 "1:02:05.125") == 0);
    CHECK(strcmp(FB_FormatTimeMs(buf, sizeof(buf), 0, '.'), "0:00") == 0);

    CHECK(FB_ParseTime("1:05.5", ',', &ms) == 0 && ms == 65500);
    CHECK(FB_ParseTime(" 1:05,25 ", ',', &ms) == 0 && ms == 65250);
    CHECK(FB_ParseTime("65.5", '.', &ms) == 0 && ms == 65500);
    CHECK(FB_ParseTime("12s", '.', &ms) == 0 && ms == 12000);
    CHECK(FB_ParseTime(".5", '.', &ms) == 0 && ms == 500);
    CHECK(FB_ParseTime("1:02:05", '.', &ms) == 0 && ms == 3725000);
    CHECK(FB_ParseTime("0:00.0019", '.', &ms) == 0 && ms == 1);
    CHECK(FB_ParseTime("1:75", '.', &ms) == -1);
    CHECK(FB_ParseTime("", '.', &ms) == -1);
    CHECK(FB_ParseTime("abc", '.', &ms) == -1);
    CHECK(FB_ParseTime("1:", '.', &ms) == -1);
    CHECK(FB_ParseTime(":30", '.', &ms) == -1);
    CHECK(FB_ParseTime("1:2:3:4", '.', &ms) == -1);
    CHECK(FB_ParseTime("1:05s", '.', &ms) == -1);
    /* What's written reads back the same. */
    FB_FormatTimeMs(buf, sizeof(buf), 3725125, ',');
    CHECK(FB_ParseTime(buf, ',', &ms) == 0 && ms == 3725125);
}


/* ---- Media cache --------------------------------------------------------- */

#define CACHE_DIR "build/host"

static void write_bytes(const char *path, const unsigned char *p, size_t n)
{
    FILE *f = fopen(path, "wb");

    if (f) {
        fwrite(p, 1, n, f);
        fclose(f);
    }
}

static int exists(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f)
        fclose(f);
    return f != NULL;
}

static void test_cache_keys(void)
{
    static unsigned char big[300 * 1024];
    char k1[FB_KEY_LEN], k2[FB_KEY_LEN];
    size_t i;

    for (i = 0; i < sizeof(big); i++)
        big[i] = (unsigned char)(i * 7 + (i >> 9));

    /* The same bytes give the same key, in memory and from a file. */
    FB_CacheKeyData(big, sizeof(big), k1);
    CHECK(strlen(k1) == 16);
    write_bytes(CACHE_DIR "/key-big.bin", big, sizeof(big));
    CHECK(FB_CacheKey(CACHE_DIR "/key-big.bin", k2) == 0);
    CHECK(strcmp(k1, k2) == 0);

    /* Small files are hashed whole. */
    FB_CacheKeyData(big, 1000, k1);
    write_bytes(CACHE_DIR "/key-small.bin", big, 1000);
    CHECK(FB_CacheKey(CACHE_DIR "/key-small.bin", k2) == 0);
    CHECK(strcmp(k1, k2) == 0);

    /* A change in the start, middle or end, or in length, changes it. */
    FB_CacheKeyData(big, sizeof(big), k1);
    big[10] ^= 1;
    FB_CacheKeyData(big, sizeof(big), k2);
    CHECK(strcmp(k1, k2) != 0);
    big[10] ^= 1;
    big[sizeof(big) / 2] ^= 1;
    FB_CacheKeyData(big, sizeof(big), k2);
    CHECK(strcmp(k1, k2) != 0);
    big[sizeof(big) / 2] ^= 1;
    big[sizeof(big) - 1] ^= 1;
    FB_CacheKeyData(big, sizeof(big), k2);
    CHECK(strcmp(k1, k2) != 0);
    big[sizeof(big) - 1] ^= 1;
    FB_CacheKeyData(big, sizeof(big) - 1, k2);
    CHECK(strcmp(k1, k2) != 0);

    CHECK(FB_CacheKey(CACHE_DIR "/no-such-file", k2) == -1);
    CHECK(k2[0] == '\0');
}

static FBSource cache_source(const char *path, const char *key, int w, int h)
{
    FBSource s;

    memset(&s, 0, sizeof(s));
    strcpy(s.path, path);
    strcpy(s.key, key);
    s.duration = 12000;
    s.has_video = 1;
    s.has_audio = 1;
    FB_SourceSetVideo(&s, w, h, 1, 1, 25, 1);
    return s;
}

static void test_cache_rules(void)
{
    static FBCache c;
    FBSource s = cache_source("a.mp4", "0123456789abcdef", 1920, 1080);
    char recipe[64];

    FB_CacheInit(&c, CACHE_DIR, 0);
    CHECK(c.proxy_width == FB_PROXY_WIDTH);
    FB_CacheRecipe(&c, FB_CACHE_PROXY, recipe, sizeof(recipe));
    CHECK(strcmp(recipe, "proxy1-320") == 0);   /* the preview's width */
    FB_CacheInit(&c, CACHE_DIR, 33);
    CHECK(c.proxy_width == FB_PROXY_MIN_WIDTH);
    FB_CacheInit(&c, CACHE_DIR, 481);
    CHECK(c.proxy_width == 480);

    CHECK(FB_CacheWantsProxy(&c, &s));
    s.intra = 1;                       /* full HD MJPEG: still worth it */
    CHECK(FB_CacheWantsProxy(&c, &s));
    FB_SourceSetVideo(&s, 720, 576, 64, 45, 25, 1);   /* DV */
    CHECK(!FB_CacheWantsProxy(&c, &s));
    s.intra = 0;                       /* small H.264: yes, for seeking */
    CHECK(FB_CacheWantsProxy(&c, &s));
    s.still = 1;
    CHECK(!FB_CacheWantsProxy(&c, &s));
    s.still = 0;
    s.has_video = 0;                   /* music */
    CHECK(!FB_CacheWantsProxy(&c, &s));
    s.has_video = 1;
    s.key[0] = '\0';                   /* couldn't be read */
    CHECK(!FB_CacheWantsProxy(&c, &s));

    /* Auto is the preview's width on every machine: wider proxies are
     * only scaled down again (measured: 40 % more work at 480). */
    CHECK(FB_CacheAutoWidth(1800, 2) == FB_PREVIEW_WIDTH);   /* X1000 */
    CHECK(FB_CacheAutoWidth(1200, 2) == FB_PREVIEW_WIDTH);   /* A1222 */
    CHECK(FB_CacheAutoWidth(800, 1) == FB_PREVIEW_WIDTH);    /* G4 */
    CHECK(FB_CacheAutoWidth(0, 0) == FB_PROXY_WIDTH);
}

/* Thumbnail strips: steps, names, the queue and the job. */
static void test_thumb_strips(void)
{
    static FBCache c;
    static FBJob job;
    FBSource s = cache_source("Work:a.mp4", "00000000strip001", 1920, 1080);
    FBSource pic = cache_source("Work:p.png", "00000000strip002", 640, 480);
    char recipe[64], path[FB_PATH_LEN];
    int t;

    /* A second a frame, fewer for long files, never over the limit. */
    CHECK(FB_ThumbStep(0) == 1000);
    CHECK(FB_ThumbStep(1200000) == 1000);      /* 20 minutes */
    CHECK(FB_ThumbStep(1200001) == 2000);
    CHECK(FB_ThumbStep(3600000) == 3000);      /* an hour */
    CHECK(3600000 / FB_ThumbStep(3600000) <= FB_THUMB_MAX_FRAMES);
    /* The nearest frame. */
    CHECK(FB_ThumbIndex(1000, 0) == 0);
    CHECK(FB_ThumbIndex(1000, -5) == 0);
    CHECK(FB_ThumbIndex(1000, 499999) == 0);
    CHECK(FB_ThumbIndex(1000, 500000) == 1);
    CHECK(FB_ThumbIndex(2000, 5000000) == 3);

    FB_CacheInit(&c, CACHE_DIR, 0);
    CHECK(c.thumb_w == FB_THUMB_W && c.thumb_h == FB_THUMB_H);
    FB_CacheRecipe(&c, FB_CACHE_THUMBS, recipe, sizeof(recipe));
    CHECK(strcmp(recipe, "thumbs1-46x27") == 0);
    CHECK(FB_CachePath(&c, s.key, FB_CACHE_THUMBS, 1, path,
                       sizeof(path)) == 0);
    CHECK(strcmp(path, CACHE_DIR "/00000000strip001.thumbs1-46x27-part.rgb")
          == 0);
    FB_CacheSetThumbSize(&c, 62, 35);
    FB_CacheRecipe(&c, FB_CACHE_THUMBS, recipe, sizeof(recipe));
    CHECK(strcmp(recipe, "thumbs1-62x35") == 0);
    FB_CacheSetThumbSize(&c, 0, 35);           /* ignored */
    CHECK(c.thumb_w == 62);
    FB_CacheSetThumbSize(&c, FB_THUMB_W, FB_THUMB_H);

    /* Stills get one too; sound and unreadable files don't. */
    pic.still = 1;
    CHECK(FB_CacheWantsThumbs(&c, &s) && FB_CacheWantsThumbs(&c, &pic));
    s.has_video = 0;
    CHECK(!FB_CacheWantsThumbs(&c, &s));
    s.has_video = 1;
    s.key[0] = '\0';
    CHECK(FB_CacheRequestThumbs(&c, &s) == FB_CACHE_NONE);
    strcpy(s.key, "00000000strip001");

    /* Proxy first; the strip waits for it and is made from it. */
    CHECK(FB_CacheRequest(&c, &s) == FB_CACHE_QUEUED);
    CHECK(FB_CacheRequestThumbs(&c, &s) == FB_CACHE_QUEUED);
    CHECK(FB_CacheRequestThumbs(&c, &s) == FB_CACHE_QUEUED);   /* once */
    CHECK(FB_CacheRequestThumbs(&c, &pic) == FB_CACHE_QUEUED);
    CHECK(c.num_tasks == 3);
    t = FB_CacheNextJob(&c, &job);
    CHECK(t == 0 && c.tasks[0].kind == FB_CACHE_PROXY);
    /* One job at a time: nothing else starts while the proxy is made. */
    CHECK(FB_CacheNextJob(&c, &job) == -1);
    {
        char proxy[FB_PATH_LEN];

        FB_CachePath(&c, s.key, FB_CACHE_PROXY, 1, proxy, sizeof(proxy));
        write_bytes(proxy, (const unsigned char *)"x", 1);
        CHECK(FB_CacheFinish(&c, 1) == FB_CACHE_READY);
    }
    t = FB_CacheNextJob(&c, &job);
    CHECK(t == 1 && c.tasks[1].kind == FB_CACHE_THUMBS);
    CHECK(strstr(job.inputs[0], "00000000strip001.proxy1-320.mov") != NULL);
    CHECK(strcmp(job.filtergraph,
                 "[0:v]fps=fps=1000/1000:start_time=0:round=up,"
                 "scale=46:27:force_original_aspect_ratio=increase:"
                 "flags=fast_bilinear,crop=46:27,setsar=1,format=argb[outv]")
          == 0);
    CHECK(strcmp(job.video_out, "outv") == 0 && job.audio_out[0] == '\0');
    CHECK(strcmp(job.output.vcodec, "rawvideo") == 0);
    CHECK(strstr(job.output.path, "thumbs1-46x27-part.rgb") != NULL);
    CHECK(job.length == 12000);
    FB_CacheAbort(&c);

    /* A still: no fps, read from the file itself. And pixels that
     * aren't square are made square first. */
    c.tasks[1].state = FB_CACHE_READY;
    t = FB_CacheNextJob(&c, &job);
    CHECK(t == 2);
    CHECK(strcmp(job.inputs[0], "Work:p.png") == 0);
    CHECK(strncmp(job.filtergraph, "[0:v]scale=46:27:", 17) == 0);
    FB_CacheAbort(&c);
    c.tasks[2].state = FB_CACHE_READY;
    {
        FBCacheTask task = c.tasks[0];

        task.kind = FB_CACHE_THUMBS;
        strcpy(task.key, "00000000strip003");
        FB_SourceSetVideo(&task.source, 720, 576, 64, 45, 25, 1);
        CHECK(FB_CacheBuildJob(&c, &task, &job) == 0);
        CHECK(strstr(job.filtergraph, "round=up,scale=iw*sar:ih,scale=46")
              != NULL);
    }

    /* Dropping one kind leaves the others. */
    c.tasks[0].state = c.tasks[1].state = c.tasks[2].state = FB_CACHE_QUEUED;
    FB_CacheDrop(&c, FB_CACHE_PROXY);
    CHECK(c.num_tasks == 2 && c.tasks[0].kind == FB_CACHE_THUMBS &&
          c.tasks[1].kind == FB_CACHE_THUMBS);
    /* With its proxy gone, a strip doesn't wait any more. */
    CHECK(FB_CacheNextJob(&c, &job) == 0);
    FB_CacheAbort(&c);
    remove(CACHE_DIR "/00000000strip001.proxy1-320.mov");
}

/* The thumbnail store, with hooks counting what it does. */
static int ThumbLocks, ThumbWakes;
static void thumb_lock(void *d) { (void)d; ThumbLocks++; }
static void thumb_unlock(void *d) { (void)d; ThumbLocks--; }
static void thumb_wake(void *d) { (void)d; ThumbWakes++; }

static void test_thumb_store(void)
{
    static FBCache c;
    static FBThumbStore st;
    static unsigned char strip[5 * 4 * 3 * 4];
    const unsigned char *px;
    char path[FB_PATH_LEN];
    const char *key = "00000000store001";
    int i;

    FB_CacheInit(&c, CACHE_DIR, 0);
    FB_CacheSetThumbSize(&c, 4, 3);
    /* Five frames of 4 x 3, every byte of frame k is k. */
    for (i = 0; i < (int)sizeof(strip); i++)
        strip[i] = (unsigned char)(i / (4 * 3 * 4));
    FB_CachePath(&c, key, FB_CACHE_THUMBS, 0, path, sizeof(path));
    remove(path);

    memset(&st, 0, sizeof(st));
    st.lock = thumb_lock;
    st.unlock = thumb_unlock;
    st.wake = thumb_wake;
    CHECK(FB_ThumbsInit(&st, 4, 3, 2) == 0);
    CHECK(st.lock == thumb_lock);              /* hooks kept */

    CHECK(FB_ThumbsLock(&st, "", 0, &px) == FB_THUMB_NONE);
    /* A miss is noted once, and wakes the loader once. */
    CHECK(FB_ThumbsLock(&st, key, 2, &px) == FB_THUMB_WAIT && !px);
    CHECK(FB_ThumbsLock(&st, key, 2, &px) == FB_THUMB_WAIT);
    CHECK(FB_ThumbsWanted(&st) == 1 && ThumbWakes == 1);
    CHECK(ThumbLocks == 0);
    /* No strip yet: it's missing, and lookups wait without asking. */
    CHECK(FB_ThumbsLoad(&st, &c) == 0);
    CHECK(FB_ThumbsLock(&st, key, 2, &px) == FB_THUMB_WAIT);
    CHECK(FB_ThumbsWanted(&st) == 0);

    /* It's made: reset, and the lookups read it. */
    write_bytes(path, strip, sizeof(strip));
    FB_ThumbsReset(&st, key);
    CHECK(FB_ThumbsLock(&st, key, 2, &px) == FB_THUMB_WAIT);
    CHECK(FB_ThumbsLock(&st, key, 99, &px) == FB_THUMB_WAIT);
    CHECK(FB_ThumbsLoad(&st, &c) == 2);
    CHECK(FB_ThumbsLock(&st, key, 2, &px) == FB_THUMB_HIT);
    CHECK(px && px[0] == 2 && px[4 * 3 * 4 - 1] == 2);
    CHECK(ThumbLocks == 1);                    /* held while drawing */
    FB_ThumbsUnlock(&st);
    CHECK(ThumbLocks == 0);
    /* Past the end is the last frame. */
    CHECK(FB_ThumbsLock(&st, key, 99, &px) == FB_THUMB_HIT && px[0] == 4);
    FB_ThumbsUnlock(&st);

    /* Two slots: a third thumbnail pushes out the one used longest ago
     * (frame 2, as 4 was looked at last). */
    CHECK(FB_ThumbsLock(&st, key, 0, &px) == FB_THUMB_WAIT);
    CHECK(FB_ThumbsLoad(&st, &c) == 1);
    CHECK(FB_ThumbsLock(&st, key, 0, &px) == FB_THUMB_HIT && px[0] == 0);
    FB_ThumbsUnlock(&st);
    CHECK(FB_ThumbsLock(&st, key, 4, &px) == FB_THUMB_HIT);
    FB_ThumbsUnlock(&st);
    CHECK(FB_ThumbsLock(&st, key, 2, &px) == FB_THUMB_WAIT);

    /* Failed: no more waiting. */
    FB_ThumbsSetNone(&st, key);
    CHECK(FB_ThumbsLock(&st, key, 0, &px) == FB_THUMB_NONE);
    CHECK(ThumbLocks == 0);
    FB_ThumbsFree(&st);
    CHECK(FB_ThumbsLock(&st, key, 0, &px) == FB_THUMB_NONE);
    remove(path);
}

static void test_cache_queue(void)
{
    static FBCache c;
    static FBJob job, back;
    static char text[FB_FILTERGRAPH_LEN + 4096];
    FBSource a = cache_source("Work:Video/a.mp4", "00000000000000aa", 1920, 1080);
    FBSource b = cache_source("Work:Video/b.mp4", "00000000000000bb", 480, 270);
    char path[FB_PATH_LEN];
    int t;

    b.has_audio = 0;
    remove(CACHE_DIR "/00000000000000aa.proxy1-640.mov");
    remove(CACHE_DIR "/00000000000000bb.proxy1-640.mov");
    FB_CacheInit(&c, CACHE_DIR, 640);

    CHECK(FB_CachePath(&c, a.key, FB_CACHE_PROXY, 1, path, sizeof(path)) == 0);
    CHECK(strcmp(path, CACHE_DIR "/00000000000000aa.proxy1-640-part.mov") == 0);

    /* Asking twice queues it once. */
    CHECK(FB_CacheRequest(&c, &a) == FB_CACHE_QUEUED);
    CHECK(FB_CacheRequest(&c, &a) == FB_CACHE_QUEUED);
    CHECK(FB_CacheRequest(&c, &b) == FB_CACHE_QUEUED);
    CHECK(c.num_tasks == 2 && FB_CachePending(&c) == 2);

    /* The job: scaled down, MJPEG and PCM, into the partial file. */
    t = FB_CacheNextJob(&c, &job);
    CHECK(t == 0);
    CHECK(FB_CacheNextJob(&c, &back) == -1);   /* one at a time */
    CHECK(FB_CacheRequest(&c, &a) == FB_CACHE_BUSY);
    CHECK(job.num_inputs == 1 && strcmp(job.inputs[0], a.path) == 0);
    CHECK(strcmp(job.filtergraph,
                 "[0:v]scale=640:-2:flags=fast_bilinear[outv];"
                 "[0:a]anull[outa]") == 0);
    CHECK(strcmp(job.output.vcodec, "mjpeg") == 0);
    CHECK(strcmp(job.output.acodec, "pcm_s16le") == 0);
    CHECK(strcmp(job.output.path, path) == 0);
    CHECK(job.length == 12000);
    CHECK(FB_JobWrite(&job, text, sizeof(text)) > 0);
    CHECK(FB_JobParse(&back, text) == 0);
    CHECK(strcmp(back.filtergraph, job.filtergraph) == 0);

    /* Finished: the partial file becomes the real one. */
    write_bytes(path, (const unsigned char *)"x", 1);
    CHECK(FB_CacheFinish(&c, 1) == FB_CACHE_READY);
    CHECK(!exists(path));
    CHECK(FB_CacheFind(&c, a.key, FB_CACHE_PROXY, path, sizeof(path)));
    CHECK(strcmp(path, CACHE_DIR "/00000000000000aa.proxy1-640.mov") == 0);
    CHECK(exists(CACHE_DIR "/00000000000000aa.info"));
    CHECK(FB_CacheRequest(&c, &a) == FB_CACHE_READY);
    CHECK(FB_CacheState(&c, a.key, FB_CACHE_PROXY) == FB_CACHE_READY);

    /* Another width is another file. */
    {
        static FBCache small;

        FB_CacheInit(&small, CACHE_DIR, 320);
        CHECK(!FB_CacheFind(&small, a.key, FB_CACHE_PROXY, NULL, 0));
    }

    /* A small one isn't scaled; no sound, no audio chain. Aborting puts
     * it back in the queue; failing doesn't retry it. */
    t = FB_CacheNextJob(&c, &job);
    CHECK(t == 1);
    CHECK(strcmp(job.filtergraph, "[0:v]null[outv]") == 0);
    CHECK(job.audio_out[0] == '\0');
    FB_CacheAbort(&c);
    CHECK(FB_CacheState(&c, b.key, FB_CACHE_PROXY) == FB_CACHE_QUEUED);
    CHECK(FB_CacheNextJob(&c, &job) == 1);
    CHECK(FB_CacheFinish(&c, 0) == FB_CACHE_FAILED);
    CHECK(FB_CacheRequest(&c, &b) == FB_CACHE_FAILED);
    CHECK(FB_CachePending(&c) == 0);
    CHECK(FB_CacheNextJob(&c, &job) == -1);

    /* A rendered piece, made outside the queue: committed, or not. */
    {
        char part[FB_PATH_LEN];

        CHECK(FB_CachePath(&c, "00000000000000ee", FB_CACHE_RENDER, 1, part,
                           sizeof(part)) == 0);
        CHECK(strcmp(part, CACHE_DIR "/00000000000000ee.render1-320-part.mov")
              == 0);
        write_bytes(part, (const unsigned char *)"x", 1);
        CHECK(FB_CacheCommit(&c, "00000000000000ee", FB_CACHE_RENDER, 1) == 0);
        CHECK(FB_CacheFind(&c, "00000000000000ee", FB_CACHE_RENDER, NULL, 0));
        CHECK(!exists(part));
        write_bytes(part, (const unsigned char *)"x", 1);
        CHECK(FB_CacheCommit(&c, "00000000000000ef", FB_CACHE_RENDER, 0) != 0);
        remove(CACHE_DIR "/00000000000000ee.render1-320.mov");
    }

    /* A copy that appears meanwhile (from a render server) isn't made. */
    {
        FBSource d = cache_source("Work:d.mp4", "00000000000000dd", 1280, 720);

        remove(CACHE_DIR "/00000000000000dd.proxy1-640.mov");
        CHECK(FB_CacheRequest(&c, &d) == FB_CACHE_QUEUED);
        write_bytes(CACHE_DIR "/00000000000000dd.proxy1-640.mov",
                    (const unsigned char *)"x", 1);
        CHECK(FB_CacheNextJob(&c, &job) == -1);
        CHECK(FB_CacheState(&c, d.key, FB_CACHE_PROXY) == FB_CACHE_READY);
    }
}

static void test_move_clips(void)
{
    FBTimeline tl;
    int a, b, m, i;

    FB_TimelineInit(&tl);
    a = FB_TimelineAddSource(&tl, "a.mp4", 10000, 1, 1);
    b = FB_TimelineAddSource(&tl, "b.mp4", 10000, 1, 0);
    m = FB_TimelineAddSource(&tl, "music.mp3", 60000, 0, 1);
    /* Video 1 (track 0): A 0-2, B 2-5, A 5-6. Audio 1 (track 1): music. */
    FB_TimelineAppendToTrack(&tl, 0, a, 0, 2000);
    FB_TimelineAppendToTrack(&tl, 0, b, 0, 3000);
    FB_TimelineAppendToTrack(&tl, 0, a, 4000, 5000);
    FB_TimelineAppendToTrack(&tl, 1, m, 0, 20000);
    tl.clips[0].volume = 0.5;
    FB_ClipAddEffect(&tl.clips[0], "fade");

    /* Where a 1 s clip dropped at a time goes: there, if nothing is in
     * the way; else in front of the first clip whose middle is after its
     * middle, or after the last clip. */
    {
        int ins;

        CHECK(FB_TimelineDropPoint(&tl, 0, 0, 1000, -1, &ins) == 0 && ins);
        CHECK(FB_TimelineDropPoint(&tl, 0, 1000, 1000, -1, &ins) == 2000 &&
              ins);
        CHECK(FB_TimelineDropPoint(&tl, 0, 3500, 1000, -1, &ins) == 5000 &&
              ins);
        CHECK(FB_TimelineDropPoint(&tl, 0, 5800, 1000, -1, &ins) == 6000 &&
              !ins);                                /* after the last */
        CHECK(FB_TimelineDropPoint(&tl, 0, 9000, 1000, -1, &ins) == 9000 &&
              !ins);                                /* free: a gap */
        CHECK(FB_TimelineDropPoint(&tl, 0, 6000, 1000, -1, &ins) == 6000 &&
              !ins);
        CHECK(FB_TimelineDropPoint(&tl, 5, 7000, 1000, -1, &ins) == 7000 &&
              !ins);                                /* no clips */
        /* With B (2-5) taken out, the last A moves up to 2-3: 3-4 is
         * free, and where it was (5-6) is free too. */
        CHECK(FB_TimelineStartWithout(&tl, 2, 1) == 2000);
        CHECK(FB_TimelineStartWithout(&tl, 0, 1) == 0);
        CHECK(FB_TimelineStartWithout(&tl, 3, 1) == 0);   /* other track */
        CHECK(FB_TimelineDropPoint(&tl, 0, 3000, 1000, 1, &ins) == 3000 &&
              !ins);
        CHECK(FB_TimelineDropPoint(&tl, 0, 2200, 1000, 1, &ins) == 3000 &&
              !ins);                                /* in the way: after */
    }

    /* The last clip, dragged further out, keeps a gap in front of it. */
    i = FB_TimelineMoveClipTo(&tl, 2, 0, 8000);
    CHECK(i == 2 && tl.clips[2].start == 8000);
    CHECK(FB_TimelineMoveClipTo(&tl, 2, 0, 5000) == 2);   /* and back */
    CHECK(tl.clips[2].start == 5000);

    /* The first clip (A 0-2, with a fade) to the end of its track:
     * B 0-3, A 3-4, A 4-6. */
    i = FB_TimelineMoveClipTo(&tl, 0, 0, 4000);
    CHECK(i == 2);
    CHECK(tl.clips[0].source == b && tl.clips[0].start == 0);
    CHECK(tl.clips[1].start == 3000 && tl.clips[1].in == 4000);
    CHECK(tl.clips[2].start == 4000 && tl.clips[2].in == 0 &&
          tl.clips[2].volume == 0.5 && tl.clips[2].num_effects == 1);
    CHECK(FB_TimelineTrackEnd(&tl, 0) == 6000);

    /* Back to the front: A 0-2, B 2-5, A 5-6. */
    i = FB_TimelineMoveClipTo(&tl, 2, 0, 0);
    CHECK(i == 0 && tl.clips[0].start == 0 && tl.clips[0].num_effects == 1);
    CHECK(tl.clips[1].start == 2000 && tl.clips[2].start == 5000);

    /* A video clip can't go on the music track; sound can't go on video. */
    CHECK(FB_TimelineMoveClipTo(&tl, 1, 1, 0) == -1);
    CHECK(FB_TimelineMoveClipTo(&tl, 3, 0, 0) == -1);
    CHECK(tl.num_clips == 4);

    /* B to a new track, at 1.5 s: Video 1 closes up to A 0-2, A 2-3. */
    i = FB_TimelineMoveClipTo(&tl, 1, tl.num_tracks, 1500);
    CHECK(tl.num_tracks == 3 && tl.tracks[2].kind == FB_TRACK_VIDEO);
    CHECK(i == 3 && tl.clips[i].track == 2 && tl.clips[i].start == 1500);
    CHECK(tl.clips[1].start == 2000 && FB_TimelineTrackEnd(&tl, 0) == 3000);

    /* And onto Video 1 between the two A clips. */
    i = FB_TimelineMoveClipTo(&tl, 3, 0, 500);   /* its middle: 2 s */
    CHECK(i == 1 && tl.clips[1].source == b && tl.clips[1].start == 2000);
    CHECK(tl.clips[2].start == 5000 && FB_TimelineTrackEnd(&tl, 2) == 0);

    /* Nothing to move, or no such track. */
    CHECK(FB_TimelineMoveClipTo(&tl, 9, 0, 0) == -1);
    CHECK(FB_TimelineMoveClipTo(&tl, 0, 7, 0) == -1);
}

/* The tracks go on after the last clip, and drags near an edge scroll. */
static void test_view_room(void)
{
    static FBTimeline tl;
    FBTimeView v;
    int k, s;

    FB_TimelineInit(&tl);
    CHECK(FB_TimelineViewLength(&tl) == 0);

    /* A still picture made 15.6 s long and an 11.9 s video: the room
     * after the end is the longest clip, so the video can go after it. */
    k = FB_TimelineAddSource(&tl, "k.jpg", 0, 1, 0);
    tl.sources[k].still = 1;
    s = FB_TimelineAddSource(&tl, "s.mp4", 11895, 1, 1);
    FB_TimelineAppendToTrack(&tl, 0, k, 0, 15600);
    FB_TimelineAppendToTrack(&tl, 1, s, 0, 11895);
    CHECK(FB_TimelineViewLength(&tl) == 15600 + 15600);

    /* Short: at least FB_VIEW_MIN_ROOM. Long: half again. */
    FB_TimelineInit(&tl);
    s = FB_TimelineAddSource(&tl, "s.mp4", 60000, 1, 1);
    FB_TimelineAppendToTrack(&tl, 0, s, 0, 3000);
    CHECK(FB_TimelineViewLength(&tl) == 3000 + FB_VIEW_MIN_ROOM);
    FB_TimelineAppendToTrack(&tl, 0, s, 0, 20000);
    FB_TimelineAppendToTrack(&tl, 0, s, 0, 20000);
    CHECK(FB_TimelineViewLength(&tl) == 43000 + 21500);

    /* Dragging something to end past the room makes the view longer. */
    FB_ViewInit(&v, 400);
    FB_ViewSetLength(&v, FB_US(20000));
    FB_ViewZoomToFit(&v);
    CHECK(!FB_ViewMakeRoom(&v, FB_US(5000)));
    CHECK(FB_ViewMakeRoom(&v, FB_US(15000)));
    CHECK(v.length == FB_US(15000 + FB_VIEW_MIN_ROOM));
    CHECK(FB_ViewXToTime(&v, 10000) == v.length);   /* still clamped */

    /* Zoomed in, the pointer near or past the right edge scrolls right,
     * further when further out; near the left edge it scrolls back. */
    FB_ViewSetZoom(&v, v.min_per_px * 4, 0);
    FB_ViewSetStart(&v, 0);
    CHECK(!FB_ViewEdgeScroll(&v, 200, FB_VIEW_EDGE_PX));
    CHECK(FB_ViewEdgeScroll(&v, 399, FB_VIEW_EDGE_PX));
    {
        fb_us_t a = v.start, b;

        CHECK(a > 0);
        CHECK(FB_ViewEdgeScroll(&v, 500, FB_VIEW_EDGE_PX));
        b = v.start - a;
        CHECK(b > a);
        CHECK(FB_ViewEdgeScroll(&v, 0, FB_VIEW_EDGE_PX));
        CHECK(v.start < a + b);
    }
    FB_ViewSetStart(&v, 0);
    CHECK(!FB_ViewEdgeScroll(&v, 0, FB_VIEW_EDGE_PX));   /* at the start */
}

/* A stretch of the edit for previews from the playhead, and jumps into
 * inputs instead of decoding up to the first frame used. */
static void test_window_and_seek(void)
{
    static FBTimeline tl, w;
    static FBJob job, back;
    static char text[FB_FILTERGRAPH_LEN + 4096];
    FBOutput out;
    int k, s, m;

    FB_TimelineInit(&tl);
    k = FB_TimelineAddSource(&tl, "k.jpg", 0, 1, 0);
    FB_SourceSetVideo(&tl.sources[k], 800, 600, 1, 1, 0, 0);
    tl.sources[k].still = 1;
    s = FB_TimelineAddSource(&tl, "s.mp4", 20000, 1, 1);
    FB_SourceSetVideo(&tl.sources[s], 640, 360, 1, 1, 25, 1);
    m = FB_TimelineAddSource(&tl, "m.mpg", 20000, 1, 1);
    FB_SourceSetVideo(&tl.sources[m], 640, 360, 1, 1, 25, 1);
    FB_TimelineAppendToTrack(&tl, 0, k, 0, 15600);    /* 0 - 15.6 s */
    FB_TimelineAppendToTrack(&tl, 1, s, 2000, 11895); /* 0 - 9.895 s */
    FB_TimelineAppendToTrack(&tl, 2, m, 1000, 7000);  /* 0 - 6 s */
    FB_TimelineAppendToTrack(&tl, 2, s, 500, 3500);   /* 6 - 9 s */

    /* 4 s to 9 s: the still and the first video cut at both ends, the
     * second track's two clips cut and kept whole. */
    CHECK(FB_TimelineWindow(&w, &tl, 4000, 5000) == 4);
    CHECK(w.clips[0].start == 0 && w.clips[0].in == 4000 &&
          w.clips[0].out == 9000);
    CHECK(w.clips[1].start == 0 && w.clips[1].in == 6000 &&
          w.clips[1].out == 11000);
    CHECK(w.clips[2].start == 0 && w.clips[2].in == 5000 &&
          w.clips[2].out == 7000);
    CHECK(w.clips[3].start == 2000 && w.clips[3].in == 500 &&
          w.clips[3].out == 3500);
    CHECK(FB_TimelineDuration(&w) == 5000);

    /* Past every clip but the still; past everything; nothing long. */
    CHECK(FB_TimelineWindow(&w, &tl, 12000, 10000) == 1);
    CHECK(FB_TimelineDuration(&w) == 3600);
    CHECK(FB_TimelineWindow(&w, &tl, 16000, 10000) == 0);
    CHECK(FB_TimelineWindow(&w, &tl, 0, 0) == 0);
    /* In place works too. */
    w = tl;
    CHECK(FB_TimelineWindow(&w, &w, 9000, 1000) == 2);

    /* Seeks: each input's earliest in point; none for a still. */
    memset(&out, 0, sizeof(out));
    strcpy(out.path, "out.mp4");
    out.quality = FB_QUALITY_PREVIEW;
    CHECK(FB_TimelineWindow(&w, &tl, 4000, 5000) == 4);
    CHECK(FB_JobBuild(&job, &w, &out) == FB_JOB_OK);
    CHECK(job.num_inputs == 3);
    CHECK(job.seek[0] == 0);       /* k.jpg */
    CHECK(job.seek[1] == 500);     /* s.mp4: min(6000, 500) */
    CHECK(job.seek[2] == 5000);    /* m.mpg */
    CHECK(FB_JobWrite(&job, text, sizeof(text)) > 0);
    CHECK(strstr(text, "SEEK 1 500\n") && strstr(text, "SEEK 2 5000\n"));
    CHECK(!strstr(text, "SEEK 0"));
    CHECK(FB_JobParse(&back, text) == 0);
    CHECK(back.seek[0] == 0 && back.seek[1] == 500 && back.seek[2] == 5000);
    CHECK(FB_JobToCommandLine(&job, text, sizeof(text)) > 0);
    CHECK(strstr(text, "ffmpeg -y -copyts -start_at_zero -i \"k.jpg\" "
                       "-ss 0.500 -i \"s.mp4\" -ss 5.000 -i \"m.mpg\"") != NULL);

    /* A whole edit from the files' starts: no seeks, same command. */
    FB_TimelineInit(&w);
    s = FB_TimelineAddSource(&w, "s.mp4", 20000, 1, 1);
    FB_TimelineAppendToTrack(&w, 0, s, 0, 3000);
    CHECK(FB_JobBuild(&job, &w, &out) == FB_JOB_OK);
    CHECK(FB_JobToCommandLine(&job, text, sizeof(text)) > 0);
    CHECK(strncmp(text, "ffmpeg -y -i ", 13) == 0);

    /* Previews have their sound at the player's rate (44.1 kHz), so it's
     * resampled once at most; exports at 48 kHz. */
    CHECK(FB_JobBuild(&job, &w, &out) == FB_JOB_OK);
    CHECK(strstr(job.filtergraph, "aformat=sample_rates=44100") != NULL);
    CHECK(strstr(job.filtergraph, "48000") == NULL);
    out.quality = FB_QUALITY_FINAL;
    CHECK(FB_JobBuild(&job, &w, &out) == FB_JOB_OK);
    CHECK(strstr(job.filtergraph, "aformat=sample_rates=48000") != NULL);
}

/* Windows cutting into crossfades. */
static void test_window_crossfade(void)
{
    static FBTimeline tl, w;
    static FBJob job;
    FBOutput out;
    int s;

    FB_TimelineInit(&tl);
    s = FB_TimelineAddSource(&tl, "s.mp4", 20000, 1, 1);
    FB_SourceSetVideo(&tl.sources[s], 640, 360, 1, 1, 25, 1);
    FB_TimelineAppendToTrack(&tl, 0, s, 0, 6000);      /* A: 0 - 6 s */
    FB_TimelineAppendToTrack(&tl, 0, s, 8000, 14000);  /* B: 6 - 12 s */
    /* B fades in over A's last 2 s: B now starts at 4 s. */
    CHECK(FB_TimelineSetCrossfade(&tl, 1, 2000, "wipeleft") == 2000);
    CHECK(tl.clips[1].start == 4000 && tl.clips[1].xfade == 2000);

    /* Around the whole crossfade: kept as it is. */
    CHECK(FB_TimelineWindow(&w, &tl, 3000, 5000) == 2);
    CHECK(w.clips[1].start == 1000 && w.clips[1].xfade == 2000);

    /* From the middle of it: 1 s of overlap is left. */
    CHECK(FB_TimelineWindow(&w, &tl, 5000, 5000) == 2);
    CHECK(w.clips[0].start == 0 && w.clips[0].out - w.clips[0].in == 1000);
    CHECK(w.clips[1].start == 0 && w.clips[1].xfade == 1000);

    /* Ending in the middle of it: 0.5 s of overlap is left. */
    CHECK(FB_TimelineWindow(&w, &tl, 0, 4500) == 2);
    CHECK(w.clips[1].start == 4000 && w.clips[1].xfade == 500);

    /* After A: nothing to fade from. */
    CHECK(FB_TimelineWindow(&w, &tl, 6000, 3000) == 1);
    CHECK(w.clips[0].xfade == 0);

    /* Every one of them builds. */
    memset(&out, 0, sizeof(out));
    strcpy(out.path, "out.mp4");
    out.quality = FB_QUALITY_PREVIEW;
    {
        static const fb_ms_t froms[] = { 0, 3000, 4000, 4500, 5000, 5999,
                                         6000, 9000 };
        size_t i;

        for (i = 0; i < sizeof(froms) / sizeof(froms[0]); i++) {
            CHECK(FB_TimelineWindow(&w, &tl, froms[i], 2500) > 0);
            CHECK(FB_JobBuild(&job, &w, &out) == FB_JOB_OK);
            CHECK(job.length == FB_TimelineDuration(&w));
        }
    }
}

/* The preview player's queues and clock. */
static void test_player(void)
{
    static FBPlayer pl;
    static unsigned char pic[16 * 8 * 4 + 64];
    static short snd[FB_PLAY_RATE * 2 * 2], out[FB_PLAY_RATE * 2];
    const FBPlayFrame *f;
    long long t0 = 5000000;   /* the clock in us */
    int i;

    for (i = 0; i < (int)sizeof(pic); i++)
        pic[i] = (unsigned char)i;
    for (i = 0; i < FB_PLAY_RATE * 2 * 2; i++)
        snd[i] = 1000;

    /* Pictures only. */
    CHECK(FB_PlayerInit(&pl, 0) == 0);
    CHECK(!FB_PlayerReady(&pl));
    for (i = 0; i < FB_PLAY_FRAMES; i++)
        CHECK(FB_PlayerPushVideo(&pl, pic, 16, 8, 16 * 4 + 8, i * 40) == 0);
    CHECK(FB_PlayerPushVideo(&pl, pic, 16, 8, 72, 999) == 1);   /* full */
    CHECK(FB_PlayerReady(&pl));
    CHECK(FB_PlayerFrame(&pl, t0) == NULL);                    /* no clock */
    FB_PlayerStart(&pl, t0);
    f = FB_PlayerFrame(&pl, t0);
    CHECK(f && f->pts == 0 && f->width == 16 && f->stride == 64);
    CHECK(f && f->pixels[64] == pic[72]);   /* row 1, stride removed */
    CHECK(FB_PlayerFrame(&pl, t0 + 10000) == NULL);    /* same picture */
    CHECK(FB_PlayerNextDue(&pl, t0 + 10000) == 30);
    /* 130 ms: 40, 80 and 120 are due; 120 is shown, two dropped. */
    f = FB_PlayerFrame(&pl, t0 + 130000);
    CHECK(f && f->pts == 120);
    CHECK(pl.num_dropped == 2 && pl.num_shown == 2);
    CHECK(pl.count == FB_PLAY_FRAMES - 4);
    CHECK(FB_PlayerPushVideo(&pl, pic, 16, 8, 64, 480) == 0);   /* room */
    CHECK(!FB_PlayerFinished(&pl, t0 + 10000000));
    FB_PlayerSetEnded(&pl);
    CHECK(!FB_PlayerFinished(&pl, t0 + 300000));      /* still queued */
    while (FB_PlayerFrame(&pl, t0 + 10000000))
        ;
    CHECK(FB_PlayerFinished(&pl, t0 + 10000000));
    CHECK(!FB_PlayerFinished(&pl, t0 + 500000));    /* 480 still on show */
    /* No sound output: sound is taken and thrown away, never full. */
    for (i = 0; i < 100; i++)
        CHECK(FB_PlayerPushAudio(&pl, snd, FB_PLAY_RATE, i * 1000) == 0);
    CHECK(pl.audio_count == 0);
    FB_PlayerFree(&pl);

    /* Sound: in step with its times. */
    CHECK(FB_PlayerInit(&pl, 1) == 0);
    CHECK(FB_PlayerPushVideo(&pl, pic, 16, 8, 64, 0) == 0);
    CHECK(FB_PlayerPushVideo(&pl, pic, 16, 8, 64, 40) == 0);
    CHECK(FB_PlayerPushVideo(&pl, pic, 16, 8, 64, 80) == 0);
    CHECK(!FB_PlayerReady(&pl));                     /* no sound yet */
    /* 10 ms at 0, then 10 ms at 20 ms: 10 ms of silence between. */
    CHECK(FB_PlayerPushAudio(&pl, snd, 441, 0) == 0);
    CHECK(FB_PlayerPushAudio(&pl, snd, 441, 20) == 0);
    CHECK(pl.audio_count == 3 * 441);
    /* Overlapping by 5 ms: only the new part is added. */
    CHECK(FB_PlayerPushAudio(&pl, snd, 441, 25) == 0);
    CHECK(pl.audio_count == 3 * 441 + 220 || pl.audio_count == 3 * 441 + 221);
    CHECK(FB_PlayerTakeAudio(&pl, out, 441) == 441);
    CHECK(out[0] == 1000 && out[881] == 1000);
    FB_PlayerTakeAudio(&pl, out, 441);               /* the gap: */
    CHECK(out[0] == 0 && out[881] == 0);              /* silent */
    CHECK(FB_PlayerTakeAudio(&pl, out, 4410) > 441);  /* the rest, then */
    CHECK(out[4410 * 2 - 1] == 0);                    /* silence */
    CHECK(pl.audio_count == 0);
    /* Sound that comes too late is dropped, not played late. */
    CHECK(FB_PlayerPushAudio(&pl, snd, 441, 10) == 0);
    CHECK(pl.audio_count == 0);
    /* Sound later than where the output is: silence until it. */
    CHECK(FB_PlayerPushAudio(&pl, snd, 441, 200) == 0);
    CHECK(pl.audio_start == 200LL * FB_PLAY_RATE / 1000);
    /* Read so far: 441 * 2 + 4410 = 5292. The next 4410 are silence up
     * to 8820 (200 ms), then the 441 queued. */
    CHECK(FB_PlayerTakeAudio(&pl, out, 4410) == 441);
    CHECK(out[(8820 - 5292 - 1) * 2] == 0 && out[(8820 - 5292) * 2] == 1000);
    /* Full: two seconds at most. */
    FB_PlayerFree(&pl);
    CHECK(FB_PlayerInit(&pl, 1) == 0);
    CHECK(FB_PlayerPushAudio(&pl, snd, FB_PLAY_RATE, 0) == 0);
    CHECK(FB_PlayerPushAudio(&pl, snd, FB_PLAY_RATE, 1000) == 0);
    CHECK(FB_PlayerPushAudio(&pl, snd, 10, 2000) == 1);
    CHECK(!FB_PlayerReady(&pl));     /* waits for pictures too */
    FB_PlayerFree(&pl);
}

/* Background rendering: pieces, their keys, their jobs. */
static void test_render_pieces(void)
{
    static FBTimeline tl, t2;
    static FBJob job;
    FBOutput out;
    char k0[FB_KEY_LEN], k1[FB_KEY_LEN], k2[FB_KEY_LEN], k[FB_KEY_LEN];
    int v, m, tr;

    FB_TimelineInit(&tl);
    CHECK(FB_RenderPieces(&tl) == 0);
    v = FB_TimelineAddSource(&tl, "v.mp4", 60000, 1, 1);
    FB_SourceSetVideo(&tl.sources[v], 640, 360, 1, 1, 25, 1);
    strcpy(tl.sources[v].key, "000000000000000v");
    m = FB_TimelineAddSource(&tl, "m.mp3", 60000, 0, 1);
    strcpy(tl.sources[m].key, "000000000000000m");
    FB_TimelineAppendToTrack(&tl, 0, v, 0, 12000);      /* 0 - 12 s */
    FB_TimelineAppendToTrack(&tl, 0, v, 30000, 43000);  /* 12 - 25 s */
    tr = FB_TimelineAddTrack(&tl, FB_TRACK_AUDIO);
    FB_TimelineAppendToTrack(&tl, tr, m, 0, 25000);

    /* 25 s: three pieces, the last 5 s. */
    CHECK(FB_RenderPieces(&tl) == 3);
    CHECK(FB_RenderPieceStart(2) == 20000);
    CHECK(FB_RenderPieceLength(&tl, 0) == 10000);
    CHECK(FB_RenderPieceLength(&tl, 2) == 5000);
    CHECK(FB_RenderPieceLength(&tl, 3) == 0);
    CHECK(FB_RenderPieceAt(19999) == 1 && FB_RenderPieceAt(20000) == 2);
    CHECK(FB_RenderPieceAt(-5) == 0);

    /* Keys: the same edit, the same keys; pieces differ from each other. */
    CHECK(FB_RenderPieceKey(&tl, 0, k0) == 0 && strlen(k0) == 16);
    CHECK(FB_RenderPieceKey(&tl, 1, k1) == 0);
    CHECK(FB_RenderPieceKey(&tl, 2, k2) == 0);
    CHECK(strcmp(k0, k1) != 0 && strcmp(k1, k2) != 0);
    CHECK(FB_RenderPieceKey(&tl, 0, k) == 0 && strcmp(k, k0) == 0);
    CHECK(FB_RenderPieceKey(&tl, 3, k) == -1);

    /* Every piece has the whole edit's shape, even one that starts on a
     * picture of another shape. */
    {
        static FBTimeline t3, w3;
        int p3;

        t3 = tl;
        p3 = FB_TimelineAddSource(&t3, "square.png", 0, 1, 0);
        FB_SourceSetVideo(&t3.sources[p3], 600, 600, 1, 1, 0, 0);
        t3.sources[p3].still = 1;
        t3.clips[1].source = p3;            /* 12 - 25 s: the picture */
        t3.clips[1].in = 0;
        t3.clips[1].out = 13000;
        CHECK(FB_TimelineWindow(&w3, &t3, 20000, 5000) == 2);
        {
            int ww, wh, fw, fh;

            FB_TimelineFrameSize(&t3, &fw, &fh);
            FB_TimelineFrameSize(&w3, &ww, &wh);
            CHECK(fw == 640 && fh == 360 && ww == fw && wh == fh);
        }
        CHECK(FB_RenderPieceJob(&job, &t3, 2, "T:p.mov") == FB_JOB_OK);
        CHECK(strstr(job.filtergraph, "320:180") != NULL);
    }

    /* The music goes on after the pictures end: a stretch with sound
     * only is black, the edit's size, and its piece can be rendered. */
    {
        static FBTimeline t4, w4;

        t4 = tl;
        t4.clips[2].out = 32000;              /* music to 32 s */
        CHECK(FB_RenderPieces(&t4) == 4);
        CHECK(FB_TimelineWindow(&w4, &t4, 30000, 2000) == 1);
        memset(&out, 0, sizeof(out));
        strcpy(out.path, "o.mov");
        out.quality = FB_QUALITY_PREVIEW;
        CHECK(FB_JobBuild(&job, &w4, &out) == FB_JOB_OK);
        CHECK(strstr(job.filtergraph, "color=c=0x000000:s=320x180:r=25/1:"
                                      "d=2.000,setsar=1[outv]") != NULL);
        CHECK(strstr(job.filtergraph, "[outa]") != NULL);
        CHECK(job.length == 2000);
        CHECK(FB_RenderPieceKey(&t4, 3, k) == 0);
        CHECK(FB_RenderPieceJob(&job, &t4, 3, "T:p.mov") == FB_JOB_OK);
    }

    /* Trimming the second clip's end (22 s on) changes only piece 2. */
    t2 = tl;
    t2.clips[1].out -= 1000;
    CHECK(FB_RenderPieceKey(&t2, 0, k) == 0 && strcmp(k, k0) == 0);
    CHECK(FB_RenderPieceKey(&t2, 1, k) == 0 && strcmp(k, k1) == 0);
    CHECK(FB_RenderPieceKey(&t2, 2, k) == 0 && strcmp(k, k2) != 0);

    /* A file changed under the same name changes every piece it's in. */
    t2 = tl;
    strcpy(t2.sources[m].key, "00000000000000m2");
    CHECK(FB_RenderPieceKey(&t2, 0, k) == 0 && strcmp(k, k0) != 0);

    /* Rendering it: the piece, preview size, MJPEG and PCM at 44.1 kHz. */
    CHECK(FB_RenderPieceJob(&job, &tl, 1, "T:p.mov") == FB_JOB_OK);
    CHECK(strcmp(job.output.vcodec, "mjpeg") == 0);
    CHECK(strcmp(job.output.acodec, "pcm_s16le") == 0);
    CHECK(job.output.quality == FB_QUALITY_PREVIEW);
    CHECK(job.length == 10000);
    CHECK(strstr(job.filtergraph, "sample_rates=44100") != NULL);
    CHECK(job.seek[0] == 10000);           /* v.mp4 from 10 s */

    /* Playing part of a rendered piece. */
    FB_RenderPlayJob(&job, "C:x.mov", 2500, 7500, 1);
    CHECK(job.num_inputs == 1 && job.seek[0] == 2500 && job.length == 7500);
    CHECK(strcmp(job.filtergraph,
                 "[0:v]trim=start=2.500:duration=7.500,setpts=PTS-STARTPTS"
                 "[outv];[0:a]atrim=start=2.500:duration=7.500,"
                 "asetpts=PTS-STARTPTS[outa]") == 0);
    FB_RenderPlayJob(&job, "C:x.mov", 0, 5000, 0);
    CHECK(job.audio_out[0] == '\0' && !strstr(job.filtergraph, "[0:a]"));

    /* Plans. Nothing rendered: one live step from 3 s to the end. */
    {
        FBPlayStep st[8];
        unsigned char r[3] = { 0, 0, 0 };
        int n = FB_RenderPlan(&tl, 3000, NULL, st, 8);

        CHECK(n == 1 && st[0].piece == -1 && st[0].from == 3000 &&
              st[0].len == 22000);
        /* Piece 1 rendered: live 3-10, piece 1 whole, live 20-25. */
        r[1] = 1;
        n = FB_RenderPlan(&tl, 3000, r, st, 8);
        CHECK(n == 3);
        CHECK(st[0].piece == -1 && st[0].from == 3000 && st[0].len == 7000);
        CHECK(st[1].piece == 1 && st[1].from == 10000 &&
              st[1].len == 10000 && st[1].offset == 0);
        CHECK(st[2].piece == -1 && st[2].from == 20000 && st[2].len == 5000);
        /* From inside a rendered piece: its file from the offset. */
        r[0] = r[2] = 1;
        n = FB_RenderPlan(&tl, 13500, r, st, 8);
        CHECK(n == 2 && st[0].piece == 1 && st[0].offset == 3500 &&
              st[0].len == 6500 && st[1].piece == 2 && st[1].len == 5000);
        /* All rendered from 0: three steps that add up to the edit. */
        n = FB_RenderPlan(&tl, 0, r, st, 8);
        CHECK(n == 3 && st[0].len + st[1].len + st[2].len == 25000);
        /* Room for fewer steps than needed: it stops there. */
        r[1] = 0;
        CHECK(FB_RenderPlan(&tl, 0, r, st, 2) == 2);
    }
}

/* Three picture tracks: each overlay reads what the one before wrote. */
static void test_job_three_tracks(void)
{
    static FBTimeline tl;
    static FBJob job;
    FBOutput out;
    int k, s, m;

    FB_TimelineInit(&tl);
    k = FB_TimelineAddSource(&tl, "k.jpg", 0, 1, 0);
    FB_SourceSetVideo(&tl.sources[k], 800, 600, 1, 1, 0, 0);
    tl.sources[k].still = 1;
    s = FB_TimelineAddSource(&tl, "s.mp4", 11895, 1, 1);
    FB_SourceSetVideo(&tl.sources[s], 640, 360, 1, 1, 25, 1);
    m = FB_TimelineAddSource(&tl, "m.mpg", 10466, 1, 1);
    FB_SourceSetVideo(&tl.sources[m], 720, 480, 8, 9, 30000, 1001);
    CHECK(FB_TimelineAppendToTrack(&tl, 0, k, 0, 5100) >= 0);
    CHECK(FB_TimelineAppendToTrack(&tl, 1, s, 0, 11895) >= 0);
    CHECK(FB_TimelineAppendToTrack(&tl, 2, m, 0, 10466) >= 0);
    CHECK(FB_TimelineAppendToTrack(&tl, 3, s, 0, 3000) >= 0);

    memset(&out, 0, sizeof(out));
    strcpy(out.path, "out.mp4");
    out.quality = FB_QUALITY_PREVIEW;
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    CHECK(strstr(job.filtergraph,
                 "[t0v][t1v]overlay=format=auto:eof_action=pass[c0];") != NULL);
    CHECK(strstr(job.filtergraph,
                 "[c0][t2v]overlay=format=auto:eof_action=pass[c1];") != NULL);
    CHECK(strstr(job.filtergraph,
                 "[c1][t3v]overlay=format=auto:eof_action=pass[outv]") != NULL);
    printf("three tracks: %s\n", job.filtergraph);
}

/* Three clips of 4 s on one track: 0-4, 4-8, 8-12. */
static void three_clips(FBTimeline *tl)
{
    int a;

    FB_TimelineInit(tl);
    a = FB_TimelineAddSource(tl, "a.mp4", 20000, 1, 1);
    FB_SourceSetVideo(&tl->sources[a], 320, 240, 1, 1, 25, 1);
    FB_TimelineAppendToTrack(tl, 0, a, 0, 4000);
    FB_TimelineAppendToTrack(tl, 0, a, 4000, 8000);
    FB_TimelineAppendToTrack(tl, 0, a, 8000, 12000);
}

static void test_crossfades(void)
{
    static FBTimeline tl;
    int inserted;

    three_clips(&tl);
    /* The first clip has nothing to fade from. */
    CHECK(FB_TimelineMaxCrossfade(&tl, 0) == 0);
    CHECK(FB_TimelineSetCrossfade(&tl, 0, 1000, NULL) == -1);
    CHECK(FB_TimelineMaxCrossfade(&tl, 1) == 4000);

    /* 1 s into clip 1: it and clip 2 move 1 s earlier. */
    CHECK(FB_TimelineSetCrossfade(&tl, 1, 1000, NULL) == 1000);
    CHECK(tl.clips[1].start == 3000 && tl.clips[2].start == 7000);
    CHECK(FB_TimelineDuration(&tl) == 11000);
    CHECK(strcmp(FB_ClipCrossfadeKind(&tl.clips[1]), "fade") == 0);
    /* Clip 1 has 3 s left after its own crossfade. */
    CHECK(FB_TimelineMaxCrossfade(&tl, 2) == 3000);
    CHECK(FB_TimelineSetCrossfade(&tl, 2, 9000, "wipeleft") == 3000);
    CHECK(tl.clips[2].start == 4000);
    CHECK(strcmp(FB_ClipCrossfadeKind(&tl.clips[2]), "wipeleft") == 0);
    CHECK(FB_TimelineSetCrossfade(&tl, 2, 500, NULL) == 500);
    CHECK(tl.clips[2].start == 6500);
    CHECK(strcmp(tl.clips[2].xfade_kind, "wipeleft") == 0);
    /* Only names go into the filtergraph. */
    CHECK(FB_TimelineSetCrossfade(&tl, 2, 500, "fade:x=1") == -1);
    CHECK(!FB_CrossfadeKindValid("Fade") && FB_CrossfadeKindValid(""));

    /* Removing it puts the clips back. */
    CHECK(FB_TimelineSetCrossfade(&tl, 2, 0, NULL) == 0);
    CHECK(tl.clips[2].start == 7000 && tl.clips[2].xfade_kind[0] == '\0');

    /* Trimming the clip before keeps the overlap. */
    CHECK(FB_TimelineTrimClip(&tl, 0, 0, 3000) == 0);
    CHECK(tl.clips[1].start == 2000 && tl.clips[1].xfade == 1000);
    CHECK(tl.clips[2].start == 6000);
    /* ...and shortens it when it no longer fits. */
    CHECK(FB_TimelineTrimClip(&tl, 0, 0, 500) == 0);
    CHECK(tl.clips[1].xfade == 500 && tl.clips[1].start == 0);
    CHECK(tl.clips[2].start == 4000);

    /* Typing a Starts later than the cut removes it, leaving a gap. */
    three_clips(&tl);
    FB_TimelineSetCrossfade(&tl, 1, 1000, NULL);
    CHECK(FB_TimelineMoveClip(&tl, 1, 5000) == 0);
    CHECK(tl.clips[1].xfade == 0 && tl.clips[2].start == 9000);
    /* A crossfade closes the gap first; the clip after keeps its own. */
    CHECK(FB_TimelineMaxCrossfade(&tl, 1) == 4000);
    CHECK(FB_TimelineSetCrossfade(&tl, 1, 1000, NULL) == 1000);
    CHECK(tl.clips[1].start == 3000 && tl.clips[2].start == 7000);
    FB_TimelineSetCrossfade(&tl, 1, 0, NULL);
    /* Typing an earlier one makes the crossfade longer; without one it
     * can't overlap. */
    CHECK(FB_TimelineMoveClip(&tl, 1, 4000) == 0);
    CHECK(FB_TimelineMoveClip(&tl, 1, 3000) == -1);
    FB_TimelineSetCrossfade(&tl, 1, 1000, NULL);
    CHECK(FB_TimelineEarliestStart(&tl, 1) == 0);
    CHECK(FB_TimelineMoveClip(&tl, 1, 2000) == 0);
    CHECK(tl.clips[1].xfade == 2000 && tl.clips[2].start == 6000);

    /* Removing the clip before: the next one simply follows. */
    three_clips(&tl);
    FB_TimelineSetCrossfade(&tl, 1, 1000, NULL);
    FB_TimelineSetCrossfade(&tl, 2, 1000, NULL);    /* 0-4, 3-7, 6-10 */
    CHECK(FB_TimelineStartWithout(&tl, 2, 1) == 4000);
    CHECK(FB_TimelineRemoveClip(&tl, 1) == 0);
    CHECK(tl.num_clips == 2 && tl.clips[1].start == 4000);
    CHECK(tl.clips[1].xfade == 0);
    /* Removing the first: the second loses its crossfade, starts at 0. */
    three_clips(&tl);
    FB_TimelineSetCrossfade(&tl, 1, 1000, NULL);
    CHECK(FB_TimelineRemoveClip(&tl, 0) == 0);
    CHECK(tl.clips[0].start == 0 && tl.clips[0].xfade == 0);
    CHECK(tl.clips[1].start == 4000);
    /* Removing one with a crossfade: the next closes up to the cut. */
    three_clips(&tl);
    FB_TimelineSetCrossfade(&tl, 1, 1000, NULL);    /* 0-4, 3-7, 7-11 */
    CHECK(FB_TimelineRemoveClip(&tl, 1) == 0);
    CHECK(tl.clips[1].start == 4000 && tl.clips[1].xfade == 0);

    /* Splitting: the right half follows with a cut. */
    three_clips(&tl);
    FB_TimelineSetCrossfade(&tl, 1, 1000, NULL);
    CHECK(FB_TimelineSplit(&tl, 0, 5000) == 2);
    CHECK(tl.clips[1].xfade == 1000 && tl.clips[2].xfade == 0);
    CHECK(tl.clips[2].start == 5000 && tl.clips[3].start == 7000);

    /* Dropping a clip in at a crossfade: it goes at the cut, and the clip
     * that faded in follows it with a cut. */
    three_clips(&tl);
    FB_TimelineSetCrossfade(&tl, 1, 1000, NULL);    /* 0-4, 3-7, 7-11 */
    CHECK(FB_TimelineDropPoint(&tl, 0, 2500, 2000, 2, &inserted) == 4000
          && inserted);
    CHECK(FB_TimelineMoveClipTo(&tl, 2, 0, 2500) == 1);
    CHECK(tl.clips[1].start == 4000 && tl.clips[1].xfade == 0);
    CHECK(tl.clips[2].start == 8000 && tl.clips[2].xfade == 0);
    CHECK(tl.clips[2].in == 4000);
}

static void test_job_crossfade(void)
{
    static FBTimeline tl;
    static FBJob job;
    FBOutput out;
    int m;

    memset(&out, 0, sizeof(out));
    strcpy(out.path, "out.mp4");
    out.quality = FB_QUALITY_FINAL;

    three_clips(&tl);
    FB_TimelineSetCrossfade(&tl, 1, 1000, NULL);
    FB_TimelineSetCrossfade(&tl, 2, 500, "wipeleft");
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    printf("crossfade: %s\n", job.filtergraph);
    CHECK(job.length == 10500);
    CHECK(strstr(job.filtergraph, "[v0][v1]xfade=transition=fade:"
                 "duration=1.000:offset=3.000[xv1];") != NULL);
    CHECK(strstr(job.filtergraph, "[a0][a1]acrossfade=d=1.000[xa1];") != NULL);
    /* The second mixes into the first result, 7 s long by then. */
    CHECK(strstr(job.filtergraph, "[xv1][v2]xfade=transition=wipeleft:"
                 "duration=0.500:offset=6.500[xv2];") != NULL);
    CHECK(strstr(job.filtergraph, "[xv2][xa2]concat=n=1:v=1:a=1[outv][outa]")
          != NULL);

    /* With a gap before and after, and music faded on a track of its own. */
    three_clips(&tl);
    FB_TimelineMoveClip(&tl, 0, 1000);              /* 1-5, 5-9, 9-13 */
    FB_TimelineSetCrossfade(&tl, 2, 2000, NULL);    /* 1-5, 5-9, 7-11 */
    m = FB_TimelineAddSource(&tl, "music.mp3", 60000, 0, 1);
    FB_TimelineAppendToTrack(&tl, 1, m, 0, 6000);
    FB_TimelineAppendToTrack(&tl, 1, m, 20000, 26000);
    FB_TimelineSetCrossfade(&tl, 4, 3000, NULL);    /* 0-6, 3-9 */
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    printf("crossfade with gaps: %s\n", job.filtergraph);
    CHECK(job.length == 11000);
    CHECK(strstr(job.filtergraph, "[v2][v3]xfade=transition=fade:"
                 "duration=2.000:offset=2.000[xv3];") != NULL);
    CHECK(strstr(job.filtergraph, "[v0][a0][v1][a1][xv3][xa3]concat=n=3")
          != NULL);
    CHECK(strstr(job.filtergraph, "[a4][a5]acrossfade=d=3.000[xa5];") != NULL);
    CHECK(strstr(job.filtergraph, "[xa5][a6]concat=n=2:v=0:a=1[t1a]") != NULL);

    /* A kind that isn't a name never reaches the filtergraph. */
    strcpy(tl.clips[2].xfade_kind, "fade[x]");
    CHECK(FB_JobBuild(&job, &tl, &out) != FB_JOB_OK);
}

static void test_project_crossfade(void)
{
    static FBTimeline tl, back;
    static char text[8192];
    int line;

    three_clips(&tl);
    FB_TimelineSetCrossfade(&tl, 1, 1000, NULL);
    FB_TimelineSetCrossfade(&tl, 2, 500, "circleopen");
    CHECK(FB_ProjectWrite(&tl, "", text, sizeof(text)) > 0);
    CHECK(strstr(text, "CLIP 1 4000 8000\n    CROSSFADE 1000\n"
                       "CLIP 1 8000 12000\n    CROSSFADE 500 circleopen\n"));
    CHECK(strstr(text, "    AT ") == NULL);
    CHECK(FB_ProjectParse(&back, text, "", &line) == FB_PROJECT_OK);
    CHECK(back.num_clips == 3);
    CHECK(back.clips[1].start == 3000 && back.clips[1].xfade == 1000);
    CHECK(back.clips[2].start == 6500 &&
          strcmp(back.clips[2].xfade_kind, "circleopen") == 0);

    /* After a gap, it closes; only a name as kind. */
    CHECK(FB_ProjectParse(&back, "FORMBROD-PROJECT 2\nSOURCE 1 a.mp4\n"
                          "TRACK video\nCLIP 1 0 1000\nCLIP 1 0 1000\n"
                          "    AT 2000\n    CROSSFADE 500\n", "", &line)
          == FB_PROJECT_OK);
    CHECK(back.clips[1].start == 500);
    CHECK(FB_ProjectParse(&back, "FORMBROD-PROJECT 2\nSOURCE 1 a.mp4\n"
                          "TRACK video\nCLIP 1 0 1000\n    CROSSFADE 500\n",
                          "", &line) == FB_PROJECT_BAD_LINE);
    CHECK(FB_ProjectParse(&back, "FORMBROD-PROJECT 2\nSOURCE 1 a.mp4\n"
                          "TRACK video\nCLIP 1 0 1000\nCLIP 1 0 1000\n"
                          "    CROSSFADE 500 a;b\n", "", &line)
          == FB_PROJECT_BAD_LINE);
}

static void test_clip_effects(void)
{
    static FBTimeline tl;
    FBClip *c;

    three_clips(&tl);
    c = &tl.clips[0];
    CHECK(FB_ClipFindEffect(c, "fade") == -1);
    FB_ClipAddEffect(c, "a");
    FB_ClipAddEffect(c, "fade");
    FB_ClipAddEffect(c, "b");
    CHECK(FB_ClipFindEffect(c, "fade") == 1);
    CHECK(FB_ClipRemoveEffect(c, 1) == 0);
    CHECK(c->num_effects == 2 && strcmp(c->effects[1].module, "b") == 0);
    CHECK(FB_ClipRemoveEffect(c, 2) == -1);
    CHECK(FB_ClipFindEffect(c, "fade") == -1);
}

static void test_looping_animation(void)
{
    static FBTimeline tl;
    static FBJob job;
    FBOutput out;
    int v, g;

    FB_TimelineInit(&tl);
    v = FB_TimelineAddSource(&tl, "a.mp4", 10000, 1, 1);
    FB_SourceSetVideo(&tl.sources[v], 320, 240, 1, 1, 25, 1);
    g = FB_TimelineAddSource(&tl, "boing.gif", 1100, 1, 0);
    FB_SourceSetVideo(&tl.sources[g], 46, 47, 1, 1, 10, 1);
    FB_TimelineAppendToTrack(&tl, 0, v, 0, 6000);
    FB_TimelineAddTrack(&tl, FB_TRACK_VIDEO);
    FB_TimelineAddClip(&tl, 1, g, 0, 1100, 500);

    /* Without looping, one round at most. */
    CHECK(FB_SourceLimit(&tl.sources[g]) == 1100);
    CHECK(FB_TimelineTrimClip(&tl, 1, 0, 5000) == -1);
    /* Looping: any length. */
    tl.sources[g].loops = 1;
    CHECK(FB_SourceLimit(&tl.sources[g]) == 0);
    CHECK(FB_SourceLimit(&tl.sources[v]) == 10000);
    CHECK(FB_TimelineTrimClip(&tl, 1, 0, 5000) == 0);

    memset(&out, 0, sizeof(out));
    strcpy(out.path, "out.mp4");
    out.quality = FB_QUALITY_FINAL;
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_OK);
    printf("looping animation: %s\n", job.filtergraph);
    /* 11 frames at 10/s, over and over. */
    CHECK(strstr(job.filtergraph, "[1:v]loop=loop=-1:size=11:start=0,"
                 "setpts=N*1/(10*TB),trim=start=0.000:end=5.000") != NULL);
}

int main(void)
{
    FB_RegisterBuiltinModules();
    test_timeline();
    test_tracks();
    test_move_clips();
    test_crossfades();
    test_job_crossfade();
    test_project_crossfade();
    test_clip_effects();
    test_looping_animation();
    test_log();
    test_job_build_and_roundtrip();
    test_job_tracks();
    test_job_three_tracks();
    test_window_and_seek();
    test_window_crossfade();
    test_player();
    test_render_pieces();
    test_still();
    test_job_errors();
    test_fit();
    test_timeview();
    test_view_room();
    test_time_text();
    test_paths();
    test_project_roundtrip();
    test_project_hand_edited();
    test_project_tracks();
    test_cache_keys();
    test_cache_rules();
    test_cache_queue();
    test_thumb_strips();
    test_thumb_store();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("All tests passed\n");
    return 0;
}
