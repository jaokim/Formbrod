/*
 * thumbs.h - film-strip thumbnails.
 *
 * Every source with a picture gets a *thumbnail strip* in the cache drawer
 * (FB_CACHE_THUMBS, see cache.h): small frames of it, one per step, one
 * after the other as raw ARGB (8 bits each, in that byte order, the same
 * on every machine), FB_THUMB_W x FB_THUMB_H unless the film strip's
 * cells are another size. Frame k is the picture at k * step in the file;
 * a still picture's strip holds one frame. There is no header: the number
 * of frames is the file's size divided by the size of one.
 *
 * Making a strip is an ordinary job (fps, scale, crop, rawvideo out), so a
 * render server can make it as well. It is made from the proxy when there
 * is one: an MJPEG proxy is quick to read, the original usually isn't.
 *
 * The film strip draws from an FBThumbStore: a few hundred thumbnails in
 * memory, read from the strips as they're needed. Drawing may happen in
 * Intuition's input task, which must not do file I/O, so a lookup that
 * misses only notes what is wanted; FB_ThumbsLoad(), from the program's
 * own process, reads those and the film strip is drawn again.
 *
 * Plain C99 and stdio, like the rest of the core. The store's lock and
 * wake hooks are where the program puts its semaphore and signal.
 */
#ifndef FORMBROD_THUMBS_H
#define FORMBROD_THUMBS_H

#include "timeline.h"
#include "timeview.h"   /* fb_us_t */

struct FBCache;

/* The film strip's cells are 88 x 48 with a pixel on either side. */
#define FB_THUMB_W           86
#define FB_THUMB_H           48
/* One thumbnail a second; long files get fewer, so a strip has at most
 * FB_THUMB_MAX_FRAMES (an hour at 86 x 48 is under 20 MB). */
#define FB_THUMB_STEP_MS     1000
#define FB_THUMB_MAX_FRAMES  1200

#define FB_THUMBS_SLOTS      128    /* thumbnails kept in memory (2 MB) */
#define FB_THUMBS_MAX_STRIPS (FB_MAX_SOURCES * 2)
#define FB_THUMBS_MAX_WANTS  64

/* The step between thumbnails of a file `duration` ms long: a whole
 * number of seconds. Changing this means a new recipe ("thumbs2"). */
fb_ms_t FB_ThumbStep(fb_ms_t duration);

/* The frame of a strip with that step for time t in the file: the
 * nearest one. Never negative; past the end is up to the reader. */
long    FB_ThumbIndex(fb_ms_t step, fb_us_t t);

/* What is known about a source's strip. */
typedef enum {
    FB_STRIP_UNKNOWN = 0,   /* not looked for yet */
    FB_STRIP_MISSING,       /* not there (yet); waiting for it to be made */
    FB_STRIP_READY,         /* there, with `count` frames */
    FB_STRIP_NONE           /* won't be: making it failed */
} FBStripState;

typedef enum {
    FB_THUMB_HIT = 0,       /* here it is; the store is locked */
    FB_THUMB_WAIT,          /* not in memory yet, or its strip isn't made */
    FB_THUMB_NONE           /* there won't be one */
} FBThumbLook;

typedef struct FBThumbStrip {
    char         key[FB_KEY_LEN];
    FBStripState state;
    long         count;
} FBThumbStrip;

typedef struct FBThumbSlot {
    char          key[FB_KEY_LEN];   /* "" = free */
    long          index;
    unsigned long used;              /* for dropping the oldest */
} FBThumbSlot;

typedef struct FBThumbWant {
    char key[FB_KEY_LEN];
    long index;
} FBThumbWant;

typedef struct FBThumbStore {
    int            w, h;
    int            num_slots;
    FBThumbSlot   *slots;
    unsigned char *pixels;           /* num_slots thumbnails, w * h * 4 */
    unsigned long  clock;
    int            num_strips;
    FBThumbStrip   strips[FB_THUMBS_MAX_STRIPS];
    int            num_wants;
    FBThumbWant    wants[FB_THUMBS_MAX_WANTS];
    /* Optional. lock/unlock guard the store between the drawing task
     * and the loading one; wake is called (store locked) when a lookup
     * adds a want, so the program knows to call FB_ThumbsLoad(). */
    void         (*lock)(void *data);
    void         (*unlock)(void *data);
    void         (*wake)(void *data);
    void          *hook_data;
} FBThumbStore;

/* Thumbnails w x h, `slots` of them in memory (0 = FB_THUMBS_SLOTS).
 * 0, or -1 without memory. Init again to change the size (Free first);
 * the hooks are kept. */
int  FB_ThumbsInit(FBThumbStore *s, int w, int h, int slots);
void FB_ThumbsFree(FBThumbStore *s);

/* Looks up frame `index` of the strip of `key`. On FB_THUMB_HIT `*pixels`
 * points to it (w * h ARGB pixels, w * 4 bytes a row) and the store stays
 * locked until FB_ThumbsUnlock(). Otherwise nothing is held; a miss is
 * noted for FB_ThumbsLoad(). Never touches files. */
FBThumbLook FB_ThumbsLock(FBThumbStore *s, const char *key, long index,
                          const unsigned char **pixels);
void FB_ThumbsUnlock(FBThumbStore *s);

/* How many lookups are waiting for FB_ThumbsLoad(). */
int  FB_ThumbsWanted(FBThumbStore *s);

/* Reads what lookups missed from the strips in the cache drawer. Returns
 * how many thumbnails came into memory (draw the film strip again if
 * any did). Call from a process: it reads files. */
int  FB_ThumbsLoad(FBThumbStore *s, const struct FBCache *c);

/* A strip has just been made (or made again): forget what was known
 * about it, so the next lookup reads it. */
void FB_ThumbsReset(FBThumbStore *s, const char *key);
/* Making a strip failed: its lookups say FB_THUMB_NONE from now on. */
void FB_ThumbsSetNone(FBThumbStore *s, const char *key);

#endif
