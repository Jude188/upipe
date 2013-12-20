/*
 * Copyright (C) 2012-2013 OpenHeadend S.A.R.L.
 *
 * Authors: Benjamin Cohen
 *          Christophe Massiot
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/** @file
 * @short Upipe avcodec encode module
 */

#include <upipe/ubase.h>
#include <upipe/uprobe.h>
#include <upipe/uclock.h>
#include <upipe/ubuf.h>
#include <upipe/uref.h>
#include <upipe/uref_attr.h>
#include <upipe/uref_clock.h>
#include <upipe/uref_pic.h>
#include <upipe/uref_flow.h>
#include <upipe/uref_pic_flow.h>
#include <upipe/uref_block_flow.h>
#include <upipe/uref_sound_flow.h>
#include <upipe/uref_block.h>
#include <upipe/uref_dump.h>
#include <upipe/upipe.h>
#include <upipe/upipe_helper_upipe.h>
#include <upipe/upipe_helper_flow.h>
#include <upipe/upipe_helper_ubuf_mgr.h>
#include <upipe/upipe_helper_output.h>
#include <upipe/upipe_helper_flow_def.h>
#include <upipe/upipe_helper_flow_def_check.h>
#include <upipe/upipe_helper_upump_mgr.h>
#include <upipe/upipe_helper_upump.h>
#include <upipe/upipe_helper_sink.h>
#include <upipe/upipe_helper_sound_stream.h>
#include <upipe-av/upipe_avcodec_encode.h>
#include <upipe-modules/upipe_proxy.h>

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
#include <upipe-av/upipe_av_pixfmt.h>
#include <upipe-av/upipe_av_samplefmt.h>
#include "upipe_av_internal.h"

#define PREFIX_FLOW "block."

/** BS for <= 2 channels */
#define AUDIO_BS 3584

UREF_ATTR_INT(avcenc, priv, "x.avcenc_priv", avcenc private pts)

/** @hidden */
static bool upipe_avcenc_encode_frame(struct upipe *upipe,
                                      struct AVFrame *frame,
                                      struct upump *upump);
/** @hidden */
static void upipe_avcenc_encode_audio(struct upipe *upipe,
                                      struct uref *uref, struct upump *upump);
/** @hidden */
static bool upipe_avcenc_encode(struct upipe *upipe,
                                struct uref *uref, struct upump *upump);

/** upipe_avcenc structure with avcenc parameters */ 
struct upipe_avcenc {
    /** input flow */
    struct uref *flow_def_input;
    /** attributes added by the pipe */
    struct uref *flow_def_attr;
    /** structure to check input flow def */
    struct uref *flow_def_check;
    /** output flow */
    struct uref *flow_def;
    /** true if the flow definition has already been sent */
    bool flow_def_sent;
    /** output pipe */
    struct upipe *output;

    /** ubuf manager */
    struct ubuf_mgr *ubuf_mgr;
    /** upump mgr */
    struct upump_mgr *upump_mgr;

    /** avcodec_open watcher */
    struct upump *upump_av_deal;
    /** temporary uref storage (used during udeal) */
    struct uchain urefs;
    /** nb urefs in storage */
    unsigned int nb_urefs;
    /** max urefs in storage */
    unsigned int max_urefs;
    /** list of blockers (used during udeal) */
    struct uchain blockers;

    /** uref associated to frames currently in encoder */
    struct uchain urefs_in_use;
    /** last incoming pts (in avcodec timebase) */
    int64_t avcpts;

    /** frame counter */
    uint64_t counter;
    /** audio BS duration */
    uint64_t audio_bs_duration;
    /** audio BS leakage per frame */
    uint64_t audio_bs_leakage;
    /** audio BS delay */
    int64_t audio_bs_delay;
    /** latency in the input flow */
    uint64_t input_latency;
    /** chroma map */
    const char *chroma_map[UPIPE_AV_MAX_PLANES];

    /** next uref to be processed */
    struct uref *next_uref;
    /** original size of the next uref */
    size_t next_uref_size;
    /** urefs received after next uref */
    struct uchain stream_urefs;

    /** avcodec context */
    AVCodecContext *context;
    /** avcodec frame */
    AVFrame *frame;
    /** true if the context will be closed */
    bool close;

    /** public upipe structure */
    struct upipe upipe;
};


UPIPE_HELPER_UPIPE(upipe_avcenc, upipe, UPIPE_AVCENC_SIGNATURE);
UPIPE_HELPER_FLOW(upipe_avcenc, "block.")
UPIPE_HELPER_OUTPUT(upipe_avcenc, output, flow_def, flow_def_sent)
UPIPE_HELPER_FLOW_DEF(upipe_avcenc, flow_def_input, flow_def_attr)
UPIPE_HELPER_FLOW_DEF_CHECK(upipe_avcenc, flow_def_check)
UPIPE_HELPER_UBUF_MGR(upipe_avcenc, ubuf_mgr);
UPIPE_HELPER_UPUMP_MGR(upipe_avcenc, upump_mgr)
UPIPE_HELPER_UPUMP(upipe_avcenc, upump_av_deal, upump_mgr)
UPIPE_HELPER_SINK(upipe_avcenc, urefs, nb_urefs, max_urefs, blockers, upipe_avcenc_encode)
UPIPE_HELPER_SOUND_STREAM(upipe_avcenc, next_uref, next_uref_size, stream_urefs)

/** @This aborts and frees an existing upump watching for exclusive access to
 * avcodec_open().
 *
 * @param upipe description structure of the pipe
 */
