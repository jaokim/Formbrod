/*
 * mainwindow.c - the ReAction main window.
 *
 *   [Add file...] [Split] [Set in] [Set out] [Remove] [Fade] [Preview]
 *   [Export...]
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
 * The menus (GadTools, attached with WINDOW_MenuStrip) repeat the buttons
 * and add what has no button: About, Quit, every effect module, and the
 * default fit mode. Menu items carry their command ID in
 * GTMENUITEM_USERDATA; for actions that have a button it is the button's
 * GID, so both go through the same code.
 *
 * Projects are opened and saved from the Project menu. Before New, Open
 * and Quit throw away unsaved changes, a requester offers to save them. The
 * window title shows the project's name, with " *" while it has changes.
 *
 * Each kind of file requester (media files, projects, export) opens in the
 * drawer it was last used in. The drawers are kept in the program's
 * settings (src/prefs/), so they are remembered between runs too.
 *
 * Files can be dropped on the window (AppWindow); a dropped project file is
 * opened rather than added. Classes are opened with
 * OpenClass() and objects created with NewObject() on the Class pointer, so
 * nothing depends on reaction_macros.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/asl.h>
#include <proto/wb.h>
#include <proto/listbrowser.h>
#include <proto/utility.h>
#include <proto/gadtools.h>

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
#include <libraries/gadtools.h>

#include "app.h"
#include "formbrod/project.h"
#include "mainwindow.h"
#include "formbrod/module.h"
#include "locale/catalog.h"
#include "gui/timebase.h"
#include "gui/track.h"
#include "prefs/prefs.h"
#include "version.h"

enum {
    GID_ADD = 1, GID_SPLIT, GID_REMOVE, GID_FADE, GID_PREVIEW, GID_EXPORT,
    GID_CLIPS, GID_POSITION, GID_PROGRESS, GID_STATUS,
    GID_RULER, GID_FILMSTRIP, GID_SCROLL, GID_ZOOM,
    GID_SET_IN, GID_SET_OUT, GID_LAST
};

/* Menu commands without a button. Menu items for button actions use the
 * button's GID. */
