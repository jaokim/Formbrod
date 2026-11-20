/*
 * playwindow.c - the preview player's window (see playwindow.h).
 *
 * The window can be any size; the picture is scaled to fit it, keeping
 * its shape, with black bars where they don't match. Scaling is done by
 * the graphics card where it can:
 *
 *   1. The picture (ARGB) is written into a 32-bit bitmap in video memory
 *      (WritePixelArray with PIXF_A8R8G8B8: OS4's graphics.library takes
 *      PIXF_ formats, not CyberGraphX's RECTFMT_ ones).
 *   2. CompositeTags scales it into a second bitmap the size of the
 *      picture area, a friend of the screen's, on the card (hardware
 *      compositing; the Radeon HD/RX drivers have it).
 *   3. BltBitMapRastPort copies that into the window, which respects
 *      windows in front of it.
 *
 * If the card can't composite, CompositeTags is tried in software; if
 * that fails too, the picture is drawn unscaled in the middle, as before.
 * The space.gadget's render hook draws the last picture again, so the
 * window refreshes itself and resizes cleanly.
 *
 * Its place and size are remembered ("PreviewWindow", winprefs.h).
 *
 * Not compiled against the SDK in the environment it was written in.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <graphics/gfx.h>         /* PIXF_A8R8G8B8 */
#include <graphics/composite.h>   /* CompositeTags */
#include <graphics/rpattr.h>      /* SetRPAttrs */

#include <classes/window.h>
#include <gadgets/layout.h>
#include <gadgets/space.h>
#include <gadgets/button.h>
#include <images/bevel.h>
#include <intuition/gadgetclass.h>
#include <utility/hooks.h>

#include "app.h"
#include "formbrod/log.h"
#include "locale/catalog.h"
#include "gui/playwindow.h"
#include "gui/mainwindow.h"
#include "gui/hints.h"
#include "gui/winprefs.h"

#define MIN_W 160                   /* the picture area at its smallest */
#define MIN_H 90

enum { PG_PICTURE = 1, PG_TIME, PG_LAST };
enum { K_WINDOW, K_LAYOUT, K_SPACE, K_BUTTON, K_COUNT };

static struct {
    struct ClassLibrary *base;
    Class *cls;
    const char *name;
} classes[] = {
    { NULL, NULL, "window.class" },
    { NULL, NULL, "gadgets/layout.gadget" },
    { NULL, NULL, "gadgets/space.gadget" },
    { NULL, NULL, "gadgets/button.gadget" },
};
#define CLS(k) (classes[k].cls)

/* Shown in the picture area while there is no picture: "Preparing the
 * preview...", or why it stopped before the first one. */
static char Message[256];

static Object *WinObj;
static struct Window *Win;
static struct Gadget *Gad[PG_LAST];
static char TimeText[64];
static struct Hook RenderHook;

/* Hover hints (shown once the window is made active). */
static const HintMsg HintMsgs[] = {
    { PG_PICTURE, MSG_HINT_PLAY_PICTURE },
    { PG_TIME,    MSG_HINT_PLAY_TIME },
};
static struct HintInfo Hints[HINT_COUNT(HintMsgs) + 1];

/* The last picture shown, a copy of its own: it is still drawn when the
 * window refreshes after the preview has stopped. */
static FBPlayFrame Shown;
static BOOL Have;

/* Scaling on the card: the picture in video memory, and the scaled
 * result the size of the picture area. */
static struct BitMap *SrcBM, *DstBM;
static int SrcW, SrcH, DstW, DstH;
static BOOL SrcDirty;               /* Shown is newer than SrcBM */
/* How scaling goes: 2 = on the card, 1 = composited in software, 0 =
 * not at all (drawn unscaled). Lowered once when a way fails. */
static int Scaling = 2;

static void free_bitmaps(void)
{
    IGraphics->WaitBlit();
    if (SrcBM)
        IGraphics->FreeBitMap(SrcBM);
    if (DstBM)
        IGraphics->FreeBitMap(DstBM);
    SrcBM = DstBM = NULL;
    SrcW = SrcH = DstW = DstH = 0;
}

