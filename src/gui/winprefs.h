/*
 * winprefs.h - where windows were, remembered in the settings.
 *
 * Each window has a key ("MainWindow", "ClipWindow", "PreviewWindow")
 * holding "left top width height" (the outer size). WinPrefs_Tags fills
 * in WA_Left/Top/Width/Height for window.class when the setting is there
 * and still fits on the screen, else the window opens where its
 * WINDOW_Position puts it. WinPrefs_Save stores where it is now; call it
 * before the window closes.
 */
#ifndef FORMBROD_WINPREFS_H
#define FORMBROD_WINPREFS_H

#include <exec/types.h>
#include <intuition/intuition.h>
#include <utility/tagitem.h>

/* Fills `tags` (room for 5) and returns TRUE if the window has a place;
 * the last tag is TAG_END either way. */
BOOL WinPrefs_Tags(const char *key, struct Screen *scr,
                   struct TagItem *tags);
void WinPrefs_Save(const char *key, struct Window *win);

#endif
