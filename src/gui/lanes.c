/*
 * lanes.c - the track editor: one row per track under the film strip.
 *
 *   Video 2    [ logo.png ]
 *   Video 1    [ holiday.mp4      ][ skate.avi        ]
 *              [ ~~~~ its sound ~~][ ~~~~~~~~~~~~~~~~ ]   linked sound lane
 *   Audio 1    [ theme.mod                                ]
 *
 *              Drop files here: each gets a track of its own
 *
 * A subclass of TrackClass (track.c), so it shares the time axis, zoom,
 * scrolling and playhead with the ruler and the film strip. Rows are
 * picture tracks from the top one down, each followed by a lane showing
 * its clips' sound (the same clips: a picture clip carries its sound),
 * then the sound tracks. Below them is the drop area.
 *
 * Mouse:
 *   - a track's name selects the track (for the Track menu);
 *   - a clip selects it;
 *   - a clip's edge, dragged, trims it: the clip is drawn at its new
 *     length while dragging, and on release the window is told the new in
 *     and out points (LANES_EditClip/In/Out);
 *   - a clip's middle, dragged, moves it. Its old place closes up on the
 *     screen straight away, and an outline shows where it will land: where
 *     it's dropped if there's room (leaving a gap in front of it), else in
 *     between the clips there, with a line marking the spot. Its ends
 *     snap to other clips' ends, the start and the playhead when near
 *     them. Below the tracks means a new track. On release
 *     the window is told the clip, track and start (LANES_MoveClip/Track/
 *     At) and moves it with App_MoveClipTo(). A track it can't go on (a
 *     video clip on an audio track) shows a grey outline and no line.
 *     The gadget never changes the timeline itself; the window does;
 *   - anywhere else moves the playhead, like the ruler.
 */
#include <stdio.h>
#include <string.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/utility.h>
#include <proto/dos.h>

#include <intuition/gadgetclass.h>
#include <intuition/icclass.h>
#include <intuition/screens.h>
#include <graphics/text.h>
#include <devices/inputevent.h>

#include "gui/track.h"
#include "gui/timebase.h"

#define MAX_ROWS     (FB_MAX_TRACKS * 2)
#define EDGE_PX      4         /* how near an edge a click must be */
#define DROP_MIN_H   40        /* the drop area is at least this tall */

/* Clip colours: fill, edge, text. */
#define COL_PIC_FILL   0x5DCAA5
#define COL_PIC_EDGE   0x0F6E56
#define COL_PIC_TEXT   0x04342C
#define COL_STILL_FILL 0xAFA9EC
#define COL_STILL_EDGE 0x534AB7
#define COL_STILL_TEXT 0x26215C
#define COL_SND_FILL   0xF0997B
#define COL_SND_EDGE   0x993C1D
#define COL_SND_TEXT   0x4A1B0C
#define COL_LINK_FILL  0xF5C4B3
#define COL_OFF_FILL   0xB4B2A9      /* muted or hidden */
#define COL_OFF_EDGE   0x5F5E5A
#define COL_OFF_TEXT   0x2C2C2A
#define COL_SELECTED   0xFFFFFF
#define COL_GHOST      0xFFFFFF      /* a dragged clip's outline */
#define COL_GHOST_NO   0x888780      /* ...where it can't go */
#define COL_INSERT     0xEF9F27      /* where it will go in */
#define DRAG_PX        3             /* move this far before it's a drag */
#define SNAP_PX        6             /* ends this near something snap to it */

enum { ROW_PICTURE, ROW_LINKED, ROW_SOUND };

typedef struct Row {
    int  track, kind;
    long y, h;              /* from the gadget's top */
} Row;

enum { MODE_NONE, MODE_PLAYHEAD, MODE_TRIM_IN, MODE_TRIM_OUT, MODE_PRESS,
       MODE_MOVE };

struct LanesData {
    FBTimebase       *tb;
    const FBTimeline *tl;
    int               inset;
    int               selected;      /* clip, -1 for none */
    int               track;         /* selected track, -1 for none */
    CONST_STRPTR      drop_text, picture_name, sound_name;
    int               mode;
    int               drag_clip;
    fb_ms_t           drag_in, drag_out;   /* while trimming */
    int               font_h;              /* from the last drawing */
    /* While moving a clip: where it was picked up, how far into the clip
     * (ms), and where it is now: track (-1 below the tracks: a new one),
     * start, and whether it can go there. */
    long              press_x, press_y;
    fb_ms_t           grab;
    int               to_track;
    fb_ms_t           to_start;      /* where it's dropped */
    BOOL              to_ok;
    fb_ms_t           to_land;       /* where it will go */
    int               to_insert;     /* in between clips (they move on) */
};

/* ---- Rows ------------------------------------------------------------ */

