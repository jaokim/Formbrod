/*
 * timeview.c - zoom, scrolling, ruler steps and film-strip cells for the
 * timeline gadgets. See include/formbrod/timeview.h.
 */
#include <stdio.h>
#include "formbrod/timeview.h"

/* Division rounding towards minus infinity, for times left of the view. */
static int64_t floordiv(int64_t a, int64_t b)
{
    int64_t q = a / b;

    if ((a % b != 0) && ((a < 0) != (b < 0)))
        q--;
    return q;
}

static fb_us_t clamp(fb_us_t x, fb_us_t lo, fb_us_t hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

/* The next zoom level after p: 5/4 of it, but always at least one more. */
static fb_us_t next_level(fb_us_t p)
{
    fb_us_t n = (p * 5 + 3) / 4;

    return n > p ? n : p + 1;
}

static void update_limits(FBTimeView *v)
{
    fb_us_t fit = 0;

    if (v->min_per_px < 1)
        v->min_per_px = 1;
    if (v->width > 0 && v->length > 0)
        fit = (v->length + v->width - 1) / v->width;
    v->max_per_px = fit > v->min_per_px ? fit : v->min_per_px;
    v->per_px = clamp(v->per_px, v->min_per_px, v->max_per_px);
    FB_ViewSetStart(v, v->start);
}

void FB_ViewInit(FBTimeView *v, int width)
{
    v->start = 0;
    v->width = width > 0 ? width : 0;
    v->length = 0;
    v->min_per_px = 1;
    v->max_per_px = 1;
    v->per_px = 1;
    FB_ViewSetFrame(v, 0, 48);
}

void FB_ViewSetFrame(FBTimeView *v, fb_us_t frame_us, int cell_w)
{
    if (frame_us <= 0)
        frame_us = FB_DEFAULT_FRAME_US;
    if (cell_w < 1)
        cell_w = 1;
    v->min_per_px = frame_us / cell_w;
    update_limits(v);
}

void FB_ViewSetLength(FBTimeView *v, fb_us_t length)
{
    v->length = length > 0 ? length : 0;
    update_limits(v);
}

void FB_ViewSetWidth(FBTimeView *v, int width)
{
    v->width = width > 0 ? width : 0;
    update_limits(v);
}

void FB_ViewSetZoom(FBTimeView *v, fb_us_t per_px, int anchor_x)
{
    fb_us_t at = v->start + (fb_us_t)anchor_x * v->per_px;

    v->per_px = clamp(per_px, v->min_per_px, v->max_per_px);
    FB_ViewSetStart(v, at - (fb_us_t)anchor_x * v->per_px);
}

void FB_ViewZoomToFit(FBTimeView *v)
{
    v->per_px = v->max_per_px;
    FB_ViewSetStart(v, 0);
}

static fb_us_t level_zoom(const FBTimeView *v, int level)
{
    fb_us_t p = v->min_per_px;

    while (level-- > 0 && p < v->max_per_px)
        p = next_level(p);
    return p < v->max_per_px ? p : v->max_per_px;
}

int FB_ViewZoomLevels(const FBTimeView *v)
{
    fb_us_t p = v->min_per_px;
    int n = 1;

    while (p < v->max_per_px) {
        p = next_level(p);
        n++;
    }
    return n;
}

int FB_ViewZoomLevel(const FBTimeView *v)
{
    fb_us_t p = v->min_per_px, prev = p;
    int level = 0;

    while (p < v->per_px && p < v->max_per_px) {
        prev = p;
        p = next_level(p);
        level++;
    }
    if (p > v->max_per_px)
        p = v->max_per_px;
    /* Between two levels: take the nearer one. */
    if (level > 0 && v->per_px - prev < p - v->per_px)
        level--;
    return level;
}

void FB_ViewSetZoomLevel(FBTimeView *v, int level, int anchor_x)
{
    FB_ViewSetZoom(v, level_zoom(v, level < 0 ? 0 : level), anchor_x);
}

void FB_ViewSetStart(FBTimeView *v, fb_us_t start)
{
    fb_us_t last = v->length - (fb_us_t)v->width * v->per_px;

    v->start = clamp(start, 0, last > 0 ? last : 0);
}

void FB_ViewScrollPx(FBTimeView *v, int dx)
{
    FB_ViewSetStart(v, v->start + (fb_us_t)dx * v->per_px);
}

int FB_ViewEnsureVisible(FBTimeView *v, fb_us_t t)
{
    fb_us_t page = (fb_us_t)v->width * v->per_px;
    fb_us_t old = v->start;

    if (page <= 0)
        return 0;
    if (t < v->start || t >= v->start + page)
        FB_ViewSetStart(v, v->start + floordiv(t - v->start, page) * page);
    return v->start != old;
}

long FB_ViewTimeToX(const FBTimeView *v, fb_us_t t)
{
    return (long)floordiv(t - v->start, v->per_px);
}

fb_us_t FB_ViewXToTime(const FBTimeView *v, long x)
{
    return clamp(v->start + (fb_us_t)x * v->per_px, 0, v->length);
}

fb_us_t FB_ViewEnd(const FBTimeView *v)
{
    return v->start + (fb_us_t)v->width * v->per_px;
}

long FB_ViewTotalPx(const FBTimeView *v)
{
    return (long)((v->length + v->per_px - 1) / v->per_px);
}

long FB_ViewStartPx(const FBTimeView *v)
{
    return (long)(v->start / v->per_px);
}

/* ---- Ruler ---------------------------------------------------------- */

void FB_ViewRulerSteps(const FBTimeView *v, int min_label_px,
                       fb_us_t frame_us, fb_us_t *major, fb_us_t *minor)
{
    static const int frame_mult[] = { 1, 2, 5, 10, 20, 50 };
    static const int sub_sec_ms[] = { 100, 200, 500 };
    static const int secs[] = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600,
                                900, 1800 };
    fb_us_t steps[40];
    fb_us_t need = (fb_us_t)(min_label_px > 0 ? min_label_px : 1) * v->per_px;
    fb_us_t h;
    int n = 0, i;

    if (frame_us > 0) {
        for (i = 0; i < (int)(sizeof(frame_mult) / sizeof(frame_mult[0])); i++)
            if (frame_mult[i] * frame_us <= FB_US_PER_SEC / 2)
                steps[n++] = frame_mult[i] * frame_us;
    } else {
        for (i = 0; i < (int)(sizeof(sub_sec_ms) / sizeof(sub_sec_ms[0])); i++)
            steps[n++] = FB_US(sub_sec_ms[i]);
    }
    for (i = 0; i < (int)(sizeof(secs) / sizeof(secs[0])); i++)
        steps[n++] = secs[i] * FB_US_PER_SEC;
    for (h = 1; n < (int)(sizeof(steps) / sizeof(steps[0])); h *= 2)
        steps[n++] = h * 3600 * FB_US_PER_SEC;

    *major = steps[n - 1];
    for (i = 0; i < n; i++) {
        if (steps[i] >= need) {
            *major = steps[i];
            break;
        }
    }

    /* Minor ticks: the smallest of the steps above that splits the major
     * step evenly into at most 5 parts, at least 5 pixels apart. */
    *minor = 0;
    for (i = 0; i < n && steps[i] < *major; i++) {
        if (*major % steps[i] == 0 && *major / steps[i] <= 5 &&
            steps[i] / v->per_px >= 5) {
            *minor = steps[i];
            break;
        }
    }
}

