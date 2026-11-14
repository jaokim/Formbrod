/*
 * cache.c - the media cache (see formbrod/cache.h).
 */
#include <stdio.h>
#include <string.h>
#include "formbrod/cache.h"
#include "formbrod/project.h"   /* FB_PathJoin */

#define SLICE (64 * 1024)   /* bytes read from each part of a file */

/* Copies at most len - 1 characters and always ends the string. (Not
 * strncpy: newer gcc warns about it truncating, even where that's meant.) */
static void copy_str(char *dst, const char *src, size_t len)
{
    size_t n = strlen(src);

    if (n >= len)
        n = len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ---- Keys -------------------------------------------------------------- */

/* 64-bit FNV-1a. Not cryptographic: the key only has to tell a user's
 * video files apart, and must be cheap on a 1 GHz PowerPC. */
typedef unsigned long long fnv_t;

#define FNV_START 0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL

static fnv_t fnv(fnv_t h, const unsigned char *p, size_t n)
{
    while (n--) {
        h ^= *p++;
        h *= FNV_PRIME;
    }
    return h;
}

/* The size goes in as 8 bytes, lowest first, so every host agrees. */
static fnv_t fnv_size(fnv_t h, unsigned long long size)
{
    unsigned char b[8];
    int i;

    for (i = 0; i < 8; i++)
        b[i] = (unsigned char)(size >> (8 * i));
    return fnv(h, b, 8);
}

static void key_text(fnv_t h, char key[FB_KEY_LEN])
{
    static const char hex[] = "0123456789abcdef";
    int i;

    for (i = 15; i >= 0; i--) {
        key[i] = hex[h & 15];
        h >>= 4;
    }
    key[16] = '\0';
}

/* Both FB_CacheKey and FB_CacheKeyData hash the same bytes: "FBK1", the
 * size, then the whole thing if it's at most three slices, else a slice
 * from the start, the middle and the end. */
static void slices(unsigned long long size, unsigned long long off[3],
                   size_t *count, size_t *n)
{
    if (size <= 3 * SLICE) {
        off[0] = 0;
        *n = (size_t)size;
        *count = 1;
        return;
    }
    off[0] = 0;
    off[1] = size / 2 - SLICE / 2;
    off[2] = size - SLICE;
    *n = SLICE;
    *count = 3;
}

void FB_CacheKeyData(const void *data, size_t len, char key[FB_KEY_LEN])
{
    const unsigned char *p = data;
    unsigned long long off[3];
    size_t count, n, i;
    fnv_t h = fnv(FNV_START, (const unsigned char *)"FBK1", 4);

    h = fnv_size(h, len);
    slices(len, off, &count, &n);
    for (i = 0; i < count; i++)
        h = fnv(h, p + off[i], n);
    key_text(h, key);
}

int FB_CacheKey(const char *path, char key[FB_KEY_LEN])
{
    static unsigned char buf[SLICE];   /* off the stack: 64 KB */
    unsigned long long off[3];
    size_t count, n, i;
    fnv_t h = fnv(FNV_START, (const unsigned char *)"FBK1", 4);
    FILE *f;
    long size;

    key[0] = '\0';
    f = fopen(path, "rb");
    if (!f)
        return -1;
    /* ftell() is a long: files of 2 GB or more fail here on 32-bit
     * systems, and get no key (so no proxy) rather than a wrong one. */
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
        fclose(f);
        return -1;
    }
    h = fnv_size(h, (unsigned long long)size);
    slices((unsigned long long)size, off, &count, &n);
    for (i = 0; i < count; i++) {
        if (fseek(f, (long)off[i], SEEK_SET) != 0 ||
            fread(buf, 1, n, f) != n) {
            fclose(f);
            return -1;
        }
        h = fnv(h, buf, n);
    }
    fclose(f);
    key_text(h, key);
    return 0;
}

/* ---- Recipes ------------------------------------------------------------ */

void FB_CacheRecipe(const FBCache *c, FBCacheKind kind, char *out,
                    size_t len)
{
    (void)kind;   /* only proxies so far */
    snprintf(out, len, "proxy1-%d", c->proxy_width);
}

int FB_CacheWantsProxy(const FBCache *c, const FBSource *src)
{
    if (!src->has_video || src->still || !src->key[0])
        return 0;
    /* Already small and every frame stands alone: nothing to gain. */
    if (src->intra && src->width > 0 &&
        src->width <= (c->proxy_width > FB_PROXY_SKIP_WIDTH
                       ? c->proxy_width : FB_PROXY_SKIP_WIDTH))
        return 0;
    return 1;
}

