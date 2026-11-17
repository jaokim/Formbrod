/*
 * waves.c - waveforms for the sound lanes (see formbrod/waves.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "formbrod/waves.h"
#include "formbrod/cache.h"

static void copy_key(char *dst, const char *src)
{
    size_t n = strlen(src);

    if (n >= FB_KEY_LEN)
        n = FB_KEY_LEN - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ---- Columns --------------------------------------------------------------- */

/* The peak a time falls in. */
static long peak_at(fb_us_t t)
{
    return (long)(t * FB_WAVE_RATE / FB_US_PER_SEC);
}

void FB_WaveColumns(const unsigned char *peaks, long count, fb_us_t t0,
                    fb_us_t step, int n, unsigned char *out)
{
    int i;

    if (step < 1)
        step = 1;
    for (i = 0; i < n; i++) {
        fb_us_t a = t0 + step * i, b = a + step;
        long k, k0, k1;
        unsigned char m = 0;

        out[i] = 0;
        if (b <= 0 || !peaks || count <= 0)
            continue;
        k0 = a < 0 ? 0 : peak_at(a);
        k1 = peak_at(b - 1);       /* the last peak the column touches */
        if (k0 >= count)
            continue;
        if (k1 >= count)
            k1 = count - 1;
        for (k = k0; k <= k1; k++)
            if (peaks[k] > m)
                m = peaks[k];
        out[i] = m;
    }
}

/* ---- The store ------------------------------------------------------------- */

static void lock(FBWaveStore *s)
{
    if (s->lock)
        s->lock(s->hook_data);
}

static void unlock(FBWaveStore *s)
{
    if (s->unlock)
        s->unlock(s->hook_data);
}

static FBWave *find(FBWaveStore *s, const char *key)
{
    int i;

    for (i = 0; i < s->num; i++)
        if (strcmp(s->waves[i].key, key) == 0)
            return &s->waves[i];
    return NULL;
}

static FBWave *entry(FBWaveStore *s, const char *key)
{
    FBWave *w = find(s, key);

    if (w || s->num >= FB_WAVES_MAX)
        return w;
    w = &s->waves[s->num++];
    memset(w, 0, sizeof(*w));
    copy_key(w->key, key);
    w->state = FB_STRIP_UNKNOWN;
    return w;
}

/* Drops a file's peaks from memory (store locked). */
static void drop(FBWaveStore *s, FBWave *w)
{
    free(w->peaks);
    s->bytes -= w->count;
    w->peaks = NULL;
    w->count = 0;
}

void FB_WavesFree(FBWaveStore *s)
{
    int i;

    for (i = 0; i < s->num; i++)
        free(s->waves[i].peaks);
    s->num = 0;
    s->bytes = 0;
    s->num_wants = 0;
}

static void want(FBWaveStore *s, const char *key)
{
    int i;

    for (i = 0; i < s->num_wants; i++)
        if (strcmp(s->wants[i], key) == 0)
            return;
    if (s->num_wants >= FB_WAVES_MAX)
        return;
    copy_key(s->wants[s->num_wants++], key);
    if (s->num_wants == 1 && s->wake)
        s->wake(s->hook_data);
}

FBThumbLook FB_WavesLock(FBWaveStore *s, const char *key,
                         const unsigned char **peaks, long *count)
{
    FBWave *w;

    *peaks = NULL;
    *count = 0;
    if (!key || !key[0])
        return FB_THUMB_NONE;
    lock(s);
    w = entry(s, key);
    if (!w || w->state == FB_STRIP_NONE) {
        unlock(s);
        return FB_THUMB_NONE;
    }
    if (w->state == FB_STRIP_READY && w->peaks) {
        *peaks = w->peaks;
        *count = w->count;
        return FB_THUMB_HIT;       /* still locked */
    }
    if (w->state == FB_STRIP_UNKNOWN)
        want(s, key);
    unlock(s);
    return FB_THUMB_WAIT;
}

void FB_WavesUnlock(FBWaveStore *s)
{
    unlock(s);
}

int FB_WavesWanted(FBWaveStore *s)
{
    int n;

    lock(s);
    n = s->num_wants;
    unlock(s);
    return n;
}

/* Reads a whole peak file. NULL if it isn't there (or is empty). */
static unsigned char *read_peaks(const FBCache *c, const char *key,
                                 long *count)
{
    char path[FB_PATH_LEN];
    unsigned char *buf;
    FILE *f;
    long size;

    *count = 0;
    if (FB_CachePath(c, key, FB_CACHE_WAVE, 0, path, sizeof(path)) != 0)
        return NULL;
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)size);
    if (buf && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf)
        *count = size;
    return buf;
}

int FB_WavesLoad(FBWaveStore *s, const FBCache *c)
{
    char todo[FB_WAVES_MAX][FB_KEY_LEN];
    int i, n, loaded = 0;

    lock(s);
    n = s->num_wants;
    memcpy(todo, s->wants, (size_t)n * FB_KEY_LEN);
    s->num_wants = 0;
    unlock(s);

    for (i = 0; i < n; i++) {
        long count = 0;
        unsigned char *peaks;
        FBWave *w;
        int full;

        lock(s);
        full = s->bytes >= FB_WAVES_MAX_BYTES;
        unlock(s);
        peaks = full ? NULL : read_peaks(c, todo[i], &count);

        lock(s);
        w = find(s, todo[i]);
        if (!w || w->state != FB_STRIP_UNKNOWN) {
            free(peaks);          /* reset or failed meanwhile */
        } else if (full) {
            w->state = FB_STRIP_NONE;
        } else if (!peaks) {
            w->state = FB_STRIP_MISSING;
        } else {
            w->peaks = peaks;
            w->count = count;
            w->state = FB_STRIP_READY;
            s->bytes += count;
            loaded++;
        }
        unlock(s);
    }
    return loaded;
}

static void set_state(FBWaveStore *s, const char *key, FBStripState state)
{
    FBWave *w;

    if (!key || !key[0])
        return;
    lock(s);
    w = entry(s, key);
    if (w) {
        drop(s, w);
        w->state = state;
    }
    unlock(s);
}

void FB_WavesReset(FBWaveStore *s, const char *key)
{
    set_state(s, key, FB_STRIP_UNKNOWN);
}

void FB_WavesSetNone(FBWaveStore *s, const char *key)
{
    set_state(s, key, FB_STRIP_NONE);
}