static void upipe_avcenc_abort_av_deal(struct upipe *upipe)
{
    struct upipe_avcenc *upipe_avcenc = upipe_avcenc_from_upipe(upipe);
    if (unlikely(upipe_avcenc->upump_av_deal != NULL)) {
        upipe_av_deal_abort(upipe_avcenc->upump_av_deal);
        upump_free(upipe_avcenc->upump_av_deal);
        upipe_avcenc->upump_av_deal = NULL;
    }
}

/** @internal @This actually calls avcodec_open(). It may only be called by
 * one thread at a time.
 *
 * @param upipe description structure of the pipe
 * @return false if the buffers mustn't be dequeued
 */
static bool upipe_avcenc_do_av_deal(struct upipe *upipe)
{
    assert(upipe);
    struct upipe_avcenc *upipe_avcenc = upipe_avcenc_from_upipe(upipe);
    AVCodecContext *context = upipe_avcenc->context;

    if (upipe_avcenc->close) {
        upipe_notice_va(upipe, "codec %s (%s) %d closed", context->codec->name, 
                        context->codec->long_name, context->codec->id);
        avcodec_close(context);
        /* trigger deferred release of the pipe */
        upipe_release(upipe);
        return false;
    }

    /* open new context */
    int err;
    if (unlikely((err = avcodec_open2(context, context->codec, NULL)) < 0)) {
        upipe_av_strerror(err, buf);
        upipe_warn_va(upipe, "could not open codec (%s)", buf);
        upipe_throw_fatal(upipe, UPROBE_ERR_EXTERNAL);
        return false;
    }
    upipe_notice_va(upipe, "codec %s (%s) %d opened", context->codec->name, 
                    context->codec->long_name, context->codec->id);

    return true;
}

/** @internal @This is called to try an exclusive access on avcodec_open() or
 * avcodec_close().
 *
 * @param upump description structure of the pump
 */
static void upipe_avcenc_cb_av_deal(struct upump *upump)
{
    assert(upump);
    struct upipe *upipe = upump_get_opaque(upump, struct upipe *);
    struct upipe_avcenc *upipe_avcenc = upipe_avcenc_from_upipe(upipe);

    /* check udeal */
    if (unlikely(!upipe_av_deal_grab()))
        return;

    /* avoid having the pipe disappear on us */
    upipe_use(upipe);

    /* real open_codec function */
    bool ret = upipe_avcenc_do_av_deal(upipe);

    /* clean dealer */
    upipe_av_deal_yield(upump);
    upump_free(upipe_avcenc->upump_av_deal);
    upipe_avcenc->upump_av_deal = NULL;

    if (ret) 
        upipe_avcenc_output_sink(upipe);
    else
        upipe_avcenc_flush_sink(upipe);
    upipe_avcenc_unblock_sink(upipe);

    upipe_release(upipe);
}

/** @internal @This is called to trigger avcodec_open() or avcodec_close().
 *
 * @param upipe description structure of the pipe
 */
static void upipe_avcenc_start_av_deal(struct upipe *upipe)
{
    struct upipe_avcenc *upipe_avcenc = upipe_avcenc_from_upipe(upipe);
    /* abort a pending open request */
    upipe_avcenc_abort_av_deal(upipe);

    /* use udeal/upump callback if available */
    if (upipe_avcenc->upump_mgr == NULL) {
        upipe_dbg(upipe, "no upump_mgr present, direct call to avcodec_open");
        upipe_avcenc_do_av_deal(upipe);
        return;
    }

    upipe_dbg(upipe, "upump_mgr present, using udeal");
    struct upump *upump_av_deal =
        upipe_av_deal_upump_alloc(upipe_avcenc->upump_mgr,
                                  upipe_avcenc_cb_av_deal, upipe);
    if (unlikely(!upump_av_deal)) {
        upipe_err(upipe, "can't create dealer");
        upipe_throw_fatal(upipe, UPROBE_ERR_UPUMP);
        return;
    }
    upipe_avcenc->upump_av_deal = upump_av_deal;
    upipe_av_deal_start(upump_av_deal);
}

/** @internal @This is called to trigger avcodec_open().
 *
 * @param upipe description structure of the pipe
 */
static void upipe_avcenc_open(struct upipe *upipe)
{
    struct upipe_avcenc *upipe_avcenc = upipe_avcenc_from_upipe(upipe);
    upipe_avcenc->close = false;
    upipe_avcenc_start_av_deal(upipe);
}

/** @internal @This is called to trigger avcodec_close().
 *
 * @param upipe description structure of the pipe
 */
static void upipe_avcenc_close(struct upipe *upipe)
{
    struct upipe_avcenc *upipe_avcenc = upipe_avcenc_from_upipe(upipe);
    AVCodecContext *context = upipe_avcenc->context;
    if (context == NULL) {
        upipe_release(upipe);
        return;
    }

    if (upipe_avcenc->next_uref != NULL && upipe_avcenc->ubuf_mgr != NULL) {
        /* Feed avcodec with the last incomplete uref (sound only). */
        struct uref *uref = upipe_avcenc->next_uref;
        size_t next_uref_size = upipe_avcenc->next_uref_size;
        upipe_avcenc->next_uref = NULL;
        upipe_avcenc->next_uref_size = 0;

        int frame_size = av_samples_get_buffer_size(NULL, context->channels,
                                                    context->frame_size,
                                                    context->sample_fmt, 0);
        int sample_size = av_get_bytes_per_sample(context->sample_fmt);
        assert(frame_size > next_uref_size);

        struct ubuf *ubuf = ubuf_block_alloc(upipe_avcenc->ubuf_mgr,
                                             frame_size - next_uref_size);
        if (likely(ubuf != NULL)) {
            int size = -1;
            uint8_t *buf;
            if (likely(ubuf_block_write(ubuf, 0, &size, &buf))) {
                av_samples_set_silence(&buf, 0,
                        size / context->channels / sample_size,
                        context->channels,
                        av_get_packed_sample_fmt(context->sample_fmt));
                ubuf_block_unmap(ubuf, 0);
                if (likely(uref_block_append(uref, ubuf)))
                    upipe_avcenc_encode_audio(upipe, uref, NULL);
                else {
                    ubuf_free(ubuf);
                    upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
                }
            } else {
                ubuf_free(ubuf);
                upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
            }
        } else
            upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
    }

    if (context->codec->capabilities & CODEC_CAP_DELAY) {
        /* Feed avcodec with NULL frames to output the remaining packets. */
        while (upipe_avcenc_encode_frame(upipe, NULL, NULL));
    }
    upipe_avcenc->close = true;
    upipe_avcenc_start_av_deal(upipe);
}