int FB_CacheAutoWidth(int mhz, int cores)
{
    int score;

    if (mhz <= 0)
        return FB_PROXY_WIDTH;
    /* A second core helps: libavcodec decodes on both. */
    score = cores > 1 ? mhz * 3 / 2 : mhz;
    if (score >= 2400)       /* X1000 (PA6T 1.8 GHz x2), X5000 */
        return 640;
    if (score >= 1500)       /* A1222 (1.2 GHz x2) */
        return 480;
    return 320;              /* Sam460, Sam440, G3/G4 machines */
}

/* ---- The cache ----------------------------------------------------------- */

void FB_CacheInit(FBCache *c, const char *dir, int proxy_width)
{
    memset(c, 0, sizeof(*c));
    copy_str(c->dir, dir, sizeof(c->dir));
    c->current = -1;
    if (proxy_width <= 0)
        proxy_width = FB_PROXY_WIDTH;
    proxy_width &= ~1;
    if (proxy_width < FB_PROXY_MIN_WIDTH)
        proxy_width = FB_PROXY_MIN_WIDTH;
    c->proxy_width = proxy_width;
}

int FB_CachePath(const FBCache *c, const char *key, FBCacheKind kind,
                 int partial, char *out, size_t len)
{
    char recipe[64], name[128];

    FB_CacheRecipe(c, kind, recipe, sizeof(recipe));
    if (snprintf(name, sizeof(name), "%s.%s%s.mov", key, recipe,
                 partial ? "-part" : "") >= (int)sizeof(name))
        return -1;
    return FB_PathJoin(c->dir, name, out, len);
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (!f)
        return 0;
    fclose(f);
    return 1;
}

int FB_CacheFind(const FBCache *c, const char *key, FBCacheKind kind,
                 char *out, size_t len)
{
    char path[FB_PATH_LEN];

    if (!key || !key[0] || !c->dir[0])
        return 0;
    if (FB_CachePath(c, key, kind, 0, path, sizeof(path)) != 0 ||
        !file_exists(path))
        return 0;
    if (out && len)
        copy_str(out, path, len);
    return 1;
}

static int find_task(const FBCache *c, const char *key, FBCacheKind kind)
{
    int i;

    for (i = 0; i < c->num_tasks; i++)
        if (c->tasks[i].kind == kind && strcmp(c->tasks[i].key, key) == 0)
            return i;
    return -1;
}

FBCacheState FB_CacheState(const FBCache *c, const char *key,
                           FBCacheKind kind)
{
    int t = find_task(c, key, kind);

    if (t >= 0 && c->tasks[t].state != FB_CACHE_READY)
        return c->tasks[t].state;
    return FB_CacheFind(c, key, kind, NULL, 0) ? FB_CACHE_READY
                                               : FB_CACHE_NONE;
}

/* Drops tasks that are done, to make room. */
static void compact(FBCache *c)
{
    int i, j = 0;

    for (i = 0; i < c->num_tasks; i++) {
        if (c->tasks[i].state == FB_CACHE_READY && i != c->current)
            continue;
        if (i == c->current)
            c->current = j;
        if (i != j)
            c->tasks[j] = c->tasks[i];
        j++;
    }
    c->num_tasks = j;
}

FBCacheState FB_CacheRequest(FBCache *c, const FBSource *src)
{
    FBCacheTask *t;
    int i;

    if (!c->dir[0] || !FB_CacheWantsProxy(c, src))
        return FB_CACHE_NONE;
    i = find_task(c, src->key, FB_CACHE_PROXY);
    if (i >= 0 && c->tasks[i].state != FB_CACHE_READY)
        return c->tasks[i].state;   /* queued, busy or failed: once only */
    if (FB_CacheFind(c, src->key, FB_CACHE_PROXY, NULL, 0))
        return FB_CACHE_READY;
    if (i < 0) {
        if (c->num_tasks >= FB_CACHE_MAX_TASKS)
            compact(c);
        if (c->num_tasks >= FB_CACHE_MAX_TASKS)
            return FB_CACHE_NONE;
        i = c->num_tasks++;
    }
    t = &c->tasks[i];
    memset(t, 0, sizeof(*t));
    copy_str(t->key, src->key, sizeof(t->key));
    t->kind = FB_CACHE_PROXY;
    t->state = FB_CACHE_QUEUED;
    t->source = *src;
    return FB_CACHE_QUEUED;
}

int FB_CachePending(const FBCache *c)
{
    int i, n = 0;

    for (i = 0; i < c->num_tasks; i++)
        if (c->tasks[i].state == FB_CACHE_QUEUED ||
            c->tasks[i].state == FB_CACHE_BUSY)
            n++;
    return n;
}

