/*
 * render.h - background rendering: the edit, rendered ahead of time.
 *
 * When playing is slower than real time on a machine, the preview can't
 * keep up however much it buffers. So while the window is left alone, the
 * edit is rendered in pieces of FB_RENDER_PIECE_MS into the cache drawer,
 * at preview size, as MJPEG with PCM sound at the player's rate. Playing a
 * rendered piece is cheap: one small file, nothing to scale, overlay or
 * mix. Pieces that aren't rendered yet are played live as before.
 *
 *   - A piece is known by a key: a fingerprint of the job that renders it
 *     (built from the edit with the original files) and of those files'
 *     own fingerprints. Changing anything that shows in a piece changes
 *     its key; other pieces keep theirs and stay rendered.
 *   - The file is "<key>.render1-<width>.mov" in the cache drawer, written
 *     and renamed like the proxies (FB_CACHE_RENDER), so it is only there
 *     when it's whole.
 *   - Rendering reads the proxies where they're ready (the app swaps them
 *     in); the key doesn't depend on that.
 *
 * Plain C99; the app drives it (App_PollCache, App_PlayStart).
 */
#ifndef FORMBROD_RENDER_H
#define FORMBROD_RENDER_H

#include "timeline.h"
#include "job.h"

#define FB_RENDER_PIECE_MS 10000

/* How many pieces the edit has (the last may be shorter). */
int     FB_RenderPieces(const FBTimeline *tl);
/* Where piece i starts and how long it is, in the edit. */
fb_ms_t FB_RenderPieceStart(int i);
fb_ms_t FB_RenderPieceLength(const FBTimeline *tl, int i);
/* The piece that `at` (ms in the edit) is in. */
int     FB_RenderPieceAt(fb_ms_t at);

/* The key of piece i of `tl` (the edit as it is, with the original
 * files and their keys). 0, or -1 if its job can't be built. */
int FB_RenderPieceKey(const FBTimeline *tl, int i, char key[FB_KEY_LEN]);

/* The job that renders piece i of `src` (proxies swapped in, or not)
 * into `path`: preview quality, MJPEG and PCM. */
FBJobError FB_RenderPieceJob(FBJob *job, const FBTimeline *src, int i,
                             const char *path);

/* The job that plays `len` ms of a rendered piece, from `offset` ms into
 * it. `audio`: whether the piece has sound (its edit had). */
void FB_RenderPlayJob(FBJob *job, const char *path, fb_ms_t offset,
                      fb_ms_t len, int audio);

/* Playing from `from` to the end, as steps: a rendered piece (`piece` is
 * its number, `offset` where in its file to start) or, for a run of
 * pieces that aren't rendered, one live stretch (`piece` -1). `from` and
 * `len` are in the edit. `rendered[i]` says whether piece i is (NULL:
 * none). Returns the number of steps (at most `max`). */
typedef struct FBPlayStep {
    int     piece;
    fb_ms_t from, len;
    fb_ms_t offset;
} FBPlayStep;

int FB_RenderPlan(const FBTimeline *tl, fb_ms_t from,
                  const unsigned char *rendered, FBPlayStep *steps, int max);

#endif
