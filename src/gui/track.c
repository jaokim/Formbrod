/*
 * track.c - the timeline track gadgets: track (base), ruler and filmstrip.
 * See track.h for what each one does and gadgets/timeline/README.md for
 * the plan they are part of.
 *
 * Drawing uses RGB pens (RPTAG_APenColor), which needs a screen with more
 * than 256 colours for the colours to come out right. Everything is drawn
 * straight into the window for now; a later step draws into an offscreen
 * bitmap and scrolls it, so only the newly exposed strip is drawn.
 */
#include <string.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/utility.h>
#include <proto/dos.h>

#include <intuition/gadgetclass.h>
#include <intuition/icclass.h>
#include <intuition/screens.h>
#include <graphics/rpattr.h>
#include <graphics/text.h>
#include <devices/inputevent.h>

#include "gui/track.h"
#include "gui/timebase.h"
#include "locale/catalog.h"

Class *TrackClass, *RulerClass, *FilmstripClass, *LanesClass;

/* Colours, 0xRRGGBB. */
#define COL_PLAYHEAD 0xE24B4A
#define COL_CUT      0xEF9F27
#define COL_FILM     0x202020
#define COL_HOLE     0xD3D1C7
#define COL_LOADING  0x5F5E5A
#define COL_SUN      0xFAC775

#define HOLE_PITCH   12          /* sprocket holes, pixels apart */
#define HOLE_W       5
#define FILM_BAND    7           /* sprocket-hole band above and below */
#define MAX_NAMES    32          /* file names shown at once, plain mode */
#define FS_MAX_CELLS 160         /* 160 * 48 px is wider than any screen */

void Track_SetRGB(struct RastPort *rp, uint32 rgb)
{
    IGraphics->SetRPAttrs(rp, RPTAG_APenColor, 0xFF000000 | rgb, TAG_END);
}

/* Fills the part of the rectangle (x0,y0)-(x1,y1) that is inside `c`. */
void Track_FillClipped(struct RastPort *rp, const struct Rectangle *c,
                       long x0, long y0, long x1, long y1)
{
    if (x0 < c->MinX) x0 = c->MinX;
    if (y0 < c->MinY) y0 = c->MinY;
    if (x1 > c->MaxX) x1 = c->MaxX;
    if (y1 > c->MaxY) y1 = c->MaxY;
    if (x0 <= x1 && y0 <= y1)
        IGraphics->RectFill(rp, x0, y0, x1, y1);
}

/* Draws up to `room` pixels of `text` with its top-left corner at x, y. */
void Track_Text(struct RastPort *rp, struct TextFont *font,
                long x, long y, long room, const char *text)
{
    struct TextExtent te;
    uint32 n;

    if (room <= 0)
        return;
    n = IGraphics->TextFit(rp, text, strlen(text), &te, NULL, 1,
                           room, font->tf_YSize);
    if (n == 0)
        return;
    IGraphics->Move(rp, x, y + font->tf_Baseline);
    IGraphics->Text(rp, text, n);
}

/* ==== track: the base class ========================================== */

struct TrackData {
    FBTimebase       *tb;
    const FBTimeline *tl;
    CONST_STRPTR      empty_text;
    CONST_STRPTR      label;
    int               inset;
};

static uint32 playhead_ms(const struct TrackData *td)
{
    return td->tb ? (uint32)FB_MS(td->tb->playhead) : 0;
}

static int timeline_empty(const struct TrackData *td)
{
    return !td->tb || !td->tl || td->tb->view.length <= 0;
}

/* Applies the base class's tags. Returns TRUE if the gadget should be
 * redrawn. */
