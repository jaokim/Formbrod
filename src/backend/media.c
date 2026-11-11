/*
 * media.c - probe a media file for duration, streams and video format.
 */
#include <string.h>
#include "app.h"

#ifdef HAVE_LIBAV
#include <libavformat/avformat.h>
#include <libavutil/error.h>

/* Before ffmpeg 4.0 (libavformat 58.9.100) demuxers had to be registered,
 * or avformat_open_input() fails for every file. Older AmigaOS 4 ports of
 * ffmpeg are in that range. */
static void ensure_registered(void)
{
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
    static int done;

    if (!done) {
        av_register_all();
        done = 1;
    }
#endif
}

static void set_why(char *why, size_t len, int averr)
{
    if (!why || !len)
        return;
    if (av_strerror(averr, why, len) < 0) {
        strncpy(why, "unknown error", len - 1);
        why[len - 1] = '\0';
    }
}

FBProbeResult FB_ProbeMedia(const char *path, FBSource *info,
                            char *why, size_t why_len)
{
    AVFormatContext *fmt = NULL;
    fb_ms_t *duration = &info->duration;
    int *has_video = &info->has_video, *has_audio = &info->has_audio;
    int best;
    unsigned i;
    int err;

    memset(info, 0, sizeof(*info));
    if (why && why_len)
        why[0] = '\0';

    ensure_registered();

    if ((err = avformat_open_input(&fmt, path, NULL, NULL)) < 0) {
        set_why(why, why_len, err);
        return FB_PROBE_CANT_OPEN;
    }
    if ((err = avformat_find_stream_info(fmt, NULL)) < 0) {
        set_why(why, why_len, err);
        avformat_close_input(&fmt);
        return FB_PROBE_CANT_OPEN;
    }
    if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0)
        *duration = fmt->duration / (AV_TIME_BASE / 1000);
    for (i = 0; i < fmt->nb_streams; i++) {
        AVStream *st = fmt->streams[i];
        enum AVMediaType t = st->codecpar->codec_type;

        if (t == AVMEDIA_TYPE_VIDEO)
            *has_video = 1;
        else if (t == AVMEDIA_TYPE_AUDIO)
            *has_audio = 1;

        /* Some containers only know the length per stream. */
        if (*duration <= 0 && st->duration != AV_NOPTS_VALUE &&
            st->duration > 0)
            *duration = av_rescale_q(st->duration, st->time_base,
                                     (AVRational){ 1, 1000 });
    }

    /* The video format, from the stream the render backend picks for
     * [n:v] (see find_stream() in local_backend.c). */
    if ((best = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1,
                                    NULL, 0)) >= 0) {
        AVStream *st = fmt->streams[best];
        AVRational sar = av_guess_sample_aspect_ratio(fmt, st, NULL);
        AVRational fr  = av_guess_frame_rate(fmt, st, NULL);

        FB_SourceSetVideo(info, st->codecpar->width, st->codecpar->height,
                          sar.num, sar.den, fr.num, fr.den);
    }
    avformat_close_input(&fmt);

    /* A sound-only file (music) is fine: it goes on a sound track. */
    if (!*has_video && !*has_audio)
        return FB_PROBE_NO_VIDEO;
    if (*duration <= 0)
        return FB_PROBE_NO_DURATION;
    return FB_PROBE_OK;
}

#else

FBProbeResult FB_ProbeMedia(const char *path, FBSource *info,
                            char *why, size_t why_len)
{
    (void)path;
    memset(info, 0, sizeof(*info));
    if (why && why_len)
        why[0] = '\0';
    return FB_PROBE_NO_LIBAV;
}

#endif
