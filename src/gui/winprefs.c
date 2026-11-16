/*
 * winprefs.c - where windows were (see winprefs.h).
 */
#include <stdio.h>

#include <proto/intuition.h>

#include "formbrod/log.h"
#include "prefs/prefs.h"
#include "gui/winprefs.h"

BOOL WinPrefs_Tags(const char *key, struct Screen *scr,
                   struct TagItem *tags)
{
    int l, t, w, h;

    tags[0].ti_Tag = TAG_END;
    if (sscanf(Prefs_GetString(key, ""), "%d %d %d %d", &l, &t, &w, &h) != 4) {
        FB_TRACE("gui", "%s: no saved place", key);
        return FALSE;
    }
    /* A screen smaller than when it was saved: let it be placed anew. */
    if (w < 40 || h < 20 || l < 0 || t < 0 ||
        (scr && (l + w > scr->Width || t + h > scr->Height))) {
        FB_TRACE("gui", "%s: saved place %d,%d %dx%d doesn't fit the "
                 "screen (%dx%d): placed anew", key, l, t, w, h,
                 scr ? scr->Width : -1, scr ? scr->Height : -1);
        return FALSE;
    }
    FB_TRACE("gui", "%s: saved place %d,%d %dx%d", key, l, t, w, h);
    tags[0].ti_Tag = WA_Left;   tags[0].ti_Data = l;
    tags[1].ti_Tag = WA_Top;    tags[1].ti_Data = t;
    tags[2].ti_Tag = WA_Width;  tags[2].ti_Data = w;
    tags[3].ti_Tag = WA_Height; tags[3].ti_Data = h;
    tags[4].ti_Tag = TAG_END;
    return TRUE;
}

void WinPrefs_Save(const char *key, struct Window *win)
{
    char v[48];

    if (!win)
        return;
    snprintf(v, sizeof(v), "%d %d %d %d", win->LeftEdge, win->TopEdge,
             win->Width, win->Height);
    Prefs_SetString(key, v);
    Prefs_Save();
}