static BOOL track_set(struct TrackData *td, struct TagItem *tags)
{
    struct TagItem *state = tags, *ti;
    BOOL redraw = FALSE;

    while ((ti = IUtility->NextTagItem(&state))) {
        switch (ti->ti_Tag) {
        case TRACK_Timebase:
            td->tb = (FBTimebase *)ti->ti_Data;
            redraw = TRUE;
            break;
        case TRACK_Timeline:
            td->tl = (const FBTimeline *)ti->ti_Data;
            redraw = TRUE;
            break;
        case TRACK_EmptyText:
            td->empty_text = (CONST_STRPTR)ti->ti_Data;
            redraw = TRUE;
            break;
        case TRACK_Inset:
            td->inset = (int)ti->ti_Data;
            redraw = TRUE;
            break;
        case TRACK_Label:
            td->label = (CONST_STRPTR)ti->ti_Data;
            redraw = TRUE;
            break;
        case TRACK_Redraw:
            redraw = TRUE;
            break;
        }
    }
    return redraw;
}

static void redraw(Object *o, struct GadgetInfo *gi)
{
    struct RastPort *rp;

    if (gi && (rp = IIntuition->ObtainGIRPort(gi))) {
        IIntuition->IDoMethod(o, GM_RENDER, gi, rp, GREDRAW_UPDATE);
        IIntuition->ReleaseGIRPort(rp);
    }
}

/* Tells the target (the window, through ICA_TARGET) where the playhead is
 * now. OPUF_INTERIM while the mouse button is still down. */
static void notify(Class *cl, Object *o, struct GadgetInfo *gi, uint32 flags)
{
    struct TrackData *td = INST_DATA(cl, o);
    struct TagItem tags[3];
    struct opUpdate msg;

    tags[0].ti_Tag  = TRACK_Playhead;
    tags[0].ti_Data = playhead_ms(td);
    tags[1].ti_Tag  = GA_ID;
    tags[1].ti_Data = ((struct Gadget *)o)->GadgetID;
    tags[2].ti_Tag  = TAG_END;
    tags[2].ti_Data = 0;

    msg.MethodID     = OM_NOTIFY;
    msg.opu_AttrList = tags;
    msg.opu_GInfo    = gi;
    msg.opu_Flags    = flags;
    IIntuition->IDoSuperMethodA(cl, o, (Msg)&msg);
}

/* Moves the playhead to pixel x of the gadget, snapped to the start of a
 * frame. Returns TRUE if it moved. */
static BOOL set_playhead_x(struct TrackData *td, long x)
{
    fb_us_t t = FB_ViewXToTime(&td->tb->view, x);
    fb_us_t old = td->tb->playhead;

    if (td->tb->frame_us > 0)
        t -= t % td->tb->frame_us;
    Timebase_SetPlayhead(td->tb, t);
    return td->tb->playhead != old;
}

static void track_render(Class *cl, Object *o, struct gpRender *msg)
{
    struct TrackData *td = INST_DATA(cl, o);
    struct Gadget *g = (struct Gadget *)o;
    struct RastPort *rp = msg->gpr_RPort;
    struct DrawInfo *dri = msg->gpr_GInfo ? msg->gpr_GInfo->gi_DrInfo : NULL;
    long x0 = g->LeftEdge, y0 = g->TopEdge;
    long x1 = x0 + g->Width - 1, y1 = y0 + g->Height - 1;
    struct trackDraw draw;
    long x;

    if (!rp || !dri || g->Width <= 0 || g->Height <= 0)
        return;

    IGraphics->SetDrMd(rp, JAM1);
    IGraphics->SetAPen(rp, dri->dri_Pens[BACKGROUNDPEN]);
    IGraphics->RectFill(rp, x0, y0, x1, y1);
    if (!td->tb)
        return;

    if (td->label && td->inset > 4) {
        IGraphics->SetFont(rp, dri->dri_Font);
        IGraphics->SetAPen(rp, dri->dri_Pens[TEXTPEN]);
        Track_Text(rp, dri->dri_Font, x0 + 2,
                  y0 + (g->Height - dri->dri_Font->tf_YSize) / 2,
                  td->inset - 4, td->label);
    }
    x0 += td->inset;      /* from here on: the time axis */

    if (timeline_empty(td)) {
        if (td->empty_text) {
            struct TextFont *font = dri->dri_Font;
            long w;

            IGraphics->SetFont(rp, font);
            w = IGraphics->TextLength(rp, td->empty_text,
                                      strlen(td->empty_text));
            IGraphics->SetAPen(rp, dri->dri_Pens[TEXTPEN]);
            Track_Text(rp, font, x0 + (w < x1 - x0 ? (x1 - x0 - w) / 2 : 0),
                      y0 + (g->Height - font->tf_YSize) / 2, x1 - x0 + 1,
                      td->empty_text);
        }
        return;
    }

    draw.MethodID       = TRACKM_DRAWCONTENT;
    draw.trd_RPort      = rp;
    draw.trd_DrInfo     = dri;
    draw.trd_Box.Left   = x0;
    draw.trd_Box.Top    = g->TopEdge;
    draw.trd_Box.Width  = x1 - x0 + 1;
    draw.trd_Box.Height = g->Height;
    draw.trd_Inset      = td->inset;
    draw.trd_Timebase   = td->tb;
    draw.trd_Timeline   = td->tl;
    IIntuition->IDoMethodA(o, (Msg)&draw);

    /* The playhead, two pixels wide, on top of everything. */
    x = FB_ViewTimeToX(&td->tb->view, td->tb->playhead);
    if (x >= 0 && x0 + x <= x1) {
        Track_SetRGB(rp, COL_PLAYHEAD);
        IGraphics->RectFill(rp, x0 + x, y0, x0 + x + (x0 + x < x1), y1);
    }
}