static int track_has_sound(const FBTimeline *tl, int track)
{
    int i;

    for (i = 0; i < tl->num_clips; i++)
        if (tl->clips[i].track == track &&
            tl->sources[tl->clips[i].source].has_audio)
            return 1;
    return 0;
}

static long row_height(int kind, int font_h)
{
    switch (kind) {
    case ROW_PICTURE: return font_h + 12;
    case ROW_LINKED:  return font_h / 2 + 6;
    default:          return font_h + 8;
    }
}

/* Picture tracks from the top down, each with its sound lane, then the
 * sound tracks. Returns the number of rows; *bottom gets where they end. */
static int make_rows(const FBTimeline *tl, int font_h, Row *rows, long *bottom)
{
    long y = 2;
    int n = 0, t;

    for (t = tl->num_tracks - 1; t >= 0; t--) {
        if (tl->tracks[t].kind != FB_TRACK_VIDEO)
            continue;
        rows[n].track = t;
        rows[n].kind = ROW_PICTURE;
        rows[n].y = y;
        rows[n].h = row_height(ROW_PICTURE, font_h);
        y += rows[n++].h;
        if (track_has_sound(tl, t)) {
            rows[n].track = t;
            rows[n].kind = ROW_LINKED;
            rows[n].y = y;
            rows[n].h = row_height(ROW_LINKED, font_h);
            y += rows[n++].h;
        }
        y += 3;
    }
    for (t = 0; t < tl->num_tracks; t++) {
        if (tl->tracks[t].kind != FB_TRACK_AUDIO)
            continue;
        rows[n].track = t;
        rows[n].kind = ROW_SOUND;
        rows[n].y = y;
        rows[n].h = row_height(ROW_SOUND, font_h);
        y += rows[n++].h + 3;
    }
    *bottom = y;
    return n;
}

static int row_at(const Row *rows, int n, long y)
{
    int i;

    for (i = 0; i < n; i++)
        if (y >= rows[i].y && y < rows[i].y + rows[i].h + 3)
            return i;
    return -1;
}

/* "Video 2" / "Audio 1": numbered among the tracks of the same kind,
 * from the bottom one. */
static void track_name(const struct LanesData *ld, int track, char *buf,
                       size_t len)
{
    const FBTimeline *tl = ld->tl;
    FBTrackKind kind = tl->tracks[track].kind;
    long n = 0;
    int t;

    for (t = 0; t <= track; t++)
        if (tl->tracks[t].kind == kind)
            n++;
    snprintf(buf, len, kind == FB_TRACK_VIDEO
                       ? (ld->picture_name ? ld->picture_name : "Video %ld")
                       : (ld->sound_name ? ld->sound_name : "Audio %ld"), n);
}

/* ---- Where a clip is ----------------------------------------------- */

/* A clip's left and right pixel on the time axis, as it's shown: while
 * it's being trimmed, at its new length. */
static void clip_span(const struct LanesData *ld, int clip, long *x0, long *x1)
{
    const FBClip *c = &ld->tl->clips[clip];
    const FBTimeView *v = &ld->tb->view;
    fb_ms_t in = c->in, out = c->out, start = c->start;

    if ((ld->mode == MODE_TRIM_IN || ld->mode == MODE_TRIM_OUT) &&
        clip == ld->drag_clip) {
        in = ld->drag_in;
        out = ld->drag_out;
    }
    /* While one is being moved, the clips after it on its track are
     * already shown moved up into its place. */
    if (ld->mode == MODE_MOVE)
        start = FB_TimelineStartWithout(ld->tl, clip, ld->drag_clip);
    *x0 = FB_ViewTimeToX(v, FB_US(start + (in - c->in)));
    *x1 = FB_ViewTimeToX(v, FB_US(start + (out - c->in))) - 1;
}

enum { HIT_NONE, HIT_BODY, HIT_IN, HIT_OUT };

/* What's under x (on the time axis) in a track's row: a clip's edge, its
 * body, or nothing. Returns the clip or -1. */
static int clip_at(const struct LanesData *ld, int track, long x, int *hit)
{
    const FBTimeline *tl = ld->tl;
    int i;

    *hit = HIT_NONE;
    for (i = 0; i < tl->num_clips; i++) {
        long x0, x1;

        if (tl->clips[i].track != track)
            continue;
        clip_span(ld, i, &x0, &x1);
        if (x >= x0 && x <= x1) {
            if (x1 - x0 >= 3 * EDGE_PX && x - x0 < EDGE_PX)
                *hit = HIT_IN;
            else if (x1 - x < EDGE_PX)
                *hit = HIT_OUT;
            else
                *hit = HIT_BODY;
            return i;
        }
        /* Just past the end of a clip: its end, to make it longer. */
        if (x > x1 && x <= x1 + EDGE_PX &&
            (i + 1 >= tl->num_clips || tl->clips[i + 1].track != track ||
             FB_ViewTimeToX(&ld->tb->view,
                            FB_US(tl->clips[i + 1].start)) > x)) {
            *hit = HIT_OUT;
            return i;
        }
    }
    return -1;
}

