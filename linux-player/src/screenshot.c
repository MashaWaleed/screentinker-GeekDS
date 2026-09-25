#include "screenshot.h"
#include "stp.h"
#include <libavcodec/avcodec.h>
#include <libavutil/base64.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define SCREENSHOT_MAX_WIDTH 960
#define SCREENSHOT_MAX_HEIGHT 540

struct screenshot_capture {
    stp_display_t *disp;
    pthread_mutex_t lock;
    pthread_cond_t requested;
    pthread_t thread;
    bool stopping, pending, thread_started;
    char *ready_b64;
};

static char *encode_jpeg_b64(stp_display_t *disp) {
    int src_w = disp->width, src_h = disp->height;
    if (src_w <= 0 || src_h <= 0 || !disp->pixels) return NULL;

    double scale = 1.0;
    if (src_w > SCREENSHOT_MAX_WIDTH)
        scale = (double)SCREENSHOT_MAX_WIDTH / src_w;
    if (src_h * scale > SCREENSHOT_MAX_HEIGHT)
        scale = (double)SCREENSHOT_MAX_HEIGHT / src_h;
    int dst_w = (int)(src_w * scale) & ~1;
    int dst_h = (int)(src_h * scale) & ~1;
    if (dst_w < 2) dst_w = 2;
    if (dst_h < 2) dst_h = 2;

    /* Take a fast RAM copy first. Encoding and scaling then happen without
     * touching the scanout mapping, so playback is never held by JPEG work. */
    size_t raw_size = (size_t)src_w * src_h * 4;
    uint8_t *raw = malloc(raw_size);
    if (!raw) return NULL;
    uint8_t *scanout = disp->pixels;
    for (int y = 0; y < src_h; y++)
        memcpy(raw + (size_t)y * src_w * 4,
               scanout + (size_t)y * disp->stride, (size_t)src_w * 4);

    char *result = NULL;
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    AVCodecContext *enc = codec ? avcodec_alloc_context3(codec) : NULL;
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    struct SwsContext *sws = NULL;
    if (!enc || !frame || !packet) goto done;

    enc->width = dst_w;
    enc->height = dst_h;
    /* MJPEG accepts regular YUV420P when range is declared explicitly;
     * avoid the deprecated YUVJ identifier and its per-capture warning. */
    enc->pix_fmt = AV_PIX_FMT_YUV420P;
    enc->color_range = AVCOL_RANGE_JPEG;
    enc->time_base = (AVRational){1, 1};
    enc->flags |= AV_CODEC_FLAG_QSCALE;
    enc->global_quality = FF_QP2LAMBDA * 5;
    if (avcodec_open2(enc, codec, NULL) < 0) goto done;

    frame->format = enc->pix_fmt;
    frame->width = dst_w;
    frame->height = dst_h;
    frame->color_range = enc->color_range;
    if (av_frame_get_buffer(frame, 32) < 0) goto done;
    enum AVPixelFormat source_format = disp->pixel_layout == STP_PIXELS_RGBX
                                       ? AV_PIX_FMT_RGBA : AV_PIX_FMT_BGRA;
    sws = sws_getContext(src_w, src_h, source_format, dst_w, dst_h,
                         enc->pix_fmt, SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!sws) goto done;
    const uint8_t *src[4] = { raw, NULL, NULL, NULL };
    int src_stride[4] = { src_w * 4, 0, 0, 0 };
    sws_scale(sws, src, src_stride, 0, src_h, frame->data, frame->linesize);
    if (avcodec_send_frame(enc, frame) < 0 || avcodec_receive_packet(enc, packet) < 0) goto done;

    size_t b64_size = AV_BASE64_SIZE(packet->size);
    result = malloc(b64_size);
    if (!result || !av_base64_encode(result, b64_size, packet->data, packet->size)) {
        free(result);
        result = NULL;
    }

done:
    sws_freeContext(sws);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&enc);
    free(raw);
    return result;
}

static void *capture_thread(void *arg) {
    screenshot_capture_t *capture = arg;
    for (;;) {
        pthread_mutex_lock(&capture->lock);
        while (!capture->stopping && !capture->pending)
            pthread_cond_wait(&capture->requested, &capture->lock);
        if (capture->stopping) { pthread_mutex_unlock(&capture->lock); break; }
        capture->pending = false;
        pthread_mutex_unlock(&capture->lock);

        char *image = encode_jpeg_b64(capture->disp);
        if (!image) { LOG("screenshot: capture or JPEG encoding failed"); continue; }

        pthread_mutex_lock(&capture->lock);
        free(capture->ready_b64);
        capture->ready_b64 = image;
        pthread_mutex_unlock(&capture->lock);
    }
    return NULL;
}

screenshot_capture_t *screenshot_capture_start(stp_display_t *disp) {
    screenshot_capture_t *capture = calloc(1, sizeof(*capture));
    if (!capture) return NULL;
    capture->disp = disp;
    pthread_mutex_init(&capture->lock, NULL);
    pthread_cond_init(&capture->requested, NULL);
    if (pthread_create(&capture->thread, NULL, capture_thread, capture) != 0) {
        pthread_cond_destroy(&capture->requested);
        pthread_mutex_destroy(&capture->lock);
        free(capture);
        return NULL;
    }
    capture->thread_started = true;
    return capture;
}

void screenshot_capture_request(screenshot_capture_t *capture) {
    if (!capture) return;
    pthread_mutex_lock(&capture->lock);
    capture->pending = true; /* coalesce periodic requests while encoding */
    pthread_cond_signal(&capture->requested);
    pthread_mutex_unlock(&capture->lock);
}

char *screenshot_capture_take_result(screenshot_capture_t *capture) {
    if (!capture) return NULL;
    pthread_mutex_lock(&capture->lock);
    char *result = capture->ready_b64;
    capture->ready_b64 = NULL;
    pthread_mutex_unlock(&capture->lock);
    return result;
}

void screenshot_capture_stop(screenshot_capture_t *capture) {
    if (!capture) return;
    pthread_mutex_lock(&capture->lock);
    capture->stopping = true;
    pthread_cond_signal(&capture->requested);
    pthread_mutex_unlock(&capture->lock);
    if (capture->thread_started) pthread_join(capture->thread, NULL);
    free(capture->ready_b64);
    pthread_cond_destroy(&capture->requested);
    pthread_mutex_destroy(&capture->lock);
    free(capture);
}