/** @internal @This encodes av frames.
 *
 * @param upipe description structure of the pipe
 * @param frame frame
 * @param upump upump structure
 * @return true when a packet has been output
 */
static bool upipe_avcenc_encode_frame(struct upipe *upipe,
                                      struct AVFrame *frame,
                                      struct upump *upump)
{
    struct upipe_avcenc *upipe_avcenc = upipe_avcenc_from_upipe(upipe);
    AVCodecContext *context = upipe_avcenc->context;
    const AVCodec *codec = context->codec;

    if (unlikely(frame == NULL))
        upipe_dbg(upipe, "received null frame");

    /* encode frame */
    AVPacket avpkt;
    av_init_packet(&avpkt);
    avpkt.data = NULL;
    avpkt.size = 0;
    int gotframe = 0;
    int err;
    switch (codec->type) {
        case AVMEDIA_TYPE_VIDEO: {
            err = avcodec_encode_video2(context, &avpkt, frame, &gotframe);
            break;
        }
        case AVMEDIA_TYPE_AUDIO: {
            err = avcodec_encode_audio2(context, &avpkt, frame, &gotframe);
            break;
        }
        default: /* should never be there */
            return false;
    }

    if (err < 0) {
        upipe_av_strerror(err, buf);
        upipe_warn_va(upipe, "error while encoding frame (%s)", buf);
        return false;
    }
    /* output encoded frame if available */
    if (!(gotframe && avpkt.data)) {
        return false;
    }

    /* flow definition */
    struct uref *flow_def_attr = upipe_avcenc_alloc_flow_def_attr(upipe);
    if (unlikely(flow_def_attr == NULL)) {
        av_free_packet(&avpkt);
        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
        return false;
    }

    bool ret = true;
    const char *codec_def = upipe_av_to_flow_def(codec->id);
    ret = ret && uref_flow_set_def_va(flow_def_attr, PREFIX_FLOW "%s",
                                      codec_def);
    if (context->bit_rate) {
        uref_block_flow_set_octetrate(flow_def_attr, context->bit_rate / 8);
        if (context->rc_buffer_size)
            uref_block_flow_set_cpb_buffer(flow_def_attr,
                                           context->rc_buffer_size / 8);
        else if (codec->type == AVMEDIA_TYPE_AUDIO &&
                 strcmp(codec->name, "mp2") && strcmp(codec->name, "mp3")) {
            uref_block_flow_set_cpb_buffer(flow_def_attr, AUDIO_BS);
            upipe_avcenc->audio_bs_duration = AUDIO_BS * UCLOCK_FREQ /
                                              (context->bit_rate / 8);
            upipe_avcenc->audio_bs_leakage = UCLOCK_FREQ *
                context->frame_size / context->sample_rate;
            upipe_avcenc->audio_bs_delay = upipe_avcenc->audio_bs_duration;
        }

        if (codec->type == AVMEDIA_TYPE_AUDIO && context->frame_size > 0) {
            uref_sound_flow_set_samples(flow_def_attr, context->frame_size);
        }
    }
    struct urational fps;
    if (uref_pic_flow_get_fps(upipe_avcenc->flow_def_input, &fps) &&
        context->delay)
        ret = ret && uref_clock_set_latency(flow_def_attr,
                upipe_avcenc->input_latency +
                context->delay * UCLOCK_FREQ * fps.num / fps.den);

    if (!ret) {
        uref_free(flow_def_attr);
        av_free_packet(&avpkt);
        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
        return false;
    }

    if (unlikely(upipe_avcenc->ubuf_mgr == NULL)) {
        upipe_throw_need_ubuf_mgr(upipe, flow_def_attr);
        if (unlikely(upipe_avcenc->ubuf_mgr == NULL)) {
            uref_free(flow_def_attr);
            av_free_packet(&avpkt);
            upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
            return false;
        }
    }

    /* Find out if flow def attributes have changed. */
    if (!upipe_avcenc_check_flow_def_attr(upipe, flow_def_attr)) {
        struct uref *flow_def =
            upipe_avcenc_store_flow_def_attr(upipe, flow_def_attr);
        if (flow_def != NULL) {
            uref_pic_flow_clear_format(flow_def);
            upipe_avcenc_store_flow_def(upipe, flow_def);
        }
    } else
        uref_free(flow_def_attr);

    struct ubuf *ubuf = ubuf_block_alloc(upipe_avcenc->ubuf_mgr, avpkt.size);
    if (unlikely(ubuf == NULL)) {
        av_free_packet(&avpkt);
        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
        return false;
    }

