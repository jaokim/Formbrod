/*
 * local_backend.c - render on the Amiga with the linked libav* libraries.
 *
 * Progress() is called from the GUI loop and does a bounded slice of work
 * (decode -> filtergraph -> encode for a few packets) each time, so the GUI
 * stays responsive without a separate render task. If this turns out to be
 * too choppy, move the loop into a child process and keep the same API.
 *
 * Built without HAVE_LIBAV, Submit() fails with a clear message so the rest
 * of the app can be developed and tested without the libraries.
 *
 * How a job is rendered
 * ---------------------
 *  Submit():
 *   - every input is opened; the graph's open input pads ("[0:v]", "[1:a]",
 *     "[0:v:0]" ...) decide which streams get a decoder. A pad that appears
 *     more than once (two clips from one source) gets one buffer source per
 *     use, and each decoded frame is pushed into all of them.
 *   - the job's graph gets a tail appended that converts [video_out] /
 *     [audio_out] to formats the encoders accept, ending in buffersink /
 *     abuffersink. Output pads the job doesn't use go to null sinks.
 *   - encoders are set up from the configured sinks (size, rate, layout),
 *     and the output header is written.
 *  Progress():
 *   - drains the sinks into the encoders and the muxer, then reads one
 *     packet from the input the graph is starving for (the one whose
 *     buffer sources report the most failed requests; ties go to the
 *     lowest-numbered input, which matches how concat consumes them). This
 *     repeats for a few packets per call.
 *   - at the end of an input its decoders are flushed and its sources
 *     closed. When every sink has reached EOF the encoders are flushed and
 *     the trailer is written.
 *
 * Library versions: written for ffmpeg 4.4 and later, using only the
 * send/receive decode and encode API. Two API changes are handled with
 * version checks below:
 *   - ffmpeg 5.1 (libavcodec 59.24) replaced channel_layout/channels with
 *     AVChannelLayout (ch_layout); the old fields were removed in 7.0.
 *   - ffmpeg 7.1 (libavcodec 61.13) added avcodec_get_supported_config()
 *     and deprecated AVCodec.pix_fmts, .sample_fmts and friends.
 * Only the 5.1 .. 7.0 branches have been compiled and run so far (against
 * ffmpeg 6.1); the 4.4 and 7.1+ branches are written from the docs.
 * Format conversion for the encoders is done with "format"/"aformat"
 * filters rather than buffersink options, because the buffersink
 * "pix_fmts"/"sample_fmts" options are deprecated in newer versions.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "formbrod/backend.h"

#ifdef HAVE_LIBAV
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
#define FB_CH_LAYOUT 1          /* AVChannelLayout API (ffmpeg 5.1+) */
#endif
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
#define FB_SUPPORTED_CONFIG 1   /* avcodec_get_supported_config (7.1+) */
#endif

/* Packets read per Progress() call. Small enough that a slow machine
 * still gets back to the GUI often, large enough not to waste time. */
#define PACKETS_PER_SLICE 8
#define MAX_PAD_USES      32    /* buffer sources fed by one stream */

enum { KIND_VIDEO = 0, KIND_AUDIO = 1, NUM_KINDS = 2 };

/* One decoded stream of an input, feeding one or more buffer sources. */
typedef struct InStream {
    int              index;          /* stream index, -1 = not used */
    AVCodecContext  *dec;
    AVFilterContext *src[MAX_PAD_USES];
    int              num_src;
    int64_t          ts_offset;      /* subtracted from pts (stream tb) */
} InStream;

typedef struct Input {
    AVFormatContext *fmt;
    InStream         st[NUM_KINDS];
    int              eof;
} Input;

/* One output stream: sink -> encoder -> muxer stream. */
typedef struct OutStream {
    AVFilterContext *sink;
    AVCodecContext  *enc;
    AVStream        *st;
    int              eof;            /* sink reached EOF, encoder flushed */
    int64_t          last_pts;       /* in encoder time base */
} OutStream;
#endif

typedef struct LocalPriv {
    FBJob      job;
    FBProgress progress;
    FBResult   result;
#ifdef HAVE_LIBAV
    Input            in[FB_MAX_SOURCES];
    AVFilterGraph   *graph;
    AVFormatContext *out;
    OutStream        os[NUM_KINDS];
    int              file_opened;    /* the output file exists on disk */
    AVPacket        *pkt;
    AVFrame         *frame;
#endif
} LocalPriv;

static void finish(LocalPriv *p, FBRenderState state, const char *msg)
{
    p->progress.state = state;
    p->result.state = state;
    strncpy(p->result.message, msg, sizeof(p->result.message) - 1);
    p->result.message[sizeof(p->result.message) - 1] = '\0';
}

#ifdef HAVE_LIBAV

/* ---- Helpers ------------------------------------------------------------ */

static const char *kind_name(int kind)
{
    return kind == KIND_VIDEO ? "video" : "audio";
}

