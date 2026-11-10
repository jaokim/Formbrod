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
- [x] ARexx port
- [x] Translatable GUI through locale.library catalogs (English, Swedish)
- [x] Local rendering with libav* (`src/backend/local_backend.c`): decode,
      filter, encode and mux, a few packets per GUI tick. Tested on Linux
      with ffmpeg 6.1, not yet on AmigaOS
- [x] Timeline gadgets, first step (`gadgets/timeline/`): ruler and film
      strip on a shared time axis, zoom, scrolling and a draggable playhead.
      Thumbnails are dummies for now
- [ ] Real thumbnails, waveforms, trimming in the timeline
- [ ] Remote rendering on a render server

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
    'OPEN "Work:Video/main.mp4"'
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
| `OPEN` | `FILE/A` | |
| `SPLIT` | `AT/N/A` (ms) | |
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
| `QUIT` | | |

Clip and effect numbers start at 0.

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

## Writing a module

Copy `modules/fade/fade.c`. A module declares its parameters and writes
ffmpeg filter strings for a clip. Register it in
`FB_RegisterBuiltinModules()` (`src/job/module.c`) and add it to the
`Makefile`. It then works in the GUI, in ARexx (`EFFECT <clip> <name>`), and
on any render backend.
