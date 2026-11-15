/*
 * render.c - background rendering's pieces and jobs (see render.h).
 */
#include <stdio.h>
#include <string.h>
#include "formbrod/render.h"
#include "formbrod/cache.h"

static void copy_str(char *dst, const char *src, size_t len)
{
    size_t n = strlen(src);

    if (n >= len)
        n = len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int FB_RenderPieces(const FBTimeline *tl)
{
    fb_ms_t d = FB_TimelineDuration(tl);

    return d > 0 ? (int)((d + FB_RENDER_PIECE_MS - 1) / FB_RENDER_PIECE_MS)
                 : 0;
}

fb_ms_t FB_RenderPieceStart(int i)
{
    return (fb_ms_t)i * FB_RENDER_PIECE_MS;
}

fb_ms_t FB_RenderPieceLength(const FBTimeline *tl, int i)
{
    fb_ms_t left = FB_TimelineDuration(tl) - FB_RenderPieceStart(i);

    if (left <= 0)
        return 0;
    return left < FB_RENDER_PIECE_MS ? left : FB_RENDER_PIECE_MS;
}

int FB_RenderPieceAt(fb_ms_t at)
{
    return at > 0 ? (int)(at / FB_RENDER_PIECE_MS) : 0;
}

/* The piece as a timeline of its own, and its job, without a file. */
static FBJobError piece_job(FBJob *job, const FBTimeline *src, int i,
                            const char *path)
{
    static FBTimeline win;    /* large; keep it off the stack */
    FBOutput out;

    if (FB_RenderPieceLength(src, i) <= 0)
        return FB_JOB_EMPTY;
    FB_TimelineWindow(&win, src, FB_RenderPieceStart(i),
                      FB_RenderPieceLength(src, i));
    memset(&out, 0, sizeof(out));
    copy_str(out.path, path, sizeof(out.path));
    copy_str(out.vcodec, "mjpeg", sizeof(out.vcodec));
    copy_str(out.acodec, "pcm_s16le", sizeof(out.acodec));
    out.quality = FB_QUALITY_PREVIEW;
    return FB_JobBuild(job, &win, &out);
}

int FB_RenderPieceKey(const FBTimeline *tl, int i, char key[FB_KEY_LEN])
{
    static FBJob job;
    static char text[FB_FILTERGRAPH_LEN + FB_MAX_SOURCES * (FB_PATH_LEN + 64)
                     + 1024];
    int n, s;

    key[0] = '\0';
    if (piece_job(&job, tl, i, "") != FB_JOB_OK)
        return -1;
    n = FB_JobWrite(&job, text, sizeof(text));
    if (n < 0)
        return -1;
    /* The files' contents too: a file changed under the same name. */
    for (s = 0; s < tl->num_sources && n < (int)sizeof(text) - 64; s++)
        n += snprintf(text + n, sizeof(text) - n, "KEY %d %s\n", s,
                      tl->sources[s].key);
    n += snprintf(text + n, sizeof(text) - n, "PIECE render1 %d\n",
                  FB_PREVIEW_WIDTH);
    FB_CacheKeyData(text, (size_t)n, key);
    return 0;
}

FBJobError FB_RenderPieceJob(FBJob *job, const FBTimeline *src, int i,
                             const char *path)
{
    return piece_job(job, src, i, path);
}

void FB_RenderPlayJob(FBJob *job, const char *path, fb_ms_t offset,
                      fb_ms_t len, int audio)
{
    int n;

    memset(job, 0, sizeof(*job));
    job->version = FB_JOB_VERSION;
    job->num_inputs = 1;
    copy_str(job->inputs[0], path, FB_PATH_LEN);
    job->seek[0] = offset;
    n = snprintf(job->filtergraph, sizeof(job->filtergraph),
                 "[0:v]trim=start=%.3f:duration=%.3f,setpts=PTS-STARTPTS"
                 "[outv]", offset / 1000.0, len / 1000.0);
    if (audio)
        snprintf(job->filtergraph + n, sizeof(job->filtergraph) - n,
                 ";[0:a]atrim=start=%.3f:duration=%.3f,"
                 "asetpts=PTS-STARTPTS[outa]", offset / 1000.0,
                 len / 1000.0);
    copy_str(job->video_out, "outv", sizeof(job->video_out));
    if (audio)
        copy_str(job->audio_out, "outa", sizeof(job->audio_out));
    job->output.quality = FB_QUALITY_PREVIEW;
    job->length = len;
}

int FB_RenderPlan(const FBTimeline *tl, fb_ms_t from,
                  const unsigned char *rendered, FBPlayStep *steps, int max)
{
    fb_ms_t end = FB_TimelineDuration(tl);
    int n = 0, i;

    if (from < 0)
        from = 0;
    for (i = FB_RenderPieceAt(from); FB_RenderPieceStart(i) < end && n < max;
         i++) {
        fb_ms_t a = FB_RenderPieceStart(i), b = a + FB_RenderPieceLength(tl, i);

        if (a < from)
            a = from;
        if (rendered && rendered[i]) {
            steps[n].piece = i;
            steps[n].from = a;
            steps[n].len = b - a;
            steps[n].offset = a - FB_RenderPieceStart(i);
            n++;
        } else if (n > 0 && steps[n - 1].piece < 0) {
            steps[n - 1].len = b - steps[n - 1].from;   /* the live run */
        } else {
            steps[n].piece = -1;
            steps[n].from = a;
            steps[n].len = b - a;
            steps[n].offset = 0;
            n++;
        }
    }
    return n;
}