/* The picture, unscaled, in the middle of the box (the fallback). */
static void draw_unscaled(struct RastPort *rp, const struct IBox *box)
{
    int x = box->Left + (box->Width - Shown.width) / 2;
    int y = box->Top + (box->Height - Shown.height) / 2;
    int w = Shown.width < box->Width ? Shown.width : box->Width;
    int h = Shown.height < box->Height ? Shown.height : box->Height;

    if (x < box->Left)
        x = box->Left;
    if (y < box->Top)
        y = box->Top;
    IGraphics->WritePixelArray(Shown.pixels, 0, 0, Shown.stride,
                               PIXF_A8R8G8B8, rp, x, y, w, h);
}

/* Scales the picture into DstBM and copies it into the box. FALSE if
 * this way of scaling doesn't work here. */
static BOOL draw_scaled(struct RastPort *rp, const struct IBox *box)
{
    struct RastPort brp;
    float s, sx, sy;
    int dw, dh, ox, oy;
    uint32 err;

    /* The picture in video memory, written again only when it's new. */
    if (!SrcBM || SrcW != Shown.width || SrcH != Shown.height) {
        if (SrcBM) {
            IGraphics->WaitBlit();
            IGraphics->FreeBitMap(SrcBM);
        }
        SrcBM = IGraphics->AllocBitMapTags(Shown.width, Shown.height, 32,
                    BMATags_PixelFormat, PIXF_A8R8G8B8,
                    BMATags_Displayable, TRUE,
                    TAG_END);
        FB_TRACE("gui", "source bitmap %dx%d: %p", Shown.width, Shown.height,
                 (void *)SrcBM);
        if (!SrcBM)
            return FALSE;
        SrcW = Shown.width;
        SrcH = Shown.height;
        SrcDirty = TRUE;
    }
    if (SrcDirty) {
        IGraphics->InitRastPort(&brp);
        brp.BitMap = SrcBM;
        IGraphics->WritePixelArray(Shown.pixels, 0, 0, Shown.stride,
                                   PIXF_A8R8G8B8, &brp, 0, 0,
                                   Shown.width, Shown.height);
        SrcDirty = FALSE;
    }

    /* The scaled picture, the size of the box, like the screen. */
    if (!DstBM || DstW != box->Width || DstH != box->Height) {
        if (DstBM) {
            IGraphics->WaitBlit();
            IGraphics->FreeBitMap(DstBM);
        }
        DstBM = IGraphics->AllocBitMapTags(box->Width, box->Height,
                    IGraphics->GetBitMapAttr(rp->BitMap, BMA_DEPTH),
                    BMATags_Friend,      rp->BitMap,
                    BMATags_Displayable, TRUE,
                    TAG_END);
        FB_TRACE("gui", "scaled bitmap %dx%d: %p", box->Width, box->Height,
                 (void *)DstBM);
        if (!DstBM)
            return FALSE;
        DstW = box->Width;
        DstH = box->Height;
    }

    /* Fit, keeping the shape; black around it. */
    sx = (float)box->Width / Shown.width;
    sy = (float)box->Height / Shown.height;
    s = sx < sy ? sx : sy;
    dw = (int)(Shown.width * s);
    dh = (int)(Shown.height * s);
    if (dw < 1 || dh < 1)
        return TRUE;                /* too small to see; nothing to do */
    ox = (box->Width - dw) / 2;
    oy = (box->Height - dh) / 2;
    IGraphics->InitRastPort(&brp);
    brp.BitMap = DstBM;
    IGraphics->RectFillColor(&brp, 0, 0, box->Width - 1, box->Height - 1,
                             0xFF000000);

    err = IGraphics->CompositeTags(COMPOSITE_Src, SrcBM, DstBM,
              COMPTAG_ScaleX,     COMP_FLOAT_TO_FIX(s),
              COMPTAG_ScaleY,     COMP_FLOAT_TO_FIX(s),
              COMPTAG_OffsetX,    ox,
              COMPTAG_OffsetY,    oy,
              COMPTAG_DestX,      ox,
              COMPTAG_DestY,      oy,
              COMPTAG_DestWidth,  dw,
              COMPTAG_DestHeight, dh,
              COMPTAG_Flags,      COMPFLAG_SrcFilter |
                                  COMPFLAG_IgnoreDestAlpha |
                                  (Scaling == 2 ? COMPFLAG_HardwareOnly : 0),
              TAG_END);
    if (err != COMPERR_Success) {
        FB_Log(FB_LOG_INFO, "preview", "Scaling %s failed (error %lu)",
               Scaling == 2 ? "on the graphics card" : "in software",
               (unsigned long)err);
        return FALSE;
    }
    IGraphics->BltBitMapRastPort(DstBM, 0, 0, rp, box->Left, box->Top,
                                 box->Width, box->Height, 0xC0);
    return TRUE;
}

