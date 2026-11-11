/*
 * log.h - a plain text log file, for finding out what went wrong.
 *
 * Each line has the time, the level and where it came from:
 *
 *   21:03:12 info    render  Starting: Work:out.mp4
 *   21:03:12 warning ffmpeg  [swscaler @ 0x5a1c0] unknown range
 *   21:03:12 warning ffmpeg  (the line above repeated 249 times)
 *
 * A line that comes again straight away is counted, not written again, so a
 * warning ffmpeg gives for every frame doesn't fill the disk.
 *
 * Plain C99 and stdio, so it works the same on the host and the Amiga.
 * Everything that logs runs in the program's own task (rendering is done
 * in slices from the main loop), so there is no locking.
 */
#ifndef FORMBROD_LOG_H
#define FORMBROD_LOG_H

#include <stdarg.h>

typedef enum {
    FB_LOG_ERROR = 0,
    FB_LOG_WARNING,
    FB_LOG_INFO,
    FB_LOG_DEBUG
} FBLogLevel;

/* Starts a new log at `path` (an old one is replaced), keeping lines at
 * `level` and more important. Returns 0, or -1 if the file can't be
 * written; then nothing is logged. */
int  FB_LogOpen(const char *path, FBLogLevel level);
void FB_LogClose(void);
int  FB_LogIsOpen(void);

/* Whether a line at `level` would be written: lets callers skip work. */
int  FB_LogWants(FBLogLevel level);
FBLogLevel FB_LogGetLevel(void);

/* One line. `area` says where it came from: "app", "probe", "render",
 * "ffmpeg"... A trailing newline in the text is ignored. */
void FB_Log(FBLogLevel level, const char *area, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 3, 4)))
#endif
    ;
void FB_LogV(FBLogLevel level, const char *area, const char *fmt, va_list ap);

/* Several lines (a job, a filtergraph): each gets its own prefix. Long
 * filtergraphs are also broken after each ';' so they can be read. */
void FB_LogText(FBLogLevel level, const char *area, const char *text);

/* Level names for settings: "error", "warning", "info", "debug". FromName
 * returns `def` for anything else. */
const char *FB_LogLevelName(FBLogLevel level);
FBLogLevel  FB_LogLevelFromName(const char *name, FBLogLevel def);

#endif
