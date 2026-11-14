/*
 * player.c - the preview player's queues and clock (see formbrod/player.h).
 */
#include <stdlib.h>
#include <string.h>
#include "formbrod/player.h"

#define FRAME_MS 40   /* how long the last picture stays, for the end */

int FB_PlayerInit(FBPlayer *pl, int has_audio)
{
    memset(pl, 0, sizeof(*pl));
    pl->has_audio = has_audio;
    pl->clock0 = -1;
    pl->audio_size = (long)FB_PLAY_RATE * FB_PLAY_AUDIO_MS / 1000;
    pl->audio = malloc((size_t)pl->audio_size * 2 * sizeof(short));
    return pl->audio ? 0 : -1;
}

void FB_PlayerFree(FBPlayer *pl)
{
    int i;

    for (i = 0; i < FB_PLAY_FRAMES; i++)
        free(pl->frames[i].pixels);
    free(pl->shown.pixels);
    free(pl->audio);
    memset(pl, 0, sizeof(*pl));
    pl->clock0 = -1;
}

/* ---- From the backend ------------------------------------------------------ */

int FB_PlayerPushVideo(FBPlayer *pl, const unsigned char *argb, int width,
                       int height, int stride, fb_ms_t pts)
{
    FBPlayFrame *f;
    size_t row = (size_t)width * 4, need = row * (size_t)height;
    int y;

    if (pl->count >= FB_PLAY_FRAMES)
        return 1;
    if (width <= 0 || height <= 0)
        return 0;                        /* nothing to show; take it */
    f = &pl->frames[(pl->first + pl->count) % FB_PLAY_FRAMES];
    if (f->size < need) {
        unsigned char *p = realloc(f->pixels, need);

        if (!p)
            return 0;                    /* drop it rather than stall */
        f->pixels = p;
        f->size = need;
    }
    for (y = 0; y < height; y++)
        memcpy(f->pixels + row * y, argb + (size_t)stride * y, row);
    f->width = width;
    f->height = height;
    f->stride = (int)row;
    f->pts = pts;
    pl->count++;
    if (pts > pl->last_pts)
        pl->last_pts = pts;
    return 0;
}

int FB_PlayerPushAudio(FBPlayer *pl, const short *samples, int frames,
                       fb_ms_t pts)
{
    long long idx = (long long)pts * FB_PLAY_RATE / 1000, end;
    long skip = 0, gap = 0, n, i;

    /* Without sound output it's taken and forgotten, so a job's sound
     * never stalls the pictures. */
    if (frames <= 0 || !pl->audio || !pl->has_audio)
        return 0;
    if (pl->audio_count == 0) {
        /* Nothing queued: start at this sound, or at where the output
         * is if this is already late. */
        pl->audio_start = idx > pl->audio_read ? idx : pl->audio_read;
        pl->audio_first = 0;
    }
    end = pl->audio_start + pl->audio_count;
    if (idx < end)
        skip = (long)(end - idx);       /* overlaps what's queued */
    else
        gap = (long)(idx - end);        /* silence in between */
    if (skip >= frames)
        return 0;
    n = gap + frames - skip;
    if (pl->audio_count + n > pl->audio_size)
        return 1;
    for (i = 0; i < n; i++) {
        long at = (pl->audio_first + pl->audio_count + i) % pl->audio_size;

        if (i < gap) {
            pl->audio[at * 2] = pl->audio[at * 2 + 1] = 0;
        } else {
            const short *s = samples + (skip + i - gap) * 2;

            pl->audio[at * 2] = s[0];
            pl->audio[at * 2 + 1] = s[1];
        }
    }
    pl->audio_count += n;
    return 0;
}

static int sink_video(void *ud, const unsigned char *argb, int w, int h,
                      int stride, fb_ms_t pts)
{
    return FB_PlayerPushVideo(ud, argb, w, h, stride, pts);
}

static int sink_audio(void *ud, const short *s, int frames, fb_ms_t pts)
{
    return FB_PlayerPushAudio(ud, s, frames, pts);
}

FBFrameSink FB_PlayerSink(FBPlayer *pl)
{
    FBFrameSink sink;

    sink.Video = sink_video;
    sink.Audio = sink_audio;
    sink.ud = pl;
    return sink;
}

void FB_PlayerSetEnded(FBPlayer *pl)
{
    pl->ended = 1;
}

/* ---- Clock and pictures -------------------------------------------------- */