static uint32 track_domain(struct gpDomain *msg, int min_h, int h)
{
    msg->gpd_Domain.Left = 0;
    msg->gpd_Domain.Top  = 0;
    switch (msg->gpd_Which) {
    case GDOMAIN_MINIMUM:
        msg->gpd_Domain.Width  = 64;
        msg->gpd_Domain.Height = min_h;
        break;
    case GDOMAIN_MAXIMUM:
        msg->gpd_Domain.Width  = 16383;
        msg->gpd_Domain.Height = h;
        break;
    default:   /* GDOMAIN_NOMINAL */
        msg->gpd_Domain.Width  = 400;
        msg->gpd_Domain.Height = h;
        break;
    }
    return 1;
}

static uint32 track_go_active(Class *cl, Object *o, struct gpInput *msg)
{
    struct TrackData *td = INST_DATA(cl, o);

    if (!msg->gpi_IEvent || timeline_empty(td) ||
        msg->gpi_Mouse.X < td->inset)
        return GMR_NOREUSE;
    set_playhead_x(td, msg->gpi_Mouse.X - td->inset);
    redraw(o, msg->gpi_GInfo);
    notify(cl, o, msg->gpi_GInfo, OPUF_INTERIM);
    return GMR_MEACTIVE;
}

static uint32 track_handle_input(Class *cl, Object *o, struct gpInput *msg)
{
    struct TrackData *td = INST_DATA(cl, o);
    struct InputEvent *ie = msg->gpi_IEvent;

    if (ie->ie_Class != IECLASS_RAWMOUSE)
        return GMR_MEACTIVE;
    switch (ie->ie_Code) {
    case SELECTUP:
        *msg->gpi_Termination = playhead_ms(td);
        notify(cl, o, msg->gpi_GInfo, 0);
        return GMR_NOREUSE | GMR_VERIFY;
    case MENUDOWN:
        /* Right button: stop dragging, leaving the playhead where it is. */
        notify(cl, o, msg->gpi_GInfo, 0);
        return GMR_REUSE;
    case IECODE_NOBUTTON:
        if (set_playhead_x(td, msg->gpi_Mouse.X - td->inset)) {
            redraw(o, msg->gpi_GInfo);
            notify(cl, o, msg->gpi_GInfo, OPUF_INTERIM);
        }
        break;
    }
    return GMR_MEACTIVE;
}