/* Frees everything libav-related. Deletes the output file unless keep. */
static void release(LocalPriv *p, int keep_output)
{
    int i, k;

    for (k = 0; k < NUM_KINDS; k++) {
        avcodec_free_context(&p->os[k].enc);
        p->os[k].sink = NULL;
        p->os[k].st = NULL;
    }
    /* Freeing the graph frees every filter context in it. */
    avfilter_graph_free(&p->graph);
    for (i = 0; i < FB_MAX_SOURCES; i++) {
        Input *in = &p->in[i];

        for (k = 0; k < NUM_KINDS; k++)
            avcodec_free_context(&in->st[k].dec);
        avformat_close_input(&in->fmt);
    }
    if (p->out) {
        if (p->file_opened && !(p->out->oformat->flags & AVFMT_NOFILE))
            avio_closep(&p->out->pb);
        avformat_free_context(p->out);
        p->out = NULL;
    }
    if (p->file_opened && !keep_output)
        remove(p->job.output.path);
    p->file_opened = 0;
    av_packet_free(&p->pkt);
    av_frame_free(&p->frame);
    memset(p->in, 0, sizeof(p->in));
    memset(p->os, 0, sizeof(p->os));
}

/* Finishes with FAILED and a message like "Can't open x: No such file". */
static int fail(LocalPriv *p, int averr, const char *fmt, ...)
{
    char msg[sizeof(p->result.message)];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (averr < 0 && n >= 0 && (size_t)n < sizeof(msg)) {
        char err[AV_ERROR_MAX_STRING_SIZE];

        av_strerror(averr, err, sizeof(err));
        snprintf(msg + n, sizeof(msg) - n, ": %s", err);
    }
    release(p, 0);
    finish(p, FB_RENDER_FAILED, msg);
    return -1;
}

/* Parses an input pad label: "0:v", "1:a", "0:v:0". Returns the input
 * number and fills kind and the n-th-of-type index, or -1. */
static int parse_pad(const char *name, int *kind, int *nth)
{
    char *end;
    long in = strtol(name, &end, 10);

    if (end == name || *end != ':' || in < 0)
        return -1;
    end++;
    if (*end == 'v')
        *kind = KIND_VIDEO;
    else if (*end == 'a')
        *kind = KIND_AUDIO;
    else
        return -1;
    end++;
    *nth = 0;
    if (*end == ':') {
        char *e2;

        *nth = (int)strtol(end + 1, &e2, 10);
        if (e2 == end + 1 || *e2 || *nth < 0)
            return -1;
    } else if (*end) {
        return -1;
    }
    return (int)in;
}

/* Index of the nth stream of a kind, preferring the "best" one for 0. */
static int find_stream(AVFormatContext *fmt, int kind, int nth)
{
    enum AVMediaType type = kind == KIND_VIDEO ? AVMEDIA_TYPE_VIDEO
                                               : AVMEDIA_TYPE_AUDIO;
    unsigned i;

    if (nth == 0)
        return av_find_best_stream(fmt, type, -1, -1, NULL, 0);
    for (i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->codecpar->codec_type == type && nth-- == 0)
            return (int)i;
    return -1;
}

/* Writes the channel layout of a decoder or encoder as a string. */
static void layout_string(const AVCodecContext *c, char *buf, size_t len)
{
#ifdef FB_CH_LAYOUT
    AVChannelLayout def;

    if (c->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        av_channel_layout_default(&def, c->ch_layout.nb_channels);
        av_channel_layout_describe(&def, buf, len);
        av_channel_layout_uninit(&def);
    } else {
        av_channel_layout_describe(&c->ch_layout, buf, len);
    }
#else
    uint64_t layout = c->channel_layout;

    if (!layout)
        layout = (uint64_t)av_get_default_channel_layout(c->channels);
    av_get_channel_layout_string(buf, (int)len, 0, layout);
#endif
}

/* Lists of formats an encoder accepts. NULL means "anything". */
static const enum AVPixelFormat *enc_pix_fmts(const AVCodecContext *c,
                                              const AVCodec *codec)
{
#ifdef FB_SUPPORTED_CONFIG
    const void *list = NULL;

    if (avcodec_get_supported_config(c, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                     &list, NULL) < 0)
        return NULL;
    return list;
#else
    (void)c;
    return codec->pix_fmts;
#endif
}

static const enum AVSampleFormat *enc_sample_fmts(const AVCodecContext *c,
                                                  const AVCodec *codec)
{
#ifdef FB_SUPPORTED_CONFIG
    const void *list = NULL;

    if (avcodec_get_supported_config(c, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                     0, &list, NULL) < 0)
        return NULL;
    return list;
#else
    (void)c;
    return codec->sample_fmts;
#endif
}

static const int *enc_sample_rates(const AVCodecContext *c,
                                   const AVCodec *codec)
{
#ifdef FB_SUPPORTED_CONFIG
    const void *list = NULL;

    if (avcodec_get_supported_config(c, codec, AV_CODEC_CONFIG_SAMPLE_RATE,
                                     0, &list, NULL) < 0)
        return NULL;
    return list;
#else
    (void)c;
    return codec->supported_samplerates;
#endif
}

/* Picks a channel layout the encoder accepts, as an aformat string.
 * Leaves buf empty when the encoder takes any layout. Prefers stereo. */
