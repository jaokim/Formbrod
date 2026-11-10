/*
 * mainwindow.c - the ReAction main window.
 *
 *   [Add file...] [Split] [Remove] [Fade] [Preview] [Export...]
 *   | 0:00     | 0:05     | 0:10     | 0:15          ruler
 *   [#][#][#][#][#][#][#][#|#][#][#][#][#]          film strip
 *   [<====scroll====>                         ]
 *   playhead [0:07:12]   zoom [-----|=======]   (right = zoomed in)
 *   +-------------------------------------------------------+
 *   | clip list (ListBrowser) until the timeline gadget can |
 *   | select clips itself                                   |
 *   +-------------------------------------------------------+
 *   render   [#######.........]
 *   status line
 *
 * The ruler and film strip are the track gadgets from track.c. They share
 * one FBTimebase (timebase.c) with the scroller and the zoom slider:
 * clicking or dragging in a track moves the playhead, and the window then
 * redraws the other tracks, from idcmp_hook() while dragging.
 *
 * All visible text comes from FB_Str() (src/locale/), so it is translated
 * when a catalog for the user's language is installed.
 *
 * Files can be dropped on the window (AppWindow). Classes are opened with
 * OpenClass() and objects created with NewObject() on the Class pointer, so
 * nothing depends on reaction_macros.h.
 */
#include <stdio.h>
#include <string.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/asl.h>
#include <proto/wb.h>
#include <proto/listbrowser.h>
#include <proto/utility.h>

#include <classes/window.h>
#include <gadgets/layout.h>
#include <gadgets/button.h>
#include <gadgets/listbrowser.h>
#include <gadgets/slider.h>
#include <gadgets/scroller.h>
#include <intuition/icclass.h>
#include <gadgets/fuelgauge.h>
#include <images/label.h>
#include <workbench/workbench.h>

#include "app.h"
#include "mainwindow.h"
#include "formbrod/module.h"
#include "locale/catalog.h"
#include "gui/timebase.h"
#include "gui/track.h"

enum {
    GID_ADD = 1, GID_SPLIT, GID_REMOVE, GID_FADE, GID_PREVIEW, GID_EXPORT,
    GID_CLIPS, GID_POSITION, GID_PROGRESS, GID_STATUS,
    GID_RULER, GID_FILMSTRIP, GID_SCROLL, GID_ZOOM, GID_LAST
};

static struct {
    struct ClassLibrary *base;
    Class *cls;
    const char *name;
} classes[] = {
    { NULL, NULL, "window.class" },
    { NULL, NULL, "gadgets/layout.gadget" },
    { NULL, NULL, "gadgets/button.gadget" },
    { NULL, NULL, "gadgets/listbrowser.gadget" },
    { NULL, NULL, "gadgets/slider.gadget" },
    { NULL, NULL, "gadgets/fuelgauge.gadget" },
    { NULL, NULL, "images/label.image" },
    { NULL, NULL, "gadgets/scroller.gadget" },
};
enum { C_WINDOW, C_LAYOUT, C_BUTTON, C_LISTBROWSER, C_SLIDER, C_FUELGAUGE,
       C_LABEL, C_SCROLLER, C_COUNT };

static struct ListBrowserIFace *LB;   /* listbrowser.gadget interface */
static struct MsgPort *AppPort;
static Object *WinObj;
static struct Window *Win;
static struct Gadget *Gad[GID_LAST];
static struct List ClipList;
static struct ColumnInfo *Columns;
static FBTimebase TB;
static struct Hook IdcmpHook;
static char PositionText[32];
/* Shown when the pointer rests on a gadget (WINDOW_GadgetHelp). The texts
 * are filled in from the catalog when the window opens. */
static struct HintInfo Hints[] = {
    { GID_ZOOM,      -1, NULL, 0 },
    { GID_RULER,     -1, NULL, 0 },
    { GID_FILMSTRIP, -1, NULL, 0 },
    { -1, -1, NULL, 0 }
};
static BOOL TrackClassesMade;

#define CLS(i) (classes[i].cls)