    int size = -1;
    uint8_t *buf;
    if (unlikely(!ubuf_block_write(ubuf, 0, &size, &buf))) {
        ubuf_free(ubuf);
        av_free_packet(&avpkt);
        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
        return false;
    }
    memcpy(buf, avpkt.data, size);
    ubuf_block_unmap(ubuf, 0);
    av_free_packet(&avpkt);

    /* find uref corresponding to avpkt */
    struct uchain *uchain;
    struct uchain *uchain_tmp;
    struct uref *uref = NULL;
    ulist_delete_foreach (&upipe_avcenc->urefs_in_use, uchain, uchain_tmp) {
        struct uref *uref_chain = uref_from_uchain(uchain);
        int64_t priv = 0;
        if (uref_avcenc_get_priv(uref_chain, &priv) && priv == avpkt.pts) {
            uref = uref_chain;
            ulist_delete(uchain);
            break;
        }
    }
    if (unlikely(uref == NULL)) {
        upipe_warn_va(upipe, "could not find pts %"PRId64" in urefs in use",
                      avpkt.pts);
        ubuf_free(ubuf);
        return false;
    }

    /* unmap input */
    switch (codec->type) {
        case AVMEDIA_TYPE_VIDEO: {
            int i;
            for (i = 0; i < UPIPE_AV_MAX_PLANES &&
                        upipe_avcenc->chroma_map[i] != NULL; i++)
                uref_pic_plane_unmap(uref, upipe_avcenc->chroma_map[i],
                                     0, 0, -1, -1);
            break;
        }
        case AVMEDIA_TYPE_AUDIO: {
            uref_block_unmap(uref, 0);
            break;
        }
        default: /* should never be there */
            uref_free(uref);
            return false;
    }
    uref_attach_ubuf(uref, ubuf);
    uref_avcenc_delete_priv(uref);

    /* set dts */
    uint64_t dts_pts_delay = (uint64_t)(avpkt.pts - avpkt.dts) * UCLOCK_FREQ
                              * context->time_base.num
                              / context->time_base.den;
    uref_clock_set_dts_pts_delay(uref, dts_pts_delay);

    /* rebase to dts as we're in encoded domain now */
    uref_clock_rebase_dts_sys(uref);
    uref_clock_rebase_dts_prog(uref);
    uref_clock_rebase_dts_orig(uref);

    /* vbv delay */
    if (context->vbv_delay) {
        uref_clock_set_cr_dts_delay(uref, context->vbv_delay);
    } else if (codec->type == AVMEDIA_TYPE_AUDIO &&
               strcmp(codec->name, "mp2") && strcmp(codec->name, "mp3")) {
        upipe_avcenc->audio_bs_delay += upipe_avcenc->audio_bs_leakage;
        upipe_avcenc->audio_bs_delay -= size * UCLOCK_FREQ /
                                 (context->bit_rate / 8);
        if (upipe_avcenc->audio_bs_delay < 0) {
            upipe_warn_va(upipe, "audio BS underflow %"PRId64,
                          -upipe_avcenc->audio_bs_delay);
            upipe_avcenc->audio_bs_delay = 0;
        } else if (upipe_avcenc->audio_bs_delay >
                   upipe_avcenc->audio_bs_duration)
            upipe_avcenc->audio_bs_delay = upipe_avcenc->audio_bs_duration;
        uref_clock_set_cr_dts_delay(uref, upipe_avcenc->audio_bs_delay);
    } else {
        uref_clock_delete_cr_dts_delay(uref);
    }

    upipe_avcenc_output(upipe, uref, upump);
    return true;
}

/** @internal @This encodes video frames.
 *
 * @param upipe description structure of the pipe
 * @param uref uref structure
 * @param upump upump structure
 */
static void upipe_avcenc_encode_video(struct upipe *upipe,
                                      struct uref *uref, struct upump *upump)
{
    struct upipe_avcenc *upipe_avcenc = upipe_avcenc_from_upipe(upipe);
    AVCodecContext *context = upipe_avcenc->context;
    AVFrame *frame = upipe_avcenc->frame;

    /* FIXME check picture format against flow def */
    size_t hsize, vsize;
    if (unlikely(!uref_pic_size(uref, &hsize, &vsize, NULL) ||
                 hsize != context->width || vsize != context->height)) {
        upipe_warn(upipe, "invalid buffer received");
        uref_free(uref);
        return;
    }

    int i;
    for (i = 0; i < UPIPE_AV_MAX_PLANES && upipe_avcenc->chroma_map[i] != NULL;
         i++) {
        const uint8_t *data;
        size_t stride;
        if (unlikely(!uref_pic_plane_read(uref, upipe_avcenc->chroma_map[i],
                                          0, 0, -1, -1, &data) ||
                     !uref_pic_plane_size(uref, upipe_avcenc->chroma_map[i],
                                          &stride, NULL, NULL, NULL))) {
            upipe_warn(upipe, "invalid buffer received");
            uref_free(uref);
            return;
        }
        frame->data[i] = (uint8_t *)data;
        frame->linesize[i] = stride;
    }

    /* set pts (needed for uref/avpkt mapping) */
    frame->pts = upipe_avcenc->avcpts++;
    if (unlikely(!uref_avcenc_set_priv(uref, frame->pts))) {
        uref_free(uref);
        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
        return;
    }

    /* set aspect ratio if it was changed in the input */
    struct urational sar = {1, 1};
    if (uref_pic_flow_get_sar(upipe_avcenc->flow_def_input, &sar)) {
        context->sample_aspect_ratio.num = sar.num;
        context->sample_aspect_ratio.den = sar.den;
    }

