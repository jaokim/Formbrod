/*
 * timeview.h - the visible part of the time axis, for the timeline gadgets.
 *
 * Every track in the window (ruler, film strip, and later waveforms) draws
 * the same stretch of time. FBTimeView describes that stretch: where it
 * starts, how much time one pixel is (the zoom), and how wide it is. The
 * gadgets share one view, so zooming or scrolling one moves them all.
 *
 * Plain C99 with no AmigaOS dependencies, so the arithmetic is tested on
 * the host. See gadgets/timeline/README.md for how the gadgets use it.
 *
 * Time here is in microseconds, not milliseconds like the timeline model:
 * zoomed in to single frames, one pixel is less than a millisecond.
 */
#ifndef FORMBROD_TIMEVIEW_H
#define FORMBROD_TIMEVIEW_H

#include <stddef.h>
#include <stdint.h>
#include "formbrod/timeline.h"

typedef int64_t fb_us_t;   /* time in microseconds */

#define FB_US(ms)          ((fb_us_t)(ms) * 1000)
#define FB_MS(us)          ((fb_ms_t)((us) / 1000))
#define FB_US_PER_SEC      ((fb_us_t)1000000)

/* Frame length used when the frame rate isn't known (25 fps). */
#define FB_DEFAULT_FRAME_US ((fb_us_t)40000)

typedef struct FBTimeView {
    fb_us_t start;        /* time at the left edge (x = 0) */
    fb_us_t per_px;       /* zoom: microseconds per pixel, >= 1 */
    int     width;        /* visible width in pixels */
    fb_us_t length;       /* total length that can be scrolled over */
    fb_us_t min_per_px;   /* most zoomed in */
    fb_us_t max_per_px;   /* most zoomed out: the whole length fits */
} FBTimeView;

/* An empty view `width` pixels wide, zoomed in as far as it goes. */
void    FB_ViewInit(FBTimeView *v, int width);

/* The most zoomed-in level shows one frame (frame_us long) per film-strip
 * cell (cell_w pixels). frame_us <= 0 means FB_DEFAULT_FRAME_US. */
void    FB_ViewSetFrame(FBTimeView *v, fb_us_t frame_us, int cell_w);

/* Changing the length or width keeps the zoom and start where possible
 * and clamps them to the new limits. */
void    FB_ViewSetLength(FBTimeView *v, fb_us_t length);
void    FB_ViewSetWidth(FBTimeView *v, int width);

/* Sets the zoom, keeping the time under pixel `anchor_x` where it is (so a
 * mouse-wheel zoom zooms around the pointer). Clamped to the limits. */
void    FB_ViewSetZoom(FBTimeView *v, fb_us_t per_px, int anchor_x);

/* Zooms so the whole length fits in the width, from the start. */
void    FB_ViewZoomToFit(FBTimeView *v);

/* Zoom levels are steps of 5/4 from min_per_px up to max_per_px, for a
 * slider or the mouse wheel. Level 0 is the most zoomed in. */
int     FB_ViewZoomLevels(const FBTimeView *v);       /* number of levels */
int     FB_ViewZoomLevel(const FBTimeView *v);        /* current, nearest */
void    FB_ViewSetZoomLevel(FBTimeView *v, int level, int anchor_x);

/* Scrolling. The start is clamped so the view doesn't go past the end
 * (unless everything fits, then it stays at 0). */
void    FB_ViewSetStart(FBTimeView *v, fb_us_t start);
void    FB_ViewScrollPx(FBTimeView *v, int dx);

/* Scrolls by whole pages, if needed, so that `t` is visible. Returns 1 if
 * the view moved. Used to follow the playhead. */
int     FB_ViewEnsureVisible(FBTimeView *v, fb_us_t t);

/* Pixel <-> time. TimeToX may return values outside 0..width-1. */
long    FB_ViewTimeToX(const FBTimeView *v, fb_us_t t);
fb_us_t FB_ViewXToTime(const FBTimeView *v, long x);   /* clamped to 0..length */
fb_us_t FB_ViewEnd(const FBTimeView *v);               /* time at x = width */

/* For a scroller: everything in pixels at the current zoom. */
long    FB_ViewTotalPx(const FBTimeView *v);
long    FB_ViewStartPx(const FBTimeView *v);

/* ---- Ruler ---- */

/* Picks the spacing of labelled ticks (major) so labels are at least
 * min_label_px apart, and of unlabelled ticks between them (minor, 0 for
 * none). Steps are 1, 2, 5, 10... frames up to half a second when
 * frame_us > 0, else 100, 200 or 500 ms; then 1, 2, 5, 10, 15, 30 s,
 * 1, 2, 5, 10, 15, 30 min, and 1, 2, 4... hours. Minor ticks are the
 * smallest of these that splits the major step into at most 5 parts. */
void    FB_ViewRulerSteps(const FBTimeView *v, int min_label_px,
                          fb_us_t frame_us, fb_us_t *major, fb_us_t *minor);

/* Text for a ruler label at time t, with detail to match the step:
 *   "1:05"        whole seconds
 *   "1:05.5"      tenths (decimal point from the locale), no frame rate
 *   "1:05:12"     seconds and frame number, when frame_us > 0
 *   "1:02:05"     hours are added from an hour on
 * Returns buf. */
char   *FB_FormatTimecode(char *buf, size_t len, fb_us_t t, fb_us_t step,
                          fb_us_t frame_us, char decimal_point);

/* ---- Film-strip cells ---- */

/* A film-strip cell: one thumbnail cell_w pixels wide. Cells sit on a grid
 * that starts at time 0, so they don't jump while scrolling. */
typedef struct FBCell {
    int64_t index;     /* cell number from time 0 */
    fb_us_t t0, t1;    /* the time the cell covers, [t0, t1) */
    fb_us_t mid;       /* the time whose frame represents the cell */
    long    x;         /* left edge in pixels; the first may be negative */
} FBCell;

/* Fills `cells` with the cells that overlap the view, at most `max`, and
 * returns how many. Cells past the length are left out. */
int     FB_ViewCells(const FBTimeView *v, int cell_w, FBCell *cells, int max);

/* ---- Timeline lookups ---- */

/* Where clip `clip` starts on the timeline, in microseconds. */
fb_us_t FB_TimelineClipStart(const FBTimeline *tl, int clip);

/* The clip playing at timeline time t, or -1. When source_t isn't NULL it
 * gets the matching time in that clip's source file. */
int     FB_TimelineLocate(const FBTimeline *tl, fb_us_t t, fb_us_t *source_t);

#endif