/* ---- Drawing ----------------------------------------------------------- */

static void draw_clip(const struct LanesData *ld, struct trackDraw *d,
                      const Row *row, int clip)
{
    struct RastPort *rp = d->trd_RPort;
    struct TextFont *font = d->trd_DrInfo->dri_Font;
    const FBTimeline *tl = ld->tl;
    const FBClip *c = &tl->clips[clip];
    const FBSource *src = &tl->sources[c->source];
    const FBTrack *tr = &tl->tracks[row->track];
    long left = d->trd_Box.Left, w = d->trd_Box.Width;
    long top = d->trd_Box.Top + row->y, bottom = top + row->h - 1;
    long x0, x1;
    uint32 fill, edge, text;
    struct Rectangle box;
    int off;

    if (row->kind == ROW_LINKED && !src->has_audio)
        return;
    clip_span(ld, clip, &x0, &x1);
    if (x1 < 0 || x0 >= w)
        return;
    box.MinX = left + (x0 > 0 ? x0 : 0);
    box.MaxX = left + (x1 < w ? x1 : w - 1);
    box.MinY = top;
    box.MaxY = bottom;

    off = row->kind == ROW_PICTURE ? tr->hidden : tr->muted;
    if (off) {
        fill = COL_OFF_FILL; edge = COL_OFF_EDGE; text = COL_OFF_TEXT;
    } else if (row->kind == ROW_PICTURE && src->still) {
        fill = COL_STILL_FILL; edge = COL_STILL_EDGE; text = COL_STILL_TEXT;
    } else if (row->kind == ROW_PICTURE) {
        fill = COL_PIC_FILL; edge = COL_PIC_EDGE; text = COL_PIC_TEXT;
    } else {
        fill = row->kind == ROW_LINKED ? COL_LINK_FILL : COL_SND_FILL;
        edge = COL_SND_EDGE; text = COL_SND_TEXT;
    }

    Track_SetRGB(rp, edge);
    IGraphics->RectFill(rp, box.MinX, box.MinY, box.MaxX, box.MaxY);
    Track_SetRGB(rp, fill);
    Track_FillClipped(rp, &box, left + x0 + 1, top + 1, left + x1 - 1,
                      bottom - 1);

    if (row->kind == ROW_LINKED) {
        /* No waveform yet: a line through the middle. */
        Track_SetRGB(rp, edge);
        Track_FillClipped(rp, &box, left + x0 + 2, top + row->h / 2,
                          left + x1 - 2, top + row->h / 2);
    } else if (x1 - x0 > 16) {
        long tx = left + (x0 > 0 ? x0 : 0) + 4;

        IGraphics->SetFont(rp, font);
        Track_SetRGB(rp, text);
        Track_Text(rp, font, tx, top + (row->h - font->tf_YSize) / 2,
                   box.MaxX - tx - 2, IDOS->FilePart(src->path));
    }

    if (clip == ld->selected) {
        /* A white frame, and grips at the ends to show they can be
         * dragged. */
        Track_SetRGB(rp, COL_SELECTED);
        Track_FillClipped(rp, &box, left + x0, top, left + x1, top + 1);
        Track_FillClipped(rp, &box, left + x0, bottom - 1, left + x1, bottom);
        Track_FillClipped(rp, &box, left + x0, top, left + x0 + 2, bottom);
        Track_FillClipped(rp, &box, left + x1 - 2, top, left + x1, bottom);
    }
}

/* While a clip is dragged: its outline where it is, and on a track it can
 * go on, a line where it will go in. */
