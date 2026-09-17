/* media.c — decode a single content item straight into a display sub-rect.
 *
 * Same core trick as GeekDS's fbplay_video.c: sws_scale writes directly into
 * the mmap'd scanout buffer using its real stride, so there's no intermediate
 * malloc/memcpy per frame. Image vs video isn't decided by file extension —
 * both go through the same demux/decode loop; an image format just happens
 * to yield exactly one video frame before EOF, which this loop already
 * handles (hold the last frame for the remaining duration).
 */
#include "stp.h"
#include "media.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#ifdef WITH_ALSA
#include <alsa/asoundlib.h>
#endif

/* Compute the destination sub-rect (within `rect`) that `fit_mode` calls for,
 * given a source of src_w x src_h. Anything outside dst_rect but inside rect
 * is left as-is (the zone's background_color, painted once by the caller). */
static void fit_rect(int rect_w, int rect_h, int src_w, int src_h, const char *fit_mode,
                      int *out_x, int *out_y, int *out_w, int *out_h) {
    if (src_w <= 0 || src_h <= 0) { *out_x = 0; *out_y = 0; *out_w = rect_w; *out_h = rect_h; return; }
    double rect_ar = (double)rect_w / rect_h;
    double src_ar = (double)src_w / src_h;
    bool cover = fit_mode && strcmp(fit_mode, "cover") == 0;
    bool fill  = fit_mode && strcmp(fit_mode, "fill") == 0;

    if (fill) { *out_x = 0; *out_y = 0; *out_w = rect_w; *out_h = rect_h; return; }

    int w, h;
    bool fit_to_width = cover ? (src_ar < rect_ar) : (src_ar > rect_ar);
    if (fit_to_width) { w = rect_w; h = (int)lround(rect_w / src_ar); }
    else { h = rect_h; w = (int)lround(rect_h * src_ar); }

    *out_w = w; *out_h = h;
    *out_x = (rect_w - w) / 2;
    *out_y = (rect_h - h) / 2;
}

static void fill_bgra(stp_display_t *d, stp_rect_t r, uint32_t bgra) {
    for (int y = 0; y < r.h; y++) {
        uint32_t *row = (uint32_t *)(d->pixels + (size_t)(r.y + y) * d->stride + (size_t)r.x * 4);
        for (int x = 0; x < r.w; x++) row[x] = bgra;
    }
}

uint32_t media_parse_color(const char *hex, uint32_t fallback) {
    if (!hex || hex[0] != '#' || strlen(hex) < 7) return fallback;
    unsigned r, g, b;
    if (sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b) != 3) return fallback;
    return (0xFFu << 24) | (r << 16) | (g << 8) | b; /* stored as XRGB but bytes are B,G,R,X */
}

#ifdef WITH_ALSA
typedef struct {
    snd_pcm_t *pcm;
    struct SwrContext *swr;
    int channels;
} audio_out_t;

static bool audio_open(audio_out_t *ao, AVCodecContext *actx) {
    memset(ao, 0, sizeof(*ao));
    if (snd_pcm_open(&ao->pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) return false;
    ao->channels = actx->ch_layout.nb_channels > 0 ? actx->ch_layout.nb_channels : 2;
    if (snd_pcm_set_params(ao->pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                            ao->channels, 48000, 1, 200000 /* 200ms latency */) < 0) {
        snd_pcm_close(ao->pcm);
        return false;
    }
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, ao->channels);
    swr_alloc_set_opts2(&ao->swr, &out_layout, AV_SAMPLE_FMT_S16, 48000,
                         &actx->ch_layout, actx->sample_fmt, actx->sample_rate, 0, NULL);
    av_channel_layout_uninit(&out_layout);
    if (!ao->swr || swr_init(ao->swr) < 0) { snd_pcm_close(ao->pcm); return false; }
    return true;
}

static void audio_play(audio_out_t *ao, AVFrame *frame) {
    if (!ao->pcm) return;
    uint8_t *out = NULL;
    int out_samples = av_rescale_rnd(swr_get_delay(ao->swr, frame->sample_rate) + frame->nb_samples,
                                      48000, frame->sample_rate, AV_ROUND_UP);
    av_samples_alloc(&out, NULL, ao->channels, out_samples, AV_SAMPLE_FMT_S16, 0);
    int n = swr_convert(ao->swr, &out, out_samples, (const uint8_t **)frame->extended_data, frame->nb_samples);
    if (n > 0) {
        int rc = snd_pcm_writei(ao->pcm, out, n);
        if (rc == -EPIPE) snd_pcm_prepare(ao->pcm);
    }
    av_freep(&out);
}

static void audio_close(audio_out_t *ao) {
    if (ao->swr) swr_free(&ao->swr);
    if (ao->pcm) { snd_pcm_drain(ao->pcm); snd_pcm_close(ao->pcm); }
}
#endif

