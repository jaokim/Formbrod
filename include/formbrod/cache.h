/*
 * cache.h - the media cache: edit-friendly copies of source files.
 *
 * Camera and phone files (H.264, H.265) store most frames as differences
 * from other frames, which makes them slow to decode and to seek in on an
 * Amiga. So each video source gets a *proxy*: a small copy where every
 * frame stands alone (MJPEG, at most FB_PROXY_WIDTH wide, sound as plain
 * PCM). Previews are rendered from proxies; the final export still reads
 * the original files.
 *
 * How it fits together:
 *
 *   - A file is known by its *key*, a fingerprint of its size and three
 *     slices of its contents (start, middle, end). Reading 192 KB is quick
 *     even for big files, and the key stays the same when a file is moved
 *     or renamed, or when another computer (a render server) reads the
 *     same file over the network.
 *   - Cached files live in one drawer, named after the key and the
 *     recipe: "<key>.proxy1-640.mov". The number after "proxy" changes
 *     when the recipe does, so old copies are simply not used any more.
 *     The width is part of the name, so a slower Amiga can use smaller
 *     proxies (FBCache.proxy_width) and two machines can share one cache
 *     drawer without fighting over it.
 *   - **A copy is made only once.** It is written as "<key>.proxy1-640-part.mov"
 *     and renamed when it's complete, so a cached file that exists is
 *     always whole. Requests for a key that is queued, being made or
 *     already there do nothing.
 *   - Making a copy is an ordinary FBJob (one input, a small filtergraph,
 *     MJPEG and PCM out). Any backend can run it: the local one now, a
 *     render server later, writing into a cache drawer on a shared drive.
 *   - Next to each key there is "<key>.info", a short text saying which
 *     file it was made from, for people and for a later clean-up.
 *
 * Plain C99 and stdio, like the rest of the core. The app owns one FBCache
 * and a backend to run its jobs (see App_PollCache() in app.h).
 */
#ifndef FORMBROD_CACHE_H
#define FORMBROD_CACHE_H

#include "timeline.h"
#include "job.h"

#define FB_PROXY_WIDTH      640    /* default proxy width */
#define FB_PROXY_MIN_WIDTH  160
/* An intra-only source (MJPEG, DV, ProRes...) at most this wide is
 * already quick to read and gets no proxy. */
#define FB_PROXY_SKIP_WIDTH 1024
#define FB_CACHE_MAX_TASKS  (FB_MAX_SOURCES * 2)

typedef enum {
    FB_CACHE_PROXY = 0,
    FB_CACHE_NUM_KINDS
} FBCacheKind;

typedef enum {
    FB_CACHE_NONE = 0,   /* not wanted (a still picture, sound only...) */
    FB_CACHE_READY,      /* the copy is there */
    FB_CACHE_QUEUED,     /* waiting to be made */
    FB_CACHE_BUSY,       /* being made now */
    FB_CACHE_FAILED      /* making it failed; not tried again this run */
} FBCacheState;

typedef struct FBCacheTask {
    char         key[FB_KEY_LEN];
    FBCacheKind  kind;
    FBCacheState state;
    FBSource     source;   /* path and probed info, as when requested */
} FBCacheTask;

typedef struct FBCache {
    char        dir[FB_PATH_LEN];
    int         num_tasks;
    FBCacheTask tasks[FB_CACHE_MAX_TASKS];
    int         current;   /* task being made, or -1 */
    int         proxy_width;   /* proxies are at most this wide */
} FBCache;

/* The fingerprint of a file, as 16 hex digits. 0 on success, -1 if the
 * file can't be read. */
int  FB_CacheKey(const char *path, char key[FB_KEY_LEN]);

/* The same, for bytes in memory (for tests and for servers that compute
 * keys of uploads as they arrive). */
void FB_CacheKeyData(const void *data, size_t len, char key[FB_KEY_LEN]);

/* The recipe part of a cached file's name: "proxy1-640". */
void FB_CacheRecipe(const FBCache *c, FBCacheKind kind, char *out,
                    size_t len);

/* Whether a probed source is worth a proxy: it has a picture, isn't a
 * still, and isn't already small and intra-only. */
int  FB_CacheWantsProxy(const FBCache *c, const FBSource *src);

/* `proxy_width` 0 means FB_PROXY_WIDTH; it is made even and kept at
 * least FB_PROXY_MIN_WIDTH. */
void FB_CacheInit(FBCache *c, const char *dir, int proxy_width);

/* A proxy width that suits a computer, from its CPU clock in MHz and
 * number of cores (0 if unknown): 640 for an X1000/X5000, 480 for a
 * Sam460 or A1222, 320 for older G3/G4 machines. */
int  FB_CacheAutoWidth(int mhz, int cores);

/* The path of a cached file; `partial` gives the name it has while it's
 * being written. 0, or -1 if it doesn't fit. */
int  FB_CachePath(const FBCache *c, const char *key, FBCacheKind kind,
                  int partial, char *out, size_t len);

/* Whether the copy is there; if so (1) its path is put in `out`. */
int  FB_CacheFind(const FBCache *c, const char *key, FBCacheKind kind,
                  char *out, size_t len);

/* Asks for a proxy of a probed source whose `key` is set. Queues it
 * unless it's there, already queued or not wanted, and says which. */
FBCacheState FB_CacheRequest(FBCache *c, const FBSource *src);

/* The state of a key's copy, without queuing anything. */
FBCacheState FB_CacheState(const FBCache *c, const char *key,
                           FBCacheKind kind);

/* How many copies are waiting or being made. */
int  FB_CachePending(const FBCache *c);

/* Takes the next queued task, builds its job into `job` and marks it busy.
 * Returns the task's index, or -1 if nothing is waiting (or one is busy).
 * A task whose copy has meanwhile appeared (made by a server, say) is
 * marked ready and skipped. */
int  FB_CacheNextJob(FBCache *c, FBJob *job);

/* The job for one task: exported so a render server's tests and the app's
 * log can show it. 0, or -1 if a path doesn't fit. */
int  FB_CacheBuildJob(const FBCache *c, const FBCacheTask *task, FBJob *job);

/* Ends the busy task. With ok, the partial file is renamed to its real
 * name and the .info file written (READY); otherwise it is deleted
 * (FAILED). Returns the new state. */
FBCacheState FB_CacheFinish(FBCache *c, int ok);

/* Stops the busy task without counting it as failed (on quit, or when
 * the cache drawer changes): deletes the partial file and queues it
 * again. */
void FB_CacheAbort(FBCache *c);

#endif
