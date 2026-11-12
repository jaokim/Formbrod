/*
 * lanes.c - the track editor: one row per track under the film strip.
 *
 *   Picture 2  [ logo.png ]
 *   Picture 1  [ holiday.mp4      ][ skate.avi        ]
 *              [ ~~~~ its sound ~~][ ~~~~~~~~~~~~~~~~ ]   linked sound lane
 *   Sound 1    [ theme.mod                                ]
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
 *     and out points (LANES_EditClip/In/Out). The gadget never changes the
 *     timeline itself; the window does that through App_TrimClip();
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

enum { ROW_PICTURE, ROW_LINKED, ROW_SOUND };

typedef struct Row {
    int  track, kind;
    long y, h;              /* from the gadget's top */
} Row;

enum { MODE_NONE, MODE_PLAYHEAD, MODE_TRIM_IN, MODE_TRIM_OUT };

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

/* "Picture 2" / "Sound 1": numbered among the tracks of the same kind,
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
                       ? (ld->picture_name ? ld->picture_name : "Picture %ld")
                       : (ld->sound_name ? ld->sound_name : "Sound %ld"), n);
}

/* ---- Where a clip is ----------------------------------------------- */

/* A clip's left and right pixel on the time axis, as it's shown: while
 * it's being trimmed, at its new length. */
static void clip_span(const struct LanesData *ld, int clip, long *x0, long *x1)
{
    const FBClip *c = &ld->tl->clips[clip];
    const FBTimeView *v = &ld->tb->view;
    fb_ms_t in = c->in, out = c->out;

    if (ld->mode >= MODE_TRIM_IN && clip == ld->drag_clip) {
        in = ld->drag_in;
        out = ld->drag_out;
    }
    *x0 = FB_ViewTimeToX(v, FB_US(c->start + (in - c->in)));
    *x1 = FB_ViewTimeToX(v, FB_US(c->start + (out - c->in))) - 1;
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
            if (tl->clips[i].track == row->track)
                draw_clip(ld, d, row, i);
    }

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
    lanes_redraw(o, msg->gpi_GInfo);
    return GMR_NOREUSE;
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