static void draw_move(const struct LanesData *ld, struct trackDraw *d,
                      const Row *rows, int n, long bottom)
{
    struct RastPort *rp = d->trd_RPort;
    const FBTimeline *tl = ld->tl;
    const FBClip *c = &tl->clips[ld->drag_clip];
    const FBTimeView *v = &ld->tb->view;
    long left = d->trd_Box.Left, top = d->trd_Box.Top;
    long w = d->trd_Box.Width;
    long y0, y1, x0, x1;
    struct Rectangle box;
    int r;

    box.MinX = left;
    box.MaxX = left + w - 1;
    box.MinY = top;
    box.MaxY = top + d->trd_Box.Height - 1;

    /* The row it's over: the picture or audio row of the track. */
    y0 = top + bottom + 2;
    y1 = y0 + row_height(ROW_PICTURE, ld->font_h) - 1;
    for (r = 0; ld->to_track >= 0 && r < n; r++)
        if (rows[r].track == ld->to_track && rows[r].kind != ROW_LINKED) {
            y0 = top + rows[r].y;
            y1 = y0 + rows[r].h - 1;
            break;
        }

    x0 = left + FB_ViewTimeToX(v, FB_US(ld->to_land));
    x1 = left + FB_ViewTimeToX(v, FB_US(ld->to_land + (c->out - c->in))) - 1;
    Track_SetRGB(rp, ld->to_ok ? COL_GHOST : COL_GHOST_NO);
    Track_FillClipped(rp, &box, x0, y0, x1, y0 + 1);
    Track_FillClipped(rp, &box, x0, y1 - 1, x1, y1);
    Track_FillClipped(rp, &box, x0, y0, x0 + 1, y1);
    Track_FillClipped(rp, &box, x1 - 1, y0, x1, y1);

    if (ld->to_ok && ld->to_insert) {
        /* Going in between clips: a line where. */
        Track_SetRGB(rp, COL_INSERT);
        Track_FillClipped(rp, &box, x0 - 1, y0 - 2, x0 + 1, y1 + 2);
    }
}

static void lanes_draw(struct LanesData *ld, struct trackDraw *d)
{
    struct RastPort *rp = d->trd_RPort;
    struct DrawInfo *dri = d->trd_DrInfo;
    struct TextFont *font = dri->dri_Font;
    const FBTimeline *tl = ld->tl;
    long left = d->trd_Box.Left, top = d->trd_Box.Top;
    long right = left + d->trd_Box.Width - 1;
    long gbottom = top + d->trd_Box.Height - 1;
    Row rows[MAX_ROWS];
    long bottom;
    int n, r, i;

    ld->font_h = font->tf_YSize;
    n = make_rows(tl, font->tf_YSize, rows, &bottom);
    IGraphics->SetFont(rp, font);

    for (r = 0; r < n && top + rows[r].y <= gbottom; r++) {
        const Row *row = &rows[r];
        const FBTrack *tr = &tl->tracks[row->track];
        long y0 = top + row->y, y1 = y0 + row->h - 1;

        /* The name column. */
        if (row->kind != ROW_LINKED && ld->inset > 4) {
            char name[32];
            BOOL sel = row->track == ld->track;
            BOOL off = row->kind == ROW_PICTURE ? tr->hidden : tr->muted;

            track_name(ld, row->track, name, sizeof(name));
            IGraphics->SetAPen(rp, dri->dri_Pens[sel ? FILLPEN
                                                     : BACKGROUNDPEN]);
            IGraphics->RectFill(rp, left - ld->inset, y0, left - 3,
                                y1 > gbottom ? gbottom : y1);
            IGraphics->SetAPen(rp, dri->dri_Pens[sel ? FILLTEXTPEN
                                                : off ? SHADOWPEN : TEXTPEN]);
            Track_Text(rp, font, left - ld->inset + 3,
                       y0 + (row->h - font->tf_YSize) / 2, ld->inset - 8,
                       name);
            if (off) {
                /* Struck through: muted or hidden. */
                long w = IGraphics->TextLength(rp, name, strlen(name));

                if (w > ld->inset - 8)
                    w = ld->inset - 8;
                IGraphics->RectFill(rp, left - ld->inset + 3,
                                    y0 + row->h / 2,
                                    left - ld->inset + 3 + w, y0 + row->h / 2);
            }
        }

        for (i = 0; i < tl->num_clips; i++)
            if (tl->clips[i].track == row->track &&
                !(ld->mode == MODE_MOVE && i == ld->drag_clip))
                draw_clip(ld, d, row, i);
    }

    if (ld->mode == MODE_MOVE)
        draw_move(ld, d, rows, n, bottom);

    /* The drop area below the tracks. */
    if (ld->drop_text && top + bottom + font->tf_YSize < gbottom) {
        long y = top + bottom + (gbottom - top - bottom - font->tf_YSize) / 2;
        long w = IGraphics->TextLength(rp, ld->drop_text,
                                       strlen(ld->drop_text));
        long x = left + (w < right - left ? (right - left - w) / 2 : 0);

        IGraphics->SetAPen(rp, dri->dri_Pens[SHADOWPEN]);
        Track_Text(rp, font, x, y, right - x, ld->drop_text);
    }
}

/* ---- Input --------------------------------------------------------------- */

static void lanes_redraw(Object *o, struct GadgetInfo *gi)
{
    struct RastPort *rp;

    if (gi && (rp = IIntuition->ObtainGIRPort(gi))) {
        IIntuition->IDoMethod(o, GM_RENDER, gi, rp, GREDRAW_UPDATE);
        IIntuition->ReleaseGIRPort(rp);
    }
}

