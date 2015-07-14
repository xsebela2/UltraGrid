/**
 * @file   video_compress/libavcodec.h
 * @author Martin Pulec     <pulec@cesnet.cz>
 */
/*
 * Copyright (c) 2013-2014 CESNET, z. s. p. o.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, is permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of CESNET nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define __STDC_CONSTANT_MACROS

#ifdef HAVE_CONFIG_H
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#endif // HAVE_CONFIG_H

#include "libavcodec_common.h"
#include "video_compress/libavcodec.h"

#include <assert.h>

#include "debug.h"
#include "host.h"
#include "messaging.h"
#include "module.h"
#include "utils/misc.h"
#include "utils/resource_manager.h"
#include "utils/worker.h"
#include "video.h"
#include "video_compress.h"

#include <string>
#include <thread>
#include <unordered_map>

using namespace std;

#define DEFAULT_CODEC MJPG
#define DEFAULT_X264_PRESET "superfast"
static constexpr const char *DEFAULT_NVENC_H264_PRESET = "llhp";
static constexpr const int DEFAULT_GOP_SIZE = 20;
namespace {

struct setparam_param {
        AVCodec *codec;
        bool have_preset;
        double fps;
        bool interlaced;
        bool h264_no_periodic_intra;
        int cpu_count;
        string threads;
};

typedef struct {
        enum AVCodecID av_codec;
        const char *prefered_encoder; ///< can be nullptr
        double avg_bpp;
        void (*set_param)(AVCodecContext *, struct setparam_param *);
} codec_params_t;


static void setparam_default(AVCodecContext *, struct setparam_param *);
static void setparam_h264(AVCodecContext *, struct setparam_param *);
static void setparam_h265(AVCodecContext *, struct setparam_param *);
static void setparam_vp8(AVCodecContext *, struct setparam_param *);
static void libavcodec_check_messages(struct state_video_compress_libav *s);
static void libavcodec_compress_done(struct module *mod);
static void libavcodec_vid_enc_frame_dispose(struct video_frame *);


static unordered_map<codec_t, codec_params_t, hash<int>> codec_params = {
        {H264, { AV_CODEC_ID_H264,
                "libx264",
                0.07 * 2 /* for H.264: 1 - low motion, 2 - medium motion, 4 - high motion */
                * 2, // take into consideration that our H.264 is less effective due to specific preset/tune
                setparam_h264
        }},
        { H265, {
#if (LIBAVCODEC_VERSION_MAJOR > 55) || (LIBAVCODEC_VERSION_MAJOR == 55 && LIBAVCODEC_VERSION_MINOR > 35) || (LIBAVCODEC_VERSION_MAJOR == 55 && LIBAVCODEC_VERSION_MINOR > 35 && LIBAVCODEC_VERSION_MICRO > 100)
                AV_CODEC_ID_HEVC,
#else
                AV_CODEC_ID_NONE,
#endif
                "libx265", //nullptr,
                0.07 * 2 * 2,
                setparam_h265
        }},
        { MJPG, {
                AV_CODEC_ID_MJPEG,
                nullptr,
                1.2,
                setparam_default
        }},
        { J2K, {
                AV_CODEC_ID_JPEG2000,
                nullptr,
                1.0,
                setparam_default
        }},
        { VP8, {
                AV_CODEC_ID_VP8,
                nullptr,
                0.4,
                setparam_vp8
        }},
};

struct state_video_compress_libav {
        struct module       module_data;

        pthread_mutex_t    *lavcd_global_lock;

        struct video_desc   saved_desc;

        AVFrame            *in_frame;
        // for every core - parts of the above
        AVFrame           **in_frame_part;
        int                 cpu_count;
        AVCodec            *codec;
        AVCodecContext     *codec_ctx;

        unsigned char      *decoded;
        decoder_t           decoder;

        codec_t             selected_codec_id;
        int64_t             requested_bitrate;
        double              requested_bpp;
        // may be 422, 420 or 0 (no subsampling explicitly requested
        int                 requested_subsampling;
        // actual value used
        AVPixelFormat       selected_pixfmt;

        codec_t             out_codec;
        char               *preset;

        struct video_desc compressed_desc;

        struct setparam_param params;
        string              backend;
        int                 requested_gop;
};

void to_yuv420p(AVFrame *out_frame, unsigned char *in_data, int width, int height);
void to_yuv422p(AVFrame *out_frame, unsigned char *in_data, int width, int height);
void to_yuv444p(AVFrame *out_frame, unsigned char *in_data, int width, int height);
void to_nv12(AVFrame *out_frame, unsigned char *in_data, int width, int height);
typedef void (*pixfmt_callback_t)(AVFrame *out_frame, unsigned char *in_data, int width, int height);
pixfmt_callback_t select_pixfmt_callback(AVPixelFormat fmt);


static void usage(void);
static int parse_fmt(struct state_video_compress_libav *s, char *fmt);
static void cleanup(struct state_video_compress_libav *s);

static void libavcodec_vid_enc_frame_dispose(struct video_frame *frame) {
#if LIBAVCODEC_VERSION_MAJOR >= 54
        AVPacket *pkt = (AVPacket *) frame->dispose_udata;
        av_free_packet(pkt);
        free(pkt);
#else
        free(frame->tiles[0].data);
#endif // LIBAVCODEC_VERSION_MAJOR >= 54
        vf_free(frame);
}

