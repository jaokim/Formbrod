# Formbröd

A video editor for AmigaOS 4, built with ReAction and ffmpeg.

The aim is something in the spirit of the Video Toaster, reached in small
steps. The first version just trims, splits and joins video files and adds
simple fades. It should be:

1. **Easy to use.** Drop in a video file, and what you can do with it should
   be obvious from the window.
2. **Easy to extend,** through ARexx scripts and effect modules.
3. **Built on ffmpeg,** linked in as libraries behind a clean interface.
4. **Native ReAction,** with a custom timeline gadget.

## Status

Early skeleton. Working so far:

- [x] Timeline model: sources, clips, split, remove and effects
- [x] Render jobs: the timeline is turned into one ffmpeg filtergraph, with a
      text format for saving or sending jobs
- [x] Module interface, plus a `fade` module
- [x] Clips of different size, shape, frame rate and audio can be joined:
      each is fitted to the output frame (letterbox, crop or stretch)
- [x] ReAction main window: clip list, playhead, render progress, menus
      (with About, Quit, effects and the default fit mode), drag and
      drop
- [x] Projects saved and opened as hand-editable text files; file
      requesters remember their drawers
- [x] ARexx port
- [x] Translatable GUI through locale.library catalogs (English, Swedish)
- [x] Local rendering with libav* (`src/backend/local_backend.c`): decode,
      filter, encode and mux, a few packets per GUI tick. Tested on Linux
      with ffmpeg 6.1, not yet on AmigaOS
- [x] Timeline gadgets, first step (`gadgets/timeline/`): ruler and film
      strip on a shared time axis, zoom, scrolling and a draggable playhead
- [x] Tracks: each added file gets its own track; picture tracks are laid
      on top of each other and all sound is mixed. Cutting closes gaps
- [x] Set in / Set out at the playhead (buttons, Edit menu, I and O keys),
      typed In / Out / Length for the selected clip, and still pictures
      (PNG, JPEG...) as clips of any length
- [x] Track editor under the film strip: clips you select and trim by
      dragging their ends, track names and a Track menu, drops onto tracks
- [x] Moving clips by dragging them, within a track, to another track or
      to a new one
- [x] Preview copies (proxies): each video gets a small MJPEG copy in
      the background, once per file, and previews are rendered from them
- [x] Playing from the playhead in a preview window, with sound (AHI)
- [x] The preview window shows the frame at the playhead while it isn't
      playing; frames can be saved as pictures for paint programs
      (Project > Save frame, ARexx `GRAB`). Not yet tried on AmigaOS
- [x] A range marked with two flags in the overview, exported small as
      an IFF ANIM or a movie (Project > Export range, ARexx
      `EXPORTRANGE`). Not yet tried on AmigaOS
- [x] Crossfades between clips on a track: the clips overlap, drawn with
      an X over them; picture with ffmpeg's `xfade` (fade, wipes, slides, circle...),
      sound with `acrossfade`. Not yet tried on AmigaOS
- [x] Clip window: double-click a clip (or Edit > Clip settings) for
      all its settings: timing, crossfade, fades, fit, volume, the file's
      picture, and its effects with a field per module parameter. It
      follows the selection. Not yet tried on AmigaOS
- [x] Real thumbnails in the film strip: a small strip of pictures per
      file, one a second, made in the background (from the preview copy)
      and kept in the cache drawer. Not yet tried on AmigaOS
- [x] Waveforms on the sound lanes: the loudest sound of every 20 ms,
      made in the background like the thumbnails. Not yet tried on AmigaOS
- [ ] Remote rendering on a render server

## Editing

- **Add file** puts a file on a new track of its own: a video, a sound
  file (music) or a still picture. A picture starts out 5 seconds long.
- Under the film strip (which shows the result) is the **track editor**:
  one row per track, named *Video 1*, *Audio 1* and so on, with each
  video track's sound on a lane under it. Higher picture tracks are
  drawn on top of lower ones; all sound is mixed.
- Click a clip to select it; drag either end of a clip to make it shorter
  or longer. The clips after it on the track move along.