static uint32 track_dispatch(Class *cl, Object *o, Msg msg)
{
    struct TrackData *td;
    uint32 ret;

    switch (msg->MethodID) {
    case OM_NEW:
        o = (Object *)IIntuition->IDoSuperMethodA(cl, o, msg);
        if (o) {
            td = INST_DATA(cl, o);
            memset(td, 0, sizeof(*td));
            track_set(td, ((struct opSet *)msg)->ops_AttrList);
        }
        return (uint32)o;

    case OM_SET:
    case OM_UPDATE:
        ret = IIntuition->IDoSuperMethodA(cl, o, msg);
        td = INST_DATA(cl, o);
        if (track_set(td, ((struct opSet *)msg)->ops_AttrList))
            redraw(o, ((struct opSet *)msg)->ops_GInfo);
        return ret;

    case OM_GET:
        if (((struct opGet *)msg)->opg_AttrID == TRACK_Playhead) {
            td = INST_DATA(cl, o);
            *((struct opGet *)msg)->opg_Storage = playhead_ms(td);
            return TRUE;
        }
        break;

    case GM_DOMAIN:
        return track_domain((struct gpDomain *)msg, 8, 16);

    case GM_LAYOUT:
        ret = IIntuition->IDoSuperMethodA(cl, o, msg);
        td = INST_DATA(cl, o);
        if (td->tb)
            FB_ViewSetWidth(&td->tb->view,
                            ((struct Gadget *)o)->Width - td->inset);
        return ret;

    case GM_RENDER:
        track_render(cl, o, (struct gpRender *)msg);
        return 0;

    case GM_HITTEST:
        return GMR_GADGETHIT;

    case GM_HELPTEST:      /* the whole box answers for the hover hint */
        return GMR_HELPHIT;

    case GM_GOACTIVE:
        return track_go_active(cl, o, (struct gpInput *)msg);

    case GM_HANDLEINPUT:
        return track_handle_input(cl, o, (struct gpInput *)msg);

    case GM_GOINACTIVE:
        return 0;

    case TRACKM_DRAWCONTENT:
        return 0;   /* subclasses draw */
    }
    return IIntuition->IDoSuperMethodA(cl, o, msg);
}

/* ==== ruler =========================================================== */

static int ruler_height(struct TextFont *font)
{
    return (font ? font->tf_YSize : 8) + 8;
}

static void ruler_draw(struct trackDraw *d)
{
    struct RastPort *rp = d->trd_RPort;
    struct DrawInfo *dri = d->trd_DrInfo;
    struct TextFont *font = dri->dri_Font;
    const FBTimeView *v = &d->trd_Timebase->view;
    fb_us_t frame_us = d->trd_Timebase->frame_us;
    long left = d->trd_Box.Left, top = d->trd_Box.Top;
    long w = d->trd_Box.Width, bottom = top + d->trd_Box.Height - 1;
    fb_us_t major, minor, step, t, end;
    char buf[24];

    IGraphics->SetFont(rp, font);
    FB_ViewRulerSteps(v, IGraphics->TextLength(rp, "0:00:00:00", 10) + 8,
                      frame_us, &major, &minor);
    step = minor ? minor : major;
    end = FB_ViewEnd(v);
    if (end > v->length)
        end = v->length;

    IGraphics->SetAPen(rp, dri->dri_Pens[SHADOWPEN]);
    IGraphics->RectFill(rp, left, bottom, left + w - 1, bottom);

    for (t = (v->start / step) * step; t <= end; t += step) {
        long x = FB_ViewTimeToX(v, t);
        int is_major = t % major == 0;
        long h = is_major ? d->trd_Box.Height / 2 : d->trd_Box.Height / 4;

        if (x < 0)
            continue;
        if (x >= w)
            break;
        IGraphics->SetAPen(rp, dri->dri_Pens[SHADOWPEN]);
        IGraphics->RectFill(rp, left + x, bottom - h, left + x, bottom);
        if (is_major) {
            FB_FormatTimecode(buf, sizeof(buf), t, major, frame_us,
                              Locale_DecimalPoint());
            IGraphics->SetAPen(rp, dri->dri_Pens[TEXTPEN]);
            Track_Text(rp, font, left + x + 3, top + 1, w - x - 3, buf);
        }
    }
}