/* Sends tags (ending in TAG_END, with room for GA_ID before it) to the
 * window. */
static void lanes_notify(Class *cl, Object *o, struct GadgetInfo *gi,
                         struct TagItem *tags, int n)
{
    struct opUpdate msg;

    tags[n].ti_Tag  = GA_ID;
    tags[n].ti_Data = ((struct Gadget *)o)->GadgetID;
    tags[n + 1].ti_Tag  = TAG_END;
    tags[n + 1].ti_Data = 0;
    msg.MethodID     = OM_NOTIFY;
    msg.opu_AttrList = tags;
    msg.opu_GInfo    = gi;
    msg.opu_Flags    = 0;
    IIntuition->IDoSuperMethodA(cl, o, (Msg)&msg);
}

static void notify_selection(Class *cl, Object *o, struct GadgetInfo *gi,
                             struct LanesData *ld)
{
    struct TagItem tags[4];

    tags[0].ti_Tag  = LANES_Selected;
    tags[0].ti_Data = (uint32)ld->selected;
    tags[1].ti_Tag  = LANES_Track;
    tags[1].ti_Data = (uint32)ld->track;
    lanes_notify(cl, o, gi, tags, 2);
}

/* The in or out point for the mouse at time-axis pixel x while trimming,
 * snapped to a frame and kept to at least one frame and within the file. */
static void trim_to(struct LanesData *ld, long x)
{
    const FBClip *c = &ld->tl->clips[ld->drag_clip];
    const FBSource *src = &ld->tl->sources[c->source];
    fb_us_t frame = ld->tb->frame_us > 0 ? ld->tb->frame_us
                                         : FB_DEFAULT_FRAME_US;
    fb_us_t t = FB_ViewXToTime(&ld->tb->view, x);
    fb_ms_t min = FB_MS(frame) > 0 ? FB_MS(frame) : 1;
    fb_ms_t at;

    t -= t % frame;
    at = c->in + (FB_MS(t) - c->start);   /* the same point in the file */
    if (ld->mode == MODE_TRIM_IN) {
        if (at < 0)
            at = 0;
        if (at > c->out - min)
            at = c->out - min;
        ld->drag_in = at;
    } else {
        if (at < c->in + min)
            at = c->in + min;
        if (src->duration > 0 && at > src->duration)
            at = src->duration;
        ld->drag_out = at;
    }
}

static uint32 lanes_go_active(Class *cl, Object *o, struct gpInput *msg)
{
    struct LanesData *ld = INST_DATA(cl, o);
    long x = msg->gpi_Mouse.X - ld->inset, y = msg->gpi_Mouse.Y;
    struct TextFont *font;
    Row rows[MAX_ROWS];
    long bottom;
    int n, r, clip, hit;

    ld->mode = MODE_NONE;
    if (!msg->gpi_IEvent || !ld->tb || !ld->tl || ld->tl->num_clips == 0 ||
        !msg->gpi_GInfo || !msg->gpi_GInfo->gi_DrInfo)
        return GMR_NOREUSE;
    font = msg->gpi_GInfo->gi_DrInfo->dri_Font;
    n = make_rows(ld->tl, font->tf_YSize, rows, &bottom);
    r = row_at(rows, n, y);

    if (x < 0) {
        /* The name column: select the track. */
        ld->track = r >= 0 ? rows[r].track : -1;
        lanes_redraw(o, msg->gpi_GInfo);
        notify_selection(cl, o, msg->gpi_GInfo, ld);
        return GMR_NOREUSE;
    }
    clip = r >= 0 ? clip_at(ld, rows[r].track, x, &hit) : -1;
    if (clip < 0) {
        /* Empty space or the drop area: move the playhead. */
        ld->mode = MODE_PLAYHEAD;
        return IIntuition->IDoSuperMethodA(cl, o, (Msg)msg);
    }

    ld->selected = clip;
    ld->track = rows[r].track;
    notify_selection(cl, o, msg->gpi_GInfo, ld);
    if (hit == HIT_IN || hit == HIT_OUT) {
        ld->mode = hit == HIT_IN ? MODE_TRIM_IN : MODE_TRIM_OUT;
        ld->drag_clip = clip;
        ld->drag_in = ld->tl->clips[clip].in;
        ld->drag_out = ld->tl->clips[clip].out;
        lanes_redraw(o, msg->gpi_GInfo);
        return GMR_MEACTIVE;
    }
    /* Maybe the start of a drag to move it. */
    ld->mode = MODE_PRESS;
    ld->drag_clip = clip;
    ld->press_x = msg->gpi_Mouse.X;
    ld->press_y = y;
    ld->grab = FB_MS(FB_ViewXToTime(&ld->tb->view, x)) -
               ld->tl->clips[clip].start;
    lanes_redraw(o, msg->gpi_GInfo);
    return GMR_MEACTIVE;
}

