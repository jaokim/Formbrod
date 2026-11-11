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
      strip on a shared time axis, zoom, scrolling and a draggable playhead.
      Thumbnails are dummies for now
- [x] Tracks: each added file gets its own track; picture tracks are laid
      on top of each other and all sound is mixed. Cutting closes gaps
- [x] Set in / Set out at the playhead (buttons, Edit menu, I and O keys),
      and still pictures (PNG, JPEG...) as clips of any length
- [ ] Track editor under the film strip: clip boxes, trimming, moving
- [ ] Real thumbnails, waveforms
- [ ] Remote rendering on a render server

## Editing

- **Add file** puts a file on a new track of its own: a video, a sound
  file (music) or a still picture. A picture starts out 5 seconds long.
- Click or drag in the ruler or film strip to move the playhead.
- **Set in** (or the I key) cuts away the start of a clip up to the
  playhead; **Set out** (O) cuts away the end after it. They work on the
  clip selected in the list, or else on the picture under the playhead.
  The clips after it on the same track move along, so no gap is left.
- Put the playhead *after* the end of a clip and choose **Set out** to make
  it longer, as far as its file goes; a still picture can be any length.
  The timeline goes on a little past its last clip for this.
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
| `REMOVE` | `CLIP/N/A` | |
| `EFFECT` | `CLIP/N/A,MODULE/A` | effect index |
| `SETPARAM` | `CLIP/N/A,EFFECT/N/A,NAME/A,VALUE/A` | |
| `EXPORT` | `FILE/A,PREVIEW/S` | |
| `CANCEL` | | |
| `RENDERSTATUS` | | `IDLE`, `RUNNING <pos> <total>`, `DONE`, `FAILED`, `CANCELLED` |
| `CLIPS` | | number of clips |
| `MODULES` | | module names |
| `FIT` | `CLIP/N/A,MODE/A` | |
| `FORMAT` | `WIDTH/N,HEIGHT/N,FPS/K,FIT/K` | `<w> <h> <fps> <fit>` |
| `NEWPROJECT` | | |
| `OPENPROJECT` | `FILE/A` | |
| `SAVEPROJECT` | `FILE` (default: the current file) | path saved to |
| `QUIT` | | |

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

    'FORMAT WIDTH 1280 HEIGHT 720 FPS 25 FIT FIT'
    'FIT 2 FILL'        /* crop clip 2 instead of adding bars */

All audio is resampled to 48 kHz stereo, and clips without audio get
silence, so files with and without sound can be joined. The clip list shows
how each clip is fitted in its Effects column.

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

    SOURCE 1 clips/intro.mp4
    SOURCE 2 Video:Holiday 2026/beach.mov
    SOURCE 3 music/theme.mp3

    TRACK video
    CLIP 1 1000 5000
    CLIP 2 0 20000
        FIT fill
        EFFECT fade
            PARAM in 0.5

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
  leaving a gap; `VOLUME` scales its sound (1 = as recorded). `FIT`,
  `EFFECT` and `PARAM` lines belong to the clip above them. Indentation is
  only for reading.
- Version 1 files have no `TRACK` lines; they open as one picture track.
- `OUTPUT-SIZE`, `OUTPUT-RATE` (`0` = the first clip's), `OUTPUT-FIT` and
  `PAD-COLOR` are the output format.

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
show plain blocks with file names and decode nothing, for slower Amigas;
and for the log file `Log`, `LogFile` and `LogLevel` (see below). Without
application.library 53 the drawers are only remembered until Formbrod quits.

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
(another path) and `LogLevel` (`error`, `warning`, `info` (default) or
`debug`, which adds ffmpeg's detailed messages and the conversion filters
added for the encoders) can be changed with PrefsObjectsEditor.

## Writing a module

Copy `modules/fade/fade.c`. A module declares its parameters and writes
ffmpeg filter strings for a clip. Register it in
`FB_RegisterBuiltinModules()` (`src/job/module.c`) and add it to the
`Makefile`. It then works in the GUI, in ARexx (`EFFECT <clip> <name>`), and
on any render backend.