static uint32 ruler_dispatch(Class *cl, Object *o, Msg msg)
{
    switch (msg->MethodID) {
    case GM_DOMAIN: {
        struct gpDomain *gd = (struct gpDomain *)msg;
        struct TextFont *font = gd->gpd_RPort ? gd->gpd_RPort->Font : NULL;
        int h;

        if (!font && gd->gpd_GInfo && gd->gpd_GInfo->gi_DrInfo)
            font = gd->gpd_GInfo->gi_DrInfo->dri_Font;
        h = ruler_height(font);
        return track_domain(gd, h, h);
    }
    case TRACKM_DRAWCONTENT:
        ruler_draw((struct trackDraw *)msg);
        return 0;
    }
    return IIntuition->IDoSuperMethodA(cl, o, msg);
}

/* ==== filmstrip ======================================================= */

struct FilmData {
    int         cell_w, cell_h;
    BOOL        pictures;      /* FALSE: plain blocks, nothing decoded */
    FBThumbFunc thumb;
    APTR        thumb_data;
    FBCell      cells[FS_MAX_CELLS];
};

static BOOL film_set(struct FilmData *fd, struct TagItem *tags, BOOL init)
{
    struct TagItem *state = tags, *ti;
    BOOL changed = FALSE;

    while ((ti = IUtility->NextTagItem(&state))) {
        switch (ti->ti_Tag) {
        case FILMSTRIP_CellWidth:
            if (init && ti->ti_Data >= 8)
                fd->cell_w = ti->ti_Data;
            break;
        case FILMSTRIP_CellHeight:
            if (init && ti->ti_Data >= 4)
                fd->cell_h = ti->ti_Data;
            break;
        case FILMSTRIP_ThumbFunc:
            fd->thumb = (FBThumbFunc)ti->ti_Data;
            changed = TRUE;
            break;
        case FILMSTRIP_ThumbData:
            fd->thumb_data = (APTR)ti->ti_Data;
            changed = TRUE;
            break;
        case FILMSTRIP_Pictures:
            fd->pictures = ti->ti_Data != 0;
            changed = TRUE;
            break;
        }
    }
    return changed;
}

/* A colour per source, used by the dummy pictures and the plain blocks. */
static const uint32 sky[]    = { 0x378ADD, 0x534AB7, 0x1D9E75,
                                 0xD85A30, 0xD4537E, 0x888780 };

/* Until there are real thumbnails: a little landscape whose colours come
 * from the source and whose horizon and sun move with the time, so
 * zooming and scrolling can be tried out. */
static void film_dummy_frame(const FBThumbRequest *r)
{
    static const uint32 ground[] = { 0x639922, 0x5F5E5A, 0x085041,
                                     0x712B13, 0x72243E, 0x444441 };
    int k = r->source % 6;
    long left = r->cell.Left, top = r->cell.Top;
    long right = left + r->cell.Width - 1, bottom = top + r->cell.Height - 1;
    long phase = (long)((r->source_t / 40000) % 64);
    long tri = phase < 32 ? phase : 63 - phase;              /* 0..31 */
    long horizon = top + r->cell.Height / 2 - r->cell.Height / 8 +
                   tri * (r->cell.Height / 4) / 31;
    long span = r->cell.Width > 8 ? r->cell.Width - 8 : 1;
    long sun = left + 2 + (long)((r->source_t / 200000) % span);

    Track_SetRGB(r->rp, sky[k]);
    Track_FillClipped(r->rp, &r->clip, left, top, right, horizon - 1);
    Track_SetRGB(r->rp, ground[k]);
    Track_FillClipped(r->rp, &r->clip, left, horizon, right, bottom);
    Track_SetRGB(r->rp, COL_SUN);
    Track_FillClipped(r->rp, &r->clip, sun, top + 3, sun + 4, top + 6);
}

/* The file part of `path`, in white, from x on, cut off at the film's
 * right edge. Drawn after all the blocks, so a long name runs on over the
 * clip's next cells. */
