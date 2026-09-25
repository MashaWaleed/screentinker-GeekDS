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
#include <libavutil/opt.h>
#include <libavutil/cpu.h>
#include <libavutil/time.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef WITH_ALSA
#include <alsa/asoundlib.h>
#endif

/* Signage panels do not benefit from spending CPU on more than 24 scans per
 * second.  Decode every frame to preserve inter-frame codec state, but drop
 * excess frames before the expensive scale-to-framebuffer operation. */
#define MAX_RENDER_FPS 24
#define RENDER_INTERVAL_US (AV_TIME_BASE / MAX_RENDER_FPS)
#define MIN_RENDER_INTERVAL_US 66667 /* 15 FPS floor for CPU-bound scaling */
/* Software decode/scale has normal scheduling jitter.  Keep a frame that is
 * only slightly late so resizing does not turn that jitter into visible drops. */
#define RENDER_LATE_TOLERANCE_US 15000

static int64_t resize_render_interval(const AVStream *stream, bool resized) {
    if (!resized || !stream || stream->avg_frame_rate.den == 0) return RENDER_INTERVAL_US;
    double fps = av_q2d(stream->avg_frame_rate);
    /* Pick divisors of the most common source rates instead of an uneven
     * 24-FPS cadence that visibly judders on 30/60-FPS material. */
    if (fps >= 55.0) return AV_TIME_BASE / 20; /* 60 -> 20 */
    if (fps >= 25.0 && fps <= 35.0) return AV_TIME_BASE / 15; /* 30 -> 15 */
    return RENDER_INTERVAL_US;
}

static bool has_svg_extension(const char *path) {
    const char *ext = path ? strrchr(path, '.') : NULL;
    return ext && strlen(ext) == 4 && tolower((unsigned char)ext[1]) == 's' &&
           tolower((unsigned char)ext[2]) == 'v' &&
           tolower((unsigned char)ext[3]) == 'g' && ext[4] == '\0';
}

static bool has_static_image_extension(const char *path) {
    const char *ext = path ? strrchr(path, '.') : NULL;
    if (!ext) return false;
    char lower[8] = {0};
    size_t n = strlen(ext);
    if (n < 2 || n >= sizeof(lower)) return false;
    for (size_t i = 0; i < n; i++) lower[i] = (char)tolower((unsigned char)ext[i]);
    return strcmp(lower, ".jpg") == 0 || strcmp(lower, ".jpeg") == 0 ||
           strcmp(lower, ".png") == 0 || strcmp(lower, ".bmp") == 0 ||
           strcmp(lower, ".webp") == 0 || strcmp(lower, ".tif") == 0 ||
           strcmp(lower, ".tiff") == 0 || strcmp(lower, ".gif") == 0 ||
           strcmp(lower, ".ico") == 0;
}

/* FFmpeg can demux SVG but most builds, including the lean packages used on
 * signage devices, do not include an SVG raster decoder.  Convert it once at
 * the size it will be displayed and cache the PNG beside the downloaded asset.
 * execvp is intentional: cache paths are never sent through a shell. */
