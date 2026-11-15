/*
 * thumbs.c - film-strip thumbnails (see formbrod/thumbs.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "formbrod/thumbs.h"
#include "formbrod/cache.h"

static void copy_key(char *dst, const char *src)
{
    size_t n = strlen(src);

    if (n >= FB_KEY_LEN)
        n = FB_KEY_LEN - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ---- Steps ---------------------------------------------------------------- */

fb_ms_t FB_ThumbStep(fb_ms_t duration)
{
    fb_ms_t step = FB_THUMB_STEP_MS;

    if (duration > step * FB_THUMB_MAX_FRAMES) {
        /* Whole seconds, rounded up, so it stays at most MAX_FRAMES. */
        step = (duration + FB_THUMB_MAX_FRAMES - 1) / FB_THUMB_MAX_FRAMES;
        step = (step + 999) / 1000 * 1000;
    }
    return step;
}

long FB_ThumbIndex(fb_ms_t step, fb_us_t t)
{
    fb_us_t s = FB_US(step > 0 ? step : FB_THUMB_STEP_MS);

    if (t <= 0)
        return 0;
    return (long)((t + s / 2) / s);
}

/* ---- The store ------------------------------------------------------------ */

static void lock(FBThumbStore *s)
{
    if (s->lock)
        s->lock(s->hook_data);
}

static void unlock(FBThumbStore *s)
{
    if (s->unlock)
        s->unlock(s->hook_data);
}

static size_t thumb_bytes(const FBThumbStore *s)
{
    return (size_t)s->w * (size_t)s->h * 4;
}

int FB_ThumbsInit(FBThumbStore *s, int w, int h, int slots)
{
    void (*l)(void *) = s->lock, (*u)(void *) = s->unlock;
    void (*wk)(void *) = s->wake;
    void *data = s->hook_data;

    memset(s, 0, sizeof(*s));
    s->lock = l;
    s->unlock = u;
    s->wake = wk;
    s->hook_data = data;
    if (w <= 0 || h <= 0)
        return -1;
    s->w = w;
    s->h = h;
    s->num_slots = slots > 0 ? slots : FB_THUMBS_SLOTS;
    s->slots = calloc((size_t)s->num_slots, sizeof(*s->slots));
    s->pixels = malloc((size_t)s->num_slots * thumb_bytes(s));
    if (!s->slots || !s->pixels) {
        FB_ThumbsFree(s);
        return -1;
    }
    return 0;
}

void FB_ThumbsFree(FBThumbStore *s)
{
    free(s->slots);
    free(s->pixels);
    s->slots = NULL;
    s->pixels = NULL;
    s->num_slots = 0;
    s->num_strips = 0;
    s->num_wants = 0;
}

static FBThumbStrip *find_strip(FBThumbStore *s, const char *key)
{
    int i;

    for (i = 0; i < s->num_strips; i++)
        if (strcmp(s->strips[i].key, key) == 0)
            return &s->strips[i];
    return NULL;
}

/* The strip's entry, made (UNKNOWN) if there is none. NULL if the table
 * is full. */
static FBThumbStrip *strip_entry(FBThumbStore *s, const char *key)
{
    FBThumbStrip *st = find_strip(s, key);

    if (st || s->num_strips >= FB_THUMBS_MAX_STRIPS)
        return st;
    st = &s->strips[s->num_strips++];
    memset(st, 0, sizeof(*st));
    copy_key(st->key, key);
    st->state = FB_STRIP_UNKNOWN;
    return st;
}

static int find_slot(const FBThumbStore *s, const char *key, long index)
{
    int i;

    for (i = 0; i < s->num_slots; i++)
        if (s->slots[i].index == index && s->slots[i].key[0] &&
            strcmp(s->slots[i].key, key) == 0)
            return i;
    return -1;
}

/* Notes a miss, once. */
static void want(FBThumbStore *s, const char *key, long index)
{
    int i;

    for (i = 0; i < s->num_wants; i++)
        if (s->wants[i].index == index && strcmp(s->wants[i].key, key) == 0)
            return;
    if (s->num_wants >= FB_THUMBS_MAX_WANTS)
        return;     /* asked again at the next drawing */
    copy_key(s->wants[s->num_wants].key, key);
    s->wants[s->num_wants++].index = index;
    if (s->num_wants == 1 && s->wake)
        s->wake(s->hook_data);
}