int FB_CacheBuildJob(const FBCache *c, const FBCacheTask *task, FBJob *job)
{
    const FBSource *s = &task->source;
    int n;

    memset(job, 0, sizeof(*job));
    job->version = FB_JOB_VERSION;
    job->num_inputs = 1;
    copy_str(job->inputs[0], s->path, FB_PATH_LEN);

    /* Same frames and timing as the file, only smaller and every frame a
     * keyframe, so clip times in the edit stay valid for the proxy. The
     * pixel shape is kept: scale adjusts it to keep the picture's shape. */
    if (s->width > c->proxy_width)
        n = snprintf(job->filtergraph, sizeof(job->filtergraph),
                     "[0:v]scale=%d:-2:flags=fast_bilinear[outv]",
                     c->proxy_width);
    else
        n = snprintf(job->filtergraph, sizeof(job->filtergraph),
                     "[0:v]null[outv]");
    if (s->has_audio)
        n += snprintf(job->filtergraph + n, sizeof(job->filtergraph) - n,
                      ";[0:a]anull[outa]");
    if (n >= (int)sizeof(job->filtergraph))
        return -1;
    copy_str(job->video_out, "outv", sizeof(job->video_out));
    if (s->has_audio)
        copy_str(job->audio_out, "outa", sizeof(job->audio_out));

    if (FB_CachePath(c, task->key, task->kind, 1, job->output.path,
                     sizeof(job->output.path)) != 0)
        return -1;
    copy_str(job->output.vcodec, "mjpeg", sizeof(job->output.vcodec));
    copy_str(job->output.acodec, "pcm_s16le", sizeof(job->output.acodec));
    job->output.quality = FB_QUALITY_PREVIEW;
    job->length = s->duration;
    return 0;
}

int FB_CacheNextJob(FBCache *c, FBJob *job)
{
    int i;

    if (c->current >= 0)
        return -1;
    for (i = 0; i < c->num_tasks; i++) {
        FBCacheTask *t = &c->tasks[i];

        if (t->state != FB_CACHE_QUEUED)
            continue;
        if (FB_CacheFind(c, t->key, t->kind, NULL, 0)) {
            t->state = FB_CACHE_READY;   /* someone else made it */
            continue;
        }
        if (FB_CacheBuildJob(c, t, job) != 0) {
            t->state = FB_CACHE_FAILED;
            continue;
        }
        t->state = FB_CACHE_BUSY;
        c->current = i;
        return i;
    }
    return -1;
}

static void write_info(const FBCache *c, const FBCacheTask *t)
{
    char name[FB_KEY_LEN + 8], path[FB_PATH_LEN], recipe[64];
    FILE *f;

    snprintf(name, sizeof(name), "%s.info", t->key);
    if (FB_PathJoin(c->dir, name, path, sizeof(path)) != 0)
        return;
    f = fopen(path, "w");
    if (!f)
        return;
    FB_CacheRecipe(c, t->kind, recipe, sizeof(recipe));
    fprintf(f, "FORMBROD-CACHE 1\nSOURCE %s\nLENGTH %lld\nSIZE %d %d\n"
               "MADE %s\n",
            t->source.path, (long long)t->source.duration,
            t->source.width, t->source.height, recipe);
    fclose(f);
}

FBCacheState FB_CacheFinish(FBCache *c, int ok)
{
    char part[FB_PATH_LEN], done[FB_PATH_LEN];
    FBCacheTask *t;

    if (c->current < 0)
        return FB_CACHE_NONE;
    t = &c->tasks[c->current];
    c->current = -1;
    if (FB_CachePath(c, t->key, t->kind, 1, part, sizeof(part)) != 0 ||
        FB_CachePath(c, t->key, t->kind, 0, done, sizeof(done)) != 0) {
        t->state = FB_CACHE_FAILED;
        return t->state;
    }
    if (ok) {
        remove(done);   /* rename() won't replace a file on every system */
        if (rename(part, done) == 0) {
            write_info(c, t);
            t->state = FB_CACHE_READY;
            return t->state;
        }
    }
    remove(part);
    t->state = FB_CACHE_FAILED;
    return t->state;
}

void FB_CacheAbort(FBCache *c)
{
    char part[FB_PATH_LEN];
    FBCacheTask *t;

    if (c->current < 0)
        return;
    t = &c->tasks[c->current];
    c->current = -1;
    if (FB_CachePath(c, t->key, t->kind, 1, part, sizeof(part)) == 0)
        remove(part);
    t->state = FB_CACHE_QUEUED;
}