/* Moves t to `to` if it is within SNAP_PX of it; *best keeps the
 * nearest so far. */
static void snap_near(fb_ms_t t, fb_ms_t to, fb_ms_t range, fb_ms_t *best,
                      fb_ms_t *dist)
{
    fb_ms_t d = t > to ? t - to : to - t;

    if (d <= range && d < *dist) {
        *dist = d;
        *best = to;
    }
}

/* Lets a dragged clip's start or end snap to the start of the edit, the
 * playhead, and the ends of the clips on the track it's over (as they
 * will be once it's out of its place). */
static void snap(struct LanesData *ld, fb_ms_t len)
{
    const FBTimeline *tl = ld->tl;
    fb_ms_t range = FB_MS((fb_us_t)SNAP_PX * ld->tb->view.per_px);
    fb_ms_t start = ld->to_start, end = start + len;
    fb_ms_t best_s = start, dist_s = range + 1;
    fb_ms_t best_e = end, dist_e = range + 1;
    fb_ms_t playhead = FB_MS(ld->tb->playhead);
    int i;

    snap_near(start, 0, range, &best_s, &dist_s);
    snap_near(start, playhead, range, &best_s, &dist_s);
    snap_near(end, playhead, range, &best_e, &dist_e);
    for (i = 0; ld->to_track >= 0 && i < tl->num_clips; i++) {
        const FBClip *c = &tl->clips[i];
        fb_ms_t s;

        if (i == ld->drag_clip || c->track != ld->to_track)
            continue;
        s = FB_TimelineStartWithout(tl, i, ld->drag_clip);
        snap_near(start, s + (c->out - c->in), range, &best_s, &dist_s);
        snap_near(start, s, range, &best_s, &dist_s);
        snap_near(end, s, range, &best_e, &dist_e);
        snap_near(end, s + (c->out - c->in), range, &best_e, &dist_e);
    }
    if (dist_s <= range && dist_s <= dist_e)
        ld->to_start = best_s;
    else if (dist_e <= range && best_e - len >= 0)
        ld->to_start = best_e - len;
}

/* Where a dragged clip is now, for the pointer at x (on the time axis)
 * and y (from the gadget's top). */
static void move_to(struct LanesData *ld, long x, long y)
{
    const FBTimeline *tl = ld->tl;
    const FBClip *c = &tl->clips[ld->drag_clip];
    fb_us_t frame = ld->tb->frame_us > 0 ? ld->tb->frame_us
                                         : FB_DEFAULT_FRAME_US;
    fb_us_t t = FB_ViewXToTime(&ld->tb->view, x);
    Row rows[MAX_ROWS];
    long bottom;
    int n, r;

    t -= t % frame;
    ld->to_start = FB_MS(t) - ld->grab;
    if (ld->to_start < 0)
        ld->to_start = 0;
    n = make_rows(tl, ld->font_h > 0 ? ld->font_h : 8, rows, &bottom);
    r = row_at(rows, n, y);
    if (r >= 0) {
        ld->to_track = rows[r].track;
        ld->to_ok = FB_TrackFits(tl, ld->to_track, &tl->sources[c->source]);
    } else if (y >= bottom) {
        ld->to_track = -1;             /* below the tracks: a new track */
        ld->to_ok = TRUE;
    } else {
        ld->to_ok = FALSE;              /* above the top track */
    }
    snap(ld, c->out - c->in);

    ld->to_insert = 0;
    ld->to_land = ld->to_start;
    if (ld->to_ok && ld->to_track >= 0)
        ld->to_land = FB_TimelineDropPoint(tl, ld->to_track, ld->to_start,
                                           c->out - c->in, ld->drag_clip,
                                           &ld->to_insert);
}