static void name_at(struct RastPort *rp, struct DrawInfo *dri,
                    const struct Rectangle *film, long x, long top, long h,
                    const char *path)
{
    struct TextFont *font = dri->dri_Font;
    const char *name = IDOS->FilePart(path);

    if (font->tf_YSize > h)
        return;
    IGraphics->SetFont(rp, font);
    Track_SetRGB(rp, 0xFFFFFF);
    Track_Text(rp, font, x, top + (h - font->tf_YSize) / 2,
              film->MaxX - x + 1, name);
}

static void film_draw(struct FilmData *fd, struct trackDraw *d)
{
    struct RastPort *rp = d->trd_RPort;
    const FBTimeline *tl = d->trd_Timeline;
    const FBTimeView *v = &d->trd_Timebase->view;
    long left = d->trd_Box.Left, top = d->trd_Box.Top;
    long bottom = top + d->trd_Box.Height - 1;
    /* The film stops where the edit does; the view goes on a bit. */
    long film_end = FB_ViewTimeToX(v, FB_US(FB_TimelineDuration(tl)));
    long right = (film_end < d->trd_Box.Width ? film_end : d->trd_Box.Width) - 1;
    int band = FILM_BAND;
    int hole_h = band > 4 ? band - 4 : 1;
    struct Rectangle film;
    long x;
    int i, n, prev_clip, num_names = 0;
    long name_x[MAX_NAMES];
    int name_src[MAX_NAMES];

    if (right < 0)
        return;
    film.MinX = left;
    film.MinY = top;
    film.MaxX = left + right;
    film.MaxY = bottom;

    Track_SetRGB(rp, COL_FILM);
    IGraphics->RectFill(rp, film.MinX, film.MinY, film.MaxX, film.MaxY);

    /* Sprocket holes, moving with the film when it scrolls. */
    Track_SetRGB(rp, COL_HOLE);
    for (x = -(FB_ViewStartPx(v) % HOLE_PITCH); x <= right; x += HOLE_PITCH) {
        Track_FillClipped(rp, &film, left + x + 3, top + 2,
                     left + x + 2 + HOLE_W, top + 1 + hole_h);
        Track_FillClipped(rp, &film, left + x + 3, bottom - 1 - hole_h,
                     left + x + 2 + HOLE_W, bottom - 2);
    }

    /* One picture per cell: the frame from the middle of the cell. */
    n = FB_ViewCells(v, fd->cell_w, fd->cells, FS_MAX_CELLS);
    prev_clip = -1;
    for (i = 0; i < n; i++) {
        const FBCell *c = &fd->cells[i];
        FBThumbRequest req;
        fb_us_t st;
        int clip = FB_TimelineLocate(tl, c->mid, &st);

        if (clip < 0) {
            prev_clip = -1;
            continue;
        }
        req.source      = tl->clips[clip].source;
        req.source_t    = st;
        req.rp          = rp;
        req.cell.Left   = left + c->x + 1;
        req.cell.Top    = top + band;
        req.cell.Width  = fd->cell_w - 2;
        req.cell.Height = fd->cell_h;
        req.clip        = film;
        if (req.clip.MinX < req.cell.Left)
            req.clip.MinX = req.cell.Left;
        if (req.clip.MaxX > req.cell.Left + req.cell.Width - 1)
            req.clip.MaxX = req.cell.Left + req.cell.Width - 1;
        req.clip.MinY = req.cell.Top;
        req.clip.MaxY = req.cell.Top + req.cell.Height - 1;
        if (req.clip.MinX > req.clip.MaxX)
            continue;
        if (!fd->pictures) {
            /* Plain: the file's colour, and its name where it starts. */
            Track_SetRGB(rp, sky[req.source % 6]);
            Track_FillClipped(rp, &req.clip, req.clip.MinX, req.clip.MinY,
                         req.clip.MaxX, req.clip.MaxY);
            if (clip != prev_clip && num_names < MAX_NAMES) {
                name_x[num_names] = req.clip.MinX + 2;
                name_src[num_names++] = req.source;
            }
        } else if (!fd->thumb) {
            film_dummy_frame(&req);
        } else if (!fd->thumb(fd->thumb_data, &req)) {
            Track_SetRGB(rp, COL_LOADING);   /* not made yet */
            Track_FillClipped(rp, &req.clip, req.clip.MinX, req.clip.MinY,
                         req.clip.MaxX, req.clip.MaxY);
        }
        prev_clip = clip;
    }

    /* The names last, over the blocks after where each clip starts. */
    for (i = 0; i < num_names; i++)
        name_at(rp, d->trd_DrInfo, &film, name_x[i], top + band,
                fd->cell_h, tl->sources[name_src[i]].path);

    /* Where a picture clip starts, other than at the very beginning. */
    Track_SetRGB(rp, COL_CUT);
    for (i = 0; i < tl->num_clips; i++) {
        const FBClip *c = &tl->clips[i];
        const FBTrack *tr = &tl->tracks[c->track];

        if (c->start <= 0 || tr->kind != FB_TRACK_VIDEO || tr->hidden)
            continue;
        x = FB_ViewTimeToX(v, FB_TimelineClipStart(tl, i));
        Track_FillClipped(rp, &film, left + x - 1, top, left + x, bottom);
    }
}

