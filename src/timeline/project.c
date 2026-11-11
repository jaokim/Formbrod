/*
 * project.c - project files: the timeline as hand-editable text.
 *
 * See include/formbrod/project.h for the format.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "formbrod/project.h"

#define LINE_LEN (FB_PATH_LEN + 64)

typedef struct Buf {
    char  *p;
    size_t len;
    size_t used;
    int    overflow;
} Buf;

static void buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (b->overflow)
        return;
    va_start(ap, fmt);
    n = vsnprintf(b->p + b->used, b->len - b->used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->len - b->used)
        b->overflow = 1;
    else
        b->used += (size_t)n;
}

static int copy_str(char *dst, const char *src, size_t len)
{
    size_t n = strlen(src);

    if (n >= len) {
        memcpy(dst, src, len - 1);
        dst[len - 1] = '\0';
        return -1;
    }
    memcpy(dst, src, n + 1);
    return 0;
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* ---- Paths ------------------------------------------------------------- */

void FB_PathDir(const char *path, char *dir, size_t len)
{
    const char *slash = strrchr(path, '/');
    const char *colon = strrchr(path, ':');
    size_t n;

    if (slash && (!colon || slash > colon))
        n = (size_t)(slash - path);         /* "Work:a/b" -> "Work:a" */
    else if (colon)
        n = (size_t)(colon - path) + 1;     /* "Work:b"   -> "Work:"  */
    else
        n = 0;                              /* "b"        -> ""       */
    if (n >= len)
        n = len - 1;
    memcpy(dir, path, n);
    dir[n] = '\0';
}

int FB_PathJoin(const char *dir, const char *name, char *out, size_t len)
{
    size_t dl = strlen(dir);
    int sep = dl > 0 && dir[dl - 1] != ':' && dir[dl - 1] != '/';

    if (strchr(name, ':') || dl == 0)
        return copy_str(out, name, len);
    if (dl + (size_t)sep + strlen(name) >= len)
        return -1;
    memcpy(out, dir, dl);
    if (sep)
        out[dl++] = '/';
    strcpy(out + dl, name);
    return 0;
}

int FB_PathRelative(const char *dir, const char *path, char *out, size_t len)
{
    size_t dl = dir ? strlen(dir) : 0;
    size_t i;
    const char *rest;

    if (dl == 0 || strlen(path) <= dl)
        goto as_is;
    for (i = 0; i < dl; i++)
        if (lower(dir[i]) != lower(path[i]))
            goto as_is;
    rest = path + dl;
    /* "Work:Proj" must not match "Work:Project2/x". */
    if (dir[dl - 1] != ':' && dir[dl - 1] != '/') {
        if (*rest != '/')
            goto as_is;
        rest++;
    }
    if (!*rest)
        goto as_is;
    copy_str(out, rest, len);    /* shorter than path, so it fits if path does */
    return 1;

as_is:
    copy_str(out, path, len);
    return 0;
}

/* ---- Writing ----------------------------------------------------------- */

int FB_ProjectWrite(const FBTimeline *tl, const char *project_dir,
                    char *out, size_t len)
{
    Buf b;
    int i, e, p;

    b.p = out;
    b.len = len;
    b.used = 0;
    b.overflow = 0;

    buf_printf(&b, "FORMBROD-PROJECT %d\n", FB_PROJECT_VERSION);
    buf_printf(&b,
        "; Formbrod project. One setting per line; ';' starts a comment.\n"
        "; If files have moved, change the paths on the SOURCE lines.\n"
        "; A path without a volume name (no ':') is relative to the\n"
        "; drawer this file is in. Times are in milliseconds.\n\n");

    buf_printf(&b, "; 0 = take it from the first clip\n");
    buf_printf(&b, "OUTPUT-SIZE %d %d\n", tl->format.width, tl->format.height);
    buf_printf(&b, "OUTPUT-RATE %d %d\n",
               tl->format.fps_num, tl->format.fps_den);
    buf_printf(&b, "OUTPUT-FIT %s\n", FB_FitName(tl->format.fit));
    buf_printf(&b, "PAD-COLOR %06lX\n",
               (unsigned long)(tl->format.pad_color & 0xFFFFFF));

    if (tl->num_sources > 0)
        buf_printf(&b, "\n");
    for (i = 0; i < tl->num_sources; i++) {
        char rel[FB_PATH_LEN];

        FB_PathRelative(project_dir, tl->sources[i].path, rel, sizeof(rel));
        buf_printf(&b, "SOURCE %d %s\n", i + 1, rel);
    }

    if (tl->num_clips > 0)
        buf_printf(&b, "\n; CLIP <source> <in> <out>\n");
    for (i = 0; i < tl->num_clips; i++) {
        const FBClip *c = &tl->clips[i];

        buf_printf(&b, "CLIP %d %lld %lld\n", c->source + 1,
                   (long long)c->in, (long long)c->out);
        if (c->fit != FB_FIT_DEFAULT)
            buf_printf(&b, "    FIT %s\n", FB_FitName(c->fit));
        for (e = 0; e < c->num_effects; e++) {
            const FBEffect *fx = &c->effects[e];

            buf_printf(&b, "    EFFECT %s\n", fx->module);
            for (p = 0; p < fx->num_params; p++)
                buf_printf(&b, "        PARAM %s %.10g\n",
                           fx->params[p].name, fx->params[p].value);
        }
    }

    return b.overflow ? -1 : (int)b.used;
}