FBThumbLook FB_ThumbsLock(FBThumbStore *s, const char *key, long index,
                          const unsigned char **pixels)
{
    FBThumbStrip *st;
    int slot;

    *pixels = NULL;
    if (!key || !key[0] || !s->pixels)
        return FB_THUMB_NONE;
    if (index < 0)
        index = 0;
    lock(s);
    st = strip_entry(s, key);
    if (!st || st->state == FB_STRIP_NONE) {
        unlock(s);
        return FB_THUMB_NONE;
    }
    if (st->state == FB_STRIP_MISSING) {
        unlock(s);
        return FB_THUMB_WAIT;
    }
    if (st->state == FB_STRIP_READY && index >= st->count)
        index = st->count - 1;
    slot = find_slot(s, key, index);
    if (slot < 0) {
        want(s, key, index);
        unlock(s);
        return FB_THUMB_WAIT;
    }
    s->slots[slot].used = ++s->clock;
    *pixels = s->pixels + (size_t)slot * thumb_bytes(s);
    return FB_THUMB_HIT;       /* still locked */
}

void FB_ThumbsUnlock(FBThumbStore *s)
{
    unlock(s);
}

int FB_ThumbsWanted(FBThumbStore *s)
{
    int n;

    lock(s);
    n = s->num_wants;
    unlock(s);
    return n;
}

/* The slot used longest ago (or a free one). */
static int oldest_slot(const FBThumbStore *s)
{
    int i, best = 0;

    for (i = 0; i < s->num_slots; i++) {
        if (!s->slots[i].key[0])
            return i;
        if (s->slots[i].used < s->slots[best].used)
            best = i;
    }
    return best;
}

/* Opens a strip and counts its frames: 0 if it isn't there (or is empty,
 * which a finished strip never is). */
static FILE *open_strip(const FBThumbStore *s, const FBCache *c,
                        const char *key, long *count)
{
    char path[FB_PATH_LEN];
    FILE *f;
    long size;

    *count = 0;
    if (FB_CachePath(c, key, FB_CACHE_THUMBS, 0, path, sizeof(path)) != 0)
        return NULL;
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
        fclose(f);
        return NULL;
    }
    *count = (long)((unsigned long)size / thumb_bytes(s));
    if (*count <= 0) {
        fclose(f);
        return NULL;
    }
    return f;
}

int FB_ThumbsLoad(FBThumbStore *s, const FBCache *c)
{
    FBThumbWant todo[FB_THUMBS_MAX_WANTS];
    unsigned char *buf;
    int i, n, loaded = 0;

    if (!s->pixels)
        return 0;
    lock(s);
    n = s->num_wants;
    memcpy(todo, s->wants, (size_t)n * sizeof(todo[0]));
    s->num_wants = 0;
    unlock(s);
    if (n == 0)
        return 0;
    buf = malloc(thumb_bytes(s));
    if (!buf)
        return 0;

    /* The file is opened for each: there are a few dozen at most, and
     * nothing is held while it's read. */
    for (i = 0; i < n; i++) {
        const char *key = todo[i].key;
        long index = todo[i].index, count = 0;
        FBStripState state;
        FILE *f;
        int slot;

        lock(s);
        {
            FBThumbStrip *st = find_strip(s, key);

            state = st ? st->state : FB_STRIP_NONE;
            count = st ? st->count : 0;
        }
        unlock(s);
        if (state == FB_STRIP_NONE || state == FB_STRIP_MISSING)
            continue;

        f = open_strip(s, c, key, &count);
        lock(s);
        {
            FBThumbStrip *st = find_strip(s, key);

            if (st && st->state == FB_STRIP_UNKNOWN) {
                st->state = f ? FB_STRIP_READY : FB_STRIP_MISSING;
                st->count = count;
            }
        }
        unlock(s);
        if (!f)
            continue;
        if (index >= count)
            index = count - 1;
        if (fseek(f, (long)(index * (long)thumb_bytes(s)), SEEK_SET) != 0 ||
            fread(buf, 1, thumb_bytes(s), f) != thumb_bytes(s)) {
            fclose(f);
            continue;
        }
        fclose(f);

        lock(s);
        slot = find_slot(s, key, index);
        if (slot < 0) {
            slot = oldest_slot(s);
            copy_key(s->slots[slot].key, key);
            s->slots[slot].index = index;
        }
        memcpy(s->pixels + (size_t)slot * thumb_bytes(s), buf,
               thumb_bytes(s));
        s->slots[slot].used = ++s->clock;
        unlock(s);
        loaded++;
    }
    free(buf);
    return loaded;
}

/* Sets a strip's state, dropping its thumbnails from memory. */
static void set_state(FBThumbStore *s, const char *key, FBStripState state)
{
    FBThumbStrip *st;
    int i;

    if (!key || !key[0])
        return;
    lock(s);
    st = strip_entry(s, key);
    if (st) {
        st->state = state;
        st->count = 0;
    }
    for (i = 0; i < s->num_slots; i++)
        if (strcmp(s->slots[i].key, key) == 0)
            s->slots[i].key[0] = '\0';
    unlock(s);
}

void FB_ThumbsReset(FBThumbStore *s, const char *key)
{
    set_state(s, key, FB_STRIP_UNKNOWN);
}

void FB_ThumbsSetNone(FBThumbStore *s, const char *key)
{
    set_state(s, key, FB_STRIP_NONE);
}
