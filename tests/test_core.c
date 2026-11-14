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

    /* Only sound left: nothing to see. */
    tl.tracks[0].hidden = 1;
    tl.tracks[1].muted = 0;
    CHECK(FB_JobBuild(&job, &tl, &out) == FB_JOB_EMPTY);
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
    CHECK(strstr(job.filtergraph, "[0:v]loop=loop=-1:size=1:start=0,"
                                  "setpts=N*1/(30*TB),trim=start=0.000:"
                                  "end=3.000,setpts=PTS-STARTPTS") != NULL);
    CHECK(strstr(job.filtergraph, "[1:v]trim=start=0.000:end=4.000") != NULL);
    /* The picture has no sound: silence while it's shown. */
    CHECK(strstr(job.filtergraph, "anullsrc=r=48000:cl=stereo,atrim="
                                  "duration=3.000") != NULL);
    CHECK(job.length == 7000);
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

int main(void)
{
    FB_RegisterBuiltinModules();
    test_timeline();
    test_tracks();
    test_log();
    test_job_build_and_roundtrip();
    test_job_tracks();
    test_still();
    test_job_errors();
    test_fit();
    test_timeview();
    test_time_text();
    test_paths();
    test_project_roundtrip();
    test_project_hand_edited();
    test_project_tracks();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("All tests passed\n");
    return 0;
}