static uint32 film_dispatch(Class *cl, Object *o, Msg msg)
{
    struct FilmData *fd;
    uint32 ret;

    switch (msg->MethodID) {
    case OM_NEW:
        o = (Object *)IIntuition->IDoSuperMethodA(cl, o, msg);
        if (o) {
            fd = INST_DATA(cl, o);
            memset(fd, 0, sizeof(*fd));
            fd->cell_w = FB_CELL_W_DEFAULT;
            fd->cell_h = FB_CELL_H_DEFAULT;
            fd->pictures = TRUE;
            film_set(fd, ((struct opSet *)msg)->ops_AttrList, TRUE);
        }
        return (uint32)o;

    case OM_SET:
    case OM_UPDATE:
        fd = INST_DATA(cl, o);
        if (film_set(fd, ((struct opSet *)msg)->ops_AttrList, FALSE))
            redraw(o, ((struct opSet *)msg)->ops_GInfo);
        return IIntuition->IDoSuperMethodA(cl, o, msg);

    case GM_DOMAIN:
        fd = INST_DATA(cl, o);
        ret = fd->cell_h + 2 * FILM_BAND;
        return track_domain((struct gpDomain *)msg, ret, ret);

    case TRACKM_DRAWCONTENT:
        film_draw(INST_DATA(cl, o), (struct trackDraw *)msg);
        return 0;
    }
    return IIntuition->IDoSuperMethodA(cl, o, msg);
}

/* ==== classes ========================================================= */

BOOL Track_MakeClasses(void)
{
    TrackClass = IIntuition->MakeClass(NULL, "gadgetclass", NULL,
                                       sizeof(struct TrackData), 0);
    if (!TrackClass)
        goto fail;
    TrackClass->cl_Dispatcher.h_Entry = (HOOKFUNC)track_dispatch;

    RulerClass = IIntuition->MakeClass(NULL, NULL, TrackClass, 0, 0);
    if (!RulerClass)
        goto fail;
    RulerClass->cl_Dispatcher.h_Entry = (HOOKFUNC)ruler_dispatch;

    FilmstripClass = IIntuition->MakeClass(NULL, NULL, TrackClass,
                                           sizeof(struct FilmData), 0);
    if (!FilmstripClass)
        goto fail;
    FilmstripClass->cl_Dispatcher.h_Entry = (HOOKFUNC)film_dispatch;

    if (!(LanesClass = Lanes_MakeClass(TrackClass)))
        goto fail;
    return TRUE;

fail:
    Track_FreeClasses();
    return FALSE;
}

void Track_FreeClasses(void)
{
    if (LanesClass)
        IIntuition->FreeClass(LanesClass);
    LanesClass = NULL;
    if (FilmstripClass)
        IIntuition->FreeClass(FilmstripClass);
    if (RulerClass)
        IIntuition->FreeClass(RulerClass);
    if (TrackClass)
        IIntuition->FreeClass(TrackClass);
    FilmstripClass = RulerClass = TrackClass = NULL;
}