static void enc_layout(const AVCodecContext *c, const AVCodec *codec,
                       char *buf, size_t len)
{
#ifdef FB_CH_LAYOUT
    const AVChannelLayout *list;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    int i;

#ifdef FB_SUPPORTED_CONFIG
    const void *l = NULL;

    if (avcodec_get_supported_config(c, codec,
                                     AV_CODEC_CONFIG_CHANNEL_LAYOUT, 0,
                                     &l, NULL) < 0)
        l = NULL;
    list = l;
#else
    (void)c;
    list = codec->ch_layouts;
#endif
    buf[0] = '\0';
    if (!list || !list[0].nb_channels)
        return;
    for (i = 0; list[i].nb_channels; i++) {
        if (!av_channel_layout_compare(&list[i], &stereo)) {
            av_channel_layout_describe(&stereo, buf, len);
            return;
        }
    }
    av_channel_layout_describe(&list[0], buf, len);
#else
    const uint64_t *list = codec->channel_layouts;
    int i;

    (void)c;
    buf[0] = '\0';
    if (!list || !list[0])
        return;
    for (i = 0; list[i]; i++) {
        if (list[i] == AV_CH_LAYOUT_STEREO) {
            av_get_channel_layout_string(buf, (int)len, 0,
                                         AV_CH_LAYOUT_STEREO);
            return;
        }
    }
    av_get_channel_layout_string(buf, (int)len, 0, list[0]);
#endif
}

/* Finds the encoder named in the job, else the container's default for
 * the media type, else one of the fallbacks. */
static const AVCodec *pick_encoder(const char *name, const AVOutputFormat *of,
                                   int kind)
{
    static const enum AVCodecID vfall[] = {
        AV_CODEC_ID_MPEG4, AV_CODEC_ID_MJPEG, AV_CODEC_ID_NONE };
    static const enum AVCodecID afall[] = {
        AV_CODEC_ID_AAC, AV_CODEC_ID_MP2, AV_CODEC_ID_PCM_S16LE,
        AV_CODEC_ID_NONE };
    const enum AVCodecID *fall = kind == KIND_VIDEO ? vfall : afall;
    enum AVCodecID def = kind == KIND_VIDEO ? of->video_codec : of->audio_codec;
    const AVCodec *c = NULL;
    int i;

    if (name && name[0]) {
        c = avcodec_find_encoder_by_name(name);
        if (c && avformat_query_codec(of, c->id, FF_COMPLIANCE_NORMAL) == 0)
            c = NULL;   /* the container definitely can't hold it */
        if (c)
            return c;
    }
    if (def != AV_CODEC_ID_NONE && (c = avcodec_find_encoder(def)) != NULL)
        return c;
    for (i = 0; fall[i] != AV_CODEC_ID_NONE; i++)
        if (avformat_query_codec(of, fall[i], FF_COMPLIANCE_NORMAL) != 0
            && (c = avcodec_find_encoder(fall[i])) != NULL)
            return c;
    return NULL;
}

/* ---- Submit ------------------------------------------------------------- */

static int open_inputs(LocalPriv *p)
{
    int i, ret;

    for (i = 0; i < p->job.num_inputs; i++) {
        Input *in = &p->in[i];

        in->st[KIND_VIDEO].index = in->st[KIND_AUDIO].index = -1;
        ret = avformat_open_input(&in->fmt, p->job.inputs[i], NULL, NULL);
        if (ret < 0)
            return fail(p, ret, "Can't open %s", p->job.inputs[i]);
        ret = avformat_find_stream_info(in->fmt, NULL);
        if (ret < 0)
            return fail(p, ret, "Can't read %s", p->job.inputs[i]);
    }
    return 0;
}

/* Opens the decoder for one input stream the graph uses. */
static int open_decoder(LocalPriv *p, int input, int kind, int nth)
{
    Input *in = &p->in[input];
    InStream *is = &in->st[kind];
    AVStream *st;
    const AVCodec *codec;
    int idx, ret;

    idx = find_stream(in->fmt, kind, nth);
    if (idx < 0)
        return fail(p, 0, "%s has no %s stream", p->job.inputs[input],
                    kind_name(kind));
    if (is->dec) {
        if (is->index != idx)
            return fail(p, 0, "Only one %s stream per input is supported (%s)",
                        kind_name(kind), p->job.inputs[input]);
        return 0;
    }
    st = in->fmt->streams[idx];
    codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec)
        return fail(p, 0, "No decoder for the %s in %s (%s)", kind_name(kind),
                    p->job.inputs[input],
                    avcodec_get_name(st->codecpar->codec_id));
    is->dec = avcodec_alloc_context3(codec);
    if (!is->dec)
        return fail(p, AVERROR(ENOMEM), "Out of memory");
    ret = avcodec_parameters_to_context(is->dec, st->codecpar);
    if (ret < 0)
        return fail(p, ret, "Can't set up the %s decoder", kind_name(kind));
    is->dec->pkt_timebase = st->time_base;
    if (kind == KIND_VIDEO)
        is->dec->framerate = av_guess_frame_rate(in->fmt, st, NULL);
    ret = avcodec_open2(is->dec, codec, NULL);
    if (ret < 0)
        return fail(p, ret, "Can't open the %s decoder for %s",
                    kind_name(kind), p->job.inputs[input]);
    is->index = idx;
    /* Like the ffmpeg CLI, start every input at 0 so trim times match. */
    if (in->fmt->start_time != AV_NOPTS_VALUE)
        is->ts_offset = av_rescale_q(in->fmt->start_time, AV_TIME_BASE_Q,
                                     st->time_base);
    return 0;
}