static Object *button(uint32 id, const char *text)
{
    return Gad[id] = (struct Gadget *)IIntuition->NewObject(CLS(C_BUTTON), NULL,
        GA_ID,        id,
        GA_RelVerify, TRUE,
        GA_Text,      text,
        TAG_END);
}

static Object *label(const char *text)
{
    return IIntuition->NewObject(CLS(C_LABEL), NULL, LABEL_Text, text, TAG_END);
}

static void free_clip_nodes(void)
{
    struct Node *n;

    while ((n = IExec->RemHead(&ClipList)))
        LB->FreeListBrowserNode(n);
}

static void fmt_time(char *buf, size_t len, fb_ms_t ms)
{
    snprintf(buf, len, "%ld:%02ld%c%01ld", (long)(ms / 60000),
             (long)((ms / 1000) % 60), Locale_DecimalPoint(),
             (long)((ms / 100) % 10));
}

/* Text for how a clip is fitted into the frame, or NULL if it isn't
 * changed. Shown first in the Effects column for now; a fit-mode chooser
 * (App_SetClipFit) can go in the clip inspector later. */
static const char *fit_text(FBFitResult r)
{
    switch (r) {
    case FB_FITS_SCALE:     return FB_Str(MSG_FITS_SCALE);
    case FB_FITS_LETTERBOX: return FB_Str(MSG_FITS_LETTERBOX);
    case FB_FITS_PILLARBOX: return FB_Str(MSG_FITS_PILLARBOX);
    case FB_FITS_CROP:      return FB_Str(MSG_FITS_CROP);
    case FB_FITS_STRETCH:   return FB_Str(MSG_FITS_STRETCH);
    default:                return NULL;
    }
}

/* The zoom slider goes the other way from the zoom levels: right is
 * zoomed in (level 0), left shows the whole edit. */
static uint32 zoom_slider_level(void)
{
    return FB_ViewZoomLevels(&TB.view) - 1 - FB_ViewZoomLevel(&TB.view);
}

/* Puts the scroller, zoom slider and playhead time in step with TB. */
static void sync_controls(void)
{
    FBTimeView *v = &TB.view;

    FB_FormatTimecode(PositionText, sizeof(PositionText), TB.playhead,
                      TB.frame_us > 0 ? TB.frame_us : FB_US(100),
                      TB.frame_us, Locale_DecimalPoint());
    IIntuition->SetGadgetAttrs(Gad[GID_POSITION], Win, NULL,
                               GA_Text, PositionText, TAG_END);
    IIntuition->SetGadgetAttrs(Gad[GID_SCROLL], Win, NULL,
        SCROLLER_Total,   FB_ViewTotalPx(v),
        SCROLLER_Visible, v->width,
        SCROLLER_Top,     FB_ViewStartPx(v),
        GA_Disabled,      v->length <= 0,
        TAG_END);
    IIntuition->SetGadgetAttrs(Gad[GID_ZOOM], Win, NULL,
        SLIDER_Max,   FB_ViewZoomLevels(v) - 1,
        SLIDER_Level, zoom_slider_level(),
        GA_Disabled,  FB_ViewZoomLevels(v) <= 1,
        TAG_END);
}

/* A track, the scroller or the zoom slider changed TB: redraw the rest.
 * `except` already shows the change. */
static void timebase_changed(struct Gadget *except)
{
    Timebase_Redraw(&TB, Win, except);
    sync_controls();
}

static void scroll_to(uint32 top_px)
{
    FB_ViewSetStart(&TB.view, (fb_us_t)top_px * TB.view.per_px);
    Timebase_Redraw(&TB, Win, NULL);
}

/* Called with the zoom slider's position (see zoom_slider_level). */
static void zoom_to(uint32 slider)
{
    int level = FB_ViewZoomLevels(&TB.view) - 1 - (int)slider;

    if (level == FB_ViewZoomLevel(&TB.view))
        return;
    Timebase_SetZoomLevel(&TB, level);
    Timebase_Redraw(&TB, Win, NULL);
    /* The scroller's range changes with the zoom; the slider is already
     * where the user put it. */
    IIntuition->SetGadgetAttrs(Gad[GID_SCROLL], Win, NULL,
        SCROLLER_Total,   FB_ViewTotalPx(&TB.view),
        SCROLLER_Visible, TB.view.width,
        SCROLLER_Top,     FB_ViewStartPx(&TB.view),
        TAG_END);
}