    /* store uref in mapping list */
    ulist_add(&upipe_avcenc->urefs_in_use, uref_to_uchain(uref));
    upipe_avcenc_encode_frame(upipe, frame, upump);
}

/** @internal @This is a temporary function to uninterleave to planar formats.
 *
 * @param upipe description structure of the pipe
 * @param uref uref structure
 * @param buf output buffer
 * @param bufsize output buffer size
 * @return false in case of error
 */
static bool upipe_avcenc_uninterleave(struct upipe *upipe, struct uref *uref,
                                      uint8_t *buf, int bufsize)
{
    struct upipe_avcenc *upipe_avcenc = upipe_avcenc_from_upipe(upipe);
    AVCodecContext *context = upipe_avcenc->context;
    AVFrame *frame = upipe_avcenc->frame;
    int sample_size = av_get_bytes_per_sample(context->sample_fmt);
    int channels = context->channels;
    int nb_samples = frame->nb_samples;
    unsigned int sample = 0;

    while (sample < nb_samples) {
        int read_size = -1;
        const uint8_t *read_buffer;
        unsigned int old_sample = sample;
        if (unlikely(!uref_block_read(uref, sample * sample_size * channels,
                                      &read_size, &read_buffer)))
            return false;

        while (read_size >= channels * sample_size) {
            unsigned int channel;
            for (channel = 0; channel < channels; channel++) {
                unsigned int k;
                for (k = 0; k < sample_size; k++)
                    buf[((channel * nb_samples) + sample) * sample_size + k] =
                        read_buffer[k];
            }
            read_size -= channels * sample_size;
            read_buffer += channels * sample_size;
            sample++;
        }
        uref_block_unmap(uref, old_sample * sample_size * channels);
    }
    return true;
}

/** @internal @This encodes audio frames.
 *
 * @param upipe description structure of the pipe
 * @param uref uref structure
 * @param upump upump structure
 */
static void upipe_avcenc_encode_audio(struct upipe *upipe,
                                      struct uref *uref, struct upump *upump)
{
    struct upipe_avcenc *upipe_avcenc = upipe_avcenc_from_upipe(upipe);
    AVCodecContext *context = upipe_avcenc->context;
    AVFrame *frame = upipe_avcenc->frame;

    int size = av_samples_get_buffer_size(NULL, context->channels,
                                          context->frame_size,
                                          context->sample_fmt, 0);

    frame->nb_samples = context->frame_size;
    frame->format = context->sample_fmt;
    frame->channel_layout = context->channel_layout;

    /* TODO replace with umem */
    uint8_t *buf = malloc(size + FF_INPUT_BUFFER_PADDING_SIZE);
    if (unlikely(buf == NULL)) {
        uref_free(uref);
        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
        return;
    }

    bool ret;
    if (av_sample_fmt_is_planar(context->sample_fmt))
        ret = upipe_avcenc_uninterleave(upipe, uref, buf, size);
    else
        ret = uref_block_extract(uref, 0, size, buf);
    if (unlikely(!ret)) {
        upipe_warn(upipe, "invalid buffer received");
        uref_free(uref);
        return;
    }

    avcodec_fill_audio_frame(frame, context->channels,
                             context->sample_fmt, buf, size, 0);

    /* set pts (needed for uref/avpkt mapping) */
    frame->pts = upipe_avcenc->avcpts++;
    if (unlikely(!uref_avcenc_set_priv(uref, frame->pts))) {
        uref_free(uref);
        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
        return;
    }

    /* store uref in mapping list */
    ulist_add(&upipe_avcenc->urefs_in_use, uref_to_uchain(uref));
    upipe_avcenc_encode_frame(upipe, frame, upump);
    free(buf);
}

/** @internal @This encodes frames.
 *
 * @param upipe description structure of the pipe
 * @param uref uref structure
 * @param upump upump structure
 * @return always true
 */
static bool upipe_avcenc_encode(struct upipe *upipe,
                                struct uref *uref, struct upump *upump)
{
    struct upipe_avcenc *upipe_avcenc = upipe_avcenc_from_upipe(upipe);
    AVCodecContext *context = upipe_avcenc->context;

    /* map input */
    switch (context->codec->type) {
        case AVMEDIA_TYPE_VIDEO:
            upipe_avcenc_encode_video(upipe, uref, upump);
            break;

        case AVMEDIA_TYPE_AUDIO: {
            upipe_avcenc_append_sound_stream(upipe, uref);

            int size = av_samples_get_buffer_size(NULL, context->channels,
                                                  context->frame_size,
                                                  context->sample_fmt, 0);
            size_t remaining = 0;
            while (upipe_avcenc->next_uref &&
                   uref_block_size(upipe_avcenc->next_uref, &remaining) &&
                   (remaining >= size)) {
                uref = upipe_avcenc_extract_sound_stream(upipe, size,
                            context->channels,
                            av_get_bytes_per_sample(context->sample_fmt),
                            context->sample_rate);
                upipe_avcenc_encode_audio(upipe, uref, upump);
            }
            break;
        }
        default:
            uref_free(uref);
            break;
    }
    return true;
}

/** @internal @This handles input uref.
 *
 * @param upipe description structure of the pipe
 * @param uref uref structure
 * @param upump upump structure
 */
static void upipe_avcenc_input(struct upipe *upipe, struct uref *uref,
                               struct upump *upump)
{
    struct upipe_avcenc *upipe_avcenc = upipe_avcenc_from_upipe(upipe);

