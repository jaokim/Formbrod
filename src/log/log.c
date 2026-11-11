/*
 * log.c - a plain text log file. See include/formbrod/log.h.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "formbrod/log.h"

#define LINE_LEN   1024
#define AREA_LEN   16
#define SPLIT_AT   100      /* break longer lines of a text after ';' */

static FILE *Log;
static FBLogLevel Level = FB_LOG_INFO;

/* The last line written, to fold repeats. */
static char       Last[LINE_LEN];
static char       LastArea[AREA_LEN];
static FBLogLevel LastLevel;
static unsigned long Repeats;

static const char *const Names[] = { "error", "warning", "info", "debug" };

const char *FB_LogLevelName(FBLogLevel level)
{
    return (level >= FB_LOG_ERROR && level <= FB_LOG_DEBUG) ? Names[level]
                                                            : "info";
}

FBLogLevel FB_LogLevelFromName(const char *name, FBLogLevel def)
{
    int i;

    for (i = 0; name && i <= FB_LOG_DEBUG; i++)
        if (strcmp(name, Names[i]) == 0)
            return (FBLogLevel)i;
    return def;
}

static void put_line(FBLogLevel level, const char *area, const char *text)
{
    char stamp[16] = "--:--:--";
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    if (tm)
        strftime(stamp, sizeof(stamp), "%H:%M:%S", tm);
    fprintf(Log, "%s %-7s %-7s %s\n", stamp, FB_LogLevelName(level), area,
            text);
    fflush(Log);    /* so the log is complete even if the program crashes */
}

static void flush_repeats(void)
{
    char text[64];

    if (!Log || Repeats == 0)
        return;
    snprintf(text, sizeof(text), "(the line above repeated %lu more time%s)",
             Repeats, Repeats == 1 ? "" : "s");
    put_line(LastLevel, LastArea, text);
    Repeats = 0;
}

static void emit(FBLogLevel level, const char *area, const char *text)
{
    if (!Log || level > Level)
        return;
    if (!area)
        area = "";
    if (level == LastLevel && strcmp(text, Last) == 0 &&
        strncmp(area, LastArea, AREA_LEN - 1) == 0) {
        Repeats++;
        return;
    }
    flush_repeats();
    put_line(level, area, text);
    strncpy(Last, text, LINE_LEN - 1);
    Last[LINE_LEN - 1] = '\0';
    strncpy(LastArea, area, AREA_LEN - 1);
    LastArea[AREA_LEN - 1] = '\0';
    LastLevel = level;
}

int FB_LogOpen(const char *path, FBLogLevel level)
{
    FB_LogClose();
    if (!path || !*path)
        return -1;
    Log = fopen(path, "w");
    if (!Log)
        return -1;
    Level = level;
    Last[0] = '\0';
    LastArea[0] = '\0';
    Repeats = 0;
    return 0;
}

void FB_LogClose(void)
{
    if (!Log)
        return;
    flush_repeats();
    fclose(Log);
    Log = NULL;
}

int FB_LogIsOpen(void)
{
    return Log != NULL;
}

int FB_LogWants(FBLogLevel level)
{
    return Log != NULL && level <= Level;
}

FBLogLevel FB_LogGetLevel(void)
{
    return Level;
}

void FB_LogV(FBLogLevel level, const char *area, const char *fmt, va_list ap)
{
    char text[LINE_LEN];
    size_t n;

    if (!FB_LogWants(level))
        return;
    vsnprintf(text, sizeof(text), fmt, ap);
    n = strlen(text);
    while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == '\r'))
        text[--n] = '\0';
    emit(level, area, text);
}

void FB_Log(FBLogLevel level, const char *area, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    FB_LogV(level, area, fmt, ap);
    va_end(ap);
}

void FB_LogText(FBLogLevel level, const char *area, const char *text)
{
    char line[LINE_LEN];

    if (!FB_LogWants(level) || !text)
        return;
    while (*text) {
        const char *end = strchr(text, '\n');
        size_t len = end ? (size_t)(end - text) : strlen(text);
        size_t start = 0;

        /* A long line (a filtergraph) is broken after every ';', and
         * anything still too long for one log line is cut into pieces. */
        int split = len > SPLIT_AT;

        do {
            size_t piece = len - start;
            size_t i;

            if (split)
                for (i = start; i < len; i++)
                    if (text[i] == ';') {
                        piece = i + 1 - start;
                        break;
                    }
            if (piece >= sizeof(line))
                piece = sizeof(line) - 1;
            memcpy(line, text + start, piece);
            line[piece] = '\0';
            emit(level, area, line);
            start += piece;
        } while (start < len);
        text += len;
        if (*text == '\n')
            text++;
    }
}
