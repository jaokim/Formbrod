/*
 * xfadekinds.h - the crossfade kinds offered in the GUI (the Edit menu and
 * the clip window): ffmpeg xfade names with their labels. ARexx and project
 * files can use any other of ffmpeg's names.
 */
#ifndef FORMBROD_XFADEKINDS_H
#define FORMBROD_XFADEKINDS_H

#include "locale/catalog.h"

typedef struct XfadeKind {
    const char *name;
    long        msg;
} XfadeKind;

static const XfadeKind XfadeKinds[] = {
    { "fade",       MSG_XFADE_FADE },
    { "fadeblack",  MSG_XFADE_FADEBLACK },
    { "dissolve",   MSG_XFADE_DISSOLVE },
    { "wipeleft",   MSG_XFADE_WIPELEFT },
    { "wiperight",  MSG_XFADE_WIPERIGHT },
    { "slideleft",  MSG_XFADE_SLIDELEFT },
    { "circleopen", MSG_XFADE_CIRCLEOPEN },
    { "pixelize",   MSG_XFADE_PIXELIZE },
};
#define NUM_XFADE_KINDS ((int)(sizeof(XfadeKinds) / sizeof(XfadeKinds[0])))

/* Which of them `name` is, or -1. */
static inline int xfade_kind_index(const char *name)
{
    int i;

    for (i = 0; i < NUM_XFADE_KINDS; i++)
        if (strcmp(XfadeKinds[i].name, name) == 0)
            return i;
    return -1;
}

#endif
