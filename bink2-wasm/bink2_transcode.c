#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
#include <emscripten.h>

EM_JS(void, bink2_js_progress, (int frame, int total), {
    postMessage({ type: 'progress', frame, total });
});

static char last_error[1024];

static int failf(int code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_error, sizeof(last_error), fmt, ap);
    va_end(ap);
    return code < 0 ? code : AVERROR(EINVAL);
}

static int fail_av(int code, const char *what) {
    char err[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(code, err, sizeof(err));
    return failf(code, "%s: %s", what, err);
}

EMSCRIPTEN_KEEPALIVE
const char *bink2_last_error(void) {
    return last_error;
}

static int drain_encoder(AVCodecContext *enc, AVFormatContext *out,
                         AVStream *out_stream, AVPacket *packet) {
    int ret;
    while (1) {
        ret = avcodec_receive_packet(enc, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
            return fail_av(ret, "VP9 encoder failed");
        av_packet_rescale_ts(packet, enc->time_base, out_stream->time_base);
        packet->stream_index = out_stream->index;
        ret = av_interleaved_write_frame(out, packet);
        av_packet_unref(packet);
        if (ret < 0)
            return fail_av(ret, "WebM muxer failed");
    }
}

static int encode_frame(AVCodecContext *enc, AVFormatContext *out,
                        AVStream *out_stream, AVFrame *frame,
                        AVPacket *packet) {
    int ret = avcodec_send_frame(enc, frame);
    if (ret < 0)
        return fail_av(ret, "Could not send frame to VP9 encoder");
    return drain_encoder(enc, out, out_stream, packet);
}

EMSCRIPTEN_KEEPALIVE
int transcode_bk2(const char *input_path, const char *output_path,
                  int crf, int cpu_used) {
    AVFormatContext *in = NULL;
    AVFormatContext *out = NULL;
    AVCodecContext *dec = NULL;
    AVCodecContext *enc = NULL;
    AVFrame *frame = NULL;
    AVPacket *packet = NULL;
    AVStream *in_stream = NULL;
    AVStream *out_stream = NULL;
    const AVCodec *decoder = NULL;
    const AVCodec *encoder = NULL;
    AVDictionary *enc_opts = NULL;
    int video_index = -1;
    int frame_count = 0;
    int total_frames = 0;
    int wrote_header = 0;
    int ret = 0;
    AVRational fps;
    char value[32];

    last_error[0] = '\0';
    if (!input_path || !output_path)
        return failf(AVERROR(EINVAL), "Missing input or output path");
    if (crf < 0) crf = 18;
    if (crf > 63) crf = 63;
    if (cpu_used < 0) cpu_used = 4;
    if (cpu_used > 8) cpu_used = 8;

    ret = avformat_open_input(&in, input_path, NULL, NULL);
    if (ret < 0) { ret = fail_av(ret, "Could not open BK2 input"); goto cleanup; }
    ret = avformat_find_stream_info(in, NULL);
    if (ret < 0) { ret = fail_av(ret, "Could not read BK2 stream info"); goto cleanup; }

    video_index = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_index < 0) { ret = fail_av(video_index, "No video stream in BK2"); goto cleanup; }
    in_stream = in->streams[video_index];

    decoder = avcodec_find_decoder(in_stream->codecpar->codec_id);
    if (!decoder) { ret = failf(AVERROR_DECODER_NOT_FOUND, "Bink2 decoder is not present in this build"); goto cleanup; }
    dec = avcodec_alloc_context3(decoder);
    if (!dec) { ret = failf(AVERROR(ENOMEM), "Could not allocate Bink2 decoder"); goto cleanup; }
    ret = avcodec_parameters_to_context(dec, in_stream->codecpar);
    if (ret < 0) { ret = fail_av(ret, "Could not initialize Bink2 decoder parameters"); goto cleanup; }
    dec->thread_count = 1;
    ret = avcodec_open2(dec, decoder, NULL);
    if (ret < 0) { ret = fail_av(ret, "Could not open Bink2 decoder"); goto cleanup; }

    if (dec->pix_fmt != AV_PIX_FMT_YUVA420P && dec->pix_fmt != AV_PIX_FMT_YUV420P) {
        const char *name = av_get_pix_fmt_name(dec->pix_fmt);
        ret = failf(AVERROR(EINVAL), "Unexpected Bink2 pixel format: %s", name ? name : "unknown");
        goto cleanup;
    }

    fps = av_guess_frame_rate(in, in_stream, NULL);
    if (fps.num <= 0 || fps.den <= 0)
        fps = (AVRational){30, 1};
    if (in_stream->nb_frames > 0 && in_stream->nb_frames <= INT32_MAX)
        total_frames = (int)in_stream->nb_frames;
    else if (in_stream->duration > 0 && in_stream->duration <= INT32_MAX)
        total_frames = (int)in_stream->duration;

    ret = avformat_alloc_output_context2(&out, NULL, "webm", output_path);
    if (ret < 0 || !out) { ret = fail_av(ret < 0 ? ret : AVERROR_UNKNOWN, "Could not create WebM output"); goto cleanup; }

    encoder = avcodec_find_encoder_by_name("libvpx-vp9");
    if (!encoder) { ret = failf(AVERROR_ENCODER_NOT_FOUND, "libvpx-vp9 encoder is not present in this build"); goto cleanup; }
    enc = avcodec_alloc_context3(encoder);
    if (!enc) { ret = failf(AVERROR(ENOMEM), "Could not allocate VP9 encoder"); goto cleanup; }

    enc->width = dec->width;
    enc->height = dec->height;
    enc->pix_fmt = dec->pix_fmt;
    enc->time_base = av_inv_q(fps);
    enc->framerate = fps;
    enc->bit_rate = 0;
    enc->gop_size = fps.num > 0 ? (fps.num / fps.den) * 4 : 120;
    if (enc->gop_size < 30) enc->gop_size = 30;
    enc->color_range = dec->color_range;
    enc->colorspace = dec->colorspace;
    enc->color_primaries = dec->color_primaries;
    enc->color_trc = dec->color_trc;
    enc->thread_count = 1;
    if (out->oformat->flags & AVFMT_GLOBALHEADER)
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    snprintf(value, sizeof(value), "%d", crf);
    av_dict_set(&enc_opts, "crf", value, 0);
    av_dict_set(&enc_opts, "auto-alt-ref", "0", 0);
    snprintf(value, sizeof(value), "%d", cpu_used);
    av_dict_set(&enc_opts, "cpu-used", value, 0);
    av_dict_set(&enc_opts, "deadline", "realtime", 0);

    ret = avcodec_open2(enc, encoder, &enc_opts);
    av_dict_free(&enc_opts);
    if (ret < 0) { ret = fail_av(ret, "Could not open VP9 encoder"); goto cleanup; }

    out_stream = avformat_new_stream(out, NULL);
    if (!out_stream) { ret = failf(AVERROR(ENOMEM), "Could not create WebM video stream"); goto cleanup; }
    out_stream->time_base = enc->time_base;
    out_stream->avg_frame_rate = fps;
    ret = avcodec_parameters_from_context(out_stream->codecpar, enc);
    if (ret < 0) { ret = fail_av(ret, "Could not initialize WebM video stream"); goto cleanup; }
    if (enc->pix_fmt == AV_PIX_FMT_YUVA420P)
        av_dict_set(&out_stream->metadata, "alpha_mode", "1", 0);

    if (!(out->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&out->pb, output_path, AVIO_FLAG_WRITE);
        if (ret < 0) { ret = fail_av(ret, "Could not open WebM output file"); goto cleanup; }
    }
    ret = avformat_write_header(out, NULL);
    if (ret < 0) { ret = fail_av(ret, "Could not write WebM header"); goto cleanup; }
    wrote_header = 1;

    frame = av_frame_alloc();
    packet = av_packet_alloc();
    if (!frame || !packet) { ret = failf(AVERROR(ENOMEM), "Could not allocate decode buffers"); goto cleanup; }

    while ((ret = av_read_frame(in, packet)) >= 0) {
        if (packet->stream_index != video_index) { av_packet_unref(packet); continue; }
        ret = avcodec_send_packet(dec, packet);
        av_packet_unref(packet);
        if (ret < 0) { ret = fail_av(ret, "Could not send Bink2 packet to decoder"); goto cleanup; }
        while (1) {
            ret = avcodec_receive_frame(dec, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) { ret = fail_av(ret, "Bink2 frame decode failed"); goto cleanup; }
            if (frame->format != enc->pix_fmt) { ret = failf(AVERROR(EINVAL), "Bink2 pixel format changed during decode"); goto cleanup; }
            frame->pts = frame_count;
            ret = encode_frame(enc, out, out_stream, frame, packet);
            av_frame_unref(frame);
            if (ret < 0) goto cleanup;
            frame_count++;
            bink2_js_progress(frame_count, total_frames);
        }
    }
    if (ret != AVERROR_EOF) { ret = fail_av(ret, "Error reading BK2 packets"); goto cleanup; }

    ret = avcodec_send_packet(dec, NULL);
    if (ret < 0 && ret != AVERROR_EOF) { ret = fail_av(ret, "Could not flush Bink2 decoder"); goto cleanup; }
    while (1) {
        ret = avcodec_receive_frame(dec, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) { ret = fail_av(ret, "Bink2 decoder flush failed"); goto cleanup; }
        frame->pts = frame_count;
        ret = encode_frame(enc, out, out_stream, frame, packet);
        av_frame_unref(frame);
        if (ret < 0) goto cleanup;
        frame_count++;
        bink2_js_progress(frame_count, total_frames);
    }

    ret = encode_frame(enc, out, out_stream, NULL, packet);
    if (ret < 0) goto cleanup;
    ret = av_write_trailer(out);
    if (ret < 0) { ret = fail_av(ret, "Could not finalize WebM output"); goto cleanup; }
    wrote_header = 0;
    ret = frame_count;

cleanup:
    av_dict_free(&enc_opts);
    if (ret < 0 && wrote_header && out) av_write_trailer(out);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&enc);
    avcodec_free_context(&dec);
    if (out) {
        if (!(out->oformat->flags & AVFMT_NOFILE) && out->pb) avio_closep(&out->pb);
        avformat_free_context(out);
    }
    avformat_close_input(&in);
    return ret;
}