/* Creates a buffer/abuffer source for a stream and links it to a pad. */
static int add_source(LocalPriv *p, int input, int kind, AVFilterInOut *pad)
{
    InStream *is = &p->in[input].st[kind];
    AVStream *st = p->in[input].fmt->streams[is->index];
    AVCodecContext *d = is->dec;
    AVFilterContext *src;
    char args[512], name[64];
    int ret;

    if (is->num_src >= MAX_PAD_USES)
        return fail(p, 0, "Input %d is used too many times", input);
    snprintf(name, sizeof(name), "src_%d_%s_%d", input, kind_name(kind),
             is->num_src);

    if (kind == KIND_VIDEO) {
        AVRational fr = av_guess_frame_rate(p->in[input].fmt, st, NULL);
        AVRational sar = d->sample_aspect_ratio;

        if (d->pix_fmt == AV_PIX_FMT_NONE || d->width <= 0)
            return fail(p, 0, "Unknown video format in %s",
                        p->job.inputs[input]);
        if (sar.num <= 0 || sar.den <= 0)
            sar = (AVRational){ 0, 1 };
        snprintf(args, sizeof(args),
                 "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:"
                 "pixel_aspect=%d/%d",
                 d->width, d->height, (int)d->pix_fmt, st->time_base.num,
                 st->time_base.den, sar.num, sar.den);
        if (fr.num > 0 && fr.den > 0) {
            size_t n = strlen(args);

            snprintf(args + n, sizeof(args) - n, ":frame_rate=%d/%d",
                     fr.num, fr.den);
        }
    } else {
        char layout[128];

        if (d->sample_fmt == AV_SAMPLE_FMT_NONE || d->sample_rate <= 0)
            return fail(p, 0, "Unknown audio format in %s",
                        p->job.inputs[input]);
        layout_string(d, layout, sizeof(layout));
        snprintf(args, sizeof(args),
                 "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:"
                 "channel_layout=%s",
                 st->time_base.num, st->time_base.den, d->sample_rate,
                 av_get_sample_fmt_name(d->sample_fmt), layout);
    }

    ret = avfilter_graph_create_filter(&src,
              avfilter_get_by_name(kind == KIND_VIDEO ? "buffer" : "abuffer"),
              name, args, NULL, p->graph);
    if (ret < 0)
        return fail(p, ret, "Can't create the %s source for input %d",
                    kind_name(kind), input);
    ret = avfilter_link(src, 0, pad->filter_ctx, pad->pad_idx);
    if (ret < 0)
        return fail(p, ret, "Can't connect [%s]", pad->name);
    is->src[is->num_src++] = src;
    return 0;
}

/* Frame rate of the first video input pad in the graph ("[n:v..."), which
 * is what concat passes on. {0,1} if unknown. */
static AVRational first_video_rate(LocalPriv *p)
{
    const char *g = p->job.filtergraph;
    AVRational none = { 0, 1 };

    while ((g = strchr(g, '[')) != NULL) {
        int kind, nth, input;
        char name[32];
        size_t n = strcspn(g + 1, "]");

        g++;
        if (n >= sizeof(name))
            continue;
        memcpy(name, g, n);
        name[n] = '\0';
        input = parse_pad(name, &kind, &nth);
        if (input >= 0 && input < p->job.num_inputs && kind == KIND_VIDEO) {
            int idx = find_stream(p->in[input].fmt, kind, nth);

            if (idx < 0)
                return none;
            return av_guess_frame_rate(p->in[input].fmt,
                                       p->in[input].fmt->streams[idx], NULL);
        }
    }
    return none;
}

/* Builds the tail that adapts [label] to what the encoder takes and ends
 * in a pad called _fb_v/_fb_a, which gets the sink. */
static int encoder_tail(LocalPriv *p, int kind, const AVCodec *codec,
                        AVCodecContext *enc, char *buf, size_t len)
{
    const char *label = kind == KIND_VIDEO ? p->job.video_out
                                           : p->job.audio_out;
    int n;

    if (kind == KIND_VIDEO) {
        const enum AVPixelFormat *fmts = enc_pix_fmts(enc, codec);
        enum AVPixelFormat pf = AV_PIX_FMT_YUV420P;
        char scale[64] = "", fps[48] = "";
        AVRational fr = first_video_rate(p);
        int i, ok = !fmts;

        for (i = 0; fmts && fmts[i] != AV_PIX_FMT_NONE; i++)
            if (fmts[i] == AV_PIX_FMT_YUV420P)
                ok = 1;
        if (!ok)
            pf = fmts[0];
        /* FB_JobBuild() already scales; this is for hand-written jobs. */
        if (p->job.output.width > 0)
            snprintf(scale, sizeof(scale), "scale=%d:%d,",
                     p->job.output.width,
                     p->job.output.height ? p->job.output.height : -2);
        /* Constant frame rate output: clips with other rates (or variable
         * rate sources) get frames duplicated or dropped, as the ffmpeg
         * CLI does for most containers. */
        if (fr.num > 0 && fr.den > 0)
            snprintf(fps, sizeof(fps), "fps=%d/%d,", fr.num, fr.den);
        n = snprintf(buf, len, ";[%s]%s%sformat=pix_fmts=%s[_fb_v]", label,
                     scale, fps, av_get_pix_fmt_name(pf));
    } else {
        const enum AVSampleFormat *fmts = enc_sample_fmts(enc, codec);
        const int *rates = enc_sample_rates(enc, codec);
        char layout[128], opts[256];
        size_t o = 0;
        int i;

        opts[0] = '\0';
        if (fmts && fmts[0] != AV_SAMPLE_FMT_NONE) {
            enum AVSampleFormat sf = fmts[0];

            for (i = 0; fmts[i] != AV_SAMPLE_FMT_NONE; i++)
                if (fmts[i] == AV_SAMPLE_FMT_FLTP)
                    sf = AV_SAMPLE_FMT_FLTP;
            o += snprintf(opts + o, sizeof(opts) - o, "sample_fmts=%s",
                          av_get_sample_fmt_name(sf));
        }
        if (rates && rates[0] && o < sizeof(opts)) {
            int r = rates[0];

            for (i = 0; rates[i]; i++)
                if (rates[i] == 48000 || (rates[i] == 44100 && r != 48000))
                    r = rates[i];
            o += snprintf(opts + o, sizeof(opts) - o, "%ssample_rates=%d",
                          o ? ":" : "", r);
        }
        enc_layout(enc, codec, layout, sizeof(layout));
        if (layout[0] && o < sizeof(opts))
            snprintf(opts + o, sizeof(opts) - o, "%schannel_layouts=%s",
                     o ? ":" : "", layout);
        if (opts[0])
            n = snprintf(buf, len, ";[%s]aformat=%s[_fb_a]", label, opts);
        else
            n = snprintf(buf, len, ";[%s]anull[_fb_a]", label);
    }
    return (n < 0 || (size_t)n >= len) ? -1 : 0;
}