static void usage() {
        printf("Libavcodec encoder usage:\n");
        printf("\t-c libavcodec[:codec=<codec_name>][:bitrate=<bits_per_sec>|:bpp=<bits_per_pixel>]"
                        "[:subsampling=<subsampling>][:preset=<preset>][:gop=<gop>]"
                        "[:h264_no_periodic_intra][:threads=<thr_mode>][:backend=<backend>]\n");
        printf("\t\t<codec_name> may be specified codec name (default MJPEG), supported codecs:\n");
        for (auto && param : codec_params) {
                if(param.second.av_codec != 0) {
                        const char *availability = "not available";
                        if(avcodec_find_encoder(param.second.av_codec)) {
                                availability = "available";
                        }
                        printf("\t\t\t%s - %s\n", get_codec_name(param.first), availability);
                }

        }
        printf("\t\th264_no_periodic_intra - do not use Periodic Intra Refresh with H.264\n");
        printf("\t\t<bits_per_sec> specifies requested bitrate\n");
        printf("\t\t\t0 means codec default (same as when parameter omitted)\n");
        printf("\t\t<subsampling> may be one of 444, 422, or 420, default 420 for progresive, 422 for interlaced\n");
        printf("\t\t<preset> codec preset options, eg. ultrafast, superfast, medium etc. for H.264\n");
        printf("\t\t<thr_mode> can be one of \"no\", \"frame\" or \"slice\"\n");
        printf("\t\t<gop> specifies GOP size\n");
        printf("\t\t<backend> specifies encoder backend (eg. nvenc or libx264 for H.264)\n");
}

static int parse_fmt(struct state_video_compress_libav *s, char *fmt) {
        char *item, *save_ptr = NULL;
        if(fmt) {
                while((item = strtok_r(fmt, ":", &save_ptr)) != NULL) {
                        if(strncasecmp("help", item, strlen("help")) == 0) {
                                usage();
                                return 1;
                        } else if(strncasecmp("codec=", item, strlen("codec=")) == 0) {
                                char *codec = item + strlen("codec=");
                                s->selected_codec_id = get_codec_from_name(codec);
                                if (s->selected_codec_id == VIDEO_CODEC_NONE) {
                                        fprintf(stderr, "[lavd] Unable to find codec: \"%s\"\n", codec);
                                        return -1;
                                }
                        } else if(strncasecmp("bitrate=", item, strlen("bitrate=")) == 0) {
                                char *bitrate_str = item + strlen("bitrate=");
                                s->requested_bitrate = unit_evaluate(bitrate_str);
                        } else if(strncasecmp("bpp=", item, strlen("bpp=")) == 0) {
                                char *bpp_str = item + strlen("bpp=");
                                s->requested_bpp = unit_evaluate(bpp_str);
                        } else if(strncasecmp("subsampling=", item, strlen("subsampling=")) == 0) {
                                char *subsample_str = item + strlen("subsampling=");
                                s->requested_subsampling = atoi(subsample_str);
                                if (s->requested_subsampling != 444 &&
                                                s->requested_subsampling != 422 &&
                                                s->requested_subsampling != 420) {
                                        fprintf(stderr, "[lavc] Supported subsampling is 444, 422, or 420.\n");
                                        return -1;
                                }
                        } else if(strncasecmp("preset=", item, strlen("preset=")) == 0) {
                                char *preset = item + strlen("preset=");
                                s->preset = strdup(preset);
                        } else if (strcasecmp("h264_no_periodic_intra", item) == 0) {
                                s->params.h264_no_periodic_intra = true;
                        } else if(strncasecmp("threads=", item, strlen("threads=")) == 0) {
                                char *threads = item + strlen("threads=");
                                s->params.threads = threads;
                        } else if(strncasecmp("backend=", item, strlen("backend=")) == 0) {
                                char *backend = item + strlen("backend=");
                                s->backend = backend;
                        } else if(strncasecmp("gop=", item, strlen("gop=")) == 0) {
                                char *gop = item + strlen("gop=");
                                s->requested_gop = atoi(gop);
                        } else {
                                fprintf(stderr, "[lavc] Error: unknown option %s.\n",
                                                item);
                                return -1;
                        }
                        fmt = NULL;
                }
        }

        return 0;
}

bool libavcodec_is_supported() {
        avcodec_register_all();
        if (avcodec_find_encoder(AV_CODEC_ID_H264)) {
                return true;
        } else {
                return false;
        }
}

struct module * libavcodec_compress_init(struct module *parent, const struct video_compress_params *params)
{
        struct state_video_compress_libav *s;
        const char *opts = params->cfg;

        s = new state_video_compress_libav();
        s->lavcd_global_lock = rm_acquire_shared_lock(LAVCD_LOCK_NAME);
        if (log_level >= LOG_LEVEL_VERBOSE) {
                av_log_set_level(AV_LOG_VERBOSE);
        }
        /*  register all the codecs (you can also register only the codec
         *         you wish to have smaller code */
        avcodec_register_all();

        s->codec = NULL;
        s->codec_ctx = NULL;
        s->in_frame = NULL;
        s->selected_codec_id = DEFAULT_CODEC;
        s->requested_subsampling = 0;
        s->preset = NULL;

        s->requested_bitrate = -1;

        memset(&s->saved_desc, 0, sizeof(s->saved_desc));