static bool run_converter(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static char *rasterize_svg(const char *path, int width, int height) {
    if (width <= 0 || height <= 0) return NULL;
    char cached[PATH_MAX];
    int n = snprintf(cached, sizeof(cached), "%s.stlinux-%dx%d.png", path, width, height);
    if (n < 0 || (size_t)n >= sizeof(cached)) return NULL;

    struct stat source, rendered;
    if (stat(path, &source) == 0 && stat(cached, &rendered) == 0 &&
        rendered.st_size > 0 && rendered.st_mtime >= source.st_mtime)
        return strdup(cached);

    char temporary[PATH_MAX];
    n = snprintf(temporary, sizeof(temporary), "%s.tmpXXXXXX", cached);
    if (n < 0 || (size_t)n >= sizeof(temporary)) return NULL;
    int temp_fd = mkstemp(temporary);
    if (temp_fd < 0) return NULL;
    close(temp_fd);

    char width_arg[16], height_arg[16], size_arg[40], png_output[PATH_MAX + 5];
    snprintf(width_arg, sizeof(width_arg), "%d", width);
    snprintf(height_arg, sizeof(height_arg), "%d", height);
    snprintf(size_arg, sizeof(size_arg), "%dx%d", width, height);
    n = snprintf(png_output, sizeof(png_output), "png:%s", temporary);
    if (n < 0 || (size_t)n >= sizeof(png_output)) { unlink(temporary); return NULL; }

    char *rsvg_argv[] = {
        "rsvg-convert", "-f", "png", "-w", width_arg, "-h", height_arg,
        "-o", temporary, (char *)path, NULL
    };
    char *magick_argv[] = {
        "magick", "-background", "none", "-size", size_arg, (char *)path, png_output, NULL
    };
    char *convert_argv[] = {
        "convert", "-background", "none", "-size", size_arg, (char *)path, png_output, NULL
    };
    bool converted = run_converter(rsvg_argv) || run_converter(magick_argv) || run_converter(convert_argv);
    struct stat completed;
    if (!converted || stat(temporary, &completed) != 0 || completed.st_size == 0 ||
        rename(temporary, cached) != 0) {
        unlink(temporary);
        return NULL;
    }
    return strdup(cached);
}

static bool image_demuxer(const AVFormatContext *fmt) {
    if (!fmt || !fmt->iformat || !fmt->iformat->name) return false;
    const char *name = fmt->iformat->name;
    return strstr(name, "image2") != NULL || strcmp(name, "png_pipe") == 0 ||
           strcmp(name, "jpeg_pipe") == 0 || strcmp(name, "bmp_pipe") == 0 ||
           strcmp(name, "webp_pipe") == 0 || strcmp(name, "tiff_pipe") == 0 ||
           strcmp(name, "pcx_pipe") == 0;
}

/* New libswscale treats YUVJ as deprecated even though MJPEG decoders still
 * commonly produce it.  Its plane layout is exactly the corresponding YUV
 * format; retain the JPEG/full range explicitly instead of passing the legacy
 * identifier to the scaler (which emits a warning for every still image). */
static enum AVPixelFormat jpeg_compatible_format(enum AVPixelFormat fmt, bool *full_range) {
    *full_range = false;
    switch (fmt) {
    case AV_PIX_FMT_YUVJ420P: *full_range = true; return AV_PIX_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P: *full_range = true; return AV_PIX_FMT_YUV422P;
    case AV_PIX_FMT_YUVJ444P: *full_range = true; return AV_PIX_FMT_YUV444P;
    case AV_PIX_FMT_YUVJ440P: *full_range = true; return AV_PIX_FMT_YUV440P;
    default: return fmt;
    }
}

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

    if (fill || cover) {
        /* Scaling a cover image beyond this destination would write outside a
         * zone surface.  Keep the write bounded; the scaler fills the zone. */
        *out_x = 0; *out_y = 0; *out_w = rect_w; *out_h = rect_h;
        return;
    }

    int w, h;
    bool fit_to_width = src_ar > rect_ar;
    if (fit_to_width) { w = rect_w; h = (int)lround(rect_w / src_ar); }
    else { h = rect_h; w = (int)lround(rect_h * src_ar); }

    *out_w = w; *out_h = h;
    *out_x = (rect_w - w) / 2;
    *out_y = (rect_h - h) / 2;
}

static void fill_bgra(stp_display_t *d, stp_rect_t r, uint32_t bgra) {
    bgra = stp_pack_color(d, bgra);
    if (d->lock_pixels) d->lock_pixels(d);
    for (int y = 0; y < r.h; y++) {
        uint32_t *row = (uint32_t *)(d->pixels + (size_t)(r.y + y) * d->stride + (size_t)r.x * 4);
        for (int x = 0; x < r.w; x++) row[x] = bgra;
    }
    if (d->unlock_pixels) d->unlock_pixels(d);
}

/* sws_getContext() creates a scaler with its default thread count (one on
 * supported FFmpeg releases).  Configure it explicitly so expensive colour
 * conversion and resizing can use the available worker CPUs, like the decoder.
 */