/* Sets bit rate or quantiser from the job's quality. */
static void set_quality(AVCodecContext *enc, const AVCodec *codec, int kind,
                        FBQuality q)
{
    int final = q == FB_QUALITY_FINAL;

    if (kind == KIND_AUDIO) {
        enc->bit_rate = final ? 160000 : 96000;
        return;
    }
    switch (codec->id) {
    case AV_CODEC_ID_MPEG4:
    case AV_CODEC_ID_MPEG2VIDEO:
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MJPEG:
    case AV_CODEC_ID_H263P:
        /* constant quantiser: 3 looks good, 8 is small and fast */
        enc->flags |= AV_CODEC_FLAG_QSCALE;
        enc->global_quality = FF_QP2LAMBDA * (final ? 3 : 8);
        break;
    default:
        /* x264/x265 and friends take crf; others get a bit rate */
        if (av_opt_set(enc->priv_data, "crf", final ? "20" : "30", 0) < 0)
            enc->bit_rate = (int64_t)enc->width * enc->height *
                            (final ? 6 : 2);
        break;
    }
}

/* Opens the encoder for a sink once the graph is configured. */
static int open_encoder(LocalPriv *p, int kind, const AVCodec *codec,
                        AVCodecContext *enc)
{
    OutStream *os = &p->os[kind];
    AVFilterContext *sink = os->sink;
    int ret;

    if (kind == KIND_VIDEO) {
        AVRational fr = av_buffersink_get_frame_rate(sink);

        if (fr.num <= 0 || fr.den <= 0)
            fr = (AVRational){ 25, 1 };
        enc->width  = av_buffersink_get_w(sink);
        enc->height = av_buffersink_get_h(sink);
        enc->pix_fmt = (enum AVPixelFormat)av_buffersink_get_format(sink);
        enc->sample_aspect_ratio = av_buffersink_get_sample_aspect_ratio(sink);
        enc->framerate = fr;
        enc->time_base = av_inv_q(fr);
    } else {
        enc->sample_fmt = (enum AVSampleFormat)av_buffersink_get_format(sink);
        enc->sample_rate = av_buffersink_get_sample_rate(sink);
        enc->time_base = (AVRational){ 1, enc->sample_rate };
#ifdef FB_CH_LAYOUT
        ret = av_buffersink_get_ch_layout(sink, &enc->ch_layout);
        if (ret < 0)
            return fail(p, ret, "Can't read the audio layout");
#else
        enc->channel_layout = av_buffersink_get_channel_layout(sink);
        enc->channels = av_buffersink_get_channels(sink);
#endif
    }
    set_quality(enc, codec, kind, p->job.output.quality);
    if (p->out->oformat->flags & AVFMT_GLOBALHEADER)
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ret = avcodec_open2(enc, codec, NULL);
    if (ret < 0)
        return fail(p, ret, "Can't open the %s encoder %s", kind_name(kind),
                    codec->name);
    if (kind == KIND_AUDIO && enc->frame_size > 0 &&
        !(codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE))
        av_buffersink_set_frame_size(sink, (unsigned)enc->frame_size);

    os->st = avformat_new_stream(p->out, NULL);
    if (!os->st)
        return fail(p, AVERROR(ENOMEM), "Out of memory");
    ret = avcodec_parameters_from_context(os->st->codecpar, enc);
    if (ret < 0)
        return fail(p, ret, "Can't set up the %s stream", kind_name(kind));
    os->st->time_base = enc->time_base;
    if (kind == KIND_VIDEO) {
        os->st->avg_frame_rate = enc->framerate;
        os->st->sample_aspect_ratio = enc->sample_aspect_ratio;
    }
    os->last_pts = AV_NOPTS_VALUE;
    return 0;
}