/* Live updates while something is dragged: the tracks, the scroller and
 * the zoom slider notify the window (ICA_TARGET ICTARGET_IDCMP), and
 * window.class calls this from WM_HANDLEINPUT. */
static uint32 idcmp_hook(struct Hook *hook, Object *winobj,
                         struct IntuiMessage *imsg)
{
    struct TagItem *tags = (struct TagItem *)imsg->IAddress;
    uint32 id;

    if (imsg->Class != IDCMP_IDCMPUPDATE || !tags)
        return 0;
    id = IUtility->GetTagData(GA_ID, 0, tags);
    switch (id) {
    case GID_RULER:
    case GID_FILMSTRIP:
        /* The track has moved TB's playhead and redrawn itself. */
        timebase_changed(Gad[id]);
        break;
    case GID_SCROLL:
        if (IUtility->FindTagItem(SCROLLER_Top, tags))
            scroll_to(IUtility->GetTagData(SCROLLER_Top, 0, tags));
        break;
    case GID_ZOOM:
        if (IUtility->FindTagItem(SLIDER_Level, tags))
            zoom_to(IUtility->GetTagData(SLIDER_Level, 0, tags));
        break;
    }
    return 0;
}

void MainWindow_Refresh(void)
{
    const FBTimeline *tl = &App.timeline;
    fb_ms_t total = FB_TimelineDuration(tl);
    int fw = 0, fh = 0;
    int i;

    if (!WinObj)
        return;

    IIntuition->SetGadgetAttrs(Gad[GID_CLIPS], Win, NULL,
                               LISTBROWSER_Labels, ~0, TAG_END);
    free_clip_nodes();
    FB_TimelineFrameSize(tl, &fw, &fh);
    for (i = 0; i < tl->num_clips; i++) {
        const FBClip *c = &tl->clips[i];
        char num[8], in[16], out[16], fx[96] = "";
        const char *fit = fit_text(FB_ClipFitResult(tl, c, fw, fh));
        struct Node *n;
        int e;

        snprintf(num, sizeof(num), "%d", i + 1);
        fmt_time(in, sizeof(in), c->in);
        fmt_time(out, sizeof(out), c->out);
        if (fit)
            strncat(fx, fit, sizeof(fx) - 1);
        for (e = 0; e < c->num_effects; e++) {
            strncat(fx, fx[0] ? ", " : "", sizeof(fx) - strlen(fx) - 1);
            strncat(fx, c->effects[e].module, sizeof(fx) - strlen(fx) - 1);
        }
        n = LB->AllocListBrowserNode(5,
            LBNA_Column, 0, LBNCA_CopyText, TRUE, LBNCA_Text, num,
            LBNA_Column, 1, LBNCA_CopyText, TRUE,
                            LBNCA_Text, IDOS->FilePart(tl->sources[c->source].path),
            LBNA_Column, 2, LBNCA_CopyText, TRUE, LBNCA_Text, in,
            LBNA_Column, 3, LBNCA_CopyText, TRUE, LBNCA_Text, out,
            LBNA_Column, 4, LBNCA_CopyText, TRUE, LBNCA_Text, fx,
            TAG_END);
        if (n)
            IExec->AddTail(&ClipList, n);
    }
    IIntuition->SetGadgetAttrs(Gad[GID_CLIPS], Win, NULL,
                               LISTBROWSER_Labels, &ClipList, TAG_END);

    Timebase_SetTimeline(&TB, tl);
    IIntuition->SetGadgetAttrs(Gad[GID_RULER], Win, NULL,
                               TRACK_Timeline, tl, TAG_END);
    IIntuition->SetGadgetAttrs(Gad[GID_FILMSTRIP], Win, NULL,
                               TRACK_Timeline, tl, TAG_END);
    sync_controls();

    IIntuition->SetGadgetAttrs(Gad[GID_SPLIT], Win, NULL, GA_Disabled, total <= 0, TAG_END);
    IIntuition->SetGadgetAttrs(Gad[GID_REMOVE], Win, NULL, GA_Disabled, total <= 0, TAG_END);
    IIntuition->SetGadgetAttrs(Gad[GID_FADE], Win, NULL, GA_Disabled, total <= 0, TAG_END);
    IIntuition->SetGadgetAttrs(Gad[GID_PREVIEW], Win, NULL, GA_Disabled, total <= 0, TAG_END);
    /* While rendering, Export turns into Cancel. */
    IIntuition->SetGadgetAttrs(Gad[GID_EXPORT], Win, NULL,
        GA_Text,     FB_Str(App.rendering ? MSG_BTN_CANCEL : MSG_BTN_EXPORT),
        GA_Disabled, total <= 0 && !App.rendering,
        TAG_END);
    IIntuition->SetGadgetAttrs(Gad[GID_STATUS], Win, NULL, GA_Text, App.status, TAG_END);
}