static struct SwsContext *make_scaler(int src_w, int src_h, enum AVPixelFormat src_fmt,
                                      int dst_w, int dst_h, bool src_full_range,
                                      enum AVColorSpace colorspace,
                                      enum AVPixelFormat dst_fmt) {
    struct SwsContext *sws = sws_alloc_context();
    if (!sws) return NULL;
    /* FFmpeg's fast bilinear path uses the platform's SIMD scaler routines;
     * it is generally faster than point scaling on ARM and x86 targets. */
    int flags = SWS_FAST_BILINEAR;

    if (av_opt_set_int(sws, "srcw", src_w, 0) < 0 ||
        av_opt_set_int(sws, "srch", src_h, 0) < 0 ||
        av_opt_set_int(sws, "src_format", src_fmt, 0) < 0 ||
        av_opt_set_int(sws, "dstw", dst_w, 0) < 0 ||
        av_opt_set_int(sws, "dsth", dst_h, 0) < 0 ||
        av_opt_set_int(sws, "dst_format", dst_fmt, 0) < 0 ||
        av_opt_set_int(sws, "sws_flags", flags, 0) < 0) {
        sws_freeContext(sws);
        return NULL;
    }
    /* This is an optional AVOption on older libswscale releases, where
     * decoding remains threaded even if scaling cannot be. */
    /* 0 is FFmpeg's automatic maximum, equivalent to the CLI's -threads 0. */
    if (av_opt_set_int(sws, "threads", 0, 0) < 0)
        LOG("media: libswscale does not support threaded scaling");
    if (sws_init_context(sws, NULL, NULL) < 0) {
        sws_freeContext(sws);
        return NULL;
    }
    if (src_full_range) {
        int cs = colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_ITU601;
        const int *coefficients = sws_getCoefficients(cs);
        sws_setColorspaceDetails(sws, coefficients, 1, coefficients, 1,
                                 0, 1 << 16, 1 << 16);
    }
    return sws;
}

/* Returns true when this decoded frame was scaled and presented.  PTS-less
 * streams retain the old behaviour because there is no safe cadence signal
 * with which to select frames. */
