#ifndef FORMBROD_HINTS_H
#define FORMBROD_HINTS_H

/*
 * hints.h - hover hints (WINDOW_GadgetHelp, WINDOW_HintInfo) from the
 * catalog.
 *
 * Each window keeps a table of gadget IDs and MSG_HINT_* numbers, and a
 * HintInfo array one longer, which Hints_Fill() fills in before the window
 * object is made. The window needs IDCMP_GADGETHELP, WINDOW_GadgetHelp and
 * WINDOW_HintInfo; window.class shows the bubbles itself.
 */
#include <exec/types.h>
#include <classes/window.h>

#include "locale/catalog.h"

typedef struct {
    int16  gid;       /* the gadget's GA_ID */
    uint32 msg;       /* its MSG_HINT_* text */
} HintMsg;

/* Fills `out` (count + 1 entries) from `msgs`, ending it with -1. */
static inline void Hints_Fill(struct HintInfo *out, const HintMsg *msgs,
                              int count)
{
    int i;

    for (i = 0; i < count; i++) {
        out[i].hi_GadgetID = msgs[i].gid;
        out[i].hi_Code     = -1;
        out[i].hi_Text     = (STRPTR)FB_Str(msgs[i].msg);
        out[i].hi_Flags    = 0;
    }
    out[count].hi_GadgetID = -1;
    out[count].hi_Code     = -1;
    out[count].hi_Text     = NULL;
    out[count].hi_Flags    = 0;
}

#define HINT_COUNT(table) (int)(sizeof(table) / sizeof((table)[0]))

#endif
