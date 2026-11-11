/*
 * project.h - saving and loading a project (the timeline) as text.
 *
 * A project file is plain text meant to be read and fixed by hand, mostly so
 * source paths can be changed when files have moved:
 *
 *   FORMBROD-PROJECT 1
 *   ; comments start with ';' (or '#')
 *   OUTPUT-SIZE 0 0
 *   OUTPUT-RATE 0 0
 *   OUTPUT-FIT fit
 *   PAD-COLOR 000000
 *
 *   SOURCE 1 Work:Video/holiday.mp4
 *   SOURCE 2 clips/intro.mp4
 *
 *   CLIP 1 0 12000
 *       FIT fill
 *       EFFECT fade
 *           PARAM in 0.5
 *
 * One setting per line: a key, a space, the value. Indentation and blank
 * lines are ignored. Sources are numbered from 1, and CLIP refers to them by
 * number: CLIP <source> <in ms> <out ms>. FIT, EFFECT and PARAM belong to
 * the CLIP (and EFFECT) above them. A source path is the rest of the line,
 * so it may contain spaces.
 *
 * A path without a volume or assign (no ':') is relative to the drawer the
 * project file is in. Sources inside that drawer are saved that way, so a
 * project can be moved together with its files.
 *
 * Like the job format, the version only goes up for changes old readers
 * would get wrong. New keys can be added within a version; readers ignore
 * keys they don't know. Plain C99, no AmigaOS headers.
 */
#ifndef FORMBROD_PROJECT_H
#define FORMBROD_PROJECT_H

#include <stddef.h>
#include "timeline.h"

#define FB_PROJECT_VERSION  1

typedef enum {
    FB_PROJECT_OK = 0,
    FB_PROJECT_NOT_PROJECT,   /* no FORMBROD-PROJECT header */
    FB_PROJECT_TOO_NEW,       /* written by a newer Formbröd */
    FB_PROJECT_BAD_LINE,      /* a known key with a value that can't be read */
    FB_PROJECT_BAD_SOURCE,    /* CLIP names a source that isn't there */
    FB_PROJECT_TOO_MANY       /* more sources, clips, effects than fit */
} FBProjectError;

/* Writes the timeline as project text. `project_dir` is the drawer the file
 * will be saved in ("" or NULL: save every path as it is). Returns the bytes
 * written (excluding NUL) or -1 if buf is too small. */
int FB_ProjectWrite(const FBTimeline *tl, const char *project_dir,
                    char *buf, size_t len);

/* Reads project text into `tl` (which is reset first). Relative source paths
 * are joined to `project_dir`. Sources get duration 0 and are marked as
 * having video and no audio; the caller probes them for the real values.
 * On error, `*line` (if not NULL) is set to the line number. */
FBProjectError FB_ProjectParse(FBTimeline *tl, const char *text,
                               const char *project_dir, int *line);

const char *FB_ProjectErrorString(FBProjectError err);

/* ---- AmigaOS-style path helpers (pure string work) ---- */

/* The drawer part of a path: "Work:a/b.fbp" -> "Work:a", "Work:b" ->
 * "Work:", "b" -> "". Like dos.library PathPart(). */
void FB_PathDir(const char *path, char *dir, size_t len);

/* Joins a drawer and a name like dos.library AddPart(): "Work:a" + "b" ->
 * "Work:a/b", "Work:" + "b" -> "Work:b". A name with a ':' is returned as
 * it is. Returns 0, or -1 if it doesn't fit. */
int  FB_PathJoin(const char *dir, const char *name, char *out, size_t len);

/* If `path` is inside `dir` (case-insensitive, as Amiga filesystems are),
 * writes the part after it and returns 1. Otherwise copies `path` and
 * returns 0. */
int  FB_PathRelative(const char *dir, const char *path, char *out, size_t len);

#endif