static bool present_video_frame(stp_display_t *disp, struct SwsContext *sws,
                                AVCodecContext *vctx, AVFrame *frame,
                                size_t dst_offset, int dst_linesize,
                                uint8_t *staging, int staging_stride, int dst_w, int dst_h,
                                AVRational time_base, int64_t start_us,
                                int64_t *first_frame_us, int64_t *next_present_us,
                                int64_t *render_interval_us, int64_t *render_lead_us,
                                bool still_image) {
    int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
                    ? frame->best_effort_timestamp : frame->pts;
    /* A still image has one frame.  It must be shown even when decoding a
     * large PNG/JPEG exceeds the late-frame tolerance used to keep videos in
     * real time; dropping that sole frame leaves the zone permanently blank. */
    if (!still_image && pts != AV_NOPTS_VALUE && time_base.den > 0) {
        int64_t frame_us = av_rescale_q(pts, time_base, AV_TIME_BASE_Q);
        if (*first_frame_us == AV_NOPTS_VALUE) *first_frame_us = frame_us;
        int64_t presentation_us = frame_us - *first_frame_us;
        if (presentation_us < *next_present_us) return false;

        int64_t target = start_us + presentation_us;
        int64_t now = av_gettime_relative();
        /* Advance on a fixed output clock.  Basing this on the selected input
         * frame can turn 30 FPS input into 15 FPS when capped at 24 FPS. */
        do {
            *next_present_us += *render_interval_us;
        } while (*next_present_us <= presentation_us);
        /* Never spend CPU scaling a frame whose presentation time has passed.
         * Discarding it lets decoding catch up to the live video clock instead
         * of stretching the clip beyond its original duration. */
        if (now > target + RENDER_LATE_TOLERANCE_US) return false;
        /* Scale early enough to finish at the PTS deadline.  Waiting until
         * the deadline and only then starting a costly resize makes every
         * frame late, then forces the following frame to be dropped. */
        int64_t work_start = target - *render_lead_us;
        if (work_start > now) av_usleep((unsigned)(work_start - now));
    }

    uint8_t *dst_planes[4] = { staging ? staging : disp->pixels + dst_offset };
    int dst_linesizes[4] = { staging ? staging_stride : dst_linesize };
    int64_t scale_start_us = av_gettime_relative();
    if (!staging && disp->lock_pixels) disp->lock_pixels(disp);
    sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize,
              0, vctx->height, dst_planes, dst_linesizes);
    if (!staging && disp->unlock_pixels) disp->unlock_pixels(disp);
    if (staging) {
        if (disp->lock_pixels) disp->lock_pixels(disp);
        uint8_t *dst = disp->pixels + dst_offset;
        if (staging_stride == dst_linesize) {
            /* The usual full-screen DRM case is contiguous.  One bulk copy
             * lets libc use its tuned streaming copy path and avoids a call
             * per scanline. */
            memcpy(dst, staging, (size_t)staging_stride * dst_h);
        } else {
            for (int y = 0; y < dst_h; y++)
                memcpy(dst + (size_t)y * dst_linesize,
                       staging + (size_t)y * staging_stride, (size_t)dst_w * 4);
        }
        if (disp->unlock_pixels) disp->unlock_pixels(disp);
    }
    disp->present(disp);
    int64_t work_cost_us = av_gettime_relative() - scale_start_us;
    if (!still_image) {
        /* Leave a small margin for scheduler wake-up jitter.  Rise instantly
         * on an expensive frame; recover gently when the next frames are
         * cheap so timing does not oscillate. */
        int64_t wanted_lead = work_cost_us + 2000;
        if (wanted_lead > *render_lead_us) *render_lead_us = wanted_lead;
        else *render_lead_us = (*render_lead_us * 3 + wanted_lead) / 4;

        if (work_cost_us * 5 > *render_interval_us * 4) {
            int64_t slower_interval = work_cost_us + work_cost_us / 4;
            if (slower_interval > MIN_RENDER_INTERVAL_US) slower_interval = MIN_RENDER_INTERVAL_US;
            if (slower_interval > *render_interval_us) *render_interval_us = slower_interval;
        } else if (*render_interval_us > RENDER_INTERVAL_US) {
            /* Recover gradually so a single complex frame does not make cadence jump. */
            *render_interval_us -= (*render_interval_us - RENDER_INTERVAL_US) / 8 + 1;
        }
    }
    return true;
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
    int frame_bytes;
    uint8_t *convert_buffer;
    int convert_capacity_samples;
    uint8_t *queue;
    size_t queue_capacity, queue_read, queue_write, queue_used;
    pthread_mutex_t queue_lock;
    pthread_cond_t queue_ready;
    pthread_t thread;
    bool stopping, thread_started;
} audio_out_t;

static void audio_write_pcm(audio_out_t *ao, const uint8_t *data, int frames) {
    int written = 0;
    while (written < frames) {
        int rc = snd_pcm_writei(ao->pcm, data + (size_t)written * ao->frame_bytes, frames - written);
        if (rc > 0) { written += rc; continue; }
        if (rc == -EPIPE) {
            if (snd_pcm_prepare(ao->pcm) < 0) break;
            continue;
        }
        if (rc < 0 && snd_pcm_recover(ao->pcm, rc, 1) >= 0) continue;
        break;
    }
}

