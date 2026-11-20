#ifndef FORMBROD_TOOLBAR_H
#define FORMBROD_TOOLBAR_H

/*
 * toolbar.h - the button row as a speedbar.gadget with AISS icons.
 *
 * Each button's number is the command ID that the menus and handle_gadget()
 * already use, so a click is handled exactly like the menu item.
 */
#include <exec/types.h>
#include <intuition/classusr.h>
#include <intuition/screens.h>
#include <utility/tagitem.h>

/* One button. Filled in by the main window, which owns the command IDs. */
typedef struct {
    uint32      id;          /* command ID (a GID_*), also the node number */
    uint32      text;        /* MSG_BTN_* label; '_' is dropped */
    uint32      help;        /* MSG_HELP_* hint shown on hover */
    const char *icons;       /* AISS names to try, '|'-separated: "cut|scissors" */
    BOOL        toggle;      /* stays down while selected (Crossfade) */
    BOOL        space_after; /* a gap after it, to group the buttons */
} ToolbarButton;

/* How buttons look. "ToolbarStyle" in the settings: both, icons or text. */
typedef enum { TOOLBAR_BOTH, TOOLBAR_ICONS, TOOLBAR_TEXT } ToolbarStyle;

/* Opens the classes and makes the gadget. `buttons` must stay valid while
 * the toolbar exists. Returns NULL on failure. Icons are loaded for `scr`;
 * without TBImages: or with an icon missing, the bar shows text. */
Object *Toolbar_New(uint32 gadget_id, const ToolbarButton *buttons, int count,
                    struct Screen *scr);

/* A click comes two ways, in either order: the bar's notification (its
 * tags from IDCMP_IDCMPUPDATE, GA_ID the bar's) and WMHI_GADGETUP's code.
 * Hand both here as they come; after the window's events, Clicked gives
 * the button number of a click not yet handled, or ~0, once. */
void   Toolbar_Noted(struct TagItem *tags);
void   Toolbar_Code(uint16 code);
uint32 Toolbar_Clicked(void);

/* Changes a button. Nothing is redrawn until Toolbar_Apply(), and only if
 * something changed. `alt_text`/`alt_help`/`alt_icons` replace the label,
 * hint and icon (Export turning into Cancel); pass 0/0/NULL for the normal
 * ones. */
void Toolbar_Set(uint32 id, BOOL disabled, BOOL selected,
                 uint32 alt_text, uint32 alt_help, const char *alt_icons);
void Toolbar_Apply(struct Window *win);

/* After the window object is disposed: frees nodes and images. */
void Toolbar_Free(void);

#endif
