/*
 * player.h - the preview player's queues and clock.
 *
 * A backend plays a job into an FBPlayer (through FB_PlayerSink): pictures
 * go into a short queue, sound into a ring. The window takes the picture
 * that is due by the clock, and the sound output takes sound by its place.
 * When either is full the backend is told so and waits, so the player sets
 * the pace and the backend never runs far ahead.
 *
 *   - Time 0 is the start of the job (the playhead when Play was pressed).
 *   - The clock starts once a little is buffered (FB_PlayerReady), so the
 *     first frames aren't late at once.
 *   - Pictures that are already late when a newer one is due are dropped:
 *     on a slow machine the picture skips but keeps time.
 *   - Sound is kept in step with its times: a gap is filled with silence,
 *     overlap is skipped, and sound the output didn't take in time is
 *     dropped rather than played late.
 *
 * Plain C99, tested on the host. The window and the AHI output are in
 * src/gui/playwindow.c and src/player/audio.c.
 */
#ifndef FORMBROD_PLAYER_H
#define FORMBROD_PLAYER_H

#include "timeline.h"
#include "backend.h"

#define FB_PLAY_FRAMES       12   /* pictures queued ahead */
#define FB_PLAY_AUDIO_MS   2000   /* sound queued ahead */
#define FB_PLAY_START_FRAMES  3   /* buffered before the clock starts */
#define FB_PLAY_START_MS    300   /* likewise for sound */

typedef struct FBPlayFrame {
    unsigned char *pixels;    /* ARGB, `stride` bytes a row */
    int            width, height, stride;
    size_t         size;      /* allocated */
    fb_ms_t        pts;
} FBPlayFrame;

typedef struct FBPlayer {
    int         has_audio;
    /* Pictures: a ring of FB_PLAY_FRAMES, plus the one on show. */
    FBPlayFrame frames[FB_PLAY_FRAMES];
    int         first, count;
    FBPlayFrame shown;
    int         have_shown;
    long        num_shown, num_dropped;
    /* Sound: interleaved stereo frames, sample `audio_start` first. */
    short      *audio;
    long        audio_size;          /* in frames */
    long        audio_first, audio_count;
    long long   audio_start;         /* sample index of the first queued */
    long long   audio_read;          /* next sample index to hand out */
    int         audio_begun;
    /* Clock. */
    long long   clock0;              /* us at time 0, or -1 */
    int         ended;               /* the backend has given everything */
    fb_ms_t     last_pts;            /* of the last picture given */
} FBPlayer;

/* 0, or -1 if out of memory. */
int  FB_PlayerInit(FBPlayer *pl, int has_audio);
void FB_PlayerFree(FBPlayer *pl);

/* A sink for FBBackend.Play that fills this player. */
FBFrameSink FB_PlayerSink(FBPlayer *pl);

/* Taking frames from the backend (the sink's callbacks): 0 taken, 1 full
 * (offer it again later). A picture is copied. */
int  FB_PlayerPushVideo(FBPlayer *pl, const unsigned char *argb, int width,
                        int height, int stride, fb_ms_t pts);
int  FB_PlayerPushAudio(FBPlayer *pl, const short *samples, int frames,
                        fb_ms_t pts);

/* The backend has given everything (done, or failed). */
void FB_PlayerSetEnded(FBPlayer *pl);

/* Whether enough is buffered to start the clock. */
int  FB_PlayerReady(const FBPlayer *pl);
void FB_PlayerStart(FBPlayer *pl, long long now_us);
int  FB_PlayerStarted(const FBPlayer *pl);

/* Where playback is, in ms from time 0 (0 before the clock starts). */
fb_ms_t FB_PlayerTime(const FBPlayer *pl, long long now_us);

/* The picture due now, if it's a new one; else NULL. Pictures due before
 * it are dropped. The frame stays valid until the next call. */
const FBPlayFrame *FB_PlayerFrame(FBPlayer *pl, long long now_us);

/* How long until the next queued picture is due, in ms (0 = now or
 * overdue, -1 = none queued). For sleeping in the main loop. */
long FB_PlayerNextDue(const FBPlayer *pl, long long now_us);

/* Fills `out` with `frames` stereo frames of sound for the output, in
 * order from where the last call ended; silence where there's nothing.
 * Returns how many frames were real sound (not silence). */
long FB_PlayerTakeAudio(FBPlayer *pl, short *out, long frames);

/* Played to the end: the backend has ended, every picture has been shown
 * and the clock is past the last one. */
int  FB_PlayerFinished(const FBPlayer *pl, long long now_us);

#endif