    while (unlikely(!avcodec_is_open(upipe_avcenc->context))) {
        if (upipe_avcenc->upump_av_deal != NULL) {
            upipe_avcenc_block_sink(upipe, upump);
            upipe_avcenc_hold_sink(upipe, uref);
            return;
        }

        upipe_avcenc_open(upipe);
    }

    upipe_avcenc_encode(upipe, uref, upump);
}

/** @internal @This sets the input flow definition.
 *
 * @param upipe description structure of the pipe
 * @param flow_def flow definition packet
 * @return false if the flow definition is not handled
 */
static bool upipe_avcenc_set_flow_def(struct upipe *upipe,
                                      struct uref *flow_def)
{
    if (flow_def == NULL)
        return false;

    const char *def;
    if (unlikely(!uref_flow_get_def(flow_def, &def) ||
                 (ubase_ncmp(def, "pic.") && ubase_ncmp(def, "sound.") &&
                  strstr(def, ".sound.") == NULL)))
        return false;

    /* Extract relevant attributes to flow def check. */
    struct uref *flow_def_check =
        upipe_avcenc_alloc_flow_def_check(upipe, flow_def);
    if (unlikely(flow_def_check == NULL)) {
        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
        return false;
    }

    if (unlikely(!uref_flow_set_def(flow_def_check, def))) {
        uref_free(flow_def_check);
        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
        return false;
    }

    if (!ubase_ncmp(def, "pic.")) {
        uint64_t hsize, vsize;
        struct urational fps;
        if (!uref_pic_flow_get_hsize(flow_def, &hsize) ||
            !uref_pic_flow_get_vsize(flow_def, &vsize) ||
            !uref_pic_flow_get_fps(flow_def, &fps)) {
            upipe_err(upipe, "incompatible flow def");
            uref_free(flow_def_check);
            return false;
        }
        uref_pic_flow_get_hsize_visible(flow_def, &hsize);
        uref_pic_flow_get_vsize_visible(flow_def, &vsize);

        if (unlikely(!uref_pic_flow_copy_format(flow_def_check, flow_def) ||
                     !uref_pic_flow_set_hsize(flow_def_check, hsize) ||
                     !uref_pic_flow_set_vsize(flow_def_check, vsize) ||
                     !uref_pic_flow_set_fps(flow_def_check, fps))) {
            uref_free(flow_def_check);
            upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
            return false;
        }
    } else {
        uint8_t channels;
        uint64_t rate;
        if (!uref_sound_flow_get_channels(flow_def, &channels) ||
            !uref_sound_flow_get_rate(flow_def, &rate)) {
            upipe_err(upipe, "incompatible flow def");
            uref_free(flow_def_check);
            return false;
        }

        if (unlikely(!uref_flow_set_def(flow_def_check, def) ||
                     !uref_sound_flow_set_channels(flow_def_check, channels) ||
                     !uref_sound_flow_set_rate(flow_def_check, rate))) {
            uref_free(flow_def_check);
            upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
            return false;
        }
    }

    struct upipe_avcenc *upipe_avcenc = upipe_avcenc_from_upipe(upipe);
    AVCodecContext *context = upipe_avcenc->context;
    const AVCodec *codec = context->codec;

    if (avcodec_is_open(upipe_avcenc->context)) {
        /* Die if the attributes changed. */
        /* NB: this supposes that all attributes are in the udict, and that
         * the udict is never empty. */
        if (!upipe_avcenc_check_flow_def_check(upipe, flow_def_check)) {
            uref_free(flow_def_check);
            return false;
        }
        uref_free(flow_def_check);

    } else if (!ubase_ncmp(def, "pic.")) {
        context->pix_fmt = upipe_av_pixfmt_from_flow_def(flow_def,
                    codec->pix_fmts, upipe_avcenc->chroma_map);
        if (context->pix_fmt == AV_PIX_FMT_NONE) {
            upipe_err_va(upipe, "unsupported pixel format");
            uref_dump(flow_def, upipe->uprobe);
            uref_free(flow_def_check);
            return false;
        }

        const AVRational *supported_framerates = codec->supported_framerates;
        struct urational fps = {25, 1};
        if (uref_pic_flow_get_fps(flow_def, &fps) &&
            supported_framerates != NULL) {
            int i;
            for (i = 0; supported_framerates[i].num; i++)
                if (supported_framerates[i].num == fps.num &&
                    supported_framerates[i].den == fps.den)
                    break;
            if (!supported_framerates[i].num) {
                upipe_err_va(upipe, "unsupported frame rate %"PRIu64"/%"PRIu64,
                             fps.num, fps.den);
                uref_free(flow_def_check);
                return false;
            }
        }
        context->time_base.num = fps.den;
        context->time_base.den = fps.num;

        struct urational sar;
        if (uref_pic_flow_get_sar(flow_def, &sar)) {
            context->sample_aspect_ratio.num = sar.num;
            context->sample_aspect_ratio.den = sar.den;
        }

        uint64_t hsize = 0, vsize = 0;
        uref_pic_flow_get_hsize(flow_def, &hsize);
        uref_pic_flow_get_vsize(flow_def, &vsize);
        context->width = hsize;
        context->height = vsize;

        upipe_avcenc_store_flow_def_check(upipe, flow_def_check);

    } else {
        enum AVSampleFormat sample_fmt =
            upipe_av_samplefmt_from_flow_def(def);
        const enum AVSampleFormat *sample_fmts = codec->sample_fmts;
        if (sample_fmt == AV_SAMPLE_FMT_NONE || sample_fmts == NULL) {
            upipe_err_va(upipe, "unsupported sample format %s", def);
            uref_free(flow_def_check);
            return false;
        }
        while (*sample_fmts != -1) {
            if (*sample_fmts == sample_fmt)
                break;
            sample_fmts++;
        }
        if (*sample_fmts == -1) {
            /* Try again with planar formats. */
            sample_fmts = codec->sample_fmts;
            while (*sample_fmts != -1) {
                if (av_get_packed_sample_fmt(*sample_fmts) == sample_fmt)
                    break;
                sample_fmts++;
            }
            if (*sample_fmts == -1) {
                upipe_err_va(upipe, "unsupported sample format %s", def);
                uref_free(flow_def_check);
                return false;
            }
        }
        context->sample_fmt = *sample_fmts;

        uint64_t rate;
        const int *supported_samplerates = codec->supported_samplerates;
        if (!uref_sound_flow_get_rate(flow_def, &rate) ||
            supported_samplerates == NULL) {
            upipe_err_va(upipe, "unsupported sample rate");
            uref_free(flow_def_check);
            return false;
        }
        while (*supported_samplerates != 0) {
            if (*supported_samplerates == rate)
                break;
            supported_samplerates++;
        }
        if (*supported_samplerates == 0) {
            upipe_err_va(upipe, "unsupported sample rate %"PRIu64, rate);
            uref_free(flow_def_check);
            return false;
        }
        context->sample_rate = rate;
        context->time_base.num = 1;
        context->time_base.den = 1;//rate; FIXME

        uint8_t channels;
        const uint64_t *channel_layouts =
            upipe_avcenc->context->codec->channel_layouts;
        if (!uref_sound_flow_get_channels(flow_def, &channels) ||
            channel_layouts == NULL) {
            upipe_err_va(upipe, "unsupported channel layout");
            uref_free(flow_def_check);
            return false;
        }
        while (*channel_layouts != 0) {
            if (av_get_channel_layout_nb_channels(*channel_layouts) == channels)
                break;
            channel_layouts++;
        }
        if (*channel_layouts == 0) {
            upipe_err_va(upipe, "unsupported channel layout %"PRIu8, channels);
            uref_free(flow_def_check);
            return false;
        }
        context->channels = channels;
        context->channel_layout = *channel_layouts;

        upipe_avcenc_store_flow_def_check(upipe, flow_def_check);
    }