static void *audio_thread(void *arg) {
    audio_out_t *ao = arg;
    const int max_frames = 2048;
    uint8_t *chunk = malloc((size_t)max_frames * ao->frame_bytes);
    if (!chunk) return NULL;

    for (;;) {
        pthread_mutex_lock(&ao->queue_lock);
        while (!ao->stopping && ao->queue_used < (size_t)ao->frame_bytes)
            pthread_cond_wait(&ao->queue_ready, &ao->queue_lock);
        if (ao->stopping) { pthread_mutex_unlock(&ao->queue_lock); break; }

        size_t bytes = ao->queue_used;
        size_t max_bytes = (size_t)max_frames * ao->frame_bytes;
        if (bytes > max_bytes) bytes = max_bytes;
        bytes -= bytes % ao->frame_bytes;
        size_t first = ao->queue_capacity - ao->queue_read;
        if (first > bytes) first = bytes;
        memcpy(chunk, ao->queue + ao->queue_read, first);
        if (bytes > first) memcpy(chunk + first, ao->queue, bytes - first);
        ao->queue_read = (ao->queue_read + bytes) % ao->queue_capacity;
        ao->queue_used -= bytes;
        pthread_mutex_unlock(&ao->queue_lock);

        audio_write_pcm(ao, chunk, (int)(bytes / ao->frame_bytes));
    }

    free(chunk);
    return NULL;
}

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
    ao->frame_bytes = ao->channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
    ao->queue_capacity = (size_t)48000 * ao->frame_bytes * 2; /* two seconds */
    ao->queue = malloc(ao->queue_capacity);
    if (!ao->queue) { swr_free(&ao->swr); snd_pcm_close(ao->pcm); return false; }
    pthread_mutex_init(&ao->queue_lock, NULL);
    pthread_cond_init(&ao->queue_ready, NULL);
    if (pthread_create(&ao->thread, NULL, audio_thread, ao) != 0) {
        pthread_cond_destroy(&ao->queue_ready);
        pthread_mutex_destroy(&ao->queue_lock);
        free(ao->queue);
        swr_free(&ao->swr);
        snd_pcm_close(ao->pcm);
        return false;
    }
    ao->thread_started = true;
    return true;
}

static void audio_play(audio_out_t *ao, AVFrame *frame) {
    if (!ao->pcm) return;
    int out_samples = av_rescale_rnd(swr_get_delay(ao->swr, frame->sample_rate) + frame->nb_samples,
                                      48000, frame->sample_rate, AV_ROUND_UP);
    if (out_samples > ao->convert_capacity_samples) {
        av_freep(&ao->convert_buffer);
        if (av_samples_alloc(&ao->convert_buffer, NULL, ao->channels, out_samples,
                             AV_SAMPLE_FMT_S16, 0) < 0)
            return;
        ao->convert_capacity_samples = out_samples;
    }
    int n = swr_convert(ao->swr, &ao->convert_buffer, out_samples,
                        (const uint8_t **)frame->extended_data, frame->nb_samples);
    if (n > 0) {
        size_t bytes = (size_t)n * ao->frame_bytes;
        pthread_mutex_lock(&ao->queue_lock);
        if (bytes <= ao->queue_capacity - ao->queue_used) {
            size_t first = ao->queue_capacity - ao->queue_write;
            if (first > bytes) first = bytes;
            memcpy(ao->queue + ao->queue_write, ao->convert_buffer, first);
            if (bytes > first) memcpy(ao->queue, ao->convert_buffer + first, bytes - first);
            ao->queue_write = (ao->queue_write + bytes) % ao->queue_capacity;
            ao->queue_used += bytes;
            pthread_cond_signal(&ao->queue_ready);
        }
        pthread_mutex_unlock(&ao->queue_lock);
    }
}

static void audio_close(audio_out_t *ao) {
    if (ao->swr) swr_free(&ao->swr);
    av_freep(&ao->convert_buffer);
    if (ao->thread_started) {
        pthread_mutex_lock(&ao->queue_lock);
        ao->stopping = true;
        pthread_cond_signal(&ao->queue_ready);
        pthread_mutex_unlock(&ao->queue_lock);
        pthread_join(ao->thread, NULL);
        pthread_cond_destroy(&ao->queue_ready);
        pthread_mutex_destroy(&ao->queue_lock);
    }
    free(ao->queue);
    if (ao->pcm) { snd_pcm_drain(ao->pcm); snd_pcm_close(ao->pcm); }
}