/* ---- Reading ----------------------------------------------------------- */

/* Copies one line into dst without its line ending, leading blanks and
 * trailing blanks. Returns the start of the next line. */
static const char *next_line(const char *p, char *dst, size_t len)
{
    size_t n = 0;

    while (*p == ' ' || *p == '\t')
        p++;
    while (*p && *p != '\n' && *p != '\r') {
        if (n < len - 1)
            dst[n++] = *p;
        p++;
    }
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\t'))
        n--;
    dst[n] = '\0';
    if (*p == '\r')
        p++;
    if (*p == '\n')
        p++;
    return p;
}

/* Reads a whole decimal number and the blanks after it. */
static int read_ll(const char **s, long long *v)
{
    char *end;

    *v = strtoll(*s, &end, 10);
    if (end == *s || (*end && *end != ' ' && *end != '\t'))
        return -1;
    while (*end == ' ' || *end == '\t')
        end++;
    *s = end;
    return 0;
}

static int read_int(const char **s, int *v)
{
    long long ll;

    if (read_ll(s, &ll) != 0 || ll < -2147483647LL || ll > 2147483647LL)
        return -1;
    *v = (int)ll;
    return 0;
}

FBProjectError FB_ProjectParse(FBTimeline *tl, const char *text,
                               const char *project_dir, int *line_no)
{
    static char line[LINE_LEN];
    const char *p = text;
    FBClip *clip = NULL;
    FBEffect *fx = NULL;
    int n = 0, seen_header = 0;

    FB_TimelineInit(tl);
    if (line_no)
        *line_no = 0;

/* Stops with `err` at the current line. */
#define FAIL(err) do { if (line_no) *line_no = n; return (err); } while (0)

    while (*p) {
        const char *val;
        char *key_end;

        p = next_line(p, line, sizeof(line));
        n++;
        if (!line[0] || line[0] == ';' || line[0] == '#')
            continue;

        key_end = line + strcspn(line, " \t");
        val = key_end;
        while (*val == ' ' || *val == '\t')
            val++;
        *key_end = '\0';

        if (!seen_header) {
            int version;

            if (strcmp(line, "FORMBROD-PROJECT") != 0)
                FAIL(FB_PROJECT_NOT_PROJECT);
            if (read_int(&val, &version) != 0 || version < 1)
                FAIL(FB_PROJECT_NOT_PROJECT);
            if (version > FB_PROJECT_VERSION)
                FAIL(FB_PROJECT_TOO_NEW);
            seen_header = 1;
        } else if (strcmp(line, "OUTPUT-SIZE") == 0) {
            if (read_int(&val, &tl->format.width) != 0 ||
                read_int(&val, &tl->format.height) != 0 ||
                tl->format.width < 0 || tl->format.height < 0)
                FAIL(FB_PROJECT_BAD_LINE);
        } else if (strcmp(line, "OUTPUT-RATE") == 0) {
            if (read_int(&val, &tl->format.fps_num) != 0 ||
                read_int(&val, &tl->format.fps_den) != 0 ||
                tl->format.fps_num < 0 || tl->format.fps_den < 0)
                FAIL(FB_PROJECT_BAD_LINE);
        } else if (strcmp(line, "OUTPUT-FIT") == 0) {
            int fit = FB_FitFromName(val);

            if (fit < 0)
                FAIL(FB_PROJECT_BAD_LINE);
            tl->format.fit = fit == FB_FIT_DEFAULT ? FB_FIT_FIT : (FBFit)fit;
        } else if (strcmp(line, "PAD-COLOR") == 0) {
            char *end;
            unsigned long c = strtoul(val, &end, 16);

            if (end == val || *end)
                FAIL(FB_PROJECT_BAD_LINE);
            tl->format.pad_color = (uint32_t)(c & 0xFFFFFF);
        } else if (strcmp(line, "SOURCE") == 0) {
            char path[FB_PATH_LEN];
            int num;

            /* Numbered so CLIP lines are easy to match by eye; they must
             * still come in order. */
            if (read_int(&val, &num) != 0 || num != tl->num_sources + 1 ||
                !*val)
                FAIL(FB_PROJECT_BAD_LINE);
            if (FB_PathJoin(project_dir ? project_dir : "", val,
                            path, sizeof(path)) != 0)
                FAIL(FB_PROJECT_BAD_LINE);
            if (FB_TimelineAddSource(tl, path, 0, 1, 0) < 0)
                FAIL(FB_PROJECT_TOO_MANY);
        } else if (strcmp(line, "CLIP") == 0) {
            long long in, out;
            int src, idx;

            if (read_int(&val, &src) != 0 || read_ll(&val, &in) != 0 ||
                read_ll(&val, &out) != 0 || in < 0 || out <= in)
                FAIL(FB_PROJECT_BAD_LINE);
            if (src < 1 || src > tl->num_sources)
                FAIL(FB_PROJECT_BAD_SOURCE);
            idx = FB_TimelineAppendClip(tl, src - 1, in, out);
            if (idx < 0)
                FAIL(FB_PROJECT_TOO_MANY);
            clip = &tl->clips[idx];
            fx = NULL;
        } else if (strcmp(line, "FIT") == 0) {
            int fit = FB_FitFromName(val);

            if (!clip || fit < 0)
                FAIL(FB_PROJECT_BAD_LINE);
            clip->fit = (FBFit)fit;
        } else if (strcmp(line, "EFFECT") == 0) {
            if (!clip || !*val || strlen(val) >= FB_NAME_LEN)
                FAIL(FB_PROJECT_BAD_LINE);
            fx = FB_ClipAddEffect(clip, val);
            if (!fx)
                FAIL(FB_PROJECT_TOO_MANY);
        } else if (strcmp(line, "PARAM") == 0) {
            char name[FB_NAME_LEN];
            size_t nl = strcspn(val, " \t");
            const char *num = val + nl;
            char *end;
            double v;

            if (!fx || nl == 0 || nl >= sizeof(name))
                FAIL(FB_PROJECT_BAD_LINE);
            memcpy(name, val, nl);
            name[nl] = '\0';
            while (*num == ' ' || *num == '\t')
                num++;
            v = strtod(num, &end);
            if (end == num || *end)
                FAIL(FB_PROJECT_BAD_LINE);
            if (FB_EffectSetParam(fx, name, v) != 0)
                FAIL(FB_PROJECT_TOO_MANY);
        }
        /* Unknown keys are skipped, so later versions can add some. */
    }
#undef FAIL

    if (!seen_header)
        return FB_PROJECT_NOT_PROJECT;
    return FB_PROJECT_OK;
}

const char *FB_ProjectErrorString(FBProjectError err)
{
    switch (err) {
    case FB_PROJECT_OK:          return "OK";
    case FB_PROJECT_NOT_PROJECT: return "not a Formbrod project";
    case FB_PROJECT_TOO_NEW:     return "made by a newer Formbrod";
    case FB_PROJECT_BAD_LINE:    return "a line can't be read";
    case FB_PROJECT_BAD_SOURCE:  return "a clip refers to a missing source";
    case FB_PROJECT_TOO_MANY:    return "too many sources, clips or effects";
    }
    return "unknown error";
}