char *FB_FormatTimecode(char *buf, size_t len, fb_us_t t, fb_us_t step,
                        fb_us_t frame_us, char decimal_point)
{
    long secs, h, m, s;
    fb_us_t rem;

    if (t < 0)
        t = 0;
    secs = (long)(t / FB_US_PER_SEC);
    rem = t % FB_US_PER_SEC;
    h = secs / 3600;
    m = (secs / 60) % 60;
    s = secs % 60;

    if (h > 0)
        snprintf(buf, len, "%ld:%02ld:%02ld", h, m, s);
    else
        snprintf(buf, len, "%ld:%02ld", m, s);

    if (step % FB_US_PER_SEC != 0) {
        size_t used = 0;

        while (used < len && buf[used])
            used++;
        if (frame_us > 0)
            snprintf(buf + used, len - used, ":%02ld", (long)(rem / frame_us));
        else
            snprintf(buf + used, len - used, "%c%ld", decimal_point,
                     (long)(rem / 100000));
    }
    return buf;
}

/* ---- Film-strip cells ----------------------------------------------- */

int FB_ViewCells(const FBTimeView *v, int cell_w, FBCell *cells, int max)
{
    fb_us_t dur, end = FB_ViewEnd(v);
    int64_t k;
    int n = 0;

    if (cell_w < 1 || v->width <= 0)
        return 0;
    dur = v->per_px * cell_w;
    if (end > v->length)
        end = v->length;
    for (k = floordiv(v->start, dur); n < max; k++) {
        fb_us_t t0 = k * dur;
        fb_us_t t1 = t0 + dur;

        if (t0 >= end)
            break;
        cells[n].index = k;
        cells[n].t0 = t0;
        cells[n].t1 = t1;
        /* The middle of the part that has video in it. */
        cells[n].mid = (t0 + (t1 < v->length ? t1 : v->length)) / 2;
        cells[n].x = FB_ViewTimeToX(v, t0);
        n++;
    }
    return n;
}

/* ---- Timeline lookups ----------------------------------------------- */

fb_us_t FB_TimelineClipStart(const FBTimeline *tl, int clip)
{
    if (clip < 0 || clip >= tl->num_clips)
        return 0;
    return FB_US(tl->clips[clip].start);
}

int FB_TimelineLocate(const FBTimeline *tl, fb_us_t t, fb_us_t *source_t)
{
    int i;

    if (t < 0)
        return -1;
    i = FB_TimelineClipAt(tl, FB_MS(t), NULL);
    if (i >= 0 && source_t)
        *source_t = FB_US(tl->clips[i].in) + (t - FB_US(tl->clips[i].start));
    return i;
}
