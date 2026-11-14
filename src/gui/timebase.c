/*
 * timebase.c - the time axis shared by the timeline gadgets.
 * See timebase.h.
 */
#include <proto/intuition.h>

#include "gui/timebase.h"
#include "gui/track.h"

void Timebase_Init(FBTimebase *tb, int cell_w)
{
    tb->playhead = 0;
    tb->frame_us = 0;
    tb->cell_w = cell_w;
    tb->num_tracks = 0;
    FB_ViewInit(&tb->view, 0);
    FB_ViewSetFrame(&tb->view, 0, cell_w);
}

void Timebase_Attach(FBTimebase *tb, struct Gadget *track)
{
    if (track && tb->num_tracks < FB_TIMEBASE_MAX_TRACKS)
        tb->tracks[tb->num_tracks++] = track;
}

void Timebase_SetTimeline(FBTimebase *tb, const FBTimeline *tl)
{
    int num = 0, den = 0;
    int was_empty = tb->view.length == 0;

    tb->frame_us = 0;
    if (FB_TimelineFrameRate(tl, &num, &den) && num > 0)
        tb->frame_us = (fb_us_t)den * FB_US_PER_SEC / num;
    FB_ViewSetFrame(&tb->view, tb->frame_us, tb->cell_w);
    /* The tracks go on after the last clip, with room to drag clips
     * into (see FB_TimelineViewLength). */
    FB_ViewSetLength(&tb->view, FB_US(FB_TimelineViewLength(tl)));
    if (was_empty && tb->view.length > 0)
        FB_ViewZoomToFit(&tb->view);
    Timebase_SetPlayhead(tb, tb->playhead);
}

void Timebase_SetPlayhead(FBTimebase *tb, fb_us_t t)
{
    if (t > tb->view.length)
        t = tb->view.length;
    tb->playhead = t > 0 ? t : 0;
}

void Timebase_SetZoomLevel(FBTimebase *tb, int level)
{
    long x = FB_ViewTimeToX(&tb->view, tb->playhead);

    if (x < 0 || x >= tb->view.width)
        x = tb->view.width / 2;
    FB_ViewSetZoomLevel(&tb->view, level, (int)x);
}

void Timebase_Redraw(FBTimebase *tb, struct Window *win,
                     struct Gadget *except)
{
    int i;

    for (i = 0; i < tb->num_tracks; i++)
        if (tb->tracks[i] != except)
            IIntuition->SetGadgetAttrs(tb->tracks[i], win, NULL,
                                       TRACK_Redraw, TRUE, TAG_END);
}