static int local_setup(LocalPriv *p)
{
    const FBJob *job = &p->job;
    const AVCodec *codec[NUM_KINDS] = { NULL, NULL };
    AVFilterInOut *ins = NULL, *outs = NULL, *io;
    char *graph_text;
    size_t glen;
    int kinds = job->audio_out[0] ? 2 : 1;
    int k, ret;

    if (job->num_inputs <= 0 || !job->filtergraph[0] || !job->video_out[0])
        return fail(p, 0, "The job is empty");
    if (!job->output.path[0])
        return fail(p, 0, "The job has no output file");

    p->pkt = av_packet_alloc();
    p->frame = av_frame_alloc();
    if (!p->pkt || !p->frame)
        return fail(p, AVERROR(ENOMEM), "Out of memory");

    if (open_inputs(p) < 0)
        return -1;

    /* Output context first: the encoders decide the conversion filters. */
    ret = avformat_alloc_output_context2(&p->out, NULL, NULL,
                                         job->output.path);
    if (ret < 0 || !p->out)   /* no known extension: use MP4 */
        ret = avformat_alloc_output_context2(&p->out, NULL, "mp4",
                                             job->output.path);
    if (ret < 0 || !p->out)
        return fail(p, ret, "Can't create %s", job->output.path);

    for (k = 0; k < kinds; k++) {
        codec[k] = pick_encoder(k == KIND_VIDEO ? job->output.vcodec
                                                : job->output.acodec,
                                p->out->oformat, k);
        if (!codec[k])
            return fail(p, 0, "No %s encoder for %s", kind_name(k),
                        job->output.path);
        p->os[k].enc = avcodec_alloc_context3(codec[k]);
        if (!p->os[k].enc)
            return fail(p, AVERROR(ENOMEM), "Out of memory");
    }

    /* The job's graph plus the conversion tails. */
    glen = strlen(job->filtergraph) + 1024;
    graph_text = av_malloc(glen);
    if (!graph_text)
        return fail(p, AVERROR(ENOMEM), "Out of memory");
    strcpy(graph_text, job->filtergraph);
    for (k = 0; k < kinds; k++) {
        size_t n = strlen(graph_text);

        if (encoder_tail(p, k, codec[k], p->os[k].enc, graph_text + n,
                         glen - n) < 0) {
            av_free(graph_text);
            return fail(p, 0, "The filtergraph is too long");
        }
    }

    p->graph = avfilter_graph_alloc();
    if (!p->graph) {
        av_free(graph_text);
        return fail(p, AVERROR(ENOMEM), "Out of memory");
    }
    /* Run filters in this task only; AmigaOS has no use for threads here. */
    p->graph->nb_threads = 1;

    ret = avfilter_graph_parse2(p->graph, graph_text, &ins, &outs);
    av_free(graph_text);
    if (ret < 0)
        return fail(p, ret, "Bad filtergraph");

    /* Open input pads: one decoder per stream, one source per use. */
    for (io = ins; io; io = io->next) {
        int kind, nth;
        int input = io->name ? parse_pad(io->name, &kind, &nth) : -1;

        if (input < 0 || input >= job->num_inputs) {
            ret = fail(p, 0, "Unknown filtergraph input [%s]",
                       io->name ? io->name : "");
            goto out;
        }
        if (open_decoder(p, input, kind, nth) < 0 ||
            add_source(p, input, kind, io) < 0) {
            ret = -1;
            goto out;
        }
    }

    /* Output pads: our two tails get sinks, anything else is discarded. */
    for (io = outs; io; io = io->next) {
        int kind = -1;
        AVFilterContext *sink;
        const char *fname;
        enum AVMediaType type = avfilter_pad_get_type(
            io->filter_ctx->output_pads, io->pad_idx);

        if (io->name && strcmp(io->name, "_fb_v") == 0)
            kind = KIND_VIDEO;
        else if (io->name && strcmp(io->name, "_fb_a") == 0)
            kind = KIND_AUDIO;

        if (type == AVMEDIA_TYPE_VIDEO)
            fname = kind >= 0 ? "buffersink" : "nullsink";
        else
            fname = kind >= 0 ? "abuffersink" : "anullsink";
        ret = avfilter_graph_create_filter(&sink,
                  avfilter_get_by_name(fname),
                  kind == KIND_VIDEO ? "out_video" :
                  kind == KIND_AUDIO ? "out_audio" : NULL,
                  NULL, NULL, p->graph);
        if (ret < 0) {
            ret = fail(p, ret, "Can't create %s", fname);
            goto out;
        }
        ret = avfilter_link(io->filter_ctx, io->pad_idx, sink, 0);
        if (ret < 0) {
            ret = fail(p, ret, "Can't connect [%s]", io->name ? io->name : "");
            goto out;
        }
        if (kind >= 0)
            p->os[kind].sink = sink;
    }
    for (k = 0; k < kinds; k++) {
        if (!p->os[k].sink) {
            ret = fail(p, 0, "The filtergraph has no [%s] output",
                       k == KIND_VIDEO ? job->video_out : job->audio_out);
            goto out;
        }
    }

    ret = avfilter_graph_config(p->graph, NULL);
    if (ret < 0) {
        ret = fail(p, ret, "Can't set up the filtergraph");
        goto out;
    }

    for (k = 0; k < kinds; k++) {
        if (open_encoder(p, k, codec[k], p->os[k].enc) < 0) {
            ret = -1;
            goto out;
        }
    }
    /* Without audio there is no audio stream; mark it done. */
    for (; k < NUM_KINDS; k++) {
        avcodec_free_context(&p->os[k].enc);
        p->os[k].eof = 1;
    }

    if (!(p->out->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&p->out->pb, job->output.path, AVIO_FLAG_WRITE);
        if (ret < 0) {
            ret = fail(p, ret, "Can't write %s", job->output.path);
            goto out;
        }
    }
    p->file_opened = 1;
    ret = avformat_write_header(p->out, NULL);
    if (ret < 0) {
        ret = fail(p, ret, "Can't write %s", job->output.path);
        goto out;
    }

    /* Inputs the graph doesn't use are never read. */
    for (k = 0; k < job->num_inputs; k++)
        if (!p->in[k].st[KIND_VIDEO].dec && !p->in[k].st[KIND_AUDIO].dec)
            p->in[k].eof = 1;

    p->progress.total = FB_JobEstimateLength(job);
    if (p->progress.total == 0) {
        for (k = 0; k < job->num_inputs; k++) {
            AVFormatContext *f = p->in[k].fmt;
            fb_ms_t d;

            if (f->duration == AV_NOPTS_VALUE)
                continue;
            d = f->duration / (AV_TIME_BASE / 1000);
            if (d > p->progress.total)
                p->progress.total = d;
        }
    }
    ret = 0;
out:
    avfilter_inout_free(&ins);
    avfilter_inout_free(&outs);
    return ret;
}