        char *fmt = strdup(opts);
        int ret = parse_fmt(s, fmt);
        free(fmt);
        if(ret != 0) {
                delete s;
                if(ret > 0)
                        return &compress_init_noerr;
                else
                        return NULL;
        }

        printf("[Lavc] Using codec: %s\n", get_codec_name(s->selected_codec_id));

        s->cpu_count = thread::hardware_concurrency();
        if(s->cpu_count < 1) {
                fprintf(stderr, "Warning: Cannot get number of CPU cores!\n");
                s->cpu_count = 1;
        }
        s->in_frame_part = (AVFrame **) calloc(s->cpu_count, sizeof(AVFrame *));
        for(int i = 0; i < s->cpu_count; i++) {
                s->in_frame_part[i] = av_frame_alloc();
        }

        s->decoded = NULL;

        module_init_default(&s->module_data);
        s->module_data.cls = MODULE_CLASS_DATA;
        s->module_data.priv_data = s;
        s->module_data.deleter = libavcodec_compress_done;
        module_register(&s->module_data, parent);

        return &s->module_data;
}

static bool configure_with(struct state_video_compress_libav *s, struct video_desc desc)
{
        int ret;
        AVCodecID codec_id;
        AVPixelFormat pix_fmt;
        double avg_bpp; // average bit per pixel
        const char *prefered_encoder;

        s->compressed_desc = desc;

        auto it = codec_params.find(s->selected_codec_id);
        if (it != codec_params.end()) {
                codec_id = it->second.av_codec;
                prefered_encoder = it->second.prefered_encoder;
                if (s->requested_bpp != 0.0) {
                        avg_bpp = s->requested_bpp;
                } else {
                        avg_bpp = it->second.avg_bpp;
                }
        } else {
                fprintf(stderr, "[lavc] Requested output codec isn't "
                                "supported by libavcodec.\n");
                return false;
        }

        s->compressed_desc.color_spec = s->selected_codec_id;
        s->compressed_desc.tile_count = 1;

#ifndef HAVE_GPL
        if(s->selected_codec_id == H264) {
                fprintf(stderr, "H.264 is not available in UltraGrid BSD build. "
                                "Reconfigure UltraGrid with --enable-gpl if "
                                "needed.\n");
                exit_uv(1);
                return false;
        } else if (s->selected_codec_id == H265) {
                fprintf(stderr, "H.265 is not available in UltraGrid BSD build. "
                                "Reconfigure UltraGrid with --enable-gpl if "
                                "needed.\n");
                exit_uv(1);
                return false;
        }
#endif

        s->codec = nullptr;
        if (!s->backend.empty()) {
                s->codec = avcodec_find_encoder_by_name(s->backend.c_str());
                if (!s->codec) {
                        fprintf(stderr, "[lavc] Warning: requested encoder \"%s\" not found!\n",
                                        s->backend.c_str());
                        return false;
                }
        } else if (prefered_encoder) {
                s->codec = avcodec_find_encoder_by_name(prefered_encoder);
                if (!s->codec) {
                        fprintf(stderr, "[lavc] Warning: prefered encoder \"%s\" not found! Trying default encoder.\n",
                                        prefered_encoder);
                }
        }
        if (!s->codec) {
                /* find the video encoder */
                s->codec = avcodec_find_encoder(codec_id);
        }
        if (!s->codec) {
                fprintf(stderr, "Libavcodec doesn't contain encoder for specified codec.\n"
                                "Hint: Check if you have libavcodec-extra package installed.\n");
                return false;
        }

        enum AVPixelFormat requested_pix_fmts[100];
        int total_pix_fmts = 0;

        if (s->requested_subsampling == 0) {
                // for interlaced formats, it is better to use either 422 or 444
                if (desc.interlacing == INTERLACED_MERGED) {
                        // 422
                        memcpy(requested_pix_fmts + total_pix_fmts,
                                        fmts422, sizeof(fmts422));
                        total_pix_fmts += sizeof(fmts422) / sizeof(enum AVPixelFormat);
                        // 444
                        memcpy(requested_pix_fmts + total_pix_fmts,
                                        fmts444, sizeof(fmts444));
                        total_pix_fmts += sizeof(fmts444) / sizeof(enum AVPixelFormat);
                        // 420
                        memcpy(requested_pix_fmts + total_pix_fmts,
                                        fmts420, sizeof(fmts420));
                        total_pix_fmts += sizeof(fmts420) / sizeof(enum AVPixelFormat);
                } else {
                        // 420
                        memcpy(requested_pix_fmts + total_pix_fmts,
                                        fmts420, sizeof(fmts420));
                        total_pix_fmts += sizeof(fmts420) / sizeof(enum AVPixelFormat);
                        // 422
                        memcpy(requested_pix_fmts + total_pix_fmts,
                                        fmts422, sizeof(fmts422));
                        total_pix_fmts += sizeof(fmts422) / sizeof(enum AVPixelFormat);
                        // 444
                        memcpy(requested_pix_fmts + total_pix_fmts,
                                        fmts444, sizeof(fmts444));
                        total_pix_fmts += sizeof(fmts444) / sizeof(enum AVPixelFormat);
                }
                // there was a problem with codecs other than PIX_FMT_NV12 with NVENC.
                // Therefore, use only this with NVENC for now.
                if (strcmp(s->codec->name, "nvenc") == 0) {
                        fprintf(stderr, "[Lavc] Using %s. Other pix formats seem to be broken with NVENC.\n",
                                        av_get_pix_fmt_name(AV_PIX_FMT_NV12));
                        requested_pix_fmts[0] = AV_PIX_FMT_NV12;
                        total_pix_fmts = 1;
                }
        } else {
                switch (s->requested_subsampling) {
                case 420:
                        memcpy(requested_pix_fmts + total_pix_fmts,
                                        fmts420, sizeof(fmts420));
                        total_pix_fmts += sizeof(fmts420) / sizeof(enum AVPixelFormat);
                        break;
                case 422:
                        memcpy(requested_pix_fmts + total_pix_fmts,
                                        fmts422, sizeof(fmts422));
                        total_pix_fmts += sizeof(fmts422) / sizeof(enum AVPixelFormat);
                        break;
                case 444:
                        memcpy(requested_pix_fmts + total_pix_fmts,
                                        fmts444, sizeof(fmts444));
                        total_pix_fmts += sizeof(fmts444) / sizeof(enum AVPixelFormat);
                        break;
                default:
                        abort();
                }
        }
        requested_pix_fmts[total_pix_fmts++] = AV_PIX_FMT_NONE;

        pix_fmt = get_best_pix_fmt(requested_pix_fmts, s->codec->pix_fmts);
        if(pix_fmt == AV_PIX_FMT_NONE) {
                fprintf(stderr, "[Lavc] Unable to find suitable pixel format.\n");
                if (s->requested_subsampling != 0) {
                        fprintf(stderr, "[Lavc] Requested subsampling not supported. "
                                        "Try different subsampling, eg. "
                                        "\"subsampling={420,422,444}\".\n");
                }
                return false;
        }

        printf("[Lavc] Selected pixfmt: %s\n", av_get_pix_fmt_name(pix_fmt));
        s->selected_pixfmt = pix_fmt;

        // avcodec_alloc_context3 allocates context and sets default value
        s->codec_ctx = avcodec_alloc_context3(s->codec);
        if (!s->codec_ctx) {
                fprintf(stderr, "Could not allocate video codec context\n");
                return false;
        }

        s->codec_ctx->strict_std_compliance = -2;

        /* put parameters */
        if(s->requested_bitrate > 0) {
                s->codec_ctx->bit_rate = s->requested_bitrate;
        } else {
                s->codec_ctx->bit_rate = desc.width * desc.height *
                        avg_bpp * desc.fps;
        }

        s->codec_ctx->bit_rate_tolerance = s->codec_ctx->bit_rate / 4;

        /* resolution must be a multiple of two */
        s->codec_ctx->width = desc.width;
        s->codec_ctx->height = desc.height;
        /* frames per second */
        s->codec_ctx->time_base= (AVRational){1,(int) desc.fps};
        if (s->requested_gop) {
                s->codec_ctx->gop_size = s->requested_gop;
        } else {
                s->codec_ctx->gop_size = DEFAULT_GOP_SIZE;
        }
        s->codec_ctx->max_b_frames = 0;
        switch(desc.color_spec) {
                case UYVY:
                        s->decoder = (decoder_t) memcpy;
                        break;
                case YUYV:
                        s->decoder = (decoder_t) vc_copylineYUYV;
                        break;
                case v210:
                        s->decoder = (decoder_t) vc_copylinev210;
                        break;
                case RGB:
                        s->decoder = (decoder_t) vc_copylineRGBtoUYVY;
                        break;
                case BGR:
                        s->decoder = (decoder_t) vc_copylineBGRtoUYVY;
                        break;
                case RGBA:
                        s->decoder = (decoder_t) vc_copylineRGBAtoUYVY;
                        break;
                default:
                        fprintf(stderr, "[Libavcodec] Unable to find "
                                        "appropriate pixel format.\n");
                        return false;
        }

        s->codec_ctx->pix_fmt = pix_fmt;

        s->decoded = (unsigned char *) malloc(desc.width * desc.height * 4);

        if(s->preset) {
                if(av_opt_set(s->codec_ctx->priv_data, "preset", s->preset, 0) != 0) {
                        fprintf(stderr, "[Lavc] Error: Unable to set preset.\n");
                        return false;
                }
        }

        s->params.have_preset = s->preset != 0;
        s->params.fps = desc.fps;
        s->params.codec = s->codec;
        s->params.interlaced = desc.interlacing == INTERLACED_MERGED;
        s->params.cpu_count = s->cpu_count;

        codec_params[s->selected_codec_id].set_param(s->codec_ctx, &s->params);

        pthread_mutex_lock(s->lavcd_global_lock);
        /* open it */
        if (avcodec_open2(s->codec_ctx, s->codec, NULL) < 0) {
                fprintf(stderr, "Could not open codec\n");
                pthread_mutex_unlock(s->lavcd_global_lock);
                return false;
        }
        pthread_mutex_unlock(s->lavcd_global_lock);

        s->in_frame = av_frame_alloc();
        if (!s->in_frame) {
                fprintf(stderr, "Could not allocate video frame\n");
                return false;
        }
#if LIBAVCODEC_VERSION_MAJOR >= 53
        s->in_frame->format = s->codec_ctx->pix_fmt;
        s->in_frame->width = s->codec_ctx->width;
        s->in_frame->height = s->codec_ctx->height;
#endif

        /* the image can be allocated by any means and av_image_alloc() is
         * just the most convenient way if av_malloc() is to be used */
        ret = av_image_alloc(s->in_frame->data, s->in_frame->linesize,
                        s->codec_ctx->width, s->codec_ctx->height,
                        s->codec_ctx->pix_fmt, 32);
        if (ret < 0) {
                fprintf(stderr, "Could not allocate raw picture buffer\n");
                return false;
        }
        for(int i = 0; i < s->cpu_count; ++i) {
                int chunk_size = s->codec_ctx->height / s->cpu_count;
                chunk_size = chunk_size / 2 * 2;
                s->in_frame_part[i]->data[0] = s->in_frame->data[0] + s->in_frame->linesize[0] * i *
                        chunk_size;
                if(is420(s->selected_pixfmt)) {
                        chunk_size /= 2;
                }
                s->in_frame_part[i]->data[1] = s->in_frame->data[1] + s->in_frame->linesize[1] * i *
                        chunk_size;
                s->in_frame_part[i]->data[2] = s->in_frame->data[2] + s->in_frame->linesize[2] * i *
                        chunk_size;
                s->in_frame_part[i]->linesize[0] = s->in_frame->linesize[0];
                s->in_frame_part[i]->linesize[1] = s->in_frame->linesize[1];
                s->in_frame_part[i]->linesize[2] = s->in_frame->linesize[2];
        }

        s->saved_desc = desc;
        s->out_codec = s->compressed_desc.color_spec;

        return true;
}

