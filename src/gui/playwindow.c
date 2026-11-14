/*
 * playwindow.c - the preview player's window (see playwindow.h).
 *
 * A space.gadget holds the picture. Its render hook draws the last
 * picture shown, so the window refreshes itself; new pictures are drawn
 * straight into the window's RastPort. Pictures are ARGB and drawn with
 * WritePixelArray, which converts to the screen's format.
 *
 * Not compiled against the SDK in the environment it was written in.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>

#include <classes/window.h>
#include <gadgets/layout.h>
#include <gadgets/space.h>
#include <gadgets/button.h>
#include <images/bevel.h>
#include <intuition/gadgetclass.h>
#include <utility/hooks.h>

#include "app.h"
#include "locale/catalog.h"
#include "gui/playwindow.h"
#include "gui/mainwindow.h"

/* Picasso96's and graphics.library's pixel order for WritePixelArray. */
#ifndef RECTFMT_ARGB
#define RECTFMT_ARGB 2
#endif

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

static Object *WinObj;
static struct Window *Win;
static struct Gadget *Gad[PG_LAST];
static int PicW, PicH;              /* the size the window was made for */
/* The last picture shown, a copy of its own: it is still drawn when
 * the window refreshes after the preview has stopped. */
static FBPlayFrame Shown;
static const FBPlayFrame *Last;     /* &Shown once there is one */
static char TimeText[64];
static struct Hook RenderHook;

/* Draws the last picture centred in the space gadget's box. */
static void draw(struct RastPort *rp)
{
    struct IBox *box = NULL;
    int x, y;

    if (!Last || !Last->pixels || !Gad[PG_PICTURE])
        return;
    IIntuition->GetAttr(SPACE_AreaBox, (Object *)Gad[PG_PICTURE],
                        (uint32 *)&box);
    if (!box)
        return;
    x = box->Left + (box->Width - Last->width) / 2;
    y = box->Top + (box->Height - Last->height) / 2;
    if (x < box->Left)
        x = box->Left;
    if (y < box->Top)
        y = box->Top;
    IGraphics->WritePixelArray(Last->pixels, 0, 0, Last->stride,
                               RECTFMT_ARGB, rp, x, y,
                               Last->width < box->Width ? Last->width
                                                        : box->Width,
                               Last->height < box->Height ? Last->height
                                                          : box->Height);
}

static uint32 render_hook(struct Hook *hook, Object *space,
                          struct gpRender *msg)
{
    (void)hook;
    (void)space;
    if (msg && msg->gpr_RPort)
        draw(msg->gpr_RPort);
    return 0;
}

static void close_window(void)
{
    if (WinObj) {
        IIntuition->DisposeObject(WinObj);
        WinObj = NULL;
        Win = NULL;
    }
    memset(Gad, 0, sizeof(Gad));
}

static BOOL open_window(int w, int h)
{
    struct Screen *scr;
    Object *layout;
    int i;

    for (i = 0; i < K_COUNT; i++) {
        if (!classes[i].base)
            classes[i].base = IIntuition->OpenClass(classes[i].name, 52,
                                                    &classes[i].cls);
        if (!classes[i].base)
            return FALSE;
    }
    RenderHook.h_Entry = (HOOKFUNC)render_hook;
    RenderHook.h_Data = NULL;

    Gad[PG_PICTURE] = (struct Gadget *)IIntuition->NewObject(CLS(K_SPACE),
        NULL,
        GA_ID,            PG_PICTURE,
        SPACE_MinWidth,   w,
        SPACE_MinHeight,  h,
        SPACE_RenderHook, &RenderHook,
        TAG_END);
    Gad[PG_TIME] = (struct Gadget *)IIntuition->NewObject(CLS(K_BUTTON),
        NULL,
        GA_ID,        PG_TIME,
        GA_ReadOnly,  TRUE,
        GA_Text,      TimeText,
        BUTTON_BevelStyle, BVS_NONE,
        TAG_END);
    layout = IIntuition->NewObject(CLS(K_LAYOUT), NULL,
        LAYOUT_Orientation,  LAYOUT_ORIENT_VERT,
        LAYOUT_SpaceOuter,   TRUE,
        LAYOUT_AddChild,     Gad[PG_PICTURE],
        CHILD_MinWidth,      w,
        CHILD_MinHeight,     h,
        LAYOUT_AddChild,     Gad[PG_TIME],
        CHILD_WeightedHeight, 0,
        TAG_END);
    if (!layout)
        return FALSE;

    scr = IIntuition->LockPubScreen(NULL);
    if (!scr) {
        IIntuition->DisposeObject(layout);
        return FALSE;
    }
    WinObj = IIntuition->NewObject(CLS(K_WINDOW), NULL,
        WA_PubScreen,    scr,
        WA_Title,        FB_Str(MSG_PLAYWIN_TITLE),
        WA_Activate,     FALSE,     /* the main window keeps the keys */
        WA_DepthGadget,  TRUE,
        WA_DragBar,      TRUE,
        WA_CloseGadget,  TRUE,
        WA_SmartRefresh, TRUE,
        WA_IDCMP,        IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY,
        WINDOW_Position, WPOS_CENTERSCREEN,
        WINDOW_Layout,   layout,
        TAG_END);
    if (WinObj)
        Win = (struct Window *)IIntuition->IDoMethod(WinObj, WM_OPEN, NULL);
    IIntuition->UnlockPubScreen(NULL, scr);
    if (!Win) {
        close_window();
        return FALSE;
    }
    PicW = w;
    PicH = h;
    return TRUE;
}

void PlayWindow_Show(const FBPlayFrame *f)
{
    if (!f || f->width <= 0 || f->height <= 0)
        return;
    if (Win && (f->width != PicW || f->height != PicH))
        close_window();
    if (!Win && !open_window(f->width, f->height))
        return;
    {
        size_t need = (size_t)f->stride * f->height;

        if (Shown.size < need) {
            unsigned char *p = realloc(Shown.pixels, need);

            if (!p)
                return;
            Shown.pixels = p;
            Shown.size = need;
        }
        memcpy(Shown.pixels, f->pixels, need);
        Shown.width = f->width;
        Shown.height = f->height;
        Shown.stride = f->stride;
        Shown.pts = f->pts;
        Last = &Shown;
    }
    draw(Win->RPort);
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
        case WMHI_VANILLAKEY:
            if ((result & WMHI_KEYMASK) == 27)        /* Esc */
                close = TRUE;
            else if ((result & WMHI_KEYMASK) == ' ') {  /* Space: stop */
                App_PlayStop();
                MainWindow_Refresh();
            }
            break;
        }
    }
    if (close) {
        App_PlayStop();
        close_window();
        MainWindow_Refresh();
    }
}

void PlayWindow_Close(void)
{
    int i;

    Last = NULL;
    close_window();
    free(Shown.pixels);
    memset(&Shown, 0, sizeof(Shown));
    for (i = K_COUNT - 1; i >= 0; i--) {
        if (classes[i].base)
            IIntuition->CloseClass(classes[i].base);
        classes[i].base = NULL;
    }
}