/* ---- Progress ----------------------------------------------------------- */

/* Sends a frame (or NULL to flush) to an encoder and muxes the packets. */
static int encode(LocalPriv *p, int kind, AVFrame *frame)
{
    OutStream *os = &p->os[kind];
    int ret = avcodec_send_frame(os->enc, frame);

    if (ret < 0)
        return fail(p, ret, "Error encoding %s", kind_name(kind));
    for (;;) {
        ret = avcodec_receive_packet(os->enc, p->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
            return fail(p, ret, "Error encoding %s", kind_name(kind));
        p->pkt->stream_index = os->st->index;
        av_packet_rescale_ts(p->pkt, os->enc->time_base, os->st->time_base);
        ret = av_interleaved_write_frame(p->out, p->pkt);
        if (ret < 0)
            return fail(p, ret, "Can't write %s", p->job.output.path);
    }
}

/* Moves every frame the sinks have ready into the encoders. */
static int drain_sinks(LocalPriv *p)
{
    int k, ret;

    for (k = 0; k < NUM_KINDS; k++) {
        OutStream *os = &p->os[k];

        while (!os->eof) {
            AVRational tb;
            int64_t pts;

            ret = av_buffersink_get_frame(os->sink, p->frame);
            if (ret == AVERROR(EAGAIN))
                break;
            if (ret == AVERROR_EOF) {
                os->eof = 1;
                if (encode(p, k, NULL) < 0)
                    return -1;
                break;
            }
            if (ret < 0)
                return fail(p, ret, "Filtergraph error");

            tb = av_buffersink_get_time_base(os->sink);
            pts = p->frame->pts;
            if (pts != AV_NOPTS_VALUE) {
                fb_ms_t ms = av_rescale_q(pts, tb, (AVRational){ 1, 1000 });

                if (ms > p->progress.position)
                    p->progress.position = ms;
                pts = av_rescale_q(pts, tb, os->enc->time_base);
            } else if (os->last_pts != AV_NOPTS_VALUE) {
                pts = os->last_pts + 1;
            } else {
                pts = 0;
            }
            /* Rounding to the encoder's time base can make two frames
             * land on one tick; encoders refuse that. */
            if (k == KIND_VIDEO && os->last_pts != AV_NOPTS_VALUE &&
                pts <= os->last_pts)
                pts = os->last_pts + 1;
            p->frame->pts = pts;
            os->last_pts = pts;
            p->frame->pict_type = AV_PICTURE_TYPE_NONE;
            /* on failure, fail() has already freed the frame */
            if (encode(p, k, p->frame) < 0)
                return -1;
            av_frame_unref(p->frame);
        }
    }
    return 0;
}

/* Pushes a decoded frame (or NULL = end) into every source of a stream. */
static int push_frame(LocalPriv *p, InStream *is, AVFrame *frame)
{
    int i, ret;

    for (i = 0; i < is->num_src; i++) {
        ret = av_buffersrc_add_frame_flags(is->src[i], frame,
                                           AV_BUFFERSRC_FLAG_KEEP_REF);
        /* EOF: the filter after this source (trim) has all it wants. */
        if (ret < 0 && ret != AVERROR_EOF)
            return fail(p, ret, "Filtergraph error");
    }
    return 0;
}

/* Sends a packet (or NULL to flush) to a decoder and feeds the graph. */
static int decode(LocalPriv *p, InStream *is, AVPacket *pkt)
{
    int ret = avcodec_send_packet(is->dec, pkt);

    /* Broken packets are skipped, like the ffmpeg CLI does. */
    if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR_INVALIDDATA)
        return fail(p, ret, "Error decoding");
    for (;;) {
        ret = avcodec_receive_frame(is->dec, p->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret == AVERROR_INVALIDDATA)
            continue;
        if (ret < 0)
            return fail(p, ret, "Error decoding");
        p->frame->pts = p->frame->best_effort_timestamp;
        if (p->frame->pts != AV_NOPTS_VALUE)
            p->frame->pts -= is->ts_offset;
        if (push_frame(p, is, p->frame) < 0)
            return -1;   /* p->frame is gone already */
        av_frame_unref(p->frame);
    }
}

