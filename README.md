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
- [x] ReAction main window: clip list, playhead, render progress, drag and
      drop
- [x] ARexx port
- [x] Translatable GUI through locale.library catalogs (English, Swedish)
- [ ] Local rendering with libav* (`src/backend/local_backend.c`)
- [ ] Custom timeline gadget (`gadgets/timeline/`)
- [ ] Remote rendering on a render server

## Building

Cross-compile with the AmigaOS 4 SDK:

    make                                   # without ffmpeg
    make LIBAV=1 LIBAV_PREFIX=/path/to/ffmpeg-ppc

To run the core tests on Linux or macOS:

    make -f Makefile.host test

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
| `QUIT` | | |

Clip and effect numbers start at 0.

## Writing a module

Copy `modules/fade/fade.c`. A module declares its parameters and writes
ffmpeg filter strings for a clip. Register it in
`FB_RegisterBuiltinModules()` (`src/job/module.c`) and add it to the
`Makefile`. It then works in the GUI, in ARexx (`EFFECT <clip> <name>`), and
on any render backend.