void to_yuv420p(AVFrame *out_frame, unsigned char *in_data, int width, int height)
{
        for(int y = 0; y < height; y += 2) {
                /*  every even row */
                unsigned char *src = in_data + y * (width * 2);
                /*  every odd row */
                unsigned char *src2 = in_data + (y + 1) * (width * 2);
                unsigned char *dst_y = out_frame->data[0] + out_frame->linesize[0] * y;
                unsigned char *dst_y2 = out_frame->data[0] + out_frame->linesize[0] * (y + 1);
                unsigned char *dst_cb = out_frame->data[1] + out_frame->linesize[1] * y / 2;
                unsigned char *dst_cr = out_frame->data[2] + out_frame->linesize[2] * y / 2;
                for(int x = 0; x < width / 2; ++x) {
                        *dst_cb++ = (*src++ + *src2++) / 2;
                        *dst_y++ = *src++;
                        *dst_y2++ = *src2++;
                        *dst_cr++ = (*src++ + *src2++) / 2;
                        *dst_y++ = *src++;
                        *dst_y2++ = *src2++;
                }
        }
}

void to_yuv422p(AVFrame *out_frame, unsigned char *src, int width, int height)
{
        for(int y = 0; y < (int) height; ++y) {
                unsigned char *dst_y = out_frame->data[0] + out_frame->linesize[0] * y;
                unsigned char *dst_cb = out_frame->data[1] + out_frame->linesize[1] * y;
                unsigned char *dst_cr = out_frame->data[2] + out_frame->linesize[2] * y;
                for(int x = 0; x < width; x += 2) {
                        *dst_cb++ = *src++;
                        *dst_y++ = *src++;
                        *dst_cr++ = *src++;
                        *dst_y++ = *src++;
                }
        }
}

