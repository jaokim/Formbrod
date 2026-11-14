/*
 * toolbar.c - the button row as a speedbar.gadget with AISS icons.
 *
 * The buttons come from a table (ToolbarButton, filled in by mainwindow.c).
 * Each node's number is its command ID, so a click goes to handle_gadget()
 * like the menu item does, and a module or ARexx can later add a row to
 * the table and rebuild the bar.
 *
 * Icons are AISS images in TBImages:, three files per icon: "name",
 * "name_s" (selected) and "name_g" (ghosted). They are loaded with
 * bitmap.image. Each table entry lists names to try in turn, since sets
 * differ. If TBImages: isn't there, or any icon is missing, the whole bar
 * shows text so it looks the same all along.
 *
 * speedbar.gadget only reads its node list when SPEEDBAR_Buttons is set,
 * so a change means: detach the list (~0), change the nodes, attach it
 * again. Toolbar_Set() only records the wanted state and Toolbar_Apply()
 * does this once, and only when something changed, so the frequent
 * MainWindow_Refresh() calls don't make the bar flicker.
 *
 * The gadget frees neither the nodes nor the images: Toolbar_Free() does,
 * after the window object (which disposes the gadget) is gone.
 *
 * NOT YET COMPILED against the SDK: tag and function names follow the
 * OS4 speedbar.gadget and bitmap.image autodocs as remembered.
 */
#include <stdio.h>
#include <string.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/speedbar.h>

#include <gadgets/speedbar.h>
#include <images/bitmap.h>

#include "gui/toolbar.h"
#include "locale/catalog.h"
#include "prefs/prefs.h"

#define MAX_BUTTONS 32
#define TEXT_LEN    64
#define ICON_PATH   "TBImages:"

/* Not static: proto/speedbar.h declares it, and defining it here keeps
 * libauto from opening the class a second time. */
struct SpeedBarIFace *ISpeedBar;

static struct ClassLibrary *SbLib, *BmLib;
static Class *SbClass, *BmClass;

typedef struct {
    const ToolbarButton *def;
    struct Node *node;
    Object *image, *alt_image;   /* NULL when not found / not loaded */
    const char *alt_icons;       /* what alt_image was loaded from */
    char text[TEXT_LEN], alt_text[TEXT_LEN];
    /* Shown now, and wanted by the last Toolbar_Set(). */
    BOOL disabled, selected, alt;
    BOOL want_disabled, want_selected, want_alt;
    BOOL stale;                  /* the gadget may differ from what we think */
} Entry;

static Entry Entries[MAX_BUTTONS];
static int Count;
static struct List Buttons;
static Object *Bar;
static struct Screen *Scr;
static ToolbarStyle Style;
static BOOL Icons;               /* every button has its icon */

static ToolbarStyle style_setting(void)
{
    const char *s = Prefs_GetString("ToolbarStyle", "both");

    if (strcmp(s, "icons") == 0)
        return TOOLBAR_ICONS;
    if (strcmp(s, "text") == 0)
        return TOOLBAR_TEXT;
    return TOOLBAR_BOTH;
}

/* A label without the '_' that marks a key in the button texts. */
static void plain_text(char *dst, uint32 msg)
{
    const char *s = FB_Str(msg);
    size_t n = 0;

    for (; *s && n < TEXT_LEN - 1; s++)
        if (*s != '_')
            dst[n++] = *s;
    dst[n] = '\0';
}

static BOOL file_exists(const char *path)
{
    BPTR lock = IDOS->Lock(path, SHARED_LOCK);

    if (!lock)
        return FALSE;
    IDOS->UnLock(lock);
    return TRUE;
}

/* The first of the '|'-separated AISS names that exists, as a bitmap.image
 * with its selected and ghosted versions when those are there. */
static Object *load_icon(const char *names)
{
    char name[64], path[96], sel[100], dis[100];
    const char *p = names;
    Object *img = NULL;
    APTR oldwin;

    if (!names || !*names || !BmClass)
        return NULL;
    /* No "Please insert volume TBImages:" requester when AISS is missing. */
    oldwin = IDOS->SetProcWindow((APTR)-1);
    while (*p && !img) {
        size_t n = strcspn(p, "|");

        if (n > 0 && n < sizeof(name)) {
            memcpy(name, p, n);
            name[n] = '\0';
            snprintf(path, sizeof(path), ICON_PATH "%s", name);
            if (file_exists(path)) {
                snprintf(sel, sizeof(sel), "%s_s", path);
                snprintf(dis, sizeof(dis), "%s_g", path);
                img = IIntuition->NewObject(BmClass, NULL,
                    BITMAP_SourceFile,         path,
                    BITMAP_SelectSourceFile,   file_exists(sel) ? sel : NULL,
                    BITMAP_DisabledSourceFile, file_exists(dis) ? dis : NULL,
                    BITMAP_Screen,             Scr,
                    BITMAP_Masking,            TRUE,
                    TAG_END);
            }
        }
        p += n;
        if (*p == '|')
            p++;
    }
    IDOS->SetProcWindow(oldwin);
    return img;
}

static BOOL open_classes(void)
{
    SbLib = IIntuition->OpenClass("gadgets/speedbar.gadget", 52,
                                         &SbClass);
    if (!SbLib)
        return FALSE;
    ISpeedBar = (struct SpeedBarIFace *)IExec->GetInterface(
        (struct Library *)SbLib, "main", 1, NULL);
    if (!ISpeedBar)
        return FALSE;
    /* Without bitmap.image the bar still works, as text. */
    BmLib = IIntuition->OpenClass("images/bitmap.image", 52, &BmClass);
    return TRUE;
}

