/*
 * mainwindow.c - the ReAction main window.
 *
 *   [Add file...] [Split] [Remove] [Fade] [Preview] [Export...]
 *   +-------------------------------------------------------+
 *   | timeline: a ListBrowser of clips for now; replaced by |
 *   | the custom timeline gadget (gadgets/timeline/) later  |
 *   +-------------------------------------------------------+
 *   playhead [==========|-------------------]
 *   render   [#######.........]
 *   status line
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

#include <classes/window.h>
#include <gadgets/layout.h>
#include <gadgets/button.h>
#include <gadgets/listbrowser.h>
#include <gadgets/slider.h>
#include <gadgets/fuelgauge.h>
#include <images/label.h>
#include <workbench/workbench.h>

#include "app.h"
#include "mainwindow.h"
#include "formbrod/module.h"

enum {
    GID_ADD = 1, GID_SPLIT, GID_REMOVE, GID_FADE, GID_PREVIEW, GID_EXPORT,
    GID_CLIPS, GID_PLAYHEAD, GID_PROGRESS, GID_STATUS, GID_LAST
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
};
enum { C_WINDOW, C_LAYOUT, C_BUTTON, C_LISTBROWSER, C_SLIDER, C_FUELGAUGE,
       C_LABEL, C_COUNT };

static struct ListBrowserIFace *LB;   /* listbrowser.gadget interface */
static struct MsgPort *AppPort;
static Object *WinObj;
static struct Window *Win;
static struct Gadget *Gad[GID_LAST];
static struct List ClipList;
static struct ColumnInfo *Columns;

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
    snprintf(buf, len, "%ld:%02ld.%01ld", (long)(ms / 60000),
             (long)((ms / 1000) % 60), (long)((ms / 100) % 10));
}

