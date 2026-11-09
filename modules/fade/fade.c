/*
 * fade - fade a clip in from black and/or out to black, with matching audio.
 *
 * This is the reference module: copy it to start a new effect. It only
 * writes ffmpeg filter strings, so it renders on any backend.
 *
 * Parameters (seconds):
 *   in   length of the fade-in at the start of the clip (0 = none)
 *   out  length of the fade-out at the end of the clip  (0 = none)
 */
#include <stdio.h>
#include "formbrod/module.h"

static const FBParamDef fade_params[] = {
    { "in",  "Fade in (s)",  0.0, 10.0, 1.0 },
    { "out", "Fade out (s)", 0.0, 10.0, 0.0 },
};

static int build(const FBEffect *fx, fb_ms_t clip_len, char *buf, size_t len,
                 const char *filter)
{
    double fin   = FB_EffectGetParam(fx, "in",  fade_params[0].def);
    double fout  = FB_EffectGetParam(fx, "out", fade_params[1].def);
    double clip  = clip_len / 1000.0;
    int n = 0;

    buf[0] = '\0';
    if (fin > 0)
        n = snprintf(buf, len, "%s=t=in:st=0:d=%.3f", filter, fin);
    if (n < 0 || (size_t)n >= len)
        return -1;
    if (fout > 0) {
        double st = clip - fout;
        int m;

        if (st < 0)
            st = 0;
        m = snprintf(buf + n, len - n, "%s%s=t=out:st=%.3f:d=%.3f",
                     n ? "," : "", filter, st, fout);
        if (m < 0 || (size_t)m >= len - n)
            return -1;
    }
    return 0;
}

static int fade_video(const FBEffect *fx, fb_ms_t clip_len, char *buf, size_t len)
{
    return build(fx, clip_len, buf, len, "fade");
}

static int fade_audio(const FBEffect *fx, fb_ms_t clip_len, char *buf, size_t len)
{
    return build(fx, clip_len, buf, len, "afade");
}

const FBModule FB_Module_Fade = {
    FB_MODULE_API,
    "fade",
    "Fade in/out",
    FB_MODF_VIDEO | FB_MODF_AUDIO,
    sizeof(fade_params) / sizeof(fade_params[0]),
    fade_params,
    fade_video,
    fade_audio
};