- Drag the middle of a clip to move it. Its old place closes up straight
  away, and an outline shows where it will land: exactly where you drop
  it if there's room (after the last clip, or in a gap wide enough),
  leaving a gap in front of it; on top of other clips it goes in between
  them instead, at the orange line, and they make room. Its ends snap to
  the ends of other clips, the start and the playhead when close. Drop it
  below the tracks to give it a track of its own. A video clip only goes
  on video tracks and an audio clip on audio tracks (the outline turns
  grey). The right mouse button cancels.
- Click a track's name to select the track for the **Track** menu:
  *Append file...* (adds a file after its last clip), *Mute audio*,
  *Hide video* and *Remove track*.
- Drop files from Workbench on a track to append them to it, or on the
  empty space below the tracks to give each its own track.
- Click or drag in the ruler, the film strip or an empty part of the track
  editor to move the playhead.
- **Set in** (or the I key) cuts away the start of a clip up to the
  playhead; **Set out** (O) cuts away the end after it. They work on the
  clip selected in the list, or else on the picture under the playhead.
  The clips after it on the same track move along, so no gap is left.
- Put the playhead *after* the end of a clip and choose **Set out** to make
  it longer, as far as its file goes; a still picture can be any length,
  and so can an animation (animated GIF or APNG), which loops.
  The timeline goes on a little past its last clip for this.
- Or type the times: under the playhead and zoom controls, the selected
  clip shows where it **Starts** in the edit, where in its file it begins
  (**From file**) and its **Length**. Change any of them by writing
  `1:05.5` or plain seconds like `12.5`, then press Return. Changing
  Starts moves the clip and the ones after it on its track; starting it
  later than the clip in front of it ends leaves a gap (black, or clear on
  an upper track, and silence). A still picture has no From file.
- **Split** cuts the selected clip's track at the playhead, or all tracks
  if no clip is selected.

## Building

Cross-compile on Linux with the AmigaOS 4 SDK (for example in the
`walkero/amigagccondocker:os4-gcc11` Docker image):

    sudo apt install lhasa curl     # if lha/lhasa or curl are missing
    make deps                       # download third-party libraries into deps/
    make CC=ppc-amigaos-gcc         # links ffmpeg, since deps/ffmpeg now exists
    make config                     # shows whether ffmpeg will be linked

Without ffmpeg the program builds and runs, but can't read video:

    make CC=ppc-amigaos-gcc LIBAV=0

To use ffmpeg libraries from somewhere else:

    make CC=ppc-amigaos-gcc LIBAV=1 LIBAV_PREFIX=/path/to/ffmpeg-ppc

### Dependencies

Third-party libraries are listed in `tools/deps.list` and fetched by
`tools/fetch-deps.sh` (`make deps` runs it). Each one is unpacked into
`deps/<name>/include` and `deps/<name>/lib`. `deps/` is not committed.

    tools/fetch-deps.sh             # everything in the list
    tools/fetch-deps.sh ffmpeg      # just one
    tools/fetch-deps.sh -f ffmpeg   # unpack again
    tools/fetch-deps.sh -l          # what's listed and what's installed

Downloads are cached in `deps/cache/`. If a site can't be reached, download
the archive yourself and put it there under the name from the URL.

Archives often hold builds for more than one C runtime. Libraries must match
the program's runtime, and Formbrod builds with newlib, so newlib builds are
picked. `DEPS_CRT=clib4` prefers clib4 builds instead.

To add a dependency, add a line to `tools/deps.list` (the format is described
at the top of the file), run `tools/fetch-deps.sh <name>`, and add its
`-I`/`-L`/`-l` flags to the Makefile. Once you trust a download, pin its
checksum in the list. The script prints the checksum on every run.

Currently:

| Name | What | Source |
|---|---|---|
| `ffmpeg` | ffmpeg 8.0 libraries (libavformat, libavcodec, libavfilter, ...) by Michael Trebilcock | OS4Depot `development/misc/libffmpeg.lha` |