void to_yuv444p(AVFrame *out_frame, unsigned char *src, int width, int height)
{
        for(int y = 0; y < height; ++y) {
                unsigned char *dst_y = out_frame->data[0] + out_frame->linesize[0] * y;
                unsigned char *dst_cb = out_frame->data[1] + out_frame->linesize[1] * y;
                unsigned char *dst_cr = out_frame->data[2] + out_frame->linesize[2] * y;
                for(int x = 0; x < width; x += 2) {
                        *dst_cb++ = *src;
                        *dst_cb++ = *src++;
                        *dst_y++ = *src++;
                        *dst_cr++ = *src;
                        *dst_cr++ = *src++;
                        *dst_y++ = *src++;
                }
        }
}

void to_nv12(AVFrame *out_frame, unsigned char *in_data, int width, int height)
{
        for(int y = 0; y < height; y += 2) {
                /*  every even row */
                unsigned char *src = in_data + y * (width * 2);
                /*  every odd row */
                unsigned char *src2 = in_data + (y + 1) * (width * 2);
                unsigned char *dst_y = out_frame->data[0] + out_frame->linesize[0] * y;
                unsigned char *dst_y2 = out_frame->data[0] + out_frame->linesize[0] * (y + 1);
                unsigned char *dst_cbcr = out_frame->data[1] + out_frame->linesize[1] * y / 2;
                for(int x = 0; x < width / 2; ++x) {
                        *dst_cbcr++ = (*src++ + *src2++) / 2;
                        *dst_y++ = *src++;
                        *dst_y2++ = *src2++;
                        *dst_cbcr++ = (*src++ + *src2++) / 2;
                        *dst_y++ = *src++;
                        *dst_y2++ = *src2++;
                }
        }
}

pixfmt_callback_t select_pixfmt_callback(AVPixelFormat fmt) {
        if(is422(fmt)) {
                return to_yuv422p;
        } else if(is420(fmt)) {
                if (fmt == AV_PIX_FMT_NV12)
                        return to_nv12;
                else
                        return to_yuv420p;
        } else if (is444(fmt)) {
                return to_yuv444p;
        } else {
                fprintf(stderr, "[Lavc] Unknown subsampling.\n");
                abort();
        }
}

