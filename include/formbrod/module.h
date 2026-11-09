/*
 * module.h - effect modules.
 *
 * A module turns one effect on one clip into an ffmpeg filter chain. Modules
 * that can do this are "ffmpeg-expressible" and can render on any backend,
 * including a remote server. Modules that need native code set
 * FB_MODF_NATIVE and force local rendering.
 *
 * For now modules are compiled in and registered at startup. Loadable
 * modules (shared objects in PROGDIR:Modules/) can come later behind the
 * same struct.
 */
#ifndef FORMBROD_MODULE_H
#define FORMBROD_MODULE_H

#include <stddef.h>
#include "timeline.h"

#define FB_MODULE_API 1

enum {
    FB_MODF_VIDEO   = 1 << 0,   /* produces a video filter chain */
    FB_MODF_AUDIO   = 1 << 1,   /* produces an audio filter chain */
    FB_MODF_NATIVE  = 1 << 2    /* needs native code, local only */
};

typedef struct FBParamDef {
    const char *name;
    const char *label;     /* shown in the GUI */
    double      min, max, def;
} FBParamDef;

typedef struct FBModule {
    int               api;          /* FB_MODULE_API */
    const char       *name;         /* unique id, used in jobs and ARexx */
    const char       *label;        /* human-readable */
    unsigned          flags;
    int               num_params;
    const FBParamDef *params;

    /* Write the filter chain for this effect into buf, without labels, e.g.
     * "fade=t=in:st=0:d=1". `clip_len` is the clip's length in ms. Return
     * 0 on success, -1 if buf is too small. Either may be NULL if the
     * module doesn't handle that stream type. */
    int (*video_filter)(const FBEffect *fx, fb_ms_t clip_len, char *buf, size_t len);
    int (*audio_filter)(const FBEffect *fx, fb_ms_t clip_len, char *buf, size_t len);
} FBModule;

int             FB_RegisterModule(const FBModule *mod);
const FBModule *FB_FindModule(const char *name);
int             FB_NumModules(void);
const FBModule *FB_GetModule(int index);

/* Registers every built-in module. Call once at startup. */
void            FB_RegisterBuiltinModules(void);

#endif