To run the core tests on Linux or macOS:

    make -f Makefile.host test

With the ffmpeg development files and the `ffmpeg`/`ffprobe` tools
installed, this renders a few small jobs end to end through the local
backend:

    make -f Makefile.host test-libav

Translations are built with FlexCat (and iconv, since the sources are
UTF-8). Without FlexCat, `tools/catgen.py` can build them instead (Python 3):

    make catalogs                  # Catalogs/<language>/formbrod.catalog
    make catalogs CATTOOL=catgen   # same, without FlexCat
    make compare-catalogs          # build with both and check they agree

CI builds the catalogs with FlexCat, cross-checks them with catgen, and
uploads them together with the executable.

Copy the `Catalogs` drawer next to the program, or into `LOCALE:Catalogs/`.
Formbröd then follows the language set in the Locale preferences.

To pack a release:

    make release                   # Formbrod.lha

The archive holds everything under `release/` (the `Formbrod` drawer and
its icons), the program as `Formbrod/Formbrod` and the catalogs in
`Formbrod/Catalogs/`. Files added to `release/` later go in too. It needs
an lha that can create archives (lha for Unix, or LhA on AmigaOS); lhasa
only unpacks.

To install from the archive, unpack it anywhere and run
`Formbrod/Install-Formbrod` with Installer. It asks where to put Formbröd
and copies the drawer there, everything except the installer included.

`release/Formbrod/Formbrod.guide` is the user manual (AmigaGuide) and
`Formbrod.readme` the text for OS4Depot. Both, like the Installer
script, are ISO-8859-1; keep them that way when editing.

## Translating

The GUI text lives in `locale/formbrod.cd` (English, built in) and one
`.ct` file per language in `locale/` (`svenska.ct` is the example). To add a
language, copy `svenska.ct` to `<language>.ct`, using the language's
name as it appears in `LOCALE:Languages/`, and translate each line under its
`MSG_` name. Then run:

    python3 tools/catgen.py check locale/formbrod.cd locale/<language>.ct
    make catalogs

The checker reports missing strings (they stay in English), `%s`
conversions that don't match the original, lost `_` shortcuts, and
characters that ISO-8859-1 can't hold. The files are UTF-8; `catgen`
converts them for AmigaOS.

To add a string to the program, add it to `formbrod.cd` with a new ID, run
`make strings` to regenerate `src/locale/strings.h`, and use
`FB_Str(MSG_...)` in the code. ARexx commands and results are never
translated.

## ARexx

The port is called `FORMBROD`.

    /* Join two files, fade in the first, and export */
    ADDRESS FORMBROD
    'OPEN "Work:Video/intro.mp4"'
    track = RESULT
    'APPEND' track '"Work:Video/main.mp4"'
    'EFFECT 0 fade'
    fx = RESULT
    'SETPARAM 0' fx 'in 2.0'
    'EXPORT "Work:Video/final.mp4"'
    DO UNTIL LEFT(RESULT, 7) ~= 'RUNNING'
        'RENDERSTATUS'
        ADDRESS COMMAND 'Wait 1'
    END