    flow_def = uref_dup(flow_def);
    if (unlikely(flow_def == NULL)) {
        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
        return false;
    }
    flow_def = upipe_avcenc_store_flow_def_input(upipe, flow_def);
    if (flow_def != NULL) {
        uref_pic_flow_clear_format(flow_def);
        upipe_avcenc_store_flow_def(upipe, flow_def);
    }

    upipe_avcenc->input_latency = 0;
    uref_clock_get_latency(upipe_avcenc->flow_def_input,
                           &upipe_avcenc->input_latency);
    return true;
}

/** @internal @This sets the content of an avcodec option. It only take effect
 * after the next call to @ref upipe_avcenc_set_url.
 *
 * @param upipe description structure of the pipe
 * @param option name of the option
 * @param content content of the option, or NULL to delete it
 * @return false in case of error
 */
static bool _upipe_avcenc_set_option(struct upipe *upipe, const char *option,
                                     const char *content)
{
    struct upipe_avcenc *upipe_avcenc = upipe_avcenc_from_upipe(upipe);
    assert(option != NULL);
    int error = av_opt_set(upipe_avcenc->context, option, content,
                           AV_OPT_SEARCH_CHILDREN);
    if (unlikely(error < 0)) {
        upipe_av_strerror(error, buf);
        upipe_err_va(upipe, "can't set option %s:%s (%s)", option, content,
                     buf);
        return false;
    }
    return true;
}

/** @internal @This processes control commands on a file source pipe, and
 * checks the status of the pipe afterwards.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return false in case of error
 */
static bool upipe_avcenc_control(struct upipe *upipe, enum upipe_command command,
                               va_list args)
{
    switch (command) {
        case UPIPE_GET_UBUF_MGR: {
            struct ubuf_mgr **p = va_arg(args, struct ubuf_mgr **);
            return upipe_avcenc_get_ubuf_mgr(upipe, p);
        }
        case UPIPE_SET_UBUF_MGR: {
            struct ubuf_mgr *ubuf_mgr = va_arg(args, struct ubuf_mgr *);
            return upipe_avcenc_set_ubuf_mgr(upipe, ubuf_mgr);
        }
        case UPIPE_GET_FLOW_DEF: {
            struct uref **p = va_arg(args, struct uref **);
            return upipe_avcenc_get_flow_def(upipe, p);
        }
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_avcenc_set_flow_def(upipe, flow_def);
        }
        case UPIPE_GET_OUTPUT: {
            struct upipe **p = va_arg(args, struct upipe **);
            return upipe_avcenc_get_output(upipe, p);
        }
        case UPIPE_SET_OUTPUT: {
            struct upipe *output = va_arg(args, struct upipe *);
            return upipe_avcenc_set_output(upipe, output);
        }
        case UPIPE_GET_UPUMP_MGR: {
            struct upump_mgr **p = va_arg(args, struct upump_mgr **);
            return upipe_avcenc_get_upump_mgr(upipe, p);
        }
        case UPIPE_SET_UPUMP_MGR: {
            struct upump_mgr *upump_mgr = va_arg(args, struct upump_mgr *);
            upipe_avcenc_set_upump_av_deal(upipe, NULL);
            upipe_avcenc_abort_av_deal(upipe);
            return upipe_avcenc_set_upump_mgr(upipe, upump_mgr);
        }

        case UPIPE_AVCENC_SET_OPTION: {
            unsigned int signature = va_arg(args, unsigned int);
            assert(signature == UPIPE_AVCENC_SIGNATURE);
            const char *option = va_arg(args, const char *);
            const char *content = va_arg(args, const char *);
            return _upipe_avcenc_set_option(upipe, option, content);
        }

        default:
            return false;
    }
}