media_result_t media_play_item(stp_display_t *disp, stp_rect_t rect, const char *path,
                                const char *fit_mode, uint32_t bg_bgra,
                                int duration_sec, const volatile bool *cancel) {
    media_result_t result = MEDIA_OK;
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
        LOG("media: cannot open '%s'", path);
        fill_bgra(disp, rect, bg_bgra);
        return MEDIA_ERROR;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        LOG("media: no stream info in '%s'", path);
        avformat_close_input(&fmt);
        return MEDIA_ERROR;
    }

    int vstream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int astream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (vstream < 0) {
        LOG("media: no video stream in '%s'", path);
        avformat_close_input(&fmt);
        return MEDIA_ERROR;
    }

    AVCodecParameters *vpar = fmt->streams[vstream]->codecpar;
    const AVCodec *vcodec = avcodec_find_decoder(vpar->codec_id);
    AVCodecContext *vctx = avcodec_alloc_context3(vcodec);
    avcodec_parameters_to_context(vctx, vpar);
    if (!vcodec || avcodec_open2(vctx, vcodec, NULL) < 0) {
        LOG("media: no decoder for '%s'", path);
        avcodec_free_context(&vctx);
        avformat_close_input(&fmt);
        return MEDIA_ERROR;
    }

    bool is_still = fmt->streams[vstream]->nb_frames == 1 ||
                    (fmt->duration <= 0 && !(fmt->iformat->flags & AVFMT_TS_DISCONT) == 0 && vstream >= 0 &&
                     fmt->streams[vstream]->avg_frame_rate.num == 0);

    AVCodecContext *actx = NULL;
#ifdef WITH_ALSA
    audio_out_t ao = {0};
    bool have_audio = false;
    if (astream >= 0 && duration_sec != 0 /* skip decoding audio for stills-as-image calls */) {
        AVCodecParameters *apar = fmt->streams[astream]->codecpar;
        const AVCodec *acodec = avcodec_find_decoder(apar->codec_id);
        if (acodec) {
            actx = avcodec_alloc_context3(acodec);
            avcodec_parameters_to_context(actx, apar);
            if (avcodec_open2(actx, acodec, NULL) == 0 && audio_open(&ao, actx)) have_audio = true;
            else { if (actx) avcodec_free_context(&actx); actx = NULL; }
        }
    }
#else
    (void)astream;
#endif

    int dst_w, dst_h, dst_x, dst_y;
    fit_rect(rect.w, rect.h, vctx->width, vctx->height, fit_mode, &dst_x, &dst_y, &dst_w, &dst_h);
    if (dst_x > 0 || dst_y > 0 || dst_w < rect.w || dst_h < rect.h) fill_bgra(disp, rect, bg_bgra);

    struct SwsContext *sws = sws_getContext(vctx->width, vctx->height, vctx->pix_fmt,
                                             dst_w, dst_h, AV_PIX_FMT_BGRA,
                                             SWS_BILINEAR, NULL, NULL, NULL);

    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();

    uint8_t *dst_planes[4] = {0};
    int dst_linesize[4] = {0};
    dst_planes[0] = disp->pixels + (size_t)(rect.y + dst_y) * disp->stride + (size_t)(rect.x + dst_x) * 4;
    dst_linesize[0] = disp->stride;

    int64_t start_us = av_gettime();
    int64_t deadline_us = duration_sec > 0 ? start_us + (int64_t)duration_sec * 1000000 : -1;
    bool drew_any_frame = false;
    AVRational vtb = fmt->streams[vstream]->time_base;

    for (bool keep_looping = true; keep_looping; ) {
        int rd;
        while ((rd = av_read_frame(fmt, pkt)) >= 0) {
            if (cancel && *cancel) { av_packet_unref(pkt); goto done; }
            if (deadline_us > 0 && av_gettime() >= deadline_us) { av_packet_unref(pkt); goto done; }

            if (pkt->stream_index == vstream) {
                if (avcodec_send_packet(vctx, pkt) == 0) {
                    while (avcodec_receive_frame(vctx, frame) == 0) {
                        sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize,
                                  0, vctx->height, dst_planes, dst_linesize);
                        drew_any_frame = true;
                        disp->present(disp);

                        /* basic realtime pacing off the frame pts */
                        if (frame->pts != AV_NOPTS_VALUE && vtb.den > 0) {
                            int64_t frame_us = av_rescale_q(frame->pts, vtb, AV_TIME_BASE_Q);
                            int64_t target = start_us + frame_us;
                            int64_t now = av_gettime();
                            if (target > now) av_usleep((unsigned)(target - now));
                        }
                        av_frame_unref(frame);
                    }
                }
            }
#ifdef WITH_ALSA
            else if (have_audio && pkt->stream_index == astream) {
                if (avcodec_send_packet(actx, pkt) == 0) {
                    while (avcodec_receive_frame(actx, frame) == 0) {
                        audio_play(&ao, frame);
                        av_frame_unref(frame);
                    }
                }
            }
#endif
            av_packet_unref(pkt);
        }

        if (!drew_any_frame) { result = MEDIA_ERROR; goto done; }

        /* EOF: a still image (or a video with no duration_sec override) just
         * holds its last decoded frame on screen for the remaining time. A
         * video with duration_sec set loops back to the start instead. */
        if (deadline_us < 0 || is_still) {
            keep_looping = false;
        } else {
            av_seek_frame(fmt, vstream, 0, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(vctx);
            start_us = av_gettime() - 0; /* re-baseline pacing to "now" each loop */
        }
    }

    /* still image / EOF-without-deadline: sleep out the remaining duration
     * in short slices so cancel/reload can interrupt promptly. */
    if (deadline_us > 0) {
        while (av_gettime() < deadline_us) {
            if (cancel && *cancel) break;
            av_usleep(100000);
        }
    }

done:
    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&vctx);
#ifdef WITH_ALSA
    if (have_audio) audio_close(&ao);
    if (actx) avcodec_free_context(&actx);
#endif
    avformat_close_input(&fmt);
    return result;
}
