/*
 * waves.h - waveforms for the sound lanes.
 *
 * Every source with sound gets a *peak file* in the cache drawer
 * (FB_CACHE_WAVE, see cache.h): one byte per 1/FB_WAVE_RATE s, the
 * loudest sample in that stretch (0 = silence, 255 = full scale), with
 * the channels mixed to one. Nothing else is in the file; byte k is the
 * stretch from k / FB_WAVE_RATE s. An hour is 180 KB.
 *
 * It is made by an ordinary job, so a render server can make it too:
 * ffmpeg's showvolume draws the peak of each stretch as a bar, and
 * averaging the bar's picture to one pixel turns it back into a number
 * (see build_wave_job() in cache.c). The job's
 * output is raw grey "video" at FB_WAVE_RATE frames a second, one pixel
 * each. It is read from the proxy when there is one (its sound is plain
 * PCM, so there is nothing to decode).
 *
 * The lanes draw from an FBWaveStore: whole peak files in memory, read
 * when they're first needed. As with thumbnails (thumbs.h), drawing may
 * run in Intuition's input task, so a lookup only notes what's missing
 * and FB_WavesLoad() reads it from the program's own process.
 *
 * Plain C99 and stdio.
 */
#ifndef FORMBROD_WAVES_H
#define FORMBROD_WAVES_H

#include "timeline.h"
#include "timeview.h"
#include "thumbs.h"   /* FBStripState, FBThumbLook */

struct FBCache;

#define FB_WAVE_RATE        50          /* peaks a second: 20 ms each */
#define FB_WAVES_MAX        (FB_MAX_SOURCES * 2)
/* Peak files beyond this much memory in all aren't loaded (about 45
 * hours of sound); their lanes stay plain. */
#define FB_WAVES_MAX_BYTES  (8L * 1024 * 1024)

/* The peaks for `n` pixel columns, the first starting at time t0 in the
 * file and each `step` long: the loudest peak each covers (at least the
 * one it starts in). Columns before the start or past the end get 0. */
void FB_WaveColumns(const unsigned char *peaks, long count, fb_us_t t0,
                    fb_us_t step, int n, unsigned char *out);

typedef struct FBWave {
    char           key[FB_KEY_LEN];
    FBStripState   state;
    unsigned char *peaks;
    long           count;
} FBWave;

typedef struct FBWaveStore {
    int     num;
    FBWave  waves[FB_WAVES_MAX];
    long    bytes;                     /* peaks in memory, all files */
    int     num_wants;
    char    wants[FB_WAVES_MAX][FB_KEY_LEN];
    /* Optional, as in FBThumbStore. */
    void  (*lock)(void *data);
    void  (*unlock)(void *data);
    void  (*wake)(void *data);
    void   *hook_data;
} FBWaveStore;

/* Frees every peak file in memory and forgets them; the hooks are kept.
 * A store that was zeroed (or freed) is ready to use. */
void FB_WavesFree(FBWaveStore *s);

/* Looks up the peaks of `key`. On FB_THUMB_HIT `*peaks` and `*count` are
 * set and the store stays locked until FB_WavesUnlock(). Otherwise
 * nothing is held; a file not read yet is noted for FB_WavesLoad().
 * Never touches files. */
FBThumbLook FB_WavesLock(FBWaveStore *s, const char *key,
                         const unsigned char **peaks, long *count);
void FB_WavesUnlock(FBWaveStore *s);

int  FB_WavesWanted(FBWaveStore *s);

/* Reads the peak files lookups asked for. Returns how many came into
 * memory (draw the lanes again if any did). Call from a process. */
int  FB_WavesLoad(FBWaveStore *s, const struct FBCache *c);

/* The peak file was just made: read it again at the next lookup. */
void FB_WavesReset(FBWaveStore *s, const char *key);
/* Making it failed: lookups say FB_THUMB_NONE from now on. */
void FB_WavesSetNone(FBWaveStore *s, const char *key);

#endif