/* The tags a node shows now, for its current alt/selected/disabled. */
static void set_node(Entry *e)
{
    Object *img = e->alt && e->alt_image ? e->alt_image : e->image;
    const char *text = e->alt ? e->alt_text : e->text;

    ISpeedBar->SetSpeedButtonNodeAttrs(e->node,
        SBNA_Text,     Style == TOOLBAR_ICONS && Icons ? NULL : text,
        SBNA_Image,    Style == TOOLBAR_TEXT || !Icons ? NULL : img,
        SBNA_Help,     text,
        SBNA_Disabled, e->disabled,
        SBNA_Selected, e->selected,
        TAG_END);
}

Object *Toolbar_New(uint32 gadget_id, const ToolbarButton *buttons, int count,
                    struct Screen *scr)
{
    int i;

    if (count > MAX_BUTTONS)
        count = MAX_BUTTONS;
    if (!open_classes())
        return NULL;
    Scr = scr;
    Style = style_setting();
    IExec->NewList(&Buttons);

    Icons = Style != TOOLBAR_TEXT;
    for (i = 0; i < count; i++) {
        Entry *e = &Entries[i];

        memset(e, 0, sizeof(*e));
        e->def = &buttons[i];
        plain_text(e->text, buttons[i].text);
        if (Icons && !(e->image = load_icon(buttons[i].icons)))
            Icons = FALSE;       /* one missing: text for all */
    }
    for (i = 0; i < count; i++) {
        Entry *e = &Entries[i];

        e->node = ISpeedBar->AllocSpeedButtonNode(e->def->id,
            SBNA_Toggle,  e->def->toggle,
            SBNA_Spacing, i > 0 && buttons[i - 1].space_after ? 8 : 0,
            TAG_END);
        if (!e->node)
            break;
        set_node(e);
        IExec->AddTail(&Buttons, e->node);
        Count = i + 1;
    }

    Bar = IIntuition->NewObject(SbClass, NULL,
        GA_ID,               gadget_id,
        GA_RelVerify,        TRUE,
        SPEEDBAR_Buttons,    &Buttons,
        SPEEDBAR_Orientation, SBORIENT_HORIZ,
        SPEEDBAR_ButtonType, !Icons || Style == TOOLBAR_TEXT ? SBTYPE_TEXT
                             : Style == TOOLBAR_ICONS ? SBTYPE_IMAGE
                             : SBTYPE_BOTH,
        SPEEDBAR_EvenSize,   TRUE,
        TAG_END);
    return Bar;
}

uint32 Toolbar_Clicked(void)
{
    uint32 number = (uint32)-1;
    int i;

    if (Bar)
        IIntuition->GetAttr(SPEEDBAR_Selected, Bar, &number);
    /* A toggle button has changed itself; Toolbar_Apply() sets it back to
     * what the timeline says. */
    for (i = 0; i < Count; i++)
        if (Entries[i].def->id == number && Entries[i].def->toggle)
            Entries[i].stale = TRUE;
    return number;
}

void Toolbar_Set(uint32 id, BOOL disabled, BOOL selected,
                 uint32 alt_text, const char *alt_icons)
{
    int i;

    for (i = 0; i < Count; i++) {
        Entry *e = &Entries[i];

        if (e->def->id != id)
            continue;
        e->want_disabled = disabled;
        e->want_selected = selected;
        e->want_alt = alt_text != 0;
        if (alt_text)
            plain_text(e->alt_text, alt_text);
        if (alt_icons && alt_icons != e->alt_icons && Icons) {
            if (e->alt_image)
                IIntuition->DisposeObject(e->alt_image);
            e->alt_image = load_icon(alt_icons);
            e->alt_icons = alt_icons;
        }
        return;
    }
}

void Toolbar_Apply(struct Window *win)
{
    BOOL changed = FALSE;
    int i;

    for (i = 0; i < Count && !changed; i++) {
        Entry *e = &Entries[i];

        changed = e->stale || e->disabled != e->want_disabled ||
                  e->selected != e->want_selected || e->alt != e->want_alt;
    }
    if (!changed || !Bar)
        return;

    IIntuition->SetGadgetAttrs((struct Gadget *)Bar, win, NULL,
                               SPEEDBAR_Buttons, ~0, TAG_END);
    for (i = 0; i < Count; i++) {
        Entry *e = &Entries[i];

        e->disabled = e->want_disabled;
        e->selected = e->want_selected;
        e->alt = e->want_alt;
        e->stale = FALSE;
        set_node(e);
    }
    IIntuition->SetGadgetAttrs((struct Gadget *)Bar, win, NULL,
                               SPEEDBAR_Buttons, &Buttons, TAG_END);
}

void Toolbar_Free(void)
{
    struct Node *n;
    int i;

    if (ISpeedBar && Buttons.lh_Head)
        while ((n = IExec->RemHead(&Buttons)))
            ISpeedBar->FreeSpeedButtonNode(n);
    for (i = 0; i < Count; i++) {
        if (Entries[i].image)
            IIntuition->DisposeObject(Entries[i].image);
        if (Entries[i].alt_image)
            IIntuition->DisposeObject(Entries[i].alt_image);
        memset(&Entries[i], 0, sizeof(Entries[i]));
    }
    Count = 0;
    Bar = NULL;
    if (ISpeedBar) {
        IExec->DropInterface((struct Interface *)ISpeedBar);
        ISpeedBar = NULL;
    }
    if (BmLib)
        IIntuition->CloseClass(BmLib);
    if (SbLib)
        IIntuition->CloseClass(SbLib);
    BmLib = SbLib = NULL;
    BmClass = SbClass = NULL;
}