/* The message, centred on black, cut to fit the box. */
static void draw_message(struct RastPort *rp, const struct IBox *box)
{
    uint32 len = strlen(Message);
    int w, x, y;

    IGraphics->RectFillColor(rp, box->Left, box->Top,
                             box->Left + box->Width - 1,
                             box->Top + box->Height - 1, 0xFF000000);
    if (!len || rp->TxHeight > box->Height)
        return;
    IGraphics->SetRPAttrs(rp, RPTAG_APenColor, 0xFFD0D0D0,
                          RPTAG_DrMd, JAM1, TAG_END);
    while (len > 0 && (w = IGraphics->TextLength(rp, Message, len)) >
                      box->Width - 8)
        len--;
    if (!len)
        return;
    w = IGraphics->TextLength(rp, Message, len);
    x = box->Left + (box->Width - w) / 2;
    y = box->Top + (box->Height - rp->TxHeight) / 2 + rp->TxBaseline;
    IGraphics->Move(rp, x, y);
    IGraphics->Text(rp, Message, len);
}

/* Draws the last picture into the space gadget's box, or the message
 * while there is none. */
static void draw(struct RastPort *rp)
{
    struct IBox *box = NULL;

    if (!Gad[PG_PICTURE] || !rp)
        return;
    IIntuition->GetAttr(SPACE_AreaBox, (Object *)Gad[PG_PICTURE],
                        (uint32 *)&box);
    if (!box || box->Width <= 0 || box->Height <= 0)
        return;
    if (!Have || !Shown.pixels) {
        draw_message(rp, box);
        return;
    }
    while (Scaling > 0 && !draw_scaled(rp, box)) {
        Scaling--;                  /* card, then software, then none */
        FB_TRACE("gui", "scaling now %s", Scaling == 1 ? "in software"
                                                     : "off");
        if (Scaling == 0) {
            FB_Log(FB_LOG_WARNING, "preview",
                   "Can't scale the picture here; it's shown at its size");
            free_bitmaps();
        }
    }
    if (Scaling == 0)
        draw_unscaled(rp, box);
}

static uint32 render_hook(struct Hook *hook, Object *space,
                          struct gpRender *msg)
{
    (void)hook;
    (void)space;
    if (msg)
        draw(msg->gpr_RPort);
    return 0;
}

static void close_window(void)
{
    FB_ENTER("gui");
    if (WinObj) {
        WinPrefs_Save("PreviewWindow", Win);
        IIntuition->DisposeObject(WinObj);
        WinObj = NULL;
        Win = NULL;
    }
    memset(Gad, 0, sizeof(Gad));
    free_bitmaps();
    FB_LEAVE("gui", "closed");
}

