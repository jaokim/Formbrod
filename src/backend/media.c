/*
 * media.c - probe a media file for duration and streams.
 */
#include "app.h"

#ifdef HAVE_LIBAV
#include <libavformat/avformat.h>

int FB_ProbeMedia(const char *path, fb_ms_t *duration, int *has_video,
                  int *has_audio)
{
    AVFormatContext *fmt = NULL;
    unsigned i;

    *duration = 0;
    *has_video = *has_audio = 0;

    if (avformat_open_input(&fmt, path, NULL, NULL) < 0)
        return -1;
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        return -1;
    }
    if (fmt->duration != AV_NOPTS_VALUE)
        *duration = fmt->duration / (AV_TIME_BASE / 1000);
    for (i = 0; i < fmt->nb_streams; i++) {
        enum AVMediaType t = fmt->streams[i]->codecpar->codec_type;

        if (t == AVMEDIA_TYPE_VIDEO)
            *has_video = 1;
        else if (t == AVMEDIA_TYPE_AUDIO)
            *has_audio = 1;
    }
    avformat_close_input(&fmt);
    return 0;
}

#else

int FB_ProbeMedia(const char *path, fb_ms_t *duration, int *has_video,
                  int *has_audio)
{
    (void)path;
    *duration = 0;
    *has_video = *has_audio = 0;
    return -1;
}

#endif