void MainWindow_SetProgress(const FBProgress *p)
{
    int32 level = 0;

    if (p->total > 0)
        level = (int32)(p->position * 100 / p->total);
    IIntuition->SetGadgetAttrs(Gad[GID_PROGRESS], Win, NULL,
                               FUELGAUGE_Level, level, TAG_END);
}

static int ask_file(const char *title, int save, char *buf, size_t len)
{
    struct FileRequester *fr;
    int ok = 0;

    fr = IAsl->AllocAslRequestTags(ASL_FileRequest,
        ASLFR_Window,       Win,
        ASLFR_TitleText,    title,
        ASLFR_DoSaveMode,   save,
        TAG_END);
    if (!fr)
        return 0;
    if (IAsl->AslRequestTags(fr, TAG_END)) {
        strncpy(buf, fr->fr_Drawer, len - 1);
        buf[len - 1] = '\0';
        ok = IDOS->AddPart(buf, fr->fr_File, len) != 0;
    }
    IAsl->FreeAslRequest(fr);
    return ok;
}

static int selected_clip(void)
{
    int32 sel = -1;

    IIntuition->GetAttr(LISTBROWSER_Selected, (Object *)Gad[GID_CLIPS],
                        (uint32 *)&sel);
    return sel;
}

static void handle_gadget(uint32 id)
{
    char path[FB_PATH_LEN];
    uint32 value = 0;
    int clip;

    switch (id) {
    /* The timeline controls only change the view, so they don't need the
     * full refresh below. Their final value also comes through
     * idcmp_hook(); this catches it if that notification was missed. */
    case GID_RULER:
    case GID_FILMSTRIP:
        timebase_changed(NULL);
        return;
    case GID_SCROLL:
        IIntuition->GetAttr(SCROLLER_Top, (Object *)Gad[GID_SCROLL], &value);
        scroll_to(value);
        return;
    case GID_ZOOM:
        IIntuition->GetAttr(SLIDER_Level, (Object *)Gad[GID_ZOOM], &value);
        zoom_to(value);
        return;
    case GID_ADD:
        if (ask_file(FB_Str(MSG_REQ_ADD_FILE), FALSE, path, sizeof(path)))
            App_AddFile(path);
        break;
    case GID_SPLIT:
        App_Split(FB_MS(TB.playhead));
        break;
    case GID_REMOVE:
        if ((clip = selected_clip()) >= 0)
            App_RemoveClip(clip);
        else
            App_SetStatus("%s", FB_Str(MSG_STATUS_SELECT_CLIP));
        break;
    case GID_FADE:
        if ((clip = selected_clip()) >= 0)
            App_AddEffect(clip, "fade");
        else
            App_SetStatus("%s", FB_Str(MSG_STATUS_SELECT_CLIP));
        break;
    case GID_PREVIEW:
        App_Export("T:formbrod-preview.mp4", TRUE);
        break;
    case GID_EXPORT:
        if (App.rendering)
            App_CancelRender();
        else if (ask_file(FB_Str(MSG_REQ_EXPORT), TRUE, path, sizeof(path)))
            App_Export(path, FALSE);
        break;
    }
    MainWindow_Refresh();
}

