/*
 * track.h - the timeline track gadgets: a base class and its subclasses.
 *
 *   track      base class (from gadgetclass). Converts between time and
 *              pixels through the shared FBTimebase, sets the playhead
 *              when clicked or dragged, and draws the playhead. Subclasses
 *              only draw their content, in TRACKM_DRAWCONTENT.
 *   ruler      timecodes and tick marks.
 *   filmstrip  the clips as a film strip: one thumbnail per cell, sprocket
 *              holes, and a mark where one clip ends and the next starts.
 *              Thumbnails come from FILMSTRIP_ThumbFunc; until there is
 *              one (or it has nothing yet), a placeholder frame is drawn.
 *
 * Later: a waveform subclass for sound, and effect/title lanes. A module
 * that adds a kind of lane subclasses TrackClass the same way.
 *
 * The classes are private to the program for now. Once the interface has
 * settled they move into gadgets/timeline/ as a shared gadget.
 */
#ifndef FORMBROD_TRACK_H
#define FORMBROD_TRACK_H

#include <exec/types.h>
#include <intuition/classes.h>
#include <utility/tagitem.h>
#include "formbrod/timeview.h"

struct FBTimebase;

/* Attributes. I = at creation, S = SetAttrs, G = GetAttr, N = notifies. */
#define TRACK_Dummy          (TAG_USER + 0x46420000)
#define TRACK_Timebase       (TRACK_Dummy + 1)  /* struct FBTimebase *  (I)   */
#define TRACK_Timeline       (TRACK_Dummy + 2)  /* const FBTimeline *   (IS)  */
#define TRACK_Playhead       (TRACK_Dummy + 3)  /* uint32 ms            (GN)  */
#define TRACK_Redraw         (TRACK_Dummy + 4)  /* BOOL, redraws now    (S)   */
#define TRACK_EmptyText      (TRACK_Dummy + 5)  /* CONST_STRPTR shown when
                                                   the timeline is empty (IS) */

#define FILMSTRIP_CellWidth  (TRACK_Dummy + 101) /* uint32, default 48   (I)  */
#define FILMSTRIP_CellHeight (TRACK_Dummy + 102) /* uint32, default 27   (I)  */
#define FILMSTRIP_ThumbFunc  (TRACK_Dummy + 103) /* FBThumbFunc          (IS) */
#define FILMSTRIP_ThumbData  (TRACK_Dummy + 104) /* APTR, passed to it   (IS) */

#define FB_CELL_W_DEFAULT 48
#define FB_CELL_H_DEFAULT 27

/* Draws the thumbnail for one cell: the frame at `source_t` in source
 * `source`, scaled into `cell`, drawing only inside `clip` (the cell may
 * be cut off at the edges). Returns FALSE if it has no picture yet; the
 * film strip then draws a placeholder. Called from GM_RENDER, which may
 * run in Intuition's input task: it must not wait for the picture. */
typedef struct FBThumbRequest {
    int               source;
    fb_us_t           source_t;
    struct RastPort  *rp;
    struct IBox       cell;
    struct Rectangle  clip;
} FBThumbRequest;

typedef BOOL (*FBThumbFunc)(APTR data, const FBThumbRequest *req);

/* Method subclasses implement to draw their content. The base class has
 * already filled `box` with the background and draws the playhead on top
 * afterwards. */
#define TRACKM_DRAWCONTENT   (TRACK_Dummy + 0x200)

struct trackDraw {
    uint32              MethodID;
    struct RastPort    *trd_RPort;
    struct DrawInfo    *trd_DrInfo;
    struct IBox         trd_Box;       /* the gadget, in window coordinates */
    struct FBTimebase  *trd_Timebase;
    const FBTimeline   *trd_Timeline;
};

extern Class *TrackClass, *RulerClass, *FilmstripClass;

/* Makes the classes. Returns FALSE (and frees what was made) on failure. */
BOOL Track_MakeClasses(void);
void Track_FreeClasses(void);

#endif
