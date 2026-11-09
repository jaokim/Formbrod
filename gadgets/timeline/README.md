# timeline.gadget (planned)

A custom ReAction gadget that draws the edit as a horizontal strip of clips
and replaces the ListBrowser in the main window.

## What it should do

- Draw each clip as a box whose width matches its length, labelled with the
  file name. Thumbnails and audio waveforms come later; they can be made by a
  render server (see the remote rendering plan).
- Show a playhead you can drag. Clicking a clip selects it.
- Drag a clip's edge to trim it, and drag a clip to reorder it.
- Mark effects on a clip, such as a small ramp for a fade.

## Interface sketch

Attributes (tags), set with `SetGadgetAttrs()`:

| Tag                   | Type           | Notes                               |
|-----------------------|----------------|-------------------------------------|
| `TIMELINE_Timeline`   | `FBTimeline *` | The model to draw. The gadget only reads it. |
| `TIMELINE_Playhead`   | `fb_ms_t`      | Current position. Notifies on drag. |
| `TIMELINE_Selected`   | `int32`        | Selected clip, -1 for none.         |
| `TIMELINE_Zoom`       | `uint32`       | Milliseconds per pixel.             |
| `TIMELINE_Edit`       | (get only)     | The last trim or move, so the app can apply it through `App_*` calls. |

The gadget never changes the timeline itself. It reports what the user did,
and the app applies it, so ARexx and the GUI keep going through the same
code.

Build it as a BOOPSI class inside the executable first, and split it out
into `timeline.gadget` once the interface has settled.