static void handle_appmessages(void)
{
    struct AppMessage *am;

    while ((am = (struct AppMessage *)IExec->GetMsg(AppPort))) {
        int32 i;

        for (i = 0; i < am->am_NumArgs; i++) {
            char path[FB_PATH_LEN];
            struct WBArg *arg = &am->am_ArgList[i];

            if (IDOS->NameFromLock(arg->wa_Lock, path, sizeof(path)) &&
                IDOS->AddPart(path, arg->wa_Name, sizeof(path)))
                App_AddFile(path);
        }
        IExec->ReplyMsg((struct Message *)am);
    }
    MainWindow_Refresh();
}

/* A track gadget sharing TB, notifying the window while dragged. */
static Object *track(Class *cls, uint32 id, CONST_STRPTR empty_text)
{
    Object *o = IIntuition->NewObject(cls, NULL,
        GA_ID,           id,
        GA_RelVerify,    TRUE,
        GA_GadgetHelp,   TRUE,
        ICA_TARGET,      ICTARGET_IDCMP,
        TRACK_Timebase,  &TB,
        TRACK_Timeline,  &App.timeline,
        TRACK_EmptyText, empty_text,
        TAG_END);

    Gad[id] = (struct Gadget *)o;
    Timebase_Attach(&TB, Gad[id]);
    return o;
}