struct my_task_data {
        void (*callback)(AVFrame *out_frame, unsigned char *in_data, int width, int height);
        AVFrame *out_frame;
        unsigned char *in_data;
        int width;
        int height;
};

void *my_task(void *arg);

void *my_task(void *arg) {
        struct my_task_data *data = (struct my_task_data *) arg;
        data->callback(data->out_frame, data->in_data, data->width, data->height);
        return NULL;
}

shared_ptr<video_frame> libavcodec_compress_tile(struct module *mod, shared_ptr<video_frame> tx)
{
        struct state_video_compress_libav *s = (struct state_video_compress_libav *) mod->priv_data;
        static int frame_seq = 0;
        int ret;
#if LIBAVCODEC_VERSION_MAJOR >= 54
        int got_output;
        AVPacket *pkt;
#endif
        unsigned char *decoded;
        shared_ptr<video_frame> out{};

        libavcodec_check_messages(s);

        if(!video_desc_eq_excl_param(video_desc_from_frame(tx.get()),
                                s->saved_desc, PARAM_TILE_COUNT)) {
                cleanup(s);
                int ret = configure_with(s, video_desc_from_frame(tx.get()));
                if(!ret) {
                        goto error;
                }
        }

        out = shared_ptr<video_frame>(vf_alloc_desc(s->compressed_desc), libavcodec_vid_enc_frame_dispose);
#if LIBAVCODEC_VERSION_MAJOR >= 54
        pkt = (AVPacket *) malloc(sizeof(AVPacket));
        av_init_packet(pkt);
        pkt->data = NULL;
        pkt->size = 0;
        out->dispose_udata = pkt;
#else
        out->tiles[0].data = (char *) malloc(s->compressed_desc.width *
                        s->compressed_desc.height * 4);
#endif // LIBAVCODEC_VERSION_MAJOR >= 54


        s->in_frame->pts = frame_seq++;

        if((void *) s->decoder != (void *) memcpy) {
                unsigned char *line1 = (unsigned char *) tx->tiles[0].data;
                unsigned char *line2 = (unsigned char *) s->decoded;
                int src_linesize = vc_get_linesize(tx->tiles[0].width, tx->color_spec);
                int dst_linesize = tx->tiles[0].width * 2; /* UYVY */
                for (int i = 0; i < (int) tx->tiles[0].height; ++i) {
                        s->decoder(line2, line1, dst_linesize,
                                        0, 8, 16);
                        line1 += src_linesize;
                        line2 += dst_linesize;
                }
                decoded = s->decoded;
        } else {
                decoded = (unsigned char *) tx->tiles[0].data;
        }

        {
                task_result_handle_t handle[s->cpu_count];
                struct my_task_data data[s->cpu_count];
                for(int i = 0; i < s->cpu_count; ++i) {
                        data[i].callback = select_pixfmt_callback(s->selected_pixfmt);
                        data[i].out_frame = s->in_frame_part[i];

                        size_t height = tx->tiles[0].height / s->cpu_count;
                        // height needs to be even
                        height = height / 2 * 2;
                        if (i < s->cpu_count - 1) {
                                data[i].height = height;
                        } else { // we are last so we need to do the rest
                                data[i].height = tx->tiles[0].height -
                                        height * (s->cpu_count - 1);
                        }
                        data[i].width = tx->tiles[0].width;
                        data[i].in_data = decoded + i * height *
                                vc_get_linesize(tx->tiles[0].width, UYVY);

                        // run !
                        handle[i] = task_run_async(my_task, (void *) &data[i]);
                }

                for(int i = 0; i < s->cpu_count; ++i) {
                        wait_task(handle[i]);
                }
        }

#if LIBAVCODEC_VERSION_MAJOR >= 54
        /* encode the image */
        ret = avcodec_encode_video2(s->codec_ctx, pkt,
                        s->in_frame, &got_output);
        if (ret < 0) {
                fprintf(stderr, "Error encoding frame\n");
                goto error;
        }

        if (got_output) {
                //printf("Write frame %3d (size=%5d)\n", frame_seq, s->pkt[buffer_idx].size);
                out->tiles[0].data = (char *) pkt->data;
                out->tiles[0].data_len = pkt->size;
        } else {
                goto error;
        }
#else
        /* encode the image */
        ret = avcodec_encode_video(s->codec_ctx, (uint8_t *) out->tiles[0].data,
                        out->tiles[0].width * out->tiles[0].height * 4,
                        s->in_frame);
        if (ret < 0) {
                fprintf(stderr, "Error encoding frame\n");
                goto error;
        }

        if (ret) {
                //printf("Write frame %3d (size=%5d)\n", frame_seq, s->pkt[buffer_idx].size);
                out->tiles[0].data_len = ret;
        } else {
                goto error;
        }
#endif // LIBAVCODEC_VERSION_MAJOR >= 54

        verbose_msg("[lavc] Compressed frame size: %d\n", out->tiles[0].data_len);

        return out;

error:
        return NULL;
}

