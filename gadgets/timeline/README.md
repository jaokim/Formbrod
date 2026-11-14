# Timeline gadgets

The edit is shown as tracks stacked on one time axis: a ruler, a film strip
for the picture, and (later) a waveform for the sound. The full plan is in
the project doc *Filmroll gadget plan*; this file describes what is built.

## Pieces

| Piece | Where | What it does |
|---|---|---|
| `FBTimeView` | `include/formbrod/timeview.h`, `src/timeline/timeview.c` | Portable arithmetic: zoom, scrolling, time ↔ pixels, ruler steps, timecodes, film-strip cells. Tested on the host. |
| `FBTimebase` | `src/gui/timebase.c` | One view plus the playhead, shared by all tracks so they stay in sync. |
| `track` | `src/gui/track.c` | Base gadget class: click or drag to move the playhead, draws the playhead, takes the view's width in `GM_LAYOUT`. |
| `ruler` | `src/gui/track.c` | Subclass: tick marks and timecodes. |
| `lanes` | `src/gui/lanes.c` | Subclass: the track editor. A row per track (picture tracks from the top one down, each with a lane for its clips' sound, then sound tracks), names in the inset, the drop area below. Click selects a clip or (in the names) a track; dragging a clip's end trims it, reported as `LANES_EditClip/In/Out` when released; dragging its middle moves it, with an outline and an insert line while dragging, reported as `LANES_MoveClip/Track/At` (track -1: a new one); elsewhere it moves the playhead. `LANESM_TRACKAT` says which track is under a point, for files dropped on the window. |
| `filmstrip` | `src/gui/track.c` | Subclass: the edit as a film roll. Each cell shows the picture on the highest picture track at that time; sprocket holes; a mark where a picture clip starts. With `FILMSTRIP_Pictures` off: plain blocks with file names. |

Subclasses only implement `TRACKM_DRAWCONTENT` (and `GM_DOMAIN` for their
height). A waveform track, or a lane added by a module, is another
subclass of `TrackClass`.

All track gadgets take `TRACK_Inset`: a column on the left, the same width
for all, that isn't part of the time axis, so they line up. The film strip
shows "Result" there and the lanes show the track names.

## Time and zoom

- Time in the view is in microseconds (`fb_us_t`); the timeline model stays
  in milliseconds.
- Zoom is microseconds per pixel. Most zoomed in, one film-strip cell
  (48 px) is one frame. Most zoomed out, the whole edit fits the width.
  In between are steps of 5/4, used by the zoom slider (and later the
  mouse wheel). The slider is reversed: right zooms in, left shows more.
- Film-strip cells sit on a grid from time 0, so they don't jump while
  scrolling. Each cell shows **the frame from the middle** of the time it
  covers.

## Syncing

Each track redraws itself while it is dragged (in Intuition's input task)
and notifies the window through `ICA_TARGET, ICTARGET_IDCMP`. The window's
`WINDOW_IDCMPHook` then redraws the other tracks and updates the scroller,
zoom slider and playhead time. The scroller and zoom slider work the same
way. A track that should scroll on its own gets its own `FBTimebase`.

## Thumbnails

`FILMSTRIP_ThumbFunc` draws the picture for one cell, given the source and
the time in it. It must not wait: if the picture isn't ready it returns
`FALSE` and a grey placeholder is drawn. Without a function, a dummy
landscape is drawn whose colours follow the source and whose horizon moves
with time, so zooming and scrolling can be tried before thumbnails exist.

## Tags

| Tag | Type | | Notes |
|---|---|---|---|
| `TRACK_Timebase` | `FBTimebase *` | I | Required. |
| `TRACK_Timeline` | `const FBTimeline *` | IS | The gadget only reads it. |
| `TRACK_Playhead` | `uint32` ms | GN | Sent while dragging (`OPUF_INTERIM`) and on release. |
| `TRACK_Redraw` | `BOOL` | S | Redraws now. |
| `TRACK_EmptyText` | `CONST_STRPTR` | IS | Shown when the timeline is empty. |
| `FILMSTRIP_CellWidth` | `uint32` | I | Default 48. |
| `FILMSTRIP_CellHeight` | `uint32` | I | Default 27. |
| `FILMSTRIP_ThumbFunc` | `FBThumbFunc` | IS | See above. |
| `FILMSTRIP_ThumbData` | `APTR` | IS | Passed to the function. |
| `FILMSTRIP_Pictures` | `BOOL` | IS | Default `TRUE`. `FALSE` draws each clip as a block in its file's colour with the file name, and never calls the thumbnail function. The window sets it from the `ShowPictures` setting. |

The gadgets never change the timeline. Edits (trim, move) will be reported
to the window and applied through `App_*` calls, so ARexx and the GUI keep
going through the same code.

## Next steps

1. Real thumbnails from libav, cached on disk (or made by a render server).
2. A waveform subclass, and a clip group (header, picture, sound) per file.
3. Several groups on one timebase, with offsets and a sync lock.
4. Selecting clips, trim handles, mouse-wheel zoom, ARexx `ZOOM`/`GOTO`.
5. Drawing into an offscreen bitmap and scrolling it, instead of drawing
   everything again.

Known limits: colours use RGB pens, so they need a screen with more than
256 colours; the shared view isn't locked between the input task and the
program, which is fine as long as only one thing changes it at a time.