static void audio_pace_frame(const AVFrame *frame, AVRational time_base, int64_t start_us,
                             int64_t *first_frame_us, const volatile bool *cancel) {
    int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
                    ? frame->best_effort_timestamp : frame->pts;
    if (pts == AV_NOPTS_VALUE || time_base.den <= 0) return;
    int64_t frame_us = av_rescale_q(pts, time_base, AV_TIME_BASE_Q);
    if (*first_frame_us == AV_NOPTS_VALUE) *first_frame_us = frame_us;
    int64_t target = start_us + frame_us - *first_frame_us;
    while (!cancel || !*cancel) {
        int64_t remaining = target - av_gettime_relative();
        if (remaining <= 0) break;
        /* Short waits preserve prompt playlist cancellation while preventing
         * an audio-only file from filling the bounded PCM queue instantly. */
        av_usleep((unsigned)(remaining > 20000 ? 20000 : remaining));
    }
}

static media_result_t play_audio_only(AVFormatContext *fmt, int astream, int duration_sec,
                                      bool muted, const volatile bool *cancel) {
    int64_t start_us = av_gettime_relative();
    int64_t deadline_us = duration_sec > 0 ? start_us + (int64_t)duration_sec * AV_TIME_BASE : -1;
    if (muted) {
        while (deadline_us > 0 && av_gettime_relative() < deadline_us && (!cancel || !*cancel))
            av_usleep(20000);
        return MEDIA_OK;
    }

    AVCodecParameters *apar = fmt->streams[astream]->codecpar;
    const AVCodec *acodec = avcodec_find_decoder(apar->codec_id);
    AVCodecContext *actx = acodec ? avcodec_alloc_context3(acodec) : NULL;
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    audio_out_t ao = {0};
    bool have_audio = false;
    media_result_t result = MEDIA_ERROR;
    if (!actx || !frame || !pkt || avcodec_parameters_to_context(actx, apar) < 0 ||
        avcodec_open2(actx, acodec, NULL) < 0 || !audio_open(&ao, actx))
        goto done;
    have_audio = true;

    AVRational time_base = fmt->streams[astream]->time_base;
    int64_t first_frame_us = AV_NOPTS_VALUE;
    while (!cancel || !*cancel) {
        int rd = av_read_frame(fmt, pkt);
        if (rd < 0) break;
        if (pkt->stream_index == astream && avcodec_send_packet(actx, pkt) == 0) {
            while (avcodec_receive_frame(actx, frame) == 0) {
                audio_play(&ao, frame);
                audio_pace_frame(frame, time_base, start_us, &first_frame_us, cancel);
                av_frame_unref(frame);
                if (cancel && *cancel) break;
            }
        }
        av_packet_unref(pkt);
    }
    if (!cancel || !*cancel) {
        avcodec_send_packet(actx, NULL);
        while (avcodec_receive_frame(actx, frame) == 0) {
            audio_play(&ao, frame);
            audio_pace_frame(frame, time_base, start_us, &first_frame_us, cancel);
            av_frame_unref(frame);
        }
    }
    result = MEDIA_OK;
    /* If the playlist slot is longer than the source, preserve the quiet
     * background until its scheduled transition rather than restarting it. */
    while (deadline_us > 0 && av_gettime_relative() < deadline_us && (!cancel || !*cancel))
        av_usleep(20000);

done:
    av_packet_free(&pkt);
    av_frame_free(&frame);
    if (have_audio) audio_close(&ao);
    avcodec_free_context(&actx);
    return result;
}
#endif

