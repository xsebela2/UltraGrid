/**
 * @file   video_display/file.c
 * @author Martin Pulec     <pulec@cesnet.cz>
 */
/*
 * Copyright (c) 2023 CESNET, z. s. p. o.
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

#include <assert.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <pthread.h>

#include "audio/types.h"
#include "audio/utils.h"
#include "debug.h"
#include "lib_common.h"
#include "libavcodec/lavc_common.h"
#include "libavcodec/utils.h"
#include "types.h"
#include "utils/color_out.h"
#include "utils/fs.h"
#include "utils/macros.h"
#include "utils/misc.h"
#include "video.h"
#include "video_display.h"

#define DEFAULT_FILENAME "out.nut"
#define MOD_NAME "[File disp.] "

struct output_stream {
        AVStream           *st;
        AVCodecContext     *enc;
        AVPacket           *pkt;
        long long int       cur_pts;
        union {
                struct video_frame *vid_frm;
                AVFrame            *aud_frm;
        };
};

struct state_file {
        AVFormatContext     *format_ctx;
        struct output_stream audio;
        struct output_stream video;
        struct video_desc    video_desc;
        char                 filename[MAX_PATH_SIZE];
        pthread_t            thread_id;
        pthread_mutex_t      lock;
        pthread_cond_t       cv;
        bool                 initialized;
        bool                 should_exit;
};

static void *worker(void *arg);

static void
display_file_probe(struct device_info **available_cards, int *count,
                   void (**deleter)(void *))
{
        *deleter         = free;
        *count           = 1;
        *available_cards = calloc(*count, sizeof **available_cards);
        snprintf((*available_cards)[0].name, sizeof(*available_cards)[0].name,
                 "file");
}

static void
display_file_done(void *state)
{
        struct state_file *s = state;
        pthread_join(s->thread_id, NULL);
        pthread_mutex_destroy(&s->lock);
        pthread_cond_destroy(&s->cv);
        if (s->initialized) {
                av_write_trailer(s->format_ctx);
        }
        avcodec_free_context(&s->video.enc);
        if (!(s->format_ctx->oformat->flags & AVFMT_NOFILE)) {
                avio_closep(&s->format_ctx->pb);
        }
        av_packet_free(&s->video.pkt);
        vf_free(s->video.vid_frm);
        av_frame_free(&s->audio.aud_frm);
        free(s);
}

static void
usage(void)
{
        color_printf("Display " TBOLD("file") " syntax:\n");
        color_printf("\t" TBOLD(TRED("file") "[:file=<name>]") "\n");
}

static void *
display_file_init(struct module *parent, const char *fmt, unsigned int flags)
{
        const char *filename = DEFAULT_FILENAME;
        UNUSED(parent);
        if (strlen(fmt) > 0) {
                if (IS_KEY_PREFIX(fmt, "file")) {
                        filename = strchr(fmt, '=') + 1;
                } else {
                        usage();
                        return strcmp(fmt, "help") == 0 ? INIT_NOERR : NULL;
                }
        }
        struct state_file *s = calloc(1, sizeof *s);
        strncat(s->filename, filename, sizeof s->filename - 1);
        avformat_alloc_output_context2(&s->format_ctx, NULL, NULL, filename);
        if (s->format_ctx == NULL) {
                log_msg(LOG_LEVEL_WARNING, "Could not deduce output format "
                                           "from file extension, using NUT.\n");
                avformat_alloc_output_context2(&s->format_ctx, NULL, "nut",
                                               filename);
                assert(s->format_ctx != NULL);
        }
        s->video.st     = avformat_new_stream(s->format_ctx, NULL);
        s->video.st->id = 0;

        if (!(s->format_ctx->oformat->flags & AVFMT_NOFILE)) {
                int ret =
                    avio_open(&s->format_ctx->pb, filename, AVIO_FLAG_WRITE);
                if (ret < 0) {
                        error_msg(MOD_NAME "avio_open: %s\n", av_err2str(ret));
                        display_file_done(s);
                        return NULL;
                }
        }
        s->video.pkt   = av_packet_alloc();

        int ret = pthread_mutex_init(&s->lock, NULL);
        ret |= pthread_cond_init(&s->cv, NULL);
        ret |= pthread_create(&s->thread_id, NULL, worker, s);
        assert(ret == 0);

        if ((flags & DISPLAY_FLAG_AUDIO_ANY) != 0U) {
                s->audio.st     = avformat_new_stream(s->format_ctx, NULL);
                s->audio.st->id = 1;
                s->audio.pkt    = av_packet_alloc();
        }

        return s;
}

static void
delete_frame(struct video_frame *frame)
{
        AVFrame *avfrm = frame->callbacks.dispose_udata;
        av_frame_free(&avfrm);
}

static struct video_frame *
display_file_getf(void *state)
{
        struct state_file  *s   = state;
        AVFrame            *frame = av_frame_alloc();

        frame->format = get_ug_to_av_pixfmt(s->video_desc.color_spec);
        frame->width  = (int) s->video_desc.width;
        frame->height = (int) s->video_desc.height;
        int ret       = av_frame_get_buffer(frame, 0);
        if (ret < 0) {
                error_msg(MOD_NAME "Could not allocate frame data: %s.\n",
                          av_err2str(ret));
                av_frame_free(&frame);
                return NULL;
        }
        struct video_frame *out      = vf_alloc_desc(s->video_desc);
        out->tiles[0].data           = (char *) frame->data[0];
        out->callbacks.dispose_udata = frame;
        out->callbacks.data_deleter  = delete_frame;
        return out;
}

static bool
display_file_putf(void *state, struct video_frame *frame, long long timeout_ns)
{
        if (timeout_ns == PUTF_DISCARD) {
                return true;
        }
        struct state_file *s = state;
        pthread_mutex_lock(&s->lock);
        if (frame == NULL) {
                s->should_exit = true;
                pthread_mutex_unlock(&s->lock);
                pthread_cond_signal(&s->cv);
                return true;
        }
        bool ret = true;
        if (s->video.vid_frm != NULL) {
                log_msg(LOG_LEVEL_WARNING, MOD_NAME "Video frame dropped!\n");
                vf_free(s->video.vid_frm);
                ret = false;
        }
        s->video.vid_frm = frame;
        pthread_mutex_unlock(&s->lock);
        pthread_cond_signal(&s->cv);
        return ret;
}

static bool
display_file_get_property(void *state, int property, void *val, size_t *len)
{
        UNUSED(state);

        switch (property) {
        case DISPLAY_PROPERTY_CODECS: {
                codec_t codecs[VIDEO_CODEC_COUNT] = { 0 };
                int     count                     = 0;
                for (int i = 0; i < VIDEO_CODEC_COUNT; ++i) {
                        if (get_ug_to_av_pixfmt(i)) {
                                codecs[count++] = i;
                        }
                }
                const size_t c_len = count * sizeof codecs[0];
                assert(c_len <= *len);
                memcpy(val, codecs, c_len);
                *len = c_len;
                break;
        }
        case DISPLAY_PROPERTY_AUDIO_FORMAT: {
                struct audio_desc *desc = val;
                assert(*len == (int) sizeof *desc);
                desc->codec = AC_PCM;
                break;
        }
        default:
                return false;
        }
        return true;
}

static bool
display_file_reconfigure(void *state, struct video_desc desc)
{
        struct state_file *s = state;

        s->video_desc = desc;
        return true;
}

static bool
configure_audio(struct state_file *s, struct audio_desc aud_desc)
{
        avcodec_free_context(&s->audio.enc);
        enum AVCodecID codec_id = AV_CODEC_ID_NONE;
        switch (aud_desc.bps) {
        case 1:
                codec_id = AV_CODEC_ID_PCM_U8;
                break;
        case 2:
                codec_id = AV_CODEC_ID_PCM_S16LE;
                break;
        case 3:
        case 4:
                codec_id = AV_CODEC_ID_PCM_S32LE;
                break;
        default:
                abort();
        }
        const AVCodec *codec = avcodec_find_encoder(codec_id);
        assert(codec != NULL);
        s->audio.enc             = avcodec_alloc_context3(codec);
        s->audio.enc->sample_fmt = audio_bps_to_sample_fmt(aud_desc.bps);
        s->audio.enc->ch_layout  = (AVChannelLayout) AV_CHANNEL_LAYOUT_MASK(
            aud_desc.ch_count, (1 << aud_desc.ch_count) - 1);
        s->audio.enc->sample_rate = aud_desc.sample_rate;
        s->audio.st->time_base    = (AVRational){ 1, aud_desc.sample_rate };

        int ret = avcodec_open2(s->audio.enc, codec, NULL);
        if (ret < 0) {
                error_msg(MOD_NAME "audio avcodec_open2: %s\n",
                          av_err2str(ret));
                return false;
        }

        ret = avcodec_parameters_from_context(s->audio.st->codecpar,
                                              s->audio.enc);
        if (ret < 0) {
                error_msg(MOD_NAME
                          "Could not copy audio stream parameters: %s\n",
                          av_err2str(ret));
                return false;
        }

        return true;
}

static bool
initialize(struct state_file *s, struct video_desc *saved_vid_desc,
           const struct video_frame *vid_frm, struct audio_desc *saved_aud_desc,
           const AVFrame *aud_frm)
{
        if (!vid_frm || (s->audio.st != NULL && !aud_frm)) {
                log_msg(LOG_LEVEL_INFO, "Waiting for all streams to init.\n");
                return false;
        }

        const struct video_desc vid_desc = video_desc_from_frame(vid_frm);

        // video
        s->video.st->time_base = (AVRational){ get_framerate_d(vid_desc.fps),
                                               get_framerate_n(vid_desc.fps) };
        const AVCodec *codec   = avcodec_find_encoder(AV_CODEC_ID_RAWVIDEO);
        assert(codec != NULL);
        avcodec_free_context(&s->video.enc);
        s->video.enc            = avcodec_alloc_context3(codec);
        s->video.enc->width     = (int) vid_desc.width;
        s->video.enc->height    = (int) vid_desc.height;
        s->video.enc->time_base = s->video.st->time_base;
        s->video.enc->pix_fmt   = get_ug_to_av_pixfmt(vid_desc.color_spec);
        int ret                 = avcodec_open2(s->video.enc, codec, NULL);
        if (ret < 0) {
                error_msg(MOD_NAME "video avcodec_open2: %s\n",
                          av_err2str(ret));
                return false;
        }
        ret = avcodec_parameters_from_context(s->video.st->codecpar,
                                              s->video.enc);
        if (ret < 0) {
                error_msg(MOD_NAME
                          "Could not copy video stream parameters: %s\n",
                          av_err2str(ret));
                return false;
        }
        *saved_vid_desc = vid_desc;

        // audio
        if (aud_frm != NULL) {
                const struct audio_desc aud_desc =
                    audio_desc_from_av_frame(aud_frm);
                if (!configure_audio(s, aud_desc)) {
                        return false;
                }
                *saved_aud_desc = aud_desc;
        }

        av_dump_format(s->format_ctx, 0, s->filename, 1);

        ret = avformat_write_header(s->format_ctx, NULL);
        if (ret < 0) {
                error_msg(MOD_NAME
                          "Error occurred when opening output file: %s\n",
                          av_err2str(ret));
                return false;
        }

        s->initialized = true;
        return true;
}

static void
write_frame(AVFormatContext *format_ctx, struct output_stream *ost,
            AVFrame *frame)
{
        frame->pts = ost->cur_pts;
        int ret    = avcodec_send_frame(ost->enc, frame);
        if (ret < 0) {
                error_msg(MOD_NAME "avcodec_send_frame: %s\n",
                          av_err2str(ret));
                return;
        }
        while (ret >= 0) {
                ret = avcodec_receive_packet(ost->enc, ost->pkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                }
                if (ret < 0) {
                        error_msg(MOD_NAME "video avcodec_receive_frame: %s\n",
                                  av_err2str(ret));
                        return;
                }
                av_packet_rescale_ts(ost->pkt, ost->enc->time_base,
                                     ost->st->time_base);
                ost->pkt->stream_index = ost->st->index;
                ret = av_interleaved_write_frame(format_ctx, ost->pkt);
                if (ret < 0) {
                        error_msg(MOD_NAME "error writting video packet: %s\n",
                                  av_err2str(ret));
                }
        }
}

static bool
check_reconf(struct video_desc *saved_vid_desc,
           const struct video_frame *vid_frm, struct audio_desc *saved_aud_desc,
           const AVFrame *aud_frm)
{
        if (vid_frm != NULL) {
                const struct video_desc cur_vid_desc =
                    video_desc_from_frame(vid_frm);
                if (!video_desc_eq(*saved_vid_desc, cur_vid_desc)) {
                        return false;
                }
        }
        if (aud_frm) {
                const struct audio_desc cur_aud_desc =
                    audio_desc_from_av_frame(aud_frm);
                if (!audio_desc_eq(*saved_aud_desc, cur_aud_desc)) {
                        return false;
                }
        }
        return true;
}

static void *
worker(void *arg)
{
        struct state_file *s = arg;

        struct video_desc   saved_vid_desc = { 0 };
        struct audio_desc   saved_aud_desc = { 0 };
        struct video_frame *vid_frm        = NULL;
        AVFrame            *aud_frm        = NULL;

        while (!s->should_exit) {
                pthread_mutex_lock(&s->lock);
                while (s->audio.aud_frm == NULL && s->video.vid_frm == NULL &&
                       !s->should_exit) {
                        pthread_cond_wait(&s->cv, &s->lock);
                }
                if (s->should_exit) {
                        break;
                }
                if (s->video.vid_frm) {
                        vf_free(vid_frm);
                        vid_frm = s->video.vid_frm;
                }
                if (s->audio.aud_frm) {
                        av_frame_free(&aud_frm);
                        aud_frm = s->audio.aud_frm;
                }
                s->video.vid_frm = NULL;
                s->audio.aud_frm = NULL;
                pthread_mutex_unlock(&s->lock);

                if (!s->initialized) {
                        if (!initialize(s, &saved_vid_desc, vid_frm,
                                        &saved_aud_desc, aud_frm)) {
                                continue;
                        }
                }

                if (!check_reconf(&saved_vid_desc, vid_frm, &saved_aud_desc,
                                  aud_frm)) {
                        error_msg(MOD_NAME "Reconfiguration not implemented. "
                                           "Let us know if desired.\n");
                        continue;
                }

                if (aud_frm) {
                        write_frame(s->format_ctx, &s->audio,
                                    aud_frm);
                        s->audio.cur_pts += aud_frm->nb_samples;
                        av_frame_free(&aud_frm);
                }
                if (vid_frm) {
                        AVFrame *frame = vid_frm->callbacks.dispose_udata;
                        write_frame(s->format_ctx, &s->video, frame);
                        s->video.cur_pts += 1;
                        vf_free(vid_frm);
                        vid_frm = NULL;
                }
        }
        vf_free(vid_frm);
        av_frame_free(&aud_frm);

        pthread_mutex_unlock(&s->lock);
        return NULL;
}

static void
display_file_put_audio_frame(void *state, const struct audio_frame *frame)
{
        struct state_file *s = state;

        AVFrame *av_frm   = av_frame_alloc();
        av_frm->format    = audio_bps_to_sample_fmt(frame->bps);
        av_frm->ch_layout = (AVChannelLayout) AV_CHANNEL_LAYOUT_MASK(
            frame->ch_count, (frame->ch_count << 1) - 1);
        av_frm->sample_rate = frame->sample_rate;
        av_frm->nb_samples  = frame->data_len / frame->ch_count / frame->bps;

        int ret = av_frame_get_buffer(av_frm, 0);
        if (ret < 0) {
                error_msg(MOD_NAME "audio buf alloc: %s\n", av_err2str(ret));
                av_frame_free(&av_frm);
                return;
        }
        memcpy(av_frm->data[0], frame->data, frame->data_len);
        pthread_mutex_lock(&s->lock);
        if (s->audio.aud_frm != NULL) {
                log_msg(LOG_LEVEL_WARNING, MOD_NAME "Audio frame dropped!\n");
                av_frame_free(&s->audio.aud_frm);
        }
        s->audio.aud_frm = av_frm;
        pthread_mutex_unlock(&s->lock);
        pthread_cond_signal(&s->cv);
}

static bool
display_file_reconfigure_audio(void *state, int quant_samples, int channels,
                              int sample_rate)
{
        UNUSED(state), UNUSED(quant_samples), UNUSED(channels),
            UNUSED(sample_rate);
        return true;
}

static const void *
display_file_info_get()
{
        static const struct video_display_info display_file_info = {
                display_file_probe,
                display_file_init,
                NULL, // _run
                display_file_done,
                display_file_getf,
                display_file_putf,
                display_file_reconfigure,
                display_file_get_property,
                display_file_put_audio_frame,
                display_file_reconfigure_audio,
                MOD_NAME,
        };
        return &display_file_info;
};

REGISTER_MODULE_WITH_FUNC(file, display_file_info_get,
                          LIBRARY_CLASS_VIDEO_DISPLAY,
                          VIDEO_DISPLAY_ABI_VERSION);