int MainWindow_Open(void)
{
    Object *buttons, *timeline, *viewrow, *root;
    int i;

    for (i = 0; i < C_COUNT; i++) {
        classes[i].base = IIntuition->OpenClass(classes[i].name, 52,
                                                &classes[i].cls);
        if (!classes[i].base)
            return -1;
    }
    if (!Track_MakeClasses())
        return -1;
    TrackClassesMade = TRUE;
    Timebase_Init(&TB, FB_CELL_W_DEFAULT);
    IdcmpHook.h_Entry = (HOOKFUNC)idcmp_hook;
    strcpy(PositionText, "0:00");
    Hints[0].hi_Text = FB_Str(MSG_HINT_ZOOM);
    Hints[1].hi_Text = FB_Str(MSG_HINT_PLAYHEAD);
    Hints[2].hi_Text = FB_Str(MSG_HINT_PLAYHEAD);

    LB = (struct ListBrowserIFace *)IExec->GetInterface(
             (struct Library *)classes[C_LISTBROWSER].base, "main", 1, NULL);
    if (!LB)
        return -1;

    AppPort = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    IExec->NewList(&ClipList);

    Columns = LB->AllocLBColumnInfo(5,
        LBCIA_Column, 0, LBCIA_Title, FB_Str(MSG_COL_NUMBER),  LBCIA_Weight, 5,
        LBCIA_Column, 1, LBCIA_Title, FB_Str(MSG_COL_FILE),    LBCIA_Weight, 45,
        LBCIA_Column, 2, LBCIA_Title, FB_Str(MSG_COL_IN),      LBCIA_Weight, 15,
        LBCIA_Column, 3, LBCIA_Title, FB_Str(MSG_COL_OUT),     LBCIA_Weight, 15,
        LBCIA_Column, 4, LBCIA_Title, FB_Str(MSG_COL_EFFECTS), LBCIA_Weight, 20,
        TAG_END);

    buttons = IIntuition->NewObject(CLS(C_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
        LAYOUT_EvenSize,    TRUE,
        LAYOUT_AddChild, button(GID_ADD,     FB_Str(MSG_BTN_ADD)),
        LAYOUT_AddChild, button(GID_SPLIT,   FB_Str(MSG_BTN_SPLIT)),
        LAYOUT_AddChild, button(GID_REMOVE,  FB_Str(MSG_BTN_REMOVE)),
        LAYOUT_AddChild, button(GID_FADE,    FB_Str(MSG_BTN_FADE)),
        LAYOUT_AddChild, button(GID_PREVIEW, FB_Str(MSG_BTN_PREVIEW)),
        LAYOUT_AddChild, button(GID_EXPORT,  FB_Str(MSG_BTN_EXPORT)),
        TAG_END);

    Gad[GID_CLIPS] = (struct Gadget *)IIntuition->NewObject(CLS(C_LISTBROWSER), NULL,
        GA_ID,                   GID_CLIPS,
        GA_RelVerify,            TRUE,
        LISTBROWSER_ColumnInfo,  Columns,
        LISTBROWSER_ColumnTitles, TRUE,
        LISTBROWSER_Labels,      &ClipList,
        LISTBROWSER_ShowSelected, TRUE,
        TAG_END);

    /* Ruler, film strip and scroller, stacked without gaps. */
    timeline = IIntuition->NewObject(CLS(C_LAYOUT), NULL,
        LAYOUT_Orientation,  LAYOUT_ORIENT_VERT,
        LAYOUT_InnerSpacing, 0,
        LAYOUT_AddChild, track(RulerClass, GID_RULER, NULL),
        CHILD_WeightedHeight, 0,
        LAYOUT_AddChild, track(FilmstripClass, GID_FILMSTRIP,
                               FB_Str(MSG_TRACK_EMPTY)),
        CHILD_WeightedHeight, 0,
        LAYOUT_AddChild, Gad[GID_SCROLL] = (struct Gadget *)IIntuition->NewObject(
                CLS(C_SCROLLER), NULL,
            GA_ID,                GID_SCROLL,
            GA_RelVerify,         TRUE,
            GA_Disabled,          TRUE,
            ICA_TARGET,           ICTARGET_IDCMP,
            SCROLLER_Orientation, SORIENT_HORIZ,
            SCROLLER_Total,       1,
            SCROLLER_Visible,     1,
            SCROLLER_Top,         0,
            TAG_END),
        CHILD_WeightedHeight, 0,
        TAG_END);

    Gad[GID_POSITION] = (struct Gadget *)IIntuition->NewObject(CLS(C_BUTTON), NULL,
        GA_ID,       GID_POSITION,
        GA_ReadOnly, TRUE,
        GA_Text,     PositionText,
        TAG_END);

    Gad[GID_ZOOM] = (struct Gadget *)IIntuition->NewObject(CLS(C_SLIDER), NULL,
        GA_ID,              GID_ZOOM,
        GA_RelVerify,       TRUE,
        GA_Disabled,        TRUE,
        ICA_TARGET,         ICTARGET_IDCMP,
        SLIDER_Min,         0,
        SLIDER_Max,         1,
        SLIDER_Level,       0,
        SLIDER_Orientation, SLIDER_HORIZONTAL,
        TAG_END);

    viewrow = IIntuition->NewObject(CLS(C_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
        LAYOUT_AddChild,    Gad[GID_POSITION],
        CHILD_WeightedWidth, 0,
        CHILD_MinWidth,     80,
        CHILD_Label,        label(FB_Str(MSG_LABEL_PLAYHEAD)),
        LAYOUT_AddChild,    Gad[GID_ZOOM],
        CHILD_Label,        label(FB_Str(MSG_LABEL_ZOOM)),
        TAG_END);

    Gad[GID_PROGRESS] = (struct Gadget *)IIntuition->NewObject(CLS(C_FUELGAUGE), NULL,
        GA_ID,           GID_PROGRESS,
        FUELGAUGE_Min,   0,
        FUELGAUGE_Max,   100,
        FUELGAUGE_Level, 0,
        FUELGAUGE_Percent, TRUE,
        TAG_END);

    Gad[GID_STATUS] = (struct Gadget *)IIntuition->NewObject(CLS(C_BUTTON), NULL,
        GA_ID,       GID_STATUS,
        GA_ReadOnly, TRUE,
        GA_Text,     App.status,
        BUTTON_Justification, BCJ_LEFT,
        TAG_END);

    root = IIntuition->NewObject(CLS(C_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
        LAYOUT_SpaceOuter,  TRUE,
        LAYOUT_AddChild,    buttons,
        CHILD_WeightedHeight, 0,
        LAYOUT_AddChild,    timeline,
        CHILD_WeightedHeight, 0,
        LAYOUT_AddChild,    viewrow,
        CHILD_WeightedHeight, 0,
        LAYOUT_AddChild,    Gad[GID_CLIPS],
        LAYOUT_AddChild,    Gad[GID_PROGRESS],
        CHILD_WeightedHeight, 0,
        CHILD_Label,        label(FB_Str(MSG_LABEL_RENDER)),
        LAYOUT_AddChild,    Gad[GID_STATUS],
        CHILD_WeightedHeight, 0,
        TAG_END);

    WinObj = IIntuition->NewObject(CLS(C_WINDOW), NULL,
        WA_Title,           FB_Str(MSG_WINDOW_TITLE),
        WA_Activate,        TRUE,
        WA_DepthGadget,     TRUE,
        WA_DragBar,         TRUE,
        WA_CloseGadget,     TRUE,
        WA_SizeGadget,      TRUE,
        WA_InnerWidth,      640,
        WA_InnerHeight,     380,
        WA_IDCMP,           IDCMP_GADGETUP | IDCMP_CLOSEWINDOW |
                            IDCMP_IDCMPUPDATE | IDCMP_NEWSIZE |
                            IDCMP_GADGETHELP,
        WINDOW_GadgetHelp,  TRUE,
        WINDOW_HintInfo,    Hints,
        WINDOW_IDCMPHook,   &IdcmpHook,
        WINDOW_IDCMPHookBits, IDCMP_IDCMPUPDATE,
        WINDOW_Position,    WPOS_CENTERSCREEN,
        WINDOW_AppPort,     AppPort,
        WINDOW_AppWindow,   TRUE,
        WINDOW_Layout,      root,
        TAG_END);
    if (!WinObj)
        return -1;

    Win = (struct Window *)IIntuition->IDoMethod(WinObj, WM_OPEN, NULL);
    if (!Win)
        return -1;
    MainWindow_Refresh();
    return 0;
}

uint32 MainWindow_SigMask(void)
{
    uint32 mask = 0;

    if (WinObj)
        IIntuition->GetAttr(WINDOW_SigMask, WinObj, &mask);
    if (AppPort)
        mask |= 1UL << AppPort->mp_SigBit;
    return mask;
}

void MainWindow_Handle(uint32 sigs)
{
    uint32 result;
    uint16 code;

    if (AppPort && (sigs & (1UL << AppPort->mp_SigBit)))
        handle_appmessages();

    while ((result = IIntuition->IDoMethod(WinObj, WM_HANDLEINPUT, &code))
           != WMHI_LASTMSG) {
        switch (result & WMHI_CLASSMASK) {
        case WMHI_CLOSEWINDOW:
            App.quit = 1;
            break;
        case WMHI_GADGETUP:
            handle_gadget(result & WMHI_GADGETMASK);
            break;
        case WMHI_NEWSIZE:
            /* The tracks have taken the new width in GM_LAYOUT. */
            sync_controls();
            break;
        }
    }
}

void MainWindow_Close(void)
{
    int i;

    if (WinObj) {
        IIntuition->DisposeObject(WinObj);   /* disposes all children */
        WinObj = NULL;
        Win = NULL;
    }
    if (LB) {
        free_clip_nodes();
        if (Columns)
            LB->FreeLBColumnInfo(Columns);
        IExec->DropInterface((struct Interface *)LB);
        LB = NULL;
    }
    if (AppPort) {
        struct Message *m;

        while ((m = IExec->GetMsg(AppPort)))
            IExec->ReplyMsg(m);
        IExec->FreeSysObject(ASOT_PORT, AppPort);
        AppPort = NULL;
    }
    if (TrackClassesMade) {
        Track_FreeClasses();     /* after the window, which held the tracks */
        TrackClassesMade = FALSE;
    }
    for (i = C_COUNT - 1; i >= 0; i--) {
        if (classes[i].base)
            IIntuition->CloseClass(classes[i].base);
        classes[i].base = NULL;
    }
}