enum {
    MID_CANCEL = 100, MID_ABOUT, MID_QUIT, MID_FIT_MENU,
    MID_NEW, MID_OPEN, MID_SAVE, MID_SAVE_AS,
    MID_FIT_FIT, MID_FIT_FILL, MID_FIT_STRETCH, MID_SHOW_PICTURES,
    MID_WRITE_LOG,
    MID_EFFECT = 200     /* + module index */
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
static struct Menu *Menus;
static APTR VisualInfo;

/* Two title buffers, used in turn: Intuition keeps the pointer it was
 * given, so the one on show is never written to. */
static char Titles[2][FB_PATH_LEN + 64];
static int TitleNow;

/* The drawer each kind of file requester opens in. */
enum { DRAWER_MEDIA, DRAWER_PROJECT, DRAWER_EXPORT, DRAWER_COUNT };
static const char *const DrawerKeys[DRAWER_COUNT] = {
    "MediaDrawer", "ProjectDrawer", "ExportDrawer"
};
static char Drawers[DRAWER_COUNT][FB_PATH_LEN];

static void update_title(void);

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

/* ---- Frame pictures --------------------------------------------------- */

/* Whether the film strip shows pictures, from the "ShowPictures" setting.
 * Off is for slower Amigas: then nothing is decoded for thumbnails. */
static BOOL show_pictures(void)
{
    return strcmp(Prefs_GetString("ShowPictures", "1"), "0") != 0;
}

static void set_show_pictures(BOOL on)
{
    Prefs_SetString("ShowPictures", on ? "1" : "0");
    Prefs_Save();
    IIntuition->SetGadgetAttrs(Gad[GID_FILMSTRIP], Win, NULL,
                               FILMSTRIP_Pictures, on, TAG_END);
}

/* ---- Menus ------------------------------------------------------------ */

static FBFit current_fit(void)
{
    FBFit f = App.timeline.format.fit;

    return f == FB_FIT_DEFAULT ? FB_FIT_FIT : f;
}

/* The menu item for a fit mode. */
static uint32 fit_menu_id(FBFit f)
{
    return MID_FIT_FIT + (uint32)(f - FB_FIT_FIT);
}

static int is_fit_id(uint32 id)
{
    return id >= MID_FIT_FIT && id <= MID_FIT_STRETCH;
}

/* Whether a menu command can be used right now. Mirrors the buttons, but
 * also disables what a running render would refuse. */
static BOOL menu_enabled(uint32 id)
{
    BOOL have_clips = FB_TimelineDuration(&App.timeline) > 0;

    if (id >= MID_EFFECT)
        return have_clips;
    switch (id) {
    case GID_SPLIT:
    case GID_REMOVE:
    case GID_SET_IN:
    case GID_SET_OUT:
        return have_clips;
    case GID_PREVIEW:
    case GID_EXPORT:
        return have_clips && !App.rendering;
    case MID_CANCEL:
        return App.rendering != 0;
    default:
        return TRUE;
    }
}

/* Goes through every item and subitem. Without `apply` it counts the items
 * whose enabled or checked state is out of date; with it, fixes them. */
static int update_menu_items(BOOL apply)
{
    struct Menu *m;
    struct MenuItem *item, *sub;
    uint32 want_fit = fit_menu_id(current_fit());
    int stale = 0;

    for (m = Menus; m; m = m->NextMenu) {
        for (item = m->FirstItem; item; item = item->NextItem) {
            for (sub = item; sub; sub = (sub == item ? item->SubItem
                                                    : sub->NextItem)) {
                uint32 id = (uint32)GTMENUITEM_USERDATA(sub);
                uint16 flags = sub->Flags;

                if (id == 0)
                    continue;           /* separator */
                if (menu_enabled(id))
                    flags |= ITEMENABLED;
                else
                    flags &= ~ITEMENABLED;
                if (is_fit_id(id)) {
                    if (id == want_fit)
                        flags |= CHECKED;
                    else
                        flags &= ~CHECKED;
                }
                if (id == MID_SHOW_PICTURES) {
                    if (show_pictures())
                        flags |= CHECKED;
                    else
                        flags &= ~CHECKED;
                }
                if (id == MID_WRITE_LOG) {
                    if (App_LogEnabled())
                        flags |= CHECKED;
                    else
                        flags &= ~CHECKED;
                }
                if (flags != sub->Flags) {
                    stale++;
                    if (apply)
                        sub->Flags = flags;
                }
            }
        }
    }
    return stale;
}

/* Puts the menus in step with the timeline and render state. The strip is
 * taken off the window while it changes, as Intuition requires. */
static void sync_menus(void)
{
    if (!Menus || !update_menu_items(FALSE))
        return;
    if (Win)
        IIntuition->ClearMenuStrip(Win);
    update_menu_items(TRUE);
    if (Win)
        IIntuition->ResetMenuStrip(Win, Menus);
}

static void add_menu(struct NewMenu *nm, int *n, uint8 type,
                     const char *label, const char *key, uint32 id)
{
    nm[*n].nm_Type     = type;
    nm[*n].nm_Label    = (STRPTR)label;
    nm[*n].nm_CommKey  = (STRPTR)key;
    nm[*n].nm_UserData = (APTR)id;
    (*n)++;
}

/* A mutually exclusive, checkable fit-mode subitem. */
static void add_fit_item(struct NewMenu *nm, int *n, const char *label,
                         FBFit f)
{
    /* The fit items are the whole submenu, so bit k is the k-th of them. */
    uint32 all = (1UL << (FB_FIT_COUNT - FB_FIT_FIT)) - 1;

    add_menu(nm, n, NM_SUB, label, NULL, fit_menu_id(f));
    nm[*n - 1].nm_Flags = CHECKIT | (f == current_fit() ? CHECKED : 0);
    nm[*n - 1].nm_MutualExclude = all & ~(1UL << (f - FB_FIT_FIT));
}

/* Builds the menu strip for `scr`. The shortcuts follow the Amiga style
 * guide (N new, O open, S save, A save as, Q quit, ? about) and are the
 * same in every language. Adding a file is I, for import. */
static BOOL make_menus(struct Screen *scr)
{
    int nmods = FB_NumModules();
    struct NewMenu *nm;
    int n = 0, i;

    nm = calloc(32 + nmods, sizeof(*nm));
    if (!nm)
        return FALSE;

    add_menu(nm, &n, NM_TITLE, FB_Str(MSG_MENU_PROJECT), NULL, 0);
    add_menu(nm, &n, NM_ITEM, FB_Str(MSG_MENU_NEW),      "N", MID_NEW);
    add_menu(nm, &n, NM_ITEM, FB_Str(MSG_MENU_OPEN),     "O", MID_OPEN);
    add_menu(nm, &n, NM_ITEM, FB_Str(MSG_MENU_SAVE),     "S", MID_SAVE);
    add_menu(nm, &n, NM_ITEM, FB_Str(MSG_MENU_SAVE_AS),  "A", MID_SAVE_AS);
    add_menu(nm, &n, NM_ITEM, NM_BARLABEL,               NULL, 0);
    add_menu(nm, &n, NM_ITEM, FB_Str(MSG_MENU_ADD),      "I", GID_ADD);
    add_menu(nm, &n, NM_ITEM, NM_BARLABEL,               NULL, 0);
    add_menu(nm, &n, NM_ITEM, FB_Str(MSG_MENU_PREVIEW),  "P", GID_PREVIEW);
    add_menu(nm, &n, NM_ITEM, FB_Str(MSG_MENU_EXPORT),   "E", GID_EXPORT);
    add_menu(nm, &n, NM_ITEM, FB_Str(MSG_MENU_CANCEL),   NULL, MID_CANCEL);
    add_menu(nm, &n, NM_ITEM, NM_BARLABEL,               NULL, 0);
    add_menu(nm, &n, NM_ITEM, FB_Str(MSG_MENU_ABOUT),    "?", MID_ABOUT);
    add_menu(nm, &n, NM_ITEM, NM_BARLABEL,               NULL, 0);
    add_menu(nm, &n, NM_ITEM, FB_Str(MSG_MENU_QUIT),     "Q", MID_QUIT);

    add_menu(nm, &n, NM_TITLE, FB_Str(MSG_MENU_EDIT),    NULL, 0);
    add_menu(nm, &n, NM_ITEM, FB_Str(MSG_MENU_SPLIT),    NULL, GID_SPLIT);
    add_menu(nm, &n, NM_ITEM, FB_Str(MSG_MENU_SET_IN),   NULL, GID_SET_IN);
    add_menu(nm, &n, NM_ITEM, FB_Str(MSG_MENU_SET_OUT),  NULL, GID_SET_OUT);
    add_menu(nm, &n, NM_ITEM, FB_Str(MSG_MENU_REMOVE),   NULL, GID_REMOVE);

    /* One item per registered module, so new modules show up here without
     * GUI changes. Module labels aren't translated yet. */
    add_menu(nm, &n, NM_TITLE, FB_Str(MSG_MENU_EFFECTS), NULL, 0);
    for (i = 0; i < nmods; i++) {
        const FBModule *mod = FB_GetModule(i);

        add_menu(nm, &n, NM_ITEM, mod->label ? mod->label : mod->name,
                 NULL, MID_EFFECT + (uint32)i);
    }
    if (nmods == 0)
        nm[n - 1].nm_Flags = NM_MENUDISABLED;

    add_menu(nm, &n, NM_TITLE, FB_Str(MSG_MENU_SETTINGS), NULL, 0);
    add_menu(nm, &n, NM_ITEM, FB_Str(MSG_MENU_FIT),      NULL, MID_FIT_MENU);
    add_fit_item(nm, &n, FB_Str(MSG_MENU_FIT_FIT),       FB_FIT_FIT);
    add_fit_item(nm, &n, FB_Str(MSG_MENU_FIT_FILL),      FB_FIT_FILL);
    add_fit_item(nm, &n, FB_Str(MSG_MENU_FIT_STRETCH),   FB_FIT_STRETCH);
    add_menu(nm, &n, NM_ITEM, FB_Str(MSG_MENU_SHOW_PICTURES), NULL,
             MID_SHOW_PICTURES);
    nm[n - 1].nm_Flags = CHECKIT | MENUTOGGLE |
                         (show_pictures() ? CHECKED : 0);
    add_menu(nm, &n, NM_ITEM, FB_Str(MSG_MENU_WRITE_LOG), NULL, MID_WRITE_LOG);
    nm[n - 1].nm_Flags = CHECKIT | MENUTOGGLE |
                         (App_LogEnabled() ? CHECKED : 0);

    nm[n].nm_Type = NM_END;

    VisualInfo = IGadTools->GetVisualInfoA(scr, NULL);
    if (VisualInfo)
        Menus = IGadTools->CreateMenus(nm, GTMN_FrontPen, 0, TAG_END);
    free(nm);
    if (!Menus)
        return FALSE;
    if (!IGadTools->LayoutMenus(Menus, VisualInfo,
                                GTMN_NewLookMenus, TRUE, TAG_END))
        return FALSE;
    update_menu_items(TRUE);
    return TRUE;
}

static void free_menus(void)
{
    if (Menus) {
        IGadTools->FreeMenus(Menus);
        Menus = NULL;
    }
    if (VisualInfo) {
        IGadTools->FreeVisualInfo(VisualInfo);
        VisualInfo = NULL;
    }
}

static void show_about(void)
{
    struct EasyStruct es;
    char version[32];

    snprintf(version, sizeof(version), "%s (%s)", FB_VERSION, FB_VERSION_DATE);
    memset(&es, 0, sizeof(es));
    es.es_StructSize   = sizeof(es);
    es.es_Title        = (STRPTR)FB_Str(MSG_ABOUT_TITLE);
    es.es_TextFormat   = (STRPTR)FB_Str(MSG_ABOUT_TEXT);
    es.es_GadgetFormat = (STRPTR)FB_Str(MSG_ABOUT_OK);
    /* Modal, so a running render pauses until it is closed. */
    IIntuition->EasyRequest(Win, &es, NULL, version,
#ifdef HAVE_LIBAV
                            FB_Str(MSG_ABOUT_LIBAV)
#else
                            FB_Str(MSG_ABOUT_NO_LIBAV)
#endif
                            );
}

static void handle_gadget(uint32 id);
static int selected_clip(void);
static BOOL may_discard(void);
static void open_project(void);
static BOOL save_project(BOOL ask);

static void handle_menu(uint32 id)
{
    int clip;

    if (id >= MID_EFFECT) {
        const FBModule *mod = FB_GetModule((int)(id - MID_EFFECT));

        if (!mod)
            return;
        if ((clip = selected_clip()) >= 0)
            App_AddEffect(clip, mod->name);
        else
            App_SetStatus("%s", FB_Str(MSG_STATUS_SELECT_CLIP));
        MainWindow_Refresh();
        return;
    }
    if (is_fit_id(id)) {
        App_SetDefaultFit(FB_FIT_FIT + (int)(id - MID_FIT_FIT));
        MainWindow_Refresh();
        return;
    }
    switch (id) {
    case MID_SHOW_PICTURES:
        /* A toggle: the menu has already flipped its checkmark, and
         * sync_menus() keeps it in step with the setting either way. */
        set_show_pictures(!show_pictures());
        break;
    case MID_WRITE_LOG:
        /* A toggle: the menu has already flipped its checkmark. */
        App_SetLogging(!App_LogEnabled());
        if (App_LogEnabled())
            App_SetStatus(FB_Str(MSG_STATUS_LOG_ON), App_LogFile());
        MainWindow_Refresh();
        break;
    case MID_NEW:
        if (may_discard())
            App_NewProject();
        MainWindow_Refresh();
        break;
    case MID_OPEN:
        open_project();
        MainWindow_Refresh();
        break;
    case MID_SAVE:
    case MID_SAVE_AS:
        save_project(id == MID_SAVE_AS);
        MainWindow_Refresh();
        break;
    case MID_ABOUT:
        show_about();
        break;
    case MID_QUIT:
        if (may_discard())
            App.quit = 1;
        break;
    case MID_CANCEL:
        App_CancelRender();
        MainWindow_Refresh();
        break;
    case GID_EXPORT:
        if (!App.rendering)       /* the button means Cancel then */
            handle_gadget(id);
        break;
    case GID_ADD:
    case GID_SPLIT:
    case GID_SET_IN:
    case GID_SET_OUT:
    case GID_REMOVE:
    case GID_PREVIEW:
        handle_gadget(id);
        break;
    }
}

/* `code` is the first picked item; more may follow through NextSelect. */
static void handle_menupick(uint16 code)
{
    while (code != MENUNULL && Menus && !App.quit) {
        struct MenuItem *item = IIntuition->ItemAddress(Menus, code);

        if (!item)
            break;
        code = item->NextSelect;   /* read first: handling may reset the strip */
        handle_menu((uint32)GTMENUITEM_USERDATA(item));
    }
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
        char num[12], track[12], start[16], in[16], out[16], fx[96] = "";
        const char *fit = fit_text(FB_ClipFitResult(tl, c, fw, fh));
        struct Node *n;
        int e;

        snprintf(num, sizeof(num), "%d", i + 1);
        snprintf(track, sizeof(track), "%d", c->track + 1);
        fmt_time(start, sizeof(start), c->start);
        fmt_time(in, sizeof(in), c->in);
        fmt_time(out, sizeof(out), c->out);
        if (fit)
            strncat(fx, fit, sizeof(fx) - 1);
        for (e = 0; e < c->num_effects; e++) {
            strncat(fx, fx[0] ? ", " : "", sizeof(fx) - strlen(fx) - 1);
            strncat(fx, c->effects[e].module, sizeof(fx) - strlen(fx) - 1);
        }
        n = LB->AllocListBrowserNode(7,
            LBNA_Column, 0, LBNCA_CopyText, TRUE, LBNCA_Text, num,
            LBNA_Column, 1, LBNCA_CopyText, TRUE, LBNCA_Text, track,
            LBNA_Column, 2, LBNCA_CopyText, TRUE,
                            LBNCA_Text, IDOS->FilePart(tl->sources[c->source].path),
            LBNA_Column, 3, LBNCA_CopyText, TRUE, LBNCA_Text, start,
            LBNA_Column, 4, LBNCA_CopyText, TRUE, LBNCA_Text, in,
            LBNA_Column, 5, LBNCA_CopyText, TRUE, LBNCA_Text, out,
            LBNA_Column, 6, LBNCA_CopyText, TRUE, LBNCA_Text, fx,
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
    update_title();
    sync_menus();
}

void MainWindow_SetProgress(const FBProgress *p)
{
    int32 level = 0;

    if (p->total > 0)
        level = (int32)(p->position * 100 / p->total);
    IIntuition->SetGadgetAttrs(Gad[GID_PROGRESS], Win, NULL,
                               FUELGAUGE_Level, level, TAG_END);
}

static void load_drawers(void)
{
    int i;

    for (i = 0; i < DRAWER_COUNT; i++) {
        strncpy(Drawers[i], Prefs_GetString(DrawerKeys[i], ""),
                sizeof(Drawers[i]) - 1);
        Drawers[i][sizeof(Drawers[i]) - 1] = '\0';
    }
}

/* Keeps the drawer for next time, in the program's settings. */
static void remember_drawer(int kind, const char *drawer)
{
    if (strcmp(Drawers[kind], drawer) == 0)
        return;
    strncpy(Drawers[kind], drawer, sizeof(Drawers[kind]) - 1);
    Drawers[kind][sizeof(Drawers[kind]) - 1] = '\0';
    Prefs_SetString(DrawerKeys[kind], Drawers[kind]);
    Prefs_Save();
}

/* Asks for a file, starting in the drawer last used for this `kind`.
 * `file` is the name filled in to begin with (NULL for none). */
static int ask_file(int kind, const char *title, int save, const char *file,
                    char *buf, size_t len)
{
    struct FileRequester *fr;
    char dir[FB_PATH_LEN];
    const char *drawer = Drawers[kind];
    int ok = 0;

    /* Before any file has been added, start where the project is. */
    if (!drawer[0] && App.project[0]) {
        FB_PathDir(App.project, dir, sizeof(dir));
        drawer = dir;
    }
    fr = IAsl->AllocAslRequestTags(ASL_FileRequest,
        ASLFR_Window,        Win,
        ASLFR_TitleText,     title,
        ASLFR_DoSaveMode,    save,
        ASLFR_InitialDrawer, drawer,
        ASLFR_InitialFile,   file ? file : "",
        TAG_END);
    if (!fr)
        return 0;
    if (IAsl->AslRequestTags(fr, TAG_END)) {
        strncpy(buf, fr->fr_Drawer, len - 1);
        buf[len - 1] = '\0';
        ok = fr->fr_File[0] && IDOS->AddPart(buf, fr->fr_File, len) != 0;
        if (ok)
            remember_drawer(kind, fr->fr_Drawer);
    }
    IAsl->FreeAslRequest(fr);
    return ok;
}

/* ---- Projects --------------------------------------------------------- */

static const char *project_name(void)
{
    return App.project[0] ? (const char *)IDOS->FilePart(App.project)
                          : FB_Str(MSG_UNTITLED);
}

static void update_title(void)
{
    char want[sizeof(Titles[0])];

    snprintf(want, sizeof(want), FB_Str(MSG_WINDOW_TITLE_PROJECT),
             project_name(), App.modified ? " *" : "");
    if (strcmp(want, Titles[TitleNow]) == 0)
        return;
    TitleNow ^= 1;
    strcpy(Titles[TitleNow], want);
    if (WinObj)
        IIntuition->SetAttrs(WinObj, WA_Title, Titles[TitleNow], TAG_END);
}

/* Saves the project, asking for a file if `ask` or it has none yet.
 * FALSE if it wasn't saved. */
static BOOL save_project(BOOL ask)
{
    char path[FB_PATH_LEN];

    if (!ask && App.project[0])
        return App_SaveProject(NULL) == 0;
    if (!ask_file(DRAWER_PROJECT, FB_Str(MSG_REQ_SAVE_PROJECT), TRUE,
                  App.project[0] ? project_name() : NULL, path, sizeof(path)))
        return FALSE;
    return App_SaveProject(path) == 0;
}

/* Before something replaces the project: if it has unsaved changes, asks
 * whether to save them. FALSE means stop (Cancel, or saving failed). */
static BOOL may_discard(void)
{
    struct EasyStruct es;

    if (!App.modified)
        return TRUE;
    memset(&es, 0, sizeof(es));
    es.es_StructSize   = sizeof(es);
    es.es_Title        = (STRPTR)FB_Str(MSG_UNSAVED_TITLE);
    es.es_TextFormat   = (STRPTR)FB_Str(MSG_UNSAVED_TEXT);
    es.es_GadgetFormat = (STRPTR)FB_Str(MSG_UNSAVED_BUTTONS);
    switch (IIntuition->EasyRequest(Win, &es, NULL, project_name())) {
    case 1:                       /* Save */
        if (save_project(FALSE))
            return TRUE;
        MainWindow_Refresh();     /* show why it wasn't saved */
        return FALSE;
    case 2:                       /* Discard */
        return TRUE;
    default:                      /* Cancel */
        return FALSE;
    }
}

static void open_project(void)
{
    char path[FB_PATH_LEN];

    if (may_discard() &&
        ask_file(DRAWER_PROJECT, FB_Str(MSG_REQ_OPEN_PROJECT), FALSE, NULL,
                 path, sizeof(path)))
        App_OpenProject(path);
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
        if (ask_file(DRAWER_MEDIA, FB_Str(MSG_REQ_ADD_FILE), FALSE, NULL,
                     path, sizeof(path)))
            App_AddFile(path);
        break;
    case GID_SET_IN:
    case GID_SET_OUT: {
        fb_ms_t at = FB_MS(TB.playhead);

        /* The selected clip, or the picture under the playhead. */
        if ((clip = selected_clip()) < 0)
            clip = FB_TimelineClipAt(&App.timeline, at, NULL);
        if (id == GID_SET_OUT) {
            App_SetOut(clip, at);
        } else if (App_SetIn(clip, at) == 0) {
            /* The frame that was under the playhead now starts the clip. */
            Timebase_SetPlayhead(&TB, FB_US(App.timeline.clips[clip].start));
        }
        break;
    }
    case GID_SPLIT:
        /* The selected clip's track, or every track if none is. */
        clip = selected_clip();
        App_Split(clip >= 0 ? App.timeline.clips[clip].track : -1,
                  FB_MS(TB.playhead));
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
        else if (ask_file(DRAWER_EXPORT, FB_Str(MSG_REQ_EXPORT), TRUE, NULL,
                          path, sizeof(path)))
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

            if (!IDOS->NameFromLock(arg->wa_Lock, path, sizeof(path)) ||
                !IDOS->AddPart(path, arg->wa_Name, sizeof(path)))
                continue;
            if (!App_IsProjectFile(path))
                App_AddFile(path);
            else if (may_discard())
                App_OpenProject(path);
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
        FILMSTRIP_Pictures, show_pictures(),
        TAG_END);

    Gad[id] = (struct Gadget *)o;
    Timebase_Attach(&TB, Gad[id]);
    return o;
}

int MainWindow_Open(void)
{
    Object *buttons, *timeline, *viewrow, *root;
    struct Screen *scr;
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

    Columns = LB->AllocLBColumnInfo(7,
        LBCIA_Column, 0, LBCIA_Title, FB_Str(MSG_COL_NUMBER),  LBCIA_Weight, 5,
        LBCIA_Column, 1, LBCIA_Title, FB_Str(MSG_COL_TRACK),   LBCIA_Weight, 7,
        LBCIA_Column, 2, LBCIA_Title, FB_Str(MSG_COL_FILE),    LBCIA_Weight, 33,
        LBCIA_Column, 3, LBCIA_Title, FB_Str(MSG_COL_START),   LBCIA_Weight, 12,
        LBCIA_Column, 4, LBCIA_Title, FB_Str(MSG_COL_IN),      LBCIA_Weight, 12,
        LBCIA_Column, 5, LBCIA_Title, FB_Str(MSG_COL_OUT),     LBCIA_Weight, 12,
        LBCIA_Column, 6, LBCIA_Title, FB_Str(MSG_COL_EFFECTS), LBCIA_Weight, 19,
        TAG_END);

    buttons = IIntuition->NewObject(CLS(C_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
        LAYOUT_EvenSize,    TRUE,
        LAYOUT_AddChild, button(GID_ADD,     FB_Str(MSG_BTN_ADD)),
        LAYOUT_AddChild, button(GID_SPLIT,   FB_Str(MSG_BTN_SPLIT)),
        LAYOUT_AddChild, button(GID_SET_IN,  FB_Str(MSG_BTN_SET_IN)),
        LAYOUT_AddChild, button(GID_SET_OUT, FB_Str(MSG_BTN_SET_OUT)),
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

    /* The menus are laid out for the screen the window opens on, so lock
     * that screen and open the window there. Without menus the window is
     * still usable through the buttons. */
    load_drawers();
    update_title();                /* no window yet: just fills Titles */

    scr = IIntuition->LockPubScreen(NULL);
    if (!scr)
        return -1;
    if (!make_menus(scr))
        free_menus();

    WinObj = IIntuition->NewObject(CLS(C_WINDOW), NULL,
        WA_PubScreen,       scr,
        WA_Title,           Titles[TitleNow],
        WA_Activate,        TRUE,
        WA_DepthGadget,     TRUE,
        WA_DragBar,         TRUE,
        WA_CloseGadget,     TRUE,
        WA_SizeGadget,      TRUE,
        WA_InnerWidth,      640,
        WA_InnerHeight,     380,
        WA_IDCMP,           IDCMP_GADGETUP | IDCMP_CLOSEWINDOW |
                            IDCMP_IDCMPUPDATE | IDCMP_NEWSIZE |
                            IDCMP_GADGETHELP | IDCMP_MENUPICK |
                            IDCMP_VANILLAKEY,
        WA_NewLookMenus,    TRUE,
        WINDOW_GadgetHelp,  TRUE,
        WINDOW_HintInfo,    Hints,
        WINDOW_IDCMPHook,   &IdcmpHook,
        WINDOW_IDCMPHookBits, IDCMP_IDCMPUPDATE,
        WINDOW_Position,    WPOS_CENTERSCREEN,
        WINDOW_AppPort,     AppPort,
        WINDOW_AppWindow,   TRUE,
        WINDOW_Layout,      root,
        WINDOW_MenuStrip,   Menus,
        TAG_END);
    if (WinObj)
        Win = (struct Window *)IIntuition->IDoMethod(WinObj, WM_OPEN, NULL);
    IIntuition->UnlockPubScreen(NULL, scr);   /* the open window holds it */
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
            if (may_discard())
                App.quit = 1;
            break;
        case WMHI_GADGETUP:
            handle_gadget(result & WMHI_GADGETMASK);
            break;
        case WMHI_VANILLAKEY:
            /* I and O: Set in and Set out at the playhead. */
            switch (result & WMHI_KEYMASK) {
            case 'i': case 'I':
                handle_gadget(GID_SET_IN);
                break;
            case 'o': case 'O':
                handle_gadget(GID_SET_OUT);
                break;
            }
            break;
        case WMHI_MENUPICK:
            handle_menupick(code);
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
    free_menus();                            /* window.class doesn't */
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