/* The input the graph is waiting for: most failed requests on its
 * sources, ties to the lowest number. -1 when all inputs are done. */
static int next_input(LocalPriv *p)
{
    unsigned best_req = 0;
    int i, k, s, best = -1;

    for (i = 0; i < p->job.num_inputs; i++) {
        unsigned req = 0;

        if (p->in[i].eof)
            continue;
        for (k = 0; k < NUM_KINDS; k++)
            for (s = 0; s < p->in[i].st[k].num_src; s++)
                req += av_buffersrc_get_nb_failed_requests(
                           p->in[i].st[k].src[s]);
        if (best < 0 || req > best_req) {
            best = i;
            best_req = req;
        }
    }
    return best;
}

/* Reads and decodes one packet from an input; flushes it at the end. */
static int read_input(LocalPriv *p, int i)
{
    Input *in = &p->in[i];
    int k, ret = av_read_frame(in->fmt, p->pkt);

    if (ret == AVERROR_EOF || (ret < 0 && in->fmt->pb && avio_feof(in->fmt->pb))) {
        in->eof = 1;
        for (k = 0; k < NUM_KINDS; k++) {
            if (!in->st[k].dec)
                continue;
            if (decode(p, &in->st[k], NULL) < 0 ||
                push_frame(p, &in->st[k], NULL) < 0)
                return -1;
        }
        return 0;
    }
    if (ret < 0)
        return fail(p, ret, "Can't read %s", p->job.inputs[i]);

    for (k = 0; k < NUM_KINDS; k++) {
        if (in->st[k].dec && in->st[k].index == p->pkt->stream_index) {
            if (decode(p, &in->st[k], p->pkt) < 0)
                return -1;   /* p->pkt is gone already */
            break;
        }
    }
    av_packet_unref(p->pkt);
    return 0;
}

static void local_slice(LocalPriv *p)
{
    int n, ret;

    for (n = 0; n < PACKETS_PER_SLICE; n++) {
        int i;

        if (drain_sinks(p) < 0)
            return;
        if (p->os[KIND_VIDEO].eof && p->os[KIND_AUDIO].eof) {
            ret = av_write_trailer(p->out);
            if (ret < 0) {
                fail(p, ret, "Can't finish %s", p->job.output.path);
                return;
            }
            release(p, 1);
            finish(p, FB_RENDER_DONE, p->job.output.path);
            return;
        }
        i = next_input(p);
        if (i < 0) {
            /* Every input is flushed and the sinks still want more. */
            fail(p, 0, "The filtergraph stalled before the end");
            return;
        }
        if (read_input(p, i) < 0)
            return;
    }
}

#endif /* HAVE_LIBAV */

/* ---- Backend interface -------------------------------------------------- */

static int local_submit(FBBackend *be, const FBJob *job)
{
    LocalPriv *p = be->priv;

    if (p->progress.state == FB_RENDER_RUNNING)
        return -1;
    memset(&p->progress, 0, sizeof(p->progress));
    memset(&p->result, 0, sizeof(p->result));
    p->job = *job;

#ifdef HAVE_LIBAV
    if (local_setup(p) < 0)
        return -1;   /* fail() has released everything and set the result */
    p->progress.state = FB_RENDER_RUNNING;
    p->result.state = FB_RENDER_RUNNING;
    return 0;
#else
    finish(p, FB_RENDER_FAILED, "Built without libav (HAVE_LIBAV not set)");
    return -1;
#endif
}

static FBProgress local_progress(FBBackend *be)
{
    LocalPriv *p = be->priv;

#ifdef HAVE_LIBAV
    if (p->progress.state == FB_RENDER_RUNNING)
        local_slice(p);
#endif
    return p->progress;
}

static void local_cancel(FBBackend *be)
{
    LocalPriv *p = be->priv;

    if (p->progress.state == FB_RENDER_RUNNING) {
#ifdef HAVE_LIBAV
        release(p, 0);   /* closes everything, deletes the partial file */
#endif
        finish(p, FB_RENDER_CANCELLED, "Cancelled");
    }
}

static FBResult local_result(FBBackend *be)
{
    return ((LocalPriv *)be->priv)->result;
}

static void local_dispose(FBBackend *be)
{
    local_cancel(be);
    free(be->priv);
    free(be);
}

FBBackend *FB_CreateLocalBackend(void)
{
    FBBackend *be = calloc(1, sizeof(*be));
    LocalPriv *p = calloc(1, sizeof(*p));

    if (!be || !p) {
        free(be);
        free(p);
        return NULL;
    }
    be->name     = "local";
    be->Submit   = local_submit;
    be->Progress = local_progress;
    be->Cancel   = local_cancel;
    be->Result   = local_result;
    be->Dispose  = local_dispose;
    be->priv     = p;
    return be;
}