static uint32 lanes_handle_input(Class *cl, Object *o, struct gpInput *msg)
{
    struct LanesData *ld = INST_DATA(cl, o);
    struct InputEvent *ie = msg->gpi_IEvent;
    long x = msg->gpi_Mouse.X - ld->inset;
    const FBClip *c;

    if (ld->mode == MODE_PLAYHEAD)
        return IIntuition->IDoSuperMethodA(cl, o, (Msg)msg);
    if (ld->mode == MODE_NONE)
        return GMR_NOREUSE;
    if (ie->ie_Class != IECLASS_RAWMOUSE)
        return GMR_MEACTIVE;

    if (ld->mode == MODE_PRESS || ld->mode == MODE_MOVE) {
        long y = msg->gpi_Mouse.Y;

        switch (ie->ie_Code) {
        case SELECTUP:
            if (ld->mode == MODE_MOVE) {
                move_to(ld, x, y);
                if (ld->to_ok) {
                    struct TagItem tags[5];

                    tags[0].ti_Tag  = LANES_MoveClip;
                    tags[0].ti_Data = (uint32)ld->drag_clip;
                    tags[1].ti_Tag  = LANES_MoveTrack;
                    tags[1].ti_Data = (uint32)ld->to_track;
                    tags[2].ti_Tag  = LANES_MoveAt;
                    tags[2].ti_Data = (uint32)ld->to_start;
                    lanes_notify(cl, o, msg->gpi_GInfo, tags, 3);
                }
                ld->mode = MODE_NONE;
                lanes_redraw(o, msg->gpi_GInfo);
            }
            ld->mode = MODE_NONE;
            *msg->gpi_Termination = 0;
            return GMR_NOREUSE | GMR_VERIFY;
        case MENUDOWN:
            ld->mode = MODE_NONE;
            lanes_redraw(o, msg->gpi_GInfo);
            return GMR_REUSE;
        case IECODE_NOBUTTON: {
            BOOL started = FALSE;

            if (ld->mode == MODE_PRESS &&
                (msg->gpi_Mouse.X - ld->press_x > DRAG_PX ||
                 ld->press_x - msg->gpi_Mouse.X > DRAG_PX ||
                 y - ld->press_y > DRAG_PX || ld->press_y - y > DRAG_PX)) {
                ld->mode = MODE_MOVE;
                started = TRUE;         /* its old place closes up now */
            }
            if (ld->mode == MODE_MOVE) {
                int track = ld->to_track;
                fb_ms_t land = ld->to_land;
                BOOL ok = ld->to_ok;

                move_to(ld, x, y);
                if (started || track != ld->to_track ||
                    land != ld->to_land || ok != ld->to_ok)
                    lanes_redraw(o, msg->gpi_GInfo);
            }
            break;
        }
        }
        return GMR_MEACTIVE;
    }

    c = &ld->tl->clips[ld->drag_clip];
    switch (ie->ie_Code) {
    case SELECTUP:
        trim_to(ld, x);
        if (ld->drag_in != c->in || ld->drag_out != c->out) {
            struct TagItem tags[5];

            tags[0].ti_Tag  = LANES_EditClip;
            tags[0].ti_Data = (uint32)ld->drag_clip;
            tags[1].ti_Tag  = LANES_EditIn;
            tags[1].ti_Data = (uint32)ld->drag_in;
            tags[2].ti_Tag  = LANES_EditOut;
            tags[2].ti_Data = (uint32)ld->drag_out;
            lanes_notify(cl, o, msg->gpi_GInfo, tags, 3);
        }
        ld->mode = MODE_NONE;
        *msg->gpi_Termination = 0;
        return GMR_NOREUSE | GMR_VERIFY;
    case MENUDOWN:
        /* Right button: put it back as it was. */
        ld->mode = MODE_NONE;
        lanes_redraw(o, msg->gpi_GInfo);
        return GMR_REUSE;
    case IECODE_NOBUTTON: {
        fb_ms_t in = ld->drag_in, out = ld->drag_out;

        trim_to(ld, x);
        if (in != ld->drag_in || out != ld->drag_out)
            lanes_redraw(o, msg->gpi_GInfo);
        break;
    }
    }
    return GMR_MEACTIVE;
}

/* ---- Attributes and dispatcher ----------------------------------------- */

static BOOL lanes_set(struct LanesData *ld, struct TagItem *tags)
{
    struct TagItem *state = tags, *ti;
    BOOL redraw = FALSE;

    while ((ti = IUtility->NextTagItem(&state))) {
        switch (ti->ti_Tag) {
        case TRACK_Timebase:
            ld->tb = (FBTimebase *)ti->ti_Data;
            break;
        case TRACK_Timeline:
            ld->tl = (const FBTimeline *)ti->ti_Data;
            break;
        case TRACK_Inset:
            ld->inset = (int)ti->ti_Data;
            break;
        case LANES_Selected:
            if (ld->selected != (int)(int32)ti->ti_Data) {
                ld->selected = (int)(int32)ti->ti_Data;
                redraw = TRUE;
            }
            break;
        case LANES_Track:
            if (ld->track != (int)(int32)ti->ti_Data) {
                ld->track = (int)(int32)ti->ti_Data;
                redraw = TRUE;
            }
            break;
        case LANES_DropText:
            ld->drop_text = (CONST_STRPTR)ti->ti_Data;
            break;
        case LANES_PictureName:
            ld->picture_name = (CONST_STRPTR)ti->ti_Data;
            break;
        case LANES_SoundName:
            ld->sound_name = (CONST_STRPTR)ti->ti_Data;
            break;
        }
    }
    return redraw;
}