| Command | Arguments | Result |
|---|---|---|
| `OPEN` | `FILE/A` | the new track |
| `APPEND` | `TRACK/N/A,FILE/A` | |
| `TRACKS` | | number of tracks |
| `SPLIT` | `AT/N/A` (ms), `TRACK/N` (default: all tracks) | |
| `TRIM` | `CLIP/N/A,IN/N,OUT/N` (ms in the clip's file; left out = unchanged) | |
| `MOVE` | `CLIP/N/A,AT/N/A,TRACK/N`: without `TRACK`, start the clip at `AT` ms (the clips after it move along); with `TRACK`, move it there like a drag: at `AT` if there's room, else in between that track's clips (`TRACK` = the number of tracks makes a new one) | the clip's new number, with `TRACK` |
| `CROSSFADE` | `CLIP/N/A,LENGTH/N/A,KIND/K`: crossfade `LENGTH` ms from the clip before it on its track (0 = a plain cut); a gap between them closes first, then the clips overlap, so it and the ones after it move earlier. `KIND` is an ffmpeg `xfade` transition (`fade`, `fadeblack`, `wipeleft`, `slideleft`, `circleopen`, ...) | the length set (shortened if it didn't fit) |
| `RENDERAHEAD` | `ON/S,OFF/S`: background rendering for previews | `ON\|OFF <ready> <pieces>` |
| `PLAY` | `AT/N,STOP/S`: play in the preview window from `AT` ms to the end, or stop | |
| `PREVIEW` | `AT/N,STOP/S`: preview from `AT` ms in 10 s windows until stopped | the playlist's path |
| `PLAYHEAD` | `AT/N`: move the playhead to `AT` ms first, if given | where the playhead is (ms) |
| `GRAB` | `FILE/A,AT/N,FULL/S`: save the frame at the playhead (or at `AT` ms) as a picture: a PNG if `FILE` ends in `.png`, else a 24-bit IFF ILBM. Preview size (320 wide, quick), or with `FULL` the edit's frame size from the original files | the frame's time (ms) |
| `RANGE` | `FROM/N,TO/N,CLEAR/S`: move the marks of the range in the overview (`TO 0` = the end of the edit), or `CLEAR` for the whole edit | `<from> <to>` (ms) |
| `EXPORTRANGE` | `FILE/A,WIDTH/N,FPS/N`: render the range small, from the preview copies: an IFF ANIM (256 colours, no sound), or a movie if `FILE` ends in `.mp4`, `.mov`, `.avi`, `.mkv`, `.gif`... `WIDTH` default 320; `FPS` default 10 for an ANIM, else the edit's. Runs like `EXPORT`: poll `RENDERSTATUS` | |
| `CACHE` | `ON/S,OFF/S,WIDTH/N,AUTO/S` (preview copies) | `ON\|OFF <width> <waiting>` |
| `REMOVE` | `CLIP/N/A` | |
| `EFFECT` | `CLIP/N/A,MODULE/A` | effect index |
| `SETPARAM` | `CLIP/N/A,EFFECT/N/A,NAME/A,VALUE/A` | |
| `REMOVEEFFECT` | `CLIP/N/A,EFFECT/N/A` | |
| `FADES` | `CLIP/N/A,IN/N,OUT/N`: fade in and out in ms (0 = none, left out = unchanged); kept within the clip | `<in> <out>` |
| `VOLUME` | `CLIP/N/A,PERCENT/N/A`: 100 = as recorded, up to 400 | |
| `EXPORT` | `FILE/A,PREVIEW/S` | |
| `CANCEL` | | |
| `RENDERSTATUS` | | `IDLE`, `RUNNING <pos> <total>`, `DONE`, `FAILED`, `CANCELLED` |
| `CLIPS` | | number of clips |
| `MODULES` | | module names |
| `FIT` | `CLIP/N/A,MODE/A` | |
| `FORMAT` | `WIDTH/N,HEIGHT/N,FPS/K,FIT/K,PADCOLOR/K` | `<w> <h> <fps> <fit>` |
| `EXPORTSETTINGS` | `VIDEO/K,QUALITY/K,SOUND/K,BITRATE/N,RATE/N,TYPE/K` | `<video> <quality> <sound> <kbit/s> <Hz> <suffix>` |
| `NEWPROJECT` | | |
| `OPENPROJECT` | `FILE/A` | |
| `SAVEPROJECT` | `FILE` (default: the current file) | path saved to |
| `QUIT` | | |

`GRAB` is meant for paint programs: save the frame where an overlay
should go, draw on it as a stencil, and the picture lines up with the
film.

    /* The frame at the playhead, full size, for a paint program */
    ADDRESS FORMBROD
    'GRAB "RAM:stencil.iff" FULL'
    SAY 'Frame at' RESULT 'ms'

For effects drawn frame by frame, mark the stretch in the overview and
load it into the paint program as an ANIM:

    /* 2 to 6 s of the edit, 160 wide, as an ANIM */
    ADDRESS FORMBROD
    'RANGE FROM 2000 TO 6000'
    'EXPORTRANGE "RAM:stencil.anim" WIDTH 160'
    DO UNTIL LEFT(RESULT, 7) ~= 'RUNNING'
        'RENDERSTATUS'
        ADDRESS COMMAND 'Wait 1'
    END

`OPEN` puts each file on a new track of its own, so two files opened
that way play at the same time (the later one on top). `APPEND` joins a
file onto the end of a track. A file with only sound, like music, gets a
sound track.

Clip, track and effect numbers start at 0. `NEWPROJECT`, `OPENPROJECT` and `QUIT`
don't ask about unsaved changes; a script can `SAVEPROJECT` first.

### Output format and fitting

Clips of different sizes are fitted into one output frame. By default the
frame size and frame rate are the first clip's, and clips of another shape
are scaled to fit with black bars. `FORMAT` changes this for the whole
project; `0` (or `AUTO` for `FPS`) goes back to the first clip's value.
`FIT` overrides the mode for one clip. Modes are `FIT` (bars), `FILL` (crop
the edges), `STRETCH` (ignore aspect) and, for `FIT` only, `DEFAULT` (use
the project's).

    'FORMAT WIDTH 1280 HEIGHT 720 FPS 25 FIT FIT PADCOLOR 202020'
    'FIT 2 FILL'        /* crop clip 2 instead of adding bars */

`EXPORTSETTINGS` sets how the project is exported, like *Project →
Project settings* (below): encoders by ffmpeg's names, a quality of `LOW`,
`MEDIUM`, `HIGH` or `BEST`, the sound's bit rate in kbit/s and its rate in
Hz, and the file suffix. A bad value changes none of them.

    'EXPORTSETTINGS VIDEO libx264 QUALITY BEST SOUND aac BITRATE 192 TYPE mkv'

All audio is resampled to the export's rate (48 kHz unless set) in stereo, and clips without audio get
silence, so files with and without sound can be joined. The clip list shows
how each clip is fitted in its Effects column.

## Project settings

*Project → Project settings* opens a window with everything about the
project as a whole. Every change is kept in the project file.

- **Frame**: width and height (left empty: the first clip's), frame rate
  (or the first clip's), how clips of another shape are fitted, and the
  colour of the bars, as `RRGGBB`.
- **Export file**: the file type. The export requester suggests the
  project's name with that suffix; the name typed there decides in the
  end, as ffmpeg picks the container from it.
- **Picture**: the encoder (MPEG-4 Part 2, H.264 or Motion JPEG) and the
  quality: Low, Medium, High (what export always used) or Best. That
  means a quantiser of 6/4/3/2 for MPEG-4 and Motion JPEG, a crf of
  28/24/20/17 for H.264, and a bit rate for other encoders.
- **Sound**: the encoder (AAC, MP3, MP2, FLAC or uncompressed PCM), the
  bit rate (96 to 320 kbit/s; not for FLAC and PCM) and the sample rate
  (44.1 or 48 kHz).

An encoder this build of ffmpeg lacks is marked *not in this ffmpeg*; and
when the file type can't hold an encoder (PCM in MP4, say), export uses one
it can. Previews keep their own quick settings. The defaults are MPEG-4
Part 2 at High with AAC at 160 kbit/s and 48 kHz, in an `.mp4`, as before.

## Project files

*Project → Save project* writes the timeline to a text file that can be
opened again with *Open project*, given on the command line, or dropped on
the window. It is plain text so it can be fixed by hand, for example when
the video files have moved:

    FORMBROD-PROJECT 2
    ; Formbrod project. One setting per line; ';' starts a comment.
    OUTPUT-SIZE 0 0
    OUTPUT-RATE 0 0
    OUTPUT-FIT fit
    PAD-COLOR 000000
    EXPORT-VIDEO mpeg4 high
    EXPORT-SOUND aac 160 48000
    EXPORT-TYPE mp4

    SOURCE 1 clips/intro.mp4
    SOURCE 2 Video:Holiday 2026/beach.mov
    SOURCE 3 music/theme.mp3

    TRACK video
    CLIP 1 1000 5000
    CLIP 2 0 20000
        FIT fill
        EFFECT fade
            PARAM in 0.5
    CLIP 1 8000 12000
        CROSSFADE 1000 wipeleft

    TRACK audio
    CLIP 3 0 60000
        AT 2000
        VOLUME 0.5

- `SOURCE <n> <path>` lists the files, numbered from 1. The path is the rest
  of the line, spaces and all. A path without a volume name (no `:`) is
  relative to the drawer the project file is in; files inside that drawer
  are saved that way, so a project drawer can be moved as a whole.
- `TRACK video` or `TRACK audio` starts a track, the bottom one first;
  higher picture tracks are drawn on top. `MUTED` or `HIDDEN` under it
  turns off its sound or picture.
- `CLIP <source> <in> <out>` takes a part of a source (times in ms) and
  puts it on the track above, right after the clip before it, so changing
  a length by hand moves the rest along. `AT <ms>` starts it later instead,
  leaving a gap; `CROSSFADE <ms> [<kind>]` mixes it in from the clip before
  it over that time, so it starts that much earlier (the kind is an ffmpeg
  `xfade` transition, `fade` if left out; older versions just cut there).
  `VOLUME` scales its sound (1 = as recorded). `FIT`,
  `EFFECT` and `PARAM` lines belong to the clip above them. Indentation is
  only for reading.
- Version 1 files have no `TRACK` lines; they open as one picture track.
- `OUTPUT-SIZE`, `OUTPUT-RATE` (`0` = the first clip's), `OUTPUT-FIT` and
  `PAD-COLOR` are the output format.
- `EXPORT-VIDEO` (encoder, quality), `EXPORT-SOUND` (encoder, kbit/s, Hz)
  and `EXPORT-TYPE` (file suffix) are how it's exported. Files without
  them get the defaults.

Only paths and edits are stored. When a project is opened its files are
probed again; files that can't be found keep their clips, and the status
line says how many are missing. Unknown keys are skipped, so newer versions
can add settings; a file from a newer, incompatible version is refused.

## Settings

Program settings (not project settings) are kept with application.library's
PrefsObjects, in `ENV:Formbrod.jaokim.github.io.xml` (and `ENVARC:`), and
can be viewed with PrefsObjectsEditor. So far they are the drawers the file
requesters last used: `MediaDrawer`, `ProjectDrawer` and `ExportDrawer`, so
each requester opens where it was last used, also after a restart;
`ShowPictures` (*Settings → Show frame pictures*): `0` makes the film strip
show plain blocks with file names and decode nothing, for slower Amigas
(no thumbnails or waveforms are made either);
and for the log file `Log`, `LogFile` and `LogLevel` (see below). Without
application.library 53 the drawers are only remembered until Formbrod quits.

Sound for the preview goes through AHI unit `AHIUnit` (default 0).

Preview copies have three settings: `MakeProxies` (*Settings → Make
preview copies*, `1` or `0`), `ProxyWidth` (*Settings → Preview copy
size*: `auto` or a width in pixels) and `CacheDir` (default
`PROGDIR:Cache`; there is no menu item for it yet).

## Previews

*Preview* (or Space) plays the edit from the playhead in a small preview
window, with sound, until you press it again or the edit ends. Nothing is
written to disk: the pictures go straight to the window and the sound to
AHI. If the machine can't keep up, pictures are skipped so it stays in
time with the sound. The playhead follows along. Close the window, or
press Space or Esc in it, to stop.

Only the part being played is read: each file jumps straight to the first
frame used (to the keyframe before it). With preview copies that jump is
instant, and playing is cheaper too. Previews scale with `fast_bilinear`
and keep their sound at 44.1 kHz (the player's rate); exports use the
better scaler and 48 kHz. If playing can't keep up, the log says so when
it stops, with how long the sound was missing.

**Rendered ahead.** While the window is left alone (and the preview
copies are made), Formbrod renders the edit in 10-second pieces into the
cache drawer, starting where you last played. Playing a rendered piece
is cheap, one small file with nothing to scale or mix, so a preview that
is too heavy to render live still plays smoothly once it's rendered;
pieces that aren't yet are rendered live as before. Only the pieces an
edit touches are rendered again. *Settings → Render previews in the
background* (setting `BackgroundRender`) turns it off; the status line
says how many pieces are ready.

The preview window opens as soon as Play is pressed and says *Preparing
the preview...* until the first picture is ready; if playing stops before
then, it shows why (the status line). It can be made any size: the
picture is scaled to fit,
keeping its shape, by the graphics card where it can (compositing),
else in software, else it is shown at its own size.

**The frame at the playhead.** While the preview window is open and
nothing plays, it shows the frame at the playhead, and follows it when
the playhead is clicked or dragged, or the edit changes. *Project → Show
preview window* opens it without playing. While the playhead is dragged
it shows frames as fast as they can be made, ending on the one where the
playhead stops. Frames come from a piece rendered ahead where there is
one (instant), else from the preview copies, and are made on a backend
of their own, so they don't stop an export.

**Saving a frame.** *Project → Save frame...* saves the frame at the
playhead as a picture, full size from the original files: a PNG if the
name ends in `.png`, else a 24-bit IFF ILBM, which every paint program
reads. Use it as a stencil to draw an overlay in the right place. ARexx
`GRAB` does the same, at the preview's size unless `FULL` is given.

**The range.** The ruler at the top has two flags along its bottom, at
the start and the end of the edit to begin with. Drag them to mark a
range (a line through the film strip shows each one), or use *Edit →
Range starts here* / *Range ends here* (keys `[` and `]`) at the
playhead; *Range: whole edit* puts them back. The range is saved with
the project. *Project → Export range...* renders it small (320 wide,
from the preview copies) for a paint program: as an IFF ANIM, unless
the name ends in a movie's (`.mp4`, `.gif`...). The ANIM has 256
colours, the same in every frame (a fixed palette with an ordered
dither, so still parts make no deltas), 10 frames a second, and ANIM-5
deltas as DPaint writes them.

Sound goes through AHI, on unit 0 unless `AHIUnit` in the settings says
another. If AHI can't be opened the preview plays without sound, the
status line says so, and the log says why.

Every window (the main window, the clip window and the preview window)
opens where it was last time, at the same size (settings `MainWindow`,
`ClipWindow`, `PreviewWindow`).

ARexx `PREVIEW` still makes the older file preview: 10-second pieces in
`T:formbrod-preview-000.mp4` and on, listed in `T:formbrod-preview.m3u`,
rendered two pieces ahead of where a player started on the playlist would
be. Set `PreviewPlayer` in the settings to a command (the playlist's path
is added after it, e.g. `MPlayer -playlist`) to have that player started.

## Preview copies

Camera and phone videos (H.264, H.265) are slow to decode and to jump
around in, because most of their frames are only stored as changes from
other frames. So when a video is added, or a project is opened, Formbrod
makes a small copy of it in the background where every frame stands on
its own (MJPEG, with the sound as plain PCM), and renders previews from
that. Exports still read the original files.

- **Once per file.** A file is recognised by a fingerprint of its size and
  three 64 KB slices of it (start, middle and end), so moving or renaming
  it doesn't matter, and nor does using it in several projects. A copy is
  written under a temporary name and renamed when it is complete, so a
  copy in the cache drawer is always whole.
- **As wide as the preview.** With `ProxyWidth` set to `auto` (the
  default) copies are 320 pixels wide, the preview's width: a wider copy
  is only decoded and then scaled down again for every frame (measured:
  40 % more work at 480). A width can still be chosen in *Settings →
  Preview copy size*. The width is part of the file name
  (`<key>.proxy1-320.mov`), so changing it keeps the old copies, and two
  Amigas can share one cache drawer.
- **In the background.** Copies are made a slice at a time (about 30 ms
  of work, at least one packet), one file at a time, and only once the
  window has been left alone for 1.5 s, so editing never waits for them.
  They wait while an export runs. A file that fails is not tried again
  until Formbrod is restarted.
- Files that are already quick (MJPEG, DV and the like up to 1024 pixels
  wide), still pictures and sound files get no copy.
- Each copy has a `<key>.info` text file next to it saying which file it
  was made from. Deleting the cache drawer's contents is safe; the copies
  are made again when needed.

## Thumbnails

The film strip shows the edit as pictures, one per cell. They come from a
*thumbnail strip* made for each video and still picture: one small picture
(86 x 48, a cell's size) per second of the file, or fewer for files over
20 minutes, so a strip has at most 1200 (an hour is under 20 MB).

- **In the background, like the preview copies**, and after them: a strip
  is made from the file's preview copy once that is ready, since reading
  it is much quicker than reading the original. Still pictures and files
  that need no copy are read directly.
- **Once per file**, in the cache drawer, named by the same fingerprint as
  the copies (`<key>.thumbs1-86x48.rgb`). The file is plain pictures one
  after another, 8-bit ARGB, with nothing else in it, so it is easy to
  read and the same on every machine.
- **Drawn from memory.** The last 128 pictures used (2 MB) are kept in memory and
  the rest are read from the strip when the film strip needs them, so
  scrolling a long edit costs no more memory than a short one. A cell
  whose picture isn't there yet is grey for a moment; a file whose strip
  couldn't be made is a block in its colour.
- Zoomed in to less than a second per cell, neighbouring cells show the
  same picture.

## Waveforms

Clips on the sound lanes (a video's sound, and the sound tracks) show
their sound as a waveform. It comes from a *peak file* made for each file
with sound: the loudest sample of every 20 ms, one byte each (an hour is
180 KB), named `<key>.wave1-50.yuv` in the cache drawer.

- It is made in the background with the thumbnails, from the preview copy
  when there is one (its sound is plain PCM, so there is nothing to
  decode), else from the file. Ten minutes of AAC takes a few seconds on
  a PC.
- The bars are drawn as the square root of the level, so quiet passages
  still show; the clip's volume is taken into account. Until a file's
  peaks are there, its clips show a line through the middle.
- The sound rows are taller than before, to make room for it.
- *Settings → Show frame pictures* turns waveforms off along with the
  thumbnails: then nothing is decoded for either.

## Log file

Formbrod writes a log to `T:Formbrod.log`, started afresh each time it
runs. It is the place to look when a render goes wrong. It holds:

- the program and ffmpeg library versions;
- every file read, with its length, size, pixel shape, frame rate and
  whether it has sound, or why it couldn't be read;
- every render: the job, **the same thing as an `ffmpeg` command line**
  (to try on another computer), the decoders and encoders used with their
  pixel formats and colour range, and how it ended;
- everything ffmpeg itself reports, which used to go to the shell window.
  A message ffmpeg repeats for every frame is written once, followed by
  how many times it came again;
- the status line messages.

*Settings → Write log file* turns it off and on. The settings `LogFile`
(another path) and `LogLevel` (`error`, `warning`, `info` (default),
`debug`, which adds ffmpeg's detailed messages and the conversion filters
added for the encoders, or `trace`) can be changed with PrefsObjectsEditor.

`trace` also follows what Formbröd is doing, for finding where something
stops: windows being opened (each class, object and screen), which part
of the main loop runs, play starting and each step of it, and in the
backend each input opened, the first packet and frame of every stream,
the first picture and sound given to the player, and once a second how
many packets, frames and refusals there have been. `>` starts a function
and `<` ends it, with its result. The log grows quickly, so turn it back
to `info` afterwards.

## Writing a module

Copy `modules/fade/fade.c`. A module declares its parameters and writes
ffmpeg filter strings for a clip. Register it in
`FB_RegisterBuiltinModules()` (`src/job/module.c`) and add it to the
`Makefile`. It then works in the GUI, in ARexx (`EFFECT <clip> <name>`), and
on any render backend.