static void cleanup(struct state_video_compress_libav *s)
{
        if(s->codec_ctx) {
                pthread_mutex_lock(s->lavcd_global_lock);
                avcodec_close(s->codec_ctx);
                pthread_mutex_unlock(s->lavcd_global_lock);
        }
        if(s->in_frame) {
                av_freep(s->in_frame->data);
                av_free(s->in_frame);
                s->in_frame = NULL;
        }
        av_free(s->codec_ctx);
        s->codec_ctx = NULL;
        free(s->decoded);
        s->decoded = NULL;
}

static void libavcodec_compress_done(struct module *mod)
{
        struct state_video_compress_libav *s = (struct state_video_compress_libav *) mod->priv_data;

        cleanup(s);

        rm_release_shared_lock(LAVCD_LOCK_NAME);
        free(s->preset);
        for(int i = 0; i < s->cpu_count; i++) {
                av_free(s->in_frame_part[i]);
        }
        free(s->in_frame_part);
        delete s;
}

static void setparam_default(AVCodecContext *codec_ctx, struct setparam_param *param)
{
        if (!param->threads.empty() && param->threads != "no")  {
                if (param->threads == "slice") {
                        // zero should mean count equal to the number of virtual cores
                        if (param->codec->capabilities & CODEC_CAP_SLICE_THREADS) {
                                codec_ctx->thread_count = 0;
                                codec_ctx->thread_type = FF_THREAD_SLICE;
                        } else {
                                fprintf(stderr, "[Lavc] Warning: Codec doesn't support slice-based multithreading.\n");
                        }
                } else if (param->threads == "frame") {
                        if (param->codec->capabilities & CODEC_CAP_FRAME_THREADS) {
                                codec_ctx->thread_count = 0;
                                codec_ctx->thread_type = FF_THREAD_FRAME;
                        } else {
                                fprintf(stderr, "[Lavc] Warning: Codec doesn't support frame-based multithreading.\n");
                        }
                } else {
                        fprintf(stderr, "[Lavc] Warning: unknown thread mode: %s.\n", param->threads.c_str());
                }
        }
}

static void setparam_h265(AVCodecContext *codec_ctx, struct setparam_param *param)
{
        string params(
               //"level-idc=5.1:" // this would set level to 5.1, can be wrong or inefficent for some video formats!
               "b-adapt=0:bframes=0:no-b-pyramid=1:" // turns off B frames (bad for zero latency)
                "no-deblock=1:no-sao=1:no-weightb=1:no-weightp=1:no-b-intra=1:" 
               "me=dia:max-merge=1:subme=0:no-strong-intra-smoothing=1:"
                "rc-lookahead=2:ref=1:scenecut=0:" 
               "no-cutree=1:no-weightp=1:"
               "rd=0:" // RDO mode decision
               "ctu=32:min-cu-size=16:max-tu-size=16:" // partitioning options, heavy effect on parallelism
               "frame-threads=3:pme=1:" // trade some latency for better parallelism
               "keyint=180:min-keyint=120:" // I frames
               "aq_mode=0");

        if (param->interlaced) {
                params += ":tff=1";
        }

        if(strlen(params.c_str()) > 0) {
                int ret;
                // newer LibAV
                ret = av_opt_set(codec_ctx->priv_data, "x265-params", params.c_str(), 0);
                if(ret != 0) {
                        // newer FFMPEG
                        ret = av_opt_set(codec_ctx->priv_data, "x265opts", params.c_str(), 0);
                }
                if(ret != 0) {
                        // older version of both
                        // or superfast?? requires + some 70 % CPU but does not cause posterization
                        ret = av_opt_set(codec_ctx->priv_data, "preset", "ultrafast", 0);
                        fprintf(stderr, "[Lavc] Warning: Old FFMPEG/LibAV detected. "
                                        "Try supplying 'preset=superfast' argument to "
                                        "avoid posterization!\n");
                }
                if(ret != 0) {
                        fprintf(stderr, "[Lavc] Warning: Unable to set preset.\n");
                }
        }

        av_opt_set(codec_ctx->priv_data, "tune", "zerolatency", 0);
        av_opt_set(codec_ctx->priv_data, "tune", "fastdecode", 0);

        // try to keep frame sizes as even as possible
        codec_ctx->rc_max_rate = codec_ctx->bit_rate;
        //codec_ctx->rc_min_rate = s->codec_ctx->bit_rate / 4 * 3;
        //codec_ctx->rc_buffer_aggressivity = 1.0;
        codec_ctx->rc_buffer_size = codec_ctx->rc_max_rate / param->fps * 8;
        codec_ctx->qcompress = 0.0f;
        //codec_ctx->qblur = 0.0f;
        //codec_ctx->rc_min_vbv_overflow_use = 1.0f;
        //codec_ctx->rc_max_available_vbv_use = 1.0f;
        codec_ctx->qmin = 0;
        codec_ctx->qmax = 69;
        codec_ctx->max_qdiff = 69;
        //codec_ctx->rc_qsquish = 0;
        //codec_ctx->scenechange_threshold = 100;

#ifndef DISABLE_H265_INTRA_REFRESH
        codec_ctx->refs = 1;
        av_opt_set(codec_ctx->priv_data, "intra-refresh", "1", 0);
#endif // not defined DISABLE_H265_INTRA_REFRESH       
}