void MainWindow_Refresh(void)
{
    const FBTimeline *tl = &App.timeline;
    fb_ms_t total = FB_TimelineDuration(tl);
    int i;

    if (!WinObj)
        return;

    IIntuition->SetGadgetAttrs(Gad[GID_CLIPS], Win, NULL,
                               LISTBROWSER_Labels, ~0, TAG_END);
    free_clip_nodes();
    for (i = 0; i < tl->num_clips; i++) {
        const FBClip *c = &tl->clips[i];
        char num[8], in[16], out[16], fx[64] = "";
        struct Node *n;
        int e;

        snprintf(num, sizeof(num), "%d", i + 1);
        fmt_time(in, sizeof(in), c->in);
        fmt_time(out, sizeof(out), c->out);
        for (e = 0; e < c->num_effects; e++) {
            strncat(fx, e ? ", " : "", sizeof(fx) - strlen(fx) - 1);
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

    /* playhead in tenths of a second */
    IIntuition->SetGadgetAttrs(Gad[GID_PLAYHEAD], Win, NULL,
        SLIDER_Max,     total > 0 ? (int32)(total / 100) : 1,
        GA_Disabled,    total <= 0,
        TAG_END);

    IIntuition->SetGadgetAttrs(Gad[GID_SPLIT], Win, NULL, GA_Disabled, total <= 0, TAG_END);
    IIntuition->SetGadgetAttrs(Gad[GID_REMOVE], Win, NULL, GA_Disabled, total <= 0, TAG_END);
    IIntuition->SetGadgetAttrs(Gad[GID_FADE], Win, NULL, GA_Disabled, total <= 0, TAG_END);
    IIntuition->SetGadgetAttrs(Gad[GID_PREVIEW], Win, NULL, GA_Disabled, total <= 0, TAG_END);
    IIntuition->SetGadgetAttrs(Gad[GID_EXPORT], Win, NULL, GA_Disabled, total <= 0, TAG_END);
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
    uint32 level = 0;
    int clip;

    switch (id) {
    case GID_ADD:
        if (ask_file("Add video file", FALSE, path, sizeof(path)))
            App_AddFile(path);
        break;
    case GID_SPLIT:
        IIntuition->GetAttr(SLIDER_Level, (Object *)Gad[GID_PLAYHEAD], &level);
        App_Split((fb_ms_t)level * 100);
        break;
    case GID_REMOVE:
        if ((clip = selected_clip()) >= 0)
            App_RemoveClip(clip);
        else
            App_SetStatus("Select a clip first");
        break;
    case GID_FADE:
        if ((clip = selected_clip()) >= 0)
            App_AddEffect(clip, "fade");
        else
            App_SetStatus("Select a clip first");
        break;
    case GID_PREVIEW:
        App_Export("T:formbrod-preview.mp4", TRUE);
        break;
    case GID_EXPORT:
        if (App.rendering)
            App_CancelRender();
        else if (ask_file("Export video", TRUE, path, sizeof(path)))
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

int MainWindow_Open(void)
{
    Object *buttons, *root;
    int i;

    for (i = 0; i < C_COUNT; i++) {
        classes[i].base = IIntuition->OpenClass(classes[i].name, 52,
                                                &classes[i].cls);
        if (!classes[i].base)
            return -1;
    }
    LB = (struct ListBrowserIFace *)IExec->GetInterface(
             (struct Library *)classes[C_LISTBROWSER].base, "main", 1, NULL);
    if (!LB)
        return -1;

    AppPort = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    IExec->NewList(&ClipList);

    Columns = LB->AllocLBColumnInfo(5,
        LBCIA_Column, 0, LBCIA_Title, "#",     LBCIA_Weight, 5,
        LBCIA_Column, 1, LBCIA_Title, "File",  LBCIA_Weight, 45,
        LBCIA_Column, 2, LBCIA_Title, "In",    LBCIA_Weight, 15,
        LBCIA_Column, 3, LBCIA_Title, "Out",   LBCIA_Weight, 15,
        LBCIA_Column, 4, LBCIA_Title, "Effects", LBCIA_Weight, 20,
        TAG_END);

    buttons = IIntuition->NewObject(CLS(C_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
        LAYOUT_EvenSize,    TRUE,
        LAYOUT_AddChild, button(GID_ADD,     "_Add file..."),
        LAYOUT_AddChild, button(GID_SPLIT,   "_Split"),
        LAYOUT_AddChild, button(GID_REMOVE,  "_Remove"),
        LAYOUT_AddChild, button(GID_FADE,    "_Fade"),
        LAYOUT_AddChild, button(GID_PREVIEW, "_Preview"),
        LAYOUT_AddChild, button(GID_EXPORT,  "_Export..."),
        TAG_END);

    Gad[GID_CLIPS] = (struct Gadget *)IIntuition->NewObject(CLS(C_LISTBROWSER), NULL,
        GA_ID,                   GID_CLIPS,
        GA_RelVerify,            TRUE,
        LISTBROWSER_ColumnInfo,  Columns,
        LISTBROWSER_ColumnTitles, TRUE,
        LISTBROWSER_Labels,      &ClipList,
        LISTBROWSER_ShowSelected, TRUE,
        TAG_END);

    Gad[GID_PLAYHEAD] = (struct Gadget *)IIntuition->NewObject(CLS(C_SLIDER), NULL,
        GA_ID,          GID_PLAYHEAD,
        GA_RelVerify,   TRUE,
        GA_Disabled,    TRUE,
        SLIDER_Min,     0,
        SLIDER_Max,     1,
        SLIDER_Orientation, SLIDER_HORIZONTAL,
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
        LAYOUT_AddChild,    Gad[GID_CLIPS],
        LAYOUT_AddChild,    Gad[GID_PLAYHEAD],
        CHILD_WeightedHeight, 0,
        CHILD_Label,        label("Playhead"),
        LAYOUT_AddChild,    Gad[GID_PROGRESS],
        CHILD_WeightedHeight, 0,
        CHILD_Label,        label("Render"),
        LAYOUT_AddChild,    Gad[GID_STATUS],
        CHILD_WeightedHeight, 0,
        TAG_END);

    WinObj = IIntuition->NewObject(CLS(C_WINDOW), NULL,
        WA_Title,           "Formbröd",
        WA_Activate,        TRUE,
        WA_DepthGadget,     TRUE,
        WA_DragBar,         TRUE,
        WA_CloseGadget,     TRUE,
        WA_SizeGadget,      TRUE,
        WA_InnerWidth,      560,
        WA_InnerHeight,     300,
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
    for (i = C_COUNT - 1; i >= 0; i--) {
        if (classes[i].base)
            IIntuition->CloseClass(classes[i].base);
        classes[i].base = NULL;
    }
}
