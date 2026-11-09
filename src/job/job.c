/*
 * job.c - turn a timeline into a render job, and (de)serialise jobs.
 *
 * For each clip we emit a trimmed video chain (and an audio chain when every
 * clip has audio), run the clip's effects through their modules, then concat
 * everything into [outv]/[outa]:
 *
 *   [0:v]trim=start=1:end=4,setpts=PTS-STARTPTS,fade=...[v0];
 *   [0:a]atrim=start=1:end=4,asetpts=PTS-STARTPTS,afade=...[a0];
 *   ...
 *   [v0][a0][v1][a1]concat=n=2:v=1:a=1[outv][outa]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "formbrod/job.h"
#include "formbrod/module.h"

#define PREVIEW_WIDTH 320

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

static void copy_str(char *dst, const char *src, size_t len)
{
    strncpy(dst, src, len - 1);
    dst[len - 1] = '\0';
}

static double secs(fb_ms_t ms)
{
    return ms / 1000.0;
}

/* Appends ",<filters>" for every effect on the clip. */
static FBJobError add_effects(Buf *b, const FBClip *clip, int audio)
{
    char tmp[1024];
    int i;

    for (i = 0; i < clip->num_effects; i++) {
        const FBEffect *fx = &clip->effects[i];
        const FBModule *mod = FB_FindModule(fx->module);
        int (*fn)(const FBEffect *, fb_ms_t, char *, size_t);

        if (!mod)
            return FB_JOB_UNKNOWN_MODULE;
        if (mod->flags & FB_MODF_NATIVE)
            return FB_JOB_NOT_EXPRESSIBLE;

        fn = audio ? mod->audio_filter : mod->video_filter;
        if (!fn)
            continue;
        if (fn(fx, clip->out - clip->in, tmp, sizeof(tmp)) != 0)
            return FB_JOB_OVERFLOW;
        if (tmp[0])
            buf_printf(b, ",%s", tmp);
    }
    return FB_JOB_OK;
}

FBJobError FB_JobBuild(FBJob *job, const FBTimeline *tl, const FBOutput *out)
{
    int input_of[FB_MAX_SOURCES];
    int with_audio = 1;
    int width = out->width, height = out->height;
    Buf b;
    int i;

    memset(job, 0, sizeof(*job));
    job->version = FB_JOB_VERSION;
    job->output = *out;

    if (tl->num_clips == 0)
        return FB_JOB_EMPTY;

    /* Only sources that are actually used become inputs. */
    for (i = 0; i < FB_MAX_SOURCES; i++)
        input_of[i] = -1;
    for (i = 0; i < tl->num_clips; i++) {
        int s = tl->clips[i].source;

        if (s < 0 || s >= tl->num_sources)
            return FB_JOB_BAD_SOURCE;
        if (input_of[s] < 0) {
            input_of[s] = job->num_inputs;
            copy_str(job->inputs[job->num_inputs++], tl->sources[s].path,
                     FB_PATH_LEN);
        }
        if (!tl->sources[s].has_audio)
            with_audio = 0;
    }

    if (out->quality == FB_QUALITY_PREVIEW && width == 0) {
        width  = PREVIEW_WIDTH;
        height = -2;   /* keep aspect, even height */
    }

    b.p = job->filtergraph;
    b.len = sizeof(job->filtergraph);
    b.used = 0;
    b.overflow = 0;
    b.p[0] = '\0';

    for (i = 0; i < tl->num_clips; i++) {
        const FBClip *c = &tl->clips[i];
        int in = input_of[c->source];
        FBJobError err;

        buf_printf(&b, "[%d:v]trim=start=%.3f:end=%.3f,setpts=PTS-STARTPTS",
                   in, secs(c->in), secs(c->out));
        /* concat needs equal frame sizes, so scale every clip */
        if (width != 0)
            buf_printf(&b, ",scale=%d:%d,setsar=1", width,
                       height ? height : -2);
        if ((err = add_effects(&b, c, 0)) != FB_JOB_OK)
            return err;
        buf_printf(&b, "[v%d];", i);

        if (with_audio) {
            buf_printf(&b, "[%d:a]atrim=start=%.3f:end=%.3f,"
                       "asetpts=PTS-STARTPTS", in, secs(c->in), secs(c->out));
            if ((err = add_effects(&b, c, 1)) != FB_JOB_OK)
                return err;
            buf_printf(&b, "[a%d];", i);
        }
    }

    for (i = 0; i < tl->num_clips; i++) {
        if (with_audio)
            buf_printf(&b, "[v%d][a%d]", i, i);
        else
            buf_printf(&b, "[v%d]", i);
    }
    buf_printf(&b, "concat=n=%d:v=1:a=%d[outv]%s", tl->num_clips,
               with_audio, with_audio ? "[outa]" : "");

    if (b.overflow)
        return FB_JOB_OVERFLOW;

    copy_str(job->video_out, "outv", FB_NAME_LEN);
    copy_str(job->audio_out, with_audio ? "outa" : "", FB_NAME_LEN);
    return FB_JOB_OK;
}

const char *FB_JobErrorString(FBJobError err)
{
    switch (err) {
    case FB_JOB_OK:              return "OK";
    case FB_JOB_EMPTY:           return "The timeline is empty";
    case FB_JOB_BAD_SOURCE:      return "A clip refers to a missing source";
    case FB_JOB_UNKNOWN_MODULE:  return "An effect uses an unknown module";
    case FB_JOB_NOT_EXPRESSIBLE: return "An effect can't be rendered with ffmpeg filters";
    case FB_JOB_OVERFLOW:        return "The edit is too complex (filtergraph too long)";
    }
    return "Unknown error";
}