static uint32 lanes_domain(struct gpDomain *msg)
{
    struct TextFont *font = msg->gpd_RPort ? msg->gpd_RPort->Font : NULL;
    int fh;

    if (!font && msg->gpd_GInfo && msg->gpd_GInfo->gi_DrInfo)
        font = msg->gpd_GInfo->gi_DrInfo->dri_Font;
    fh = font ? font->tf_YSize : 8;
    msg->gpd_Domain.Left = 0;
    msg->gpd_Domain.Top  = 0;
    switch (msg->gpd_Which) {
    case GDOMAIN_MINIMUM:
        msg->gpd_Domain.Width  = 64;
        msg->gpd_Domain.Height = fh + 12 + DROP_MIN_H;
        break;
    case GDOMAIN_MAXIMUM:
        msg->gpd_Domain.Width  = 16383;
        msg->gpd_Domain.Height = 16383;
        break;
    default:
        msg->gpd_Domain.Width  = 400;
        msg->gpd_Domain.Height = 3 * (fh + 12) + 2 * DROP_MIN_H;
        break;
    }
    return 1;
}

static uint32 lanes_track_at(struct LanesData *ld, Object *o,
                             struct lanesTrackAt *msg)
{
    struct Gadget *g = (struct Gadget *)o;
    long x = msg->lta_X - g->LeftEdge, y = msg->lta_Y - g->TopEdge;
    Row rows[MAX_ROWS];
    long bottom;
    int n, r;

    if (!ld->tl || x < 0 || y < 0 || x >= g->Width || y >= g->Height)
        return (uint32)-2;
    n = make_rows(ld->tl, ld->font_h > 0 ? ld->font_h : 8, rows, &bottom);
    r = row_at(rows, n, y);
    return (uint32)(r >= 0 ? rows[r].track : -1);
}

static uint32 lanes_dispatch(Class *cl, Object *o, Msg msg)
{
    struct LanesData *ld;
    uint32 ret;

    switch (msg->MethodID) {
    case OM_NEW:
        o = (Object *)IIntuition->IDoSuperMethodA(cl, o, msg);
        if (o) {
            ld = INST_DATA(cl, o);
            memset(ld, 0, sizeof(*ld));
            ld->selected = -1;
            ld->track = -1;
            lanes_set(ld, ((struct opSet *)msg)->ops_AttrList);
        }
        return (uint32)o;

    case OM_SET:
    case OM_UPDATE:
        ld = INST_DATA(cl, o);
        if (lanes_set(ld, ((struct opSet *)msg)->ops_AttrList))
            lanes_redraw(o, ((struct opSet *)msg)->ops_GInfo);
        return IIntuition->IDoSuperMethodA(cl, o, msg);

    case OM_GET:
        ld = INST_DATA(cl, o);
        switch (((struct opGet *)msg)->opg_AttrID) {
        case LANES_Selected:
            *((struct opGet *)msg)->opg_Storage = (uint32)ld->selected;
            return TRUE;
        case LANES_Track:
            *((struct opGet *)msg)->opg_Storage = (uint32)ld->track;
            return TRUE;
        }
        break;

    case GM_DOMAIN:
        return lanes_domain((struct gpDomain *)msg);

    case GM_GOACTIVE:
        return lanes_go_active(cl, o, (struct gpInput *)msg);

    case GM_HANDLEINPUT:
        return lanes_handle_input(cl, o, (struct gpInput *)msg);

    case GM_GOINACTIVE:
        ld = INST_DATA(cl, o);
        ret = IIntuition->IDoSuperMethodA(cl, o, msg);
        if (ld->mode >= MODE_TRIM_IN) {
            /* Cut short (window closed, another gadget...): as it was. */
            ld->mode = MODE_NONE;
            lanes_redraw(o, ((struct gpGoInactive *)msg)->gpgi_GInfo);
        }
        ld->mode = MODE_NONE;
        return ret;

    case TRACKM_DRAWCONTENT:
        ld = INST_DATA(cl, o);
        if (ld->tl && ld->tb)
            lanes_draw(ld, (struct trackDraw *)msg);
        return 0;

    case LANESM_TRACKAT:
        return lanes_track_at(INST_DATA(cl, o), o,
                              (struct lanesTrackAt *)msg);
    }
    return IIntuition->IDoSuperMethodA(cl, o, msg);
}

Class *Lanes_MakeClass(Class *track_class)
{
    Class *cl = IIntuition->MakeClass(NULL, NULL, track_class,
                                      sizeof(struct LanesData), 0);

    if (cl)
        cl->cl_Dispatcher.h_Entry = (HOOKFUNC)lanes_dispatch;
    return cl;
}