int FB_PlayerReady(const FBPlayer *pl)
{
    int video = pl->count >= FB_PLAY_START_FRAMES ||
                pl->count >= FB_PLAY_FRAMES || pl->ended;
    /* Sound must be there too: starting on pictures alone would make
     * sound that comes later count as late, and be dropped. */
    int audio = !pl->has_audio || pl->ended ||
                pl->audio_count * 1000 / FB_PLAY_RATE >= FB_PLAY_START_MS;

    return video && audio;
}

void FB_PlayerStart(FBPlayer *pl, long long now_us)
{
    pl->clock0 = now_us;
}

int FB_PlayerStarted(const FBPlayer *pl)
{
    return pl->clock0 >= 0;
}

fb_ms_t FB_PlayerTime(const FBPlayer *pl, long long now_us)
{
    if (pl->clock0 < 0 || now_us < pl->clock0)
        return 0;
    return (fb_ms_t)((now_us - pl->clock0) / 1000);
}

const FBPlayFrame *FB_PlayerFrame(FBPlayer *pl, long long now_us)
{
    fb_ms_t t = FB_PlayerTime(pl, now_us);
    int due = 0, i;

    if (pl->clock0 < 0)
        return NULL;
    /* How many queued pictures are due; the last of them is shown. */
    while (due < pl->count &&
           pl->frames[(pl->first + due) % FB_PLAY_FRAMES].pts <= t)
        due++;
    if (due == 0)
        return NULL;
    for (i = 0; i < due - 1; i++) {
        pl->first = (pl->first + 1) % FB_PLAY_FRAMES;
        pl->num_dropped++;
    }
    {
        /* Swap buffers with the one shown, so neither is copied. */
        FBPlayFrame tmp = pl->shown;

        pl->shown = pl->frames[pl->first];
        pl->frames[pl->first] = tmp;
    }
    pl->first = (pl->first + 1) % FB_PLAY_FRAMES;
    pl->count -= due;
    pl->have_shown = 1;
    pl->num_shown++;
    return &pl->shown;
}

long FB_PlayerNextDue(const FBPlayer *pl, long long now_us)
{
    fb_ms_t d;

    if (pl->clock0 < 0)
        return 0;
    if (pl->count == 0)
        return -1;
    d = pl->frames[pl->first].pts - FB_PlayerTime(pl, now_us);
    return d > 0 ? (long)d : 0;
}

/* ---- Sound ----------------------------------------------------------------- */

static void drop_audio(FBPlayer *pl, long n)
{
    if (n > pl->audio_count)
        n = pl->audio_count;
    pl->audio_first = (pl->audio_first + n) % pl->audio_size;
    pl->audio_count -= n;
    pl->audio_start += n;
}

long FB_PlayerTakeAudio(FBPlayer *pl, short *out, long frames)
{
    long done = 0, real = 0;

    /* Sound that was due before here is too late to play. */
    if (pl->audio_count > 0 && pl->audio_start < pl->audio_read)
        drop_audio(pl, (long)(pl->audio_read - pl->audio_start));

    while (done < frames) {
        long long pos = pl->audio_read + done;
        long n = frames - done;

        if (pl->audio_count > 0 && pl->audio_start == pos) {
            long i;

            if (n > pl->audio_count)
                n = pl->audio_count;
            for (i = 0; i < n; i++) {
                long at = (pl->audio_first + i) % pl->audio_size;

                out[(done + i) * 2] = pl->audio[at * 2];
                out[(done + i) * 2 + 1] = pl->audio[at * 2 + 1];
            }
            drop_audio(pl, n);
            real += n;
        } else {
            /* Nothing yet for here: silence up to the next sound. */
            if (pl->audio_count > 0 && pl->audio_start - pos < n)
                n = (long)(pl->audio_start - pos);
            memset(out + done * 2, 0, (size_t)n * 2 * sizeof(short));
        }
        done += n;
    }
    pl->audio_read += frames;
    return real;
}

int FB_PlayerFinished(const FBPlayer *pl, long long now_us)
{
    fb_ms_t end = pl->last_pts + FRAME_MS;

    if (!pl->ended || pl->count > 0 || pl->clock0 < 0)
        return pl->ended && pl->clock0 < 0 && pl->count == 0 &&
               pl->audio_count == 0;   /* nothing at all came */
    if (pl->audio_count > 0) {
        fb_ms_t a = (fb_ms_t)((pl->audio_start + pl->audio_count) * 1000 /
                              FB_PLAY_RATE);

        if (a > end)
            end = a;
    }
    return FB_PlayerTime(pl, now_us) >= end;
}
