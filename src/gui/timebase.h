/*
 * timebase.h - the time axis shared by the timeline gadgets.
 *
 * The ruler, the film strip and (later) the waveforms all show the same
 * stretch of time. They point at one FBTimebase, which holds the view
 * (zoom and scroll position) and the playhead. When one track changes it,
 * the window redraws the others with Timebase_Redraw(), so they stay in
 * sync. A track that should scroll on its own would get its own timebase.
 *
 * Each track redraws itself straight away while it is being dragged (that
 * happens in Intuition's input task). It then notifies the window, and the
 * window redraws the other tracks from the program's own task.
 */
#ifndef FORMBROD_TIMEBASE_H
#define FORMBROD_TIMEBASE_H

#include <exec/types.h>
#include <intuition/intuition.h>
#include "formbrod/timeview.h"

#define FB_TIMEBASE_MAX_TRACKS 16

typedef struct FBTimebase {
    FBTimeView     view;
    fb_us_t        playhead;    /* timeline time, 0..view.length */
    fb_us_t        frame_us;    /* length of a frame, 0 if unknown */
    int            cell_w;      /* film-strip cell width; sets max zoom */
    int            num_tracks;
    struct Gadget *tracks[FB_TIMEBASE_MAX_TRACKS];
} FBTimebase;

void Timebase_Init(FBTimebase *tb, int cell_w);

/* Adds a track gadget, so Timebase_Redraw() redraws it. */
void Timebase_Attach(FBTimebase *tb, struct Gadget *track);

/* Takes the length and frame rate from the timeline. When the timeline
 * goes from empty to not empty, zooms so all of it fits. */
void Timebase_SetTimeline(FBTimebase *tb, const FBTimeline *tl);

/* Moves the playhead (clamped to the length). */
void Timebase_SetPlayhead(FBTimebase *tb, fb_us_t t);

/* Sets the zoom level (see FB_ViewZoomLevels), keeping the playhead where
 * it is on screen if it is visible, else the middle of the view. */
void Timebase_SetZoomLevel(FBTimebase *tb, int level);

/* Redraws every attached track except `except` (may be NULL). */
void Timebase_Redraw(FBTimebase *tb, struct Window *win,
                     struct Gadget *except);

#endif
