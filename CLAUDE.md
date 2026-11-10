# Formbröd – notes for Claude

A ReAction video editor for AmigaOS 4 that uses ffmpeg's libav* libraries.
It starts simple (trim, split and join files, a few effects) and grows
through modules and ARexx.

## Layout

- `include/formbrod/` – public headers: timeline model, job, module and
  backend interfaces. These are what modules and future backends code against.
- `src/timeline/`, `src/job/`, `modules/` – the **portable core**. Plain C99
  with no AmigaOS headers. Keep it that way, so it can be tested on any host.
- `src/app.c` – every user action (`App_*`). Both the GUI and ARexx call
  these. Add an action here first, then wire it to the GUI and to ARexx.
- `src/gui/` – ReAction. Classes are opened with `OpenClass()` and objects
  made with `NewObject(Class *, ...)`, not reaction_macros.h.
- `src/arexx/` – the FORMBROD port, using arexx.class.
- `src/locale/` – `FB_Str()` over locale.library. `strings.h` is generated
  from `locale/formbrod.cd` by `tools/catgen.py` (`make strings`); don't edit
  it by hand. Translations are `locale/*.ct`.
- `src/backend/` – render backends and media probing. libav code goes inside
  `#ifdef HAVE_LIBAV`.
- `gadgets/timeline/` – the planned custom timeline gadget (design notes only).

## Design rules

- **The job is the unit of work.** The editor turns the timeline into an
  `FBJob` (inputs, one ffmpeg filtergraph, output settings), and any backend
  renders it. The Amiga builds the filtergraph, which keeps module logic in
  one place. Render servers just run jobs.
- **ffmpeg is linked as libraries**, not run as an external binary. Real
  rendering happens at export; preview mode renders a low-res version.
- **Modules return filter strings.** A module with `FB_MODF_NATIVE` can't run
  remotely, and building a job with one fails with `FB_JOB_NOT_EXPRESSIBLE`
  until there is a local pre-pass.
- **No literal UI text.** Anything the user reads in the window or the shell
  goes through `FB_Str(MSG_...)`. New strings get a new, never reused ID in
  `formbrod.cd`. ARexx commands/results and the core's English error strings
  (`FB_JobErrorString`, used in logs and by render servers) stay untranslated;
  the GUI shows `App_JobErrorString()` instead.
- **The job text format is versioned** (`FB_JOB_VERSION`). Parsers ignore
  unknown keys, so add keys rather than change existing ones.

## Building and testing

- `make -f Makefile.host test` – builds and runs `tests/test_core.c` on the
  host. Run it after any change to the core. Check generated filtergraphs
  with a real ffmpeg when possible.
- The host test also checks that `strings.h` matches `formbrod.cd` and that
  every `.ct` passes `catgen.py check`.
- `make` – cross-compiles with `ppc-amigaos-gcc` (AmigaOS 4 SDK, `-lauto`).
  Add `LIBAV=1 LIBAV_PREFIX=...` to link ffmpeg.
- The Amiga-side code (GUI, ARexx, backend) has not been compiled against
  the SDK in this environment. Treat API details there as unverified until
  they build on a real toolchain.

## Style

- C99, 4-space indent, braces on their own line for functions only.
- Public names use the `FB_` prefix; app actions use `App_`.
- OS4 calls go through interfaces explicitly (`IIntuition->...`); don't
  rely on `__USE_INLINE__`.
