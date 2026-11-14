/*
 * track.h - the timeline track gadgets: a base class and its subclasses.
 *
 *   track      base class (from gadgetclass). Converts between time and
 *              pixels through the shared FBTimebase, sets the playhead
 *              when clicked or dragged, and draws the playhead. Subclasses
 *              only draw their content, in TRACKM_DRAWCONTENT.
 *   ruler      timecodes and tick marks.
 *   filmstrip  the edit as a film strip: per cell, the picture on the
 *              highest picture track, plus sprocket holes and a mark where
 *              a clip starts. Thumbnails come from FILMSTRIP_ThumbFunc;
 *              until there is one (or it has nothing yet), a placeholder
 *              frame is drawn. With FILMSTRIP_Pictures off, each clip is a
 *              plain block in its file's colour with the file name.
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
#define TRACK_Inset          (TRACK_Dummy + 6)  /* uint32: pixels on the left
                                                   for names, not part of the
                                                   time axis; give every track
                                                   the same              (I)   */
#define TRACK_Label          (TRACK_Dummy + 7)  /* CONST_STRPTR shown in the
                                                   inset                 (IS)  */

#define FILMSTRIP_CellWidth  (TRACK_Dummy + 101) /* uint32, default 48   (I)  */
#define FILMSTRIP_CellHeight (TRACK_Dummy + 102) /* uint32, default 27   (I)  */
#define FILMSTRIP_ThumbFunc  (TRACK_Dummy + 103) /* FBThumbFunc          (IS) */
#define FILMSTRIP_ThumbData  (TRACK_Dummy + 104) /* APTR, passed to it   (IS) */
#define FILMSTRIP_Pictures   (TRACK_Dummy + 105) /* BOOL, default TRUE   (IS)
                                                    FALSE: plain blocks with
                                                    file names, nothing is
                                                    decoded (slow Amigas)    */

/* lanes: the track editor (lanes.c). Also takes TRACK_Inset; its name
 * column shows the track names. */
#define LANES_Selected       (TRACK_Dummy + 201) /* int32 clip, -1   (ISGN) */
#define LANES_Track          (TRACK_Dummy + 202) /* int32 track, -1  (ISGN) */
#define LANES_EditClip       (TRACK_Dummy + 203) /* N: an edge was dragged:  */
#define LANES_EditIn         (TRACK_Dummy + 204) /*    the clip, and its new */
#define LANES_EditOut        (TRACK_Dummy + 205) /*    in and out, ms        */
#define LANES_DropText       (TRACK_Dummy + 206) /* CONST_STRPTR      (I)    */
#define LANES_PictureName    (TRACK_Dummy + 207) /* "Video %ld"       (I)    */
#define LANES_SoundName      (TRACK_Dummy + 208) /* "Audio %ld"       (I)    */

/* Which track is at a point in the window (for files dropped on it):
 * returns the track, -1 for the drop area below the tracks, or -2 if the
 * point isn't on the gadget. */
#define LANESM_TRACKAT       (TRACK_Dummy + 0x201)

struct lanesTrackAt {
    uint32 MethodID;
    int32  lta_X, lta_Y;       /* window coordinates */
};

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
    struct IBox         trd_Box;       /* the time axis part of the gadget,
                                          in window coordinates */
    int                 trd_Inset;     /* the name column left of trd_Box */
    struct FBTimebase  *trd_Timebase;
    const FBTimeline   *trd_Timeline;
};

extern Class *TrackClass, *RulerClass, *FilmstripClass, *LanesClass;

/* Drawing helpers for the subclasses (here and in lanes.c): an RGB pen
 * (0xRRGGBB), a rectangle cut to `clip`, and up to `room` pixels of text
 * with its top-left corner at x, y. */
void Track_SetRGB(struct RastPort *rp, uint32 rgb);
void Track_FillClipped(struct RastPort *rp, const struct Rectangle *clip,
                       long x0, long y0, long x1, long y1);
void Track_Text(struct RastPort *rp, struct TextFont *font,
                long x, long y, long room, const char *text);

/* lanes.c: makes the lanes class on top of TrackClass. */
Class *Lanes_MakeClass(Class *track_class);

/* Makes the classes. Returns FALSE (and frees what was made) on failure. */
BOOL Track_MakeClasses(void);
void Track_FreeClasses(void);

#endif