static void setparam_h264(AVCodecContext *codec_ctx, struct setparam_param *param)
{
        if (strcmp(codec_ctx->codec->name, "libx264") == 0) {
                if (!param->have_preset) {
                        // ultrafast + --aq-mode 2
                        // AQ=0 causes posterization. Enabling it requires some 20% additional
                        // percent of CPU.
                        string params("no-8x8dct=1:b-adapt=0:bframes=0:no-cabac=1:"
                                        "no-deblock=1:no-mbtree=1:me=dia:no-mixed-refs=1:partitions=none:"
                                        "rc-lookahead=0:ref=1:scenecut=0:subme=0:trellis=0:aq_mode=2");

                        // this options increases variance in frame sizes quite a lot
                        //if (param->interlaced) {
                        //        params += ":tff=1";
                        //}

                        int ret;
                        // newer LibAV
                        ret = av_opt_set(codec_ctx->priv_data, "x264-params", params.c_str(), 0);
                        if (ret != 0) {
                                // newer FFMPEG
                                ret = av_opt_set(codec_ctx->priv_data, "x264opts", params.c_str(), 0);
                        }
                        if (ret != 0) {
                                // older version of both
                                ret = av_opt_set(codec_ctx->priv_data, "preset", DEFAULT_X264_PRESET, 0);
                                fprintf(stderr, "[Lavc] Warning: Old FFMPEG/LibAV detected - consider "
                                                "upgrading. Using preset %s.\n", DEFAULT_X264_PRESET);
                        }
                        if (ret != 0) {
                                fprintf(stderr, "[Lavc] Warning: Unable to set preset.\n");
                        }
                }
                //av_opt_set(codec_ctx->priv_data, "tune", "fastdecode", 0);
                av_opt_set(codec_ctx->priv_data, "tune", "fastdecode,zerolatency", 0);

                // try to keep frame sizes as even as possible
                codec_ctx->rc_max_rate = codec_ctx->bit_rate;
                //codec_ctx->rc_min_rate = s->codec_ctx->bit_rate / 4 * 3;
                //codec_ctx->rc_buffer_aggressivity = 1.0;
                codec_ctx->rc_buffer_size = codec_ctx->rc_max_rate / param->fps * 8;
                codec_ctx->qcompress = 0.0f;
                //codec_ctx->qblur = 0.0f;
                //codec_ctx->rc_min_vbv_overflow_use = 1.0f;
                //codec_ctx->rc_max_available_vbv_use = 1.0f;
                codec_ctx->qmin = 0;
                codec_ctx->qmax = 69;
                codec_ctx->max_qdiff = 69;
                //codec_ctx->rc_qsquish = 0;
                //codec_ctx->scenechange_threshold = 100;

                if (!param->h264_no_periodic_intra) {
                        codec_ctx->refs = 1;
                        av_opt_set(codec_ctx->priv_data, "intra-refresh", "1", 0);
                }
        } else if (strcmp(codec_ctx->codec->name, "nvenc") == 0) {
                if (!param->have_preset) {
                        av_opt_set(codec_ctx->priv_data, "preset", DEFAULT_NVENC_H264_PRESET, 0);
                }
                av_opt_set(codec_ctx->priv_data, "cbr", "1", 0);
                char gpu[3] = "";
                snprintf(gpu, 2, "%d", cuda_devices[0]);
                av_opt_set(codec_ctx->priv_data, "gpu", gpu, 0);
                codec_ctx->rc_max_rate = codec_ctx->bit_rate;
                codec_ctx->rc_buffer_size = codec_ctx->rc_max_rate / param->fps;
	} else {
                fprintf(stderr, "[Lavc] Warning: Unknown encoder %s. Using default configuration values.\n", codec_ctx->codec->name);
        }
}

static void setparam_vp8(AVCodecContext *codec_ctx, struct setparam_param *param)
{
        codec_ctx->thread_count = param->cpu_count;
        codec_ctx->profile = 0;
        codec_ctx->slices = 4;
        codec_ctx->rc_buffer_size = codec_ctx->bit_rate / param->fps;
        //codec_ctx->rc_buffer_aggressivity = 0.5;
        av_opt_set(codec_ctx->priv_data, "deadline", "realtime", 0);
}

static void libavcodec_check_messages(struct state_video_compress_libav *s)
{
        struct message *msg;
        while ((msg = check_message(&s->module_data))) {
                struct msg_change_compress_data *data =
                        (struct msg_change_compress_data *) msg;
                if (parse_fmt(s, data->config_string) == 0) {
                        printf("[Libavcodec] Compression successfully changed.\n");
                } else {
                        fprintf(stderr, "[Libavcodec] Unable to change compression!\n");
                }
                memset(&s->saved_desc, 0, sizeof(s->saved_desc));
                free_message(msg);
        }

}

} // end of anonymous namespace

struct compress_info_t libavcodec_info = {
        "libavcodec",
        libavcodec_compress_init,
        NULL,
        libavcodec_compress_tile,
        libavcodec_is_supported,
        {
                { "codec=H.264:bpp=0.096", 20, 5*1000*1000, {25, 1.5, 0}, {15, 1, 0} },
                { "codec=H.264:bpp=0.193", 30, 10*1000*1000, {28, 1.5, 0}, {20, 1, 0} },
                { "codec=H.264:bitrate=0.289", 50, 15*1000*1000, {30, 1.5, 0}, {25, 1, 0} },
#if 0
                { "codec=MJPEG", 35, 50*1000*1000, {20, 0.75, 0}, {10, 0.5, 0}  },
#endif
        },
};