static BOOL open_window(int w, int h)
{
    struct Screen *scr;
    struct TagItem place[5];
    Object *layout;
    BOOL placed;
    int i;

    FB_ENTER("gui");
    FB_TRACE("gui", "picture %dx%d", w, h);
    for (i = 0; i < K_COUNT; i++) {
        if (!classes[i].base) {
            classes[i].base = IIntuition->OpenClass(classes[i].name, 52,
                                                    &classes[i].cls);
            FB_TRACE("gui", "OpenClass %s: %p (class %p)", classes[i].name,
                     (void *)classes[i].base, (void *)classes[i].cls);
        }
        if (!classes[i].base) {
            FB_Log(FB_LOG_WARNING, "gui", "Preview window: can't open %s "
                   "(version 52)", classes[i].name);
            FB_LEAVE("gui", "FALSE (no %s)", classes[i].name);
            return FALSE;
        }
    }
    Hints_Fill(Hints, HintMsgs, HINT_COUNT(HintMsgs));
    RenderHook.h_Entry = (HOOKFUNC)render_hook;
    RenderHook.h_Data = NULL;

    /* It starts the size of the picture; it can be made smaller, down to
     * MIN_W x MIN_H, or as big as the screen. */
    Gad[PG_PICTURE] = (struct Gadget *)IIntuition->NewObject(CLS(K_SPACE),
        NULL,
        GA_ID,            PG_PICTURE,
        SPACE_MinWidth,   MIN_W,
        SPACE_MinHeight,  MIN_H,
        SPACE_RenderHook, &RenderHook,
        TAG_END);
    Gad[PG_TIME] = (struct Gadget *)IIntuition->NewObject(CLS(K_BUTTON),
        NULL,
        GA_ID,        PG_TIME,
        GA_ReadOnly,  TRUE,
        GA_Text,      TimeText,
        BUTTON_BevelStyle, BVS_NONE,
        TAG_END);
    FB_TRACE("gui", "picture gadget %p, time gadget %p",
             (void *)Gad[PG_PICTURE], (void *)Gad[PG_TIME]);
    layout = IIntuition->NewObject(CLS(K_LAYOUT), NULL,
        LAYOUT_Orientation,   LAYOUT_ORIENT_VERT,
        LAYOUT_SpaceOuter,    TRUE,
        LAYOUT_AddChild,      Gad[PG_PICTURE],
        LAYOUT_AddChild,      Gad[PG_TIME],
        CHILD_WeightedHeight, 0,
        TAG_END);
    if (!layout) {
        FB_Log(FB_LOG_WARNING, "gui", "Preview window: can't make its "
               "layout");
        FB_LEAVE("gui", "FALSE (no layout)");
        return FALSE;
    }

    scr = IIntuition->LockPubScreen(NULL);
    if (!scr) {
        FB_Log(FB_LOG_WARNING, "gui", "Preview window: can't lock the "
               "default public screen");
        IIntuition->DisposeObject(layout);
        FB_LEAVE("gui", "FALSE (no screen)");
        return FALSE;
    }
    FB_TRACE("gui", "screen %p, %dx%d, depth %lu", (void *)scr, scr->Width,
             scr->Height, (unsigned long)IGraphics->GetBitMapAttr(
                 scr->RastPort.BitMap, BMA_DEPTH));
    placed = WinPrefs_Tags("PreviewWindow", scr, place);
    if (!placed)
        FB_TRACE("gui", "inner size %dx%d, centred", w + 16, h + 40);
    WinObj = IIntuition->NewObject(CLS(K_WINDOW), NULL,
        WA_PubScreen,    scr,
        WA_Title,        FB_Str(MSG_PLAYWIN_TITLE),
        WA_Activate,     FALSE,     /* the main window keeps the keys */
        WA_DepthGadget,  TRUE,
        WA_DragBar,      TRUE,
        WA_CloseGadget,  TRUE,
        WA_SizeGadget,   TRUE,
        WA_SmartRefresh, TRUE,
        WA_IDCMP,        IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY |
                         IDCMP_NEWSIZE | IDCMP_GADGETHELP,
        placed ? TAG_IGNORE : WA_InnerWidth,  w + 16,
        placed ? TAG_IGNORE : WA_InnerHeight, h + 40,
        placed ? TAG_IGNORE : WINDOW_Position, WPOS_CENTERSCREEN,
        WINDOW_Layout,   layout,
        WINDOW_GadgetHelp, TRUE,
        WINDOW_HintInfo, Hints,
        TAG_MORE,        place);
    FB_TRACE("gui", "window object %p", (void *)WinObj);
    if (WinObj) {
        FB_TRACE("gui", "WM_OPEN");
        Win = (struct Window *)IIntuition->IDoMethod(WinObj, WM_OPEN, NULL);
    }
    IIntuition->UnlockPubScreen(NULL, scr);
    if (!Win) {
        FB_Log(FB_LOG_WARNING, "gui", "Preview window: %s",
               WinObj ? "WM_OPEN failed" : "can't make the window object");
        close_window();
        FB_LEAVE("gui", "FALSE");
        return FALSE;
    }
    FB_Log(FB_LOG_INFO, "gui", "Preview window open: %d,%d %dx%d",
           Win->LeftEdge, Win->TopEdge, Win->Width, Win->Height);
    FB_LEAVE("gui", "TRUE (window %p)", (void *)Win);
    return TRUE;
}