media_result_t media_play_item(stp_display_t *disp, stp_rect_t rect, const char *path,
                                const char *fit_mode, uint32_t bg_bgra,
                                int duration_sec, bool muted, const volatile bool *cancel) {
    media_result_t result = MEDIA_OK;
    AVFormatContext *fmt = NULL;
    struct SwsContext *sws = NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    uint8_t *staging = NULL;
    int staging_stride = 0;
    char *svg_png = NULL;
    const char *input_path = path;
    if (has_svg_extension(path)) {
        svg_png = rasterize_svg(path, rect.w, rect.h);
        if (!svg_png) {
            LOG("media: SVG '%s' needs rsvg-convert or ImageMagick to rasterize", path);
            fill_bgra(disp, rect, bg_bgra);
            return MEDIA_ERROR;
        }
        input_path = svg_png;
    }
    if (avformat_open_input(&fmt, input_path, NULL, NULL) < 0) {
        LOG("media: cannot open '%s'", path);
        fill_bgra(disp, rect, bg_bgra);
        free(svg_png);
        return MEDIA_ERROR;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        LOG("media: no stream info in '%s'", path);
        avformat_close_input(&fmt);
        free(svg_png);
        return MEDIA_ERROR;
    }

    int vstream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int astream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (vstream < 0) {
        if (astream >= 0) {
            fill_bgra(disp, rect, bg_bgra);
            disp->present(disp);
#ifdef WITH_ALSA
            media_result_t audio_result = play_audio_only(fmt, astream, duration_sec, muted, cancel);
            avformat_close_input(&fmt);
            free(svg_png);
            return audio_result;
#else
            LOG("media: audio-only item '%s' cannot play (built without ALSA)", path);
            avformat_close_input(&fmt);
            free(svg_png);
            return MEDIA_ERROR;
#endif
        }
        LOG("media: no playable stream in '%s'", path);
        avformat_close_input(&fmt);
        free(svg_png);
        return MEDIA_ERROR;
    }
    /* Some FFmpeg builds expose a standalone PNG as png_pipe rather than
     * image2.  The file is still a one-frame image, not a video stream. */
    bool source_is_image = image_demuxer(fmt) || has_static_image_extension(input_path);

    AVCodecParameters *vpar = fmt->streams[vstream]->codecpar;
    const AVCodec *vcodec = avcodec_find_decoder(vpar->codec_id);
    if (!vcodec) {
        LOG("media: no decoder for '%s'", path);
        avformat_close_input(&fmt);
        free(svg_png);
        return MEDIA_ERROR;
    }
    AVCodecContext *vctx = avcodec_alloc_context3(vcodec);
    if (!vctx) {
        avformat_close_input(&fmt);
        free(svg_png);
        return MEDIA_ERROR;
    }
    avcodec_parameters_to_context(vctx, vpar);
    vctx->flags2 |= AV_CODEC_FLAG2_FAST;
    int preview_w, preview_h, preview_x, preview_y;
    fit_rect(rect.w, rect.h, vpar->width, vpar->height, fit_mode,
             &preview_x, &preview_y, &preview_w, &preview_h);
    if (!source_is_image && (vpar->width > preview_w || vpar->height > preview_h)) {
        vctx->skip_loop_filter = AVDISCARD_ALL;
        /* B/non-reference pictures are the first safe work to shed on a
         * CPU-only downscale.  Decoder references remain intact. */
        vctx->skip_frame = AVDISCARD_NONREF;
    }
    /* Allow the codec to use frame and/or slice workers.  The EOF drain below
     * renders frame-threaded output that remains queued for short clips and
     * still images. */
    vctx->thread_count = source_is_image ? 1 : 0; /* auto maximum for video */
    vctx->thread_type = source_is_image ? 0 : (FF_THREAD_FRAME | FF_THREAD_SLICE);
    if (avcodec_open2(vctx, vcodec, NULL) < 0) {
        LOG("media: no decoder for '%s'", path);
        avcodec_free_context(&vctx);
        avformat_close_input(&fmt);
        free(svg_png);
        return MEDIA_ERROR;
    }

    bool is_still = source_is_image || fmt->streams[vstream]->nb_frames == 1 ||
                    (fmt->duration <= 0 && !(fmt->iformat->flags & AVFMT_TS_DISCONT) == 0 && vstream >= 0 &&
                     fmt->streams[vstream]->avg_frame_rate.num == 0);

    AVCodecContext *actx = NULL;
#ifdef WITH_ALSA
    audio_out_t ao = {0};
    bool have_audio = false;
    if (!muted && astream >= 0 && duration_sec != 0 /* skip decoding audio for stills-as-image calls */) {
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

    bool jpeg_full_range;
    enum AVPixelFormat scaler_fmt = jpeg_compatible_format(vctx->pix_fmt, &jpeg_full_range);
    if (vctx->color_range == AVCOL_RANGE_JPEG) jpeg_full_range = true;
    sws = make_scaler(vctx->width, vctx->height, scaler_fmt, dst_w, dst_h,
                      jpeg_full_range, vctx->colorspace,
                      disp->pixel_layout == STP_PIXELS_RGBX ? AV_PIX_FMT_RGBA : AV_PIX_FMT_BGRA);
    if (!sws) {
        LOG("media: could not create scaler for '%s'", path);
        result = MEDIA_ERROR;
        goto done;
    }

    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!frame || !pkt) {
        result = MEDIA_ERROR;
        goto done;
    }

    size_t dst_offset = (size_t)(rect.y + dst_y) * disp->stride + (size_t)(rect.x + dst_x) * 4;
    staging_stride = dst_w * 4;
    /* DRM dumb mappings can be substantially slower than cacheable RAM for
     * per-pixel scaler writes.  Scale in RAM and use a linear copy to scanout. */
    if (disp->backend_name && strcmp(disp->backend_name, "drm") == 0)
        staging = av_malloc((size_t)staging_stride * dst_h);

    /* Playback must never react to NTP/manual wall-clock corrections. */
    int64_t start_us = av_gettime_relative();
    int64_t deadline_us = duration_sec > 0 ? start_us + (int64_t)duration_sec * 1000000 : -1;
    bool drew_any_frame = false;
    AVRational vtb = fmt->streams[vstream]->time_base;
    int64_t first_frame_us = AV_NOPTS_VALUE;
    int64_t next_present_us = 0;
    int64_t render_interval_us = resize_render_interval(fmt->streams[vstream],
                                                          vctx->width != dst_w || vctx->height != dst_h);
    int64_t render_lead_us = 0;

    for (bool keep_looping = true; keep_looping; ) {
        int rd;
        while ((rd = av_read_frame(fmt, pkt)) >= 0) {
            if (cancel && *cancel) { av_packet_unref(pkt); goto done; }
            if (pkt->stream_index == vstream) {
                if (avcodec_send_packet(vctx, pkt) == 0) {
                    while (avcodec_receive_frame(vctx, frame) == 0) {
                        if (present_video_frame(disp, sws, vctx, frame, dst_offset, disp->stride,
                                                staging, staging_stride, dst_w, dst_h,
                                                vtb, start_us, &first_frame_us, &next_present_us,
                                                &render_interval_us, &render_lead_us, is_still))
                            drew_any_frame = true;
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

        /* Frame-threaded decoders intentionally hold several frames while
         * they work ahead.  Feed EOF and drain them before deciding whether
         * this item produced a picture; otherwise a still image (one packet)
         * can remain entirely queued and leave its zone blank. */
        if (avcodec_send_packet(vctx, NULL) == 0) {
            while (avcodec_receive_frame(vctx, frame) == 0) {
                if (cancel && *cancel) { av_frame_unref(frame); goto done; }
                if (present_video_frame(disp, sws, vctx, frame, dst_offset, disp->stride,
                                        staging, staging_stride, dst_w, dst_h,
                                        vtb, start_us, &first_frame_us, &next_present_us,
                                        &render_interval_us, &render_lead_us, is_still))
                    drew_any_frame = true;
                av_frame_unref(frame);
            }
        }

        if (!drew_any_frame) { result = MEDIA_ERROR; goto done; }

        /* An item plays its source once.  If its configured playlist slot is
         * longer than the source, retain the final frame below instead of
         * seeking back to the start and replaying the clip. */
        keep_looping = false;
    }

    /* still image / EOF-without-deadline: sleep out the remaining duration
     * in short slices so cancel/reload can interrupt promptly. */
    if (deadline_us > 0) {
        while (av_gettime_relative() < deadline_us) {
            if (cancel && *cancel) break;
            av_usleep(100000);
        }
    }

done:
    av_free(staging);
    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&vctx);
#ifdef WITH_ALSA
    if (have_audio) audio_close(&ao);
    if (actx) avcodec_free_context(&actx);
#endif
    avformat_close_input(&fmt);
    free(svg_png);
    return result;
}