/* ---- Text format ------------------------------------------------------
 *
 *   FORMBROD-JOB 1
 *   INPUT <path>             (repeated)
 *   FILTERGRAPH <graph>
 *   VOUT <label>
 *   AOUT <label>             (omitted if no audio)
 *   OUTPUT <path>
 *   VCODEC <name>
 *   ACODEC <name>
 *   SIZE <w> <h>
 *   QUALITY <0|1>
 *   END
 *
 * Values run to end of line, so paths may contain spaces.
 */

int FB_JobWrite(const FBJob *job, char *out, size_t len)
{
    Buf b;
    int i;

    b.p = out;
    b.len = len;
    b.used = 0;
    b.overflow = 0;

    buf_printf(&b, "FORMBROD-JOB %d\n", job->version);
    for (i = 0; i < job->num_inputs; i++)
        buf_printf(&b, "INPUT %s\n", job->inputs[i]);
    buf_printf(&b, "FILTERGRAPH %s\n", job->filtergraph);
    buf_printf(&b, "VOUT %s\n", job->video_out);
    if (job->audio_out[0])
        buf_printf(&b, "AOUT %s\n", job->audio_out);
    buf_printf(&b, "OUTPUT %s\n", job->output.path);
    if (job->output.vcodec[0])
        buf_printf(&b, "VCODEC %s\n", job->output.vcodec);
    if (job->output.acodec[0])
        buf_printf(&b, "ACODEC %s\n", job->output.acodec);
    buf_printf(&b, "SIZE %d %d\n", job->output.width, job->output.height);
    buf_printf(&b, "QUALITY %d\n", (int)job->output.quality);
    buf_printf(&b, "END\n");

    return b.overflow ? -1 : (int)b.used;
}

/* Copies the rest of the line (after "KEY ") into dst. */
static const char *take_line(const char *p, char *dst, size_t len)
{
    size_t n = 0;

    while (*p && *p != '\n' && *p != '\r') {
        if (n < len - 1)
            dst[n++] = *p;
        p++;
    }
    dst[n] = '\0';
    while (*p == '\r' || *p == '\n')
        p++;
    return p;
}

int FB_JobParse(FBJob *job, const char *text)
{
    static char line[FB_FILTERGRAPH_LEN + 32];
    const char *p = text;
    int seen_header = 0, seen_end = 0;

    memset(job, 0, sizeof(*job));

    while (*p && !seen_end) {
        char *val;

        p = take_line(p, line, sizeof(line));
        if (!line[0])
            continue;
        val = strchr(line, ' ');
        if (val)
            *val++ = '\0';
        else
            val = line + strlen(line);

        if (!seen_header) {
            if (strcmp(line, "FORMBROD-JOB") != 0)
                return -1;
            job->version = atoi(val);
            if (job->version != FB_JOB_VERSION)
                return -1;
            seen_header = 1;
        } else if (strcmp(line, "INPUT") == 0) {
            if (job->num_inputs >= FB_MAX_SOURCES)
                return -1;
            copy_str(job->inputs[job->num_inputs++], val, FB_PATH_LEN);
        } else if (strcmp(line, "FILTERGRAPH") == 0) {
            copy_str(job->filtergraph, val, sizeof(job->filtergraph));
        } else if (strcmp(line, "VOUT") == 0) {
            copy_str(job->video_out, val, FB_NAME_LEN);
        } else if (strcmp(line, "AOUT") == 0) {
            copy_str(job->audio_out, val, FB_NAME_LEN);
        } else if (strcmp(line, "OUTPUT") == 0) {
            copy_str(job->output.path, val, FB_PATH_LEN);
        } else if (strcmp(line, "VCODEC") == 0) {
            copy_str(job->output.vcodec, val, FB_NAME_LEN);
        } else if (strcmp(line, "ACODEC") == 0) {
            copy_str(job->output.acodec, val, FB_NAME_LEN);
        } else if (strcmp(line, "SIZE") == 0) {
            if (sscanf(val, "%d %d", &job->output.width,
                       &job->output.height) != 2)
                return -1;
        } else if (strcmp(line, "QUALITY") == 0) {
            job->output.quality = atoi(val) ? FB_QUALITY_FINAL
                                            : FB_QUALITY_PREVIEW;
        } else if (strcmp(line, "END") == 0) {
            seen_end = 1;
        }
        /* Unknown keys are ignored so newer editors can talk to older
         * servers. */
    }
    return (seen_header && seen_end) ? 0 : -1;
}

static void buf_quoted(Buf *b, const char *s)
{
    buf_printf(b, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            buf_printf(b, "\\%c", *s);
        else
            buf_printf(b, "%c", *s);
    }
    buf_printf(b, "\"");
}

int FB_JobToCommandLine(const FBJob *job, char *out, size_t len)
{
    Buf b;
    int i;

    b.p = out;
    b.len = len;
    b.used = 0;
    b.overflow = 0;

    buf_printf(&b, "ffmpeg -y");
    for (i = 0; i < job->num_inputs; i++) {
        buf_printf(&b, " -i ");
        buf_quoted(&b, job->inputs[i]);
    }
    buf_printf(&b, " -filter_complex ");
    buf_quoted(&b, job->filtergraph);
    buf_printf(&b, " -map \"[%s]\"", job->video_out);
    if (job->audio_out[0])
        buf_printf(&b, " -map \"[%s]\"", job->audio_out);
    if (job->output.vcodec[0])
        buf_printf(&b, " -c:v %s", job->output.vcodec);
    if (job->output.acodec[0])
        buf_printf(&b, " -c:a %s", job->output.acodec);
    buf_printf(&b, " ");
    buf_quoted(&b, job->output.path);

    return b.overflow ? -1 : (int)b.used;
}