/** @This frees a upipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_avcenc_free(struct upipe *upipe)
{
    struct upipe_avcenc *upipe_avcenc = upipe_avcenc_from_upipe(upipe);

    if (upipe_avcenc->context != NULL)
        av_free(upipe_avcenc->context);
    av_free(upipe_avcenc->frame);

    /* free remaining urefs (should not be any) */
    struct uchain *uchain;
    while ((uchain = ulist_pop(&upipe_avcenc->urefs_in_use)) != NULL) {
        struct uref *uref = uref_from_uchain(uchain);
        int64_t priv;
        if (uref_avcenc_get_priv(uref, &priv))
            upipe_warn_va(upipe, "remaining uref %"PRId64" freed", priv);
        uref_free(uref);
    }

    upipe_throw_dead(upipe);
    upipe_avcenc_abort_av_deal(upipe);
    upipe_avcenc_clean_sink(upipe);
    upipe_avcenc_clean_ubuf_mgr(upipe);
    upipe_avcenc_clean_upump_av_deal(upipe);
    upipe_avcenc_clean_upump_mgr(upipe);
    upipe_avcenc_clean_sound_stream(upipe);

    upipe_avcenc_clean_output(upipe);
    upipe_avcenc_clean_flow_def(upipe);
    upipe_avcenc_clean_flow_def_check(upipe);
    upipe_avcenc_free_flow(upipe);
}

/** @internal @This allocates a avcenc pipe.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_avcenc_alloc(struct upipe_mgr *mgr,
                                        struct uprobe *uprobe,
                                        uint32_t signature, va_list args)
{
    AVFrame *frame = avcodec_alloc_frame();
    if (unlikely(frame == NULL))
        return NULL;

    struct uref *flow_def;
    struct upipe *upipe = upipe_avcenc_alloc_flow(mgr, uprobe, signature, args,
                                                  &flow_def);
    if (unlikely(upipe == NULL)) {
        av_free(frame);
        return NULL;
    }

    struct upipe_avcenc *upipe_avcenc = upipe_avcenc_from_upipe(upipe);
    const char *def, *name;
    enum AVCodecID codec_id;
    AVCodec *codec = NULL;

    if (!uref_avcenc_get_codec_name(flow_def, &name)
            || !(codec = avcodec_find_encoder_by_name(name))) {
        if (uref_flow_get_def(flow_def, &def) &&
                (codec_id = upipe_av_from_flow_def(def + strlen("block.")))) {
            codec = avcodec_find_encoder(codec_id);
        }
    }
    
    if ((codec == NULL) ||
            (upipe_avcenc->context = avcodec_alloc_context3(codec)) == NULL) {
        uref_free(flow_def);
        av_free(frame);
        upipe_avcenc_free_flow(upipe);
        return NULL;
    }

    uref_free(flow_def);
    upipe_avcenc->frame = frame;
    upipe_avcenc->context->codec = codec;
    upipe_avcenc->context->opaque = upipe;

    upipe_avcenc_init_ubuf_mgr(upipe);
    upipe_avcenc_init_upump_mgr(upipe);
    upipe_avcenc_init_upump_av_deal(upipe);
    upipe_avcenc_init_output(upipe);
    upipe_avcenc_init_flow_def(upipe);
    upipe_avcenc_init_flow_def_check(upipe);
    upipe_avcenc_init_sink(upipe);
    upipe_avcenc_init_sound_stream(upipe);

    ulist_init(&upipe_avcenc->urefs_in_use);
    upipe_avcenc->avcpts = 1;
    upipe_avcenc->audio_bs_duration = 0;
    upipe_avcenc->audio_bs_leakage = 0;
    upipe_avcenc->audio_bs_delay = 0;

    /* Increment our refcount so the context will have time to be closed
     * (decremented in @ref upipe_avcenc_do_av_deal) */
    upipe_use(upipe);

    upipe_throw_ready(upipe);
    return upipe;
}

/** module manager static descriptor */
static struct upipe_mgr upipe_avcenc_mgr = {
    .signature = UPIPE_AVCENC_SIGNATURE,

    .upipe_alloc = upipe_avcenc_alloc,
    .upipe_input = upipe_avcenc_input,
    .upipe_control = upipe_avcenc_control,
    .upipe_free = upipe_avcenc_free,

    .upipe_mgr_free = NULL
};

/** @internal @This returns the management structure for avcodec encoders.
 *
 * @return pointer to manager
 */
struct upipe_mgr *upipe_avcenc_mgr_alloc(void)
{
    /* We close the context even if it was not opened because it supposedly
     * "frees allocated structures" */
    return upipe_proxy_mgr_alloc(&upipe_avcenc_mgr, upipe_avcenc_close);
}

/** @This configures the given flow definition to be able to encode to the
 * av codec described by name.
 *
 * @param flow_def flow definition packet
 * @param name codec name
 * @return false if the codec was not found
 */
bool upipe_avcenc_mgr_flow_def_from_name(struct uref *flow_def,
                                         const char *name)
{
    if (name == NULL)
        return false;
    AVCodec *codec = avcodec_find_encoder_by_name(name);
    if (codec == NULL)
        return false;
    const char *def = upipe_av_to_flow_def(codec->id);
    if (def == NULL)
        return false;
    return uref_flow_set_def(flow_def, def);
}