void PlayWindow_Show(const FBPlayFrame *f)
{
    size_t need;

    if (!f || f->width <= 0 || f->height <= 0) {
        FB_TRACE("gui", "PlayWindow_Show: no picture (%p, %dx%d)",
                 (const void *)f, f ? f->width : 0, f ? f->height : 0);
        return;
    }
    if (!Win) {
        FB_TRACE("gui", "PlayWindow_Show: first picture, %dx%d at %lld ms",
                 f->width, f->height, (long long)f->pts);
        if (!open_window(f->width, f->height))
            return;
    }
    need = (size_t)f->stride * f->height;
    if (Shown.size < need) {
        unsigned char *p = realloc(Shown.pixels, need);

        if (!p) {
            FB_Log(FB_LOG_WARNING, "gui", "Preview window: no memory for "
                   "a %lu byte picture", (unsigned long)need);
            return;
        }
        Shown.pixels = p;
        Shown.size = need;
    }
    memcpy(Shown.pixels, f->pixels, need);
    Shown.width = f->width;
    Shown.height = f->height;
    Shown.stride = f->stride;
    Shown.pts = f->pts;
    Have = TRUE;
    Message[0] = '\0';
    SrcDirty = TRUE;
    draw(Win->RPort);
}

void PlayWindow_Prepare(int w, int h)
{
    FB_ENTER("gui");
    snprintf(Message, sizeof(Message), "%s", FB_Str(MSG_PLAYWIN_PREPARING));
    Have = FALSE;                   /* not the last preview's picture */
    if (!Win) {
        if (!open_window(w > 0 ? w : 320, h > 0 ? h : 180)) {
            FB_LEAVE("gui", "no window");
            return;
        }
    } else {
        IIntuition->WindowToFront(Win);
    }
    draw(Win->RPort);
    FB_LEAVE("gui", "waiting for the first picture");
}

void PlayWindow_Open(int w, int h)
{
    if (Win) {
        IIntuition->WindowToFront(Win);
        return;
    }
    PlayWindow_Prepare(w, h);
}

void PlayWindow_SetMessage(const char *text)
{
    FB_TRACE("gui", "preview window message: %s", text ? text : "");
    snprintf(Message, sizeof(Message), "%s", text ? text : "");
    if (Win && !Have)
        draw(Win->RPort);
}

int PlayWindow_IsOpen(void)
{
    return Win != NULL;
}

int PlayWindow_HasPicture(void)
{
    return Have;
}

void PlayWindow_SetText(const char *text)
{
    snprintf(TimeText, sizeof(TimeText), "%s", text);
    if (Win && Gad[PG_TIME])
        IIntuition->SetGadgetAttrs(Gad[PG_TIME], Win, NULL,
                                   GA_Text, TimeText, TAG_END);
}

uint32 PlayWindow_SigMask(void)
{
    uint32 mask = 0;

    if (WinObj && Win)
        IIntuition->GetAttr(WINDOW_SigMask, WinObj, &mask);
    return mask;
}

void PlayWindow_Handle(uint32 sigs)
{
    uint32 result;
    uint16 code;
    BOOL close = FALSE;

    if (!WinObj || !(sigs & PlayWindow_SigMask()))
        return;
    while ((result = IIntuition->IDoMethod(WinObj, WM_HANDLEINPUT, &code))
           != WMHI_LASTMSG) {
        switch (result & WMHI_CLASSMASK) {
        case WMHI_CLOSEWINDOW:
            close = TRUE;
            break;
        case WMHI_NEWSIZE:
            /* The layout has redrawn the picture area (render hook). */
            break;
        case WMHI_VANILLAKEY:
            if ((result & WMHI_KEYMASK) == 27)          /* Esc */
                close = TRUE;
            else if ((result & WMHI_KEYMASK) == ' ') {  /* Space: stop */
                App_PlayStop();
                MainWindow_Refresh();
            }
            break;
        }
    }
    if (close) {
        FB_TRACE("gui", "preview window closed by the user");
        App_PlayStop();
        close_window();
        MainWindow_Refresh();
    }
}

void PlayWindow_Close(void)
{
    int i;

    FB_ENTER("gui");
    close_window();
    Have = FALSE;
    Message[0] = '\0';
    free(Shown.pixels);
    memset(&Shown, 0, sizeof(Shown));
    for (i = K_COUNT - 1; i >= 0; i--) {
        if (classes[i].base)
            IIntuition->CloseClass(classes[i].base);
        classes[i].base = NULL;
    }
    FB_LEAVE("gui", "done");
}
