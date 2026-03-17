#include "utils.hpp"

#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
}

namespace EYEQ {

static int create_format_context(const char *filename, AVFormatContext **avf_ctx) {
  const AVOutputFormat *fmt;
  AVStream *st;

  int ret;
  ret = avformat_alloc_output_context2(avf_ctx, NULL, NULL, filename);
  if (ret < 0 || NULL == *avf_ctx) {
    av_log(NULL, AV_LOG_ERROR, "Could not create output context\n");
    return ret;
  }
  fmt = (*avf_ctx)->oformat;
  if (fmt->video_codec == AV_CODEC_ID_NONE) {
    av_log(NULL, AV_LOG_ERROR, "Output format %s do not support video\n", fmt->name);
    return AVERROR_MUXER_NOT_FOUND;
  }

  st = avformat_new_stream((*avf_ctx), NULL);
  if (NULL == st) {
    av_log(NULL, AV_LOG_ERROR, "Could not allocate stream\n");
    return AVERROR(ENOMEM);
  }
  st->id = (*avf_ctx)->nb_streams - 1;

  return 0;
}

static int create_codec_context(const AVFrame *frame, const char *filename, const AVFormatContext *avf_ctx,
                                AVCodecContext **avc_ctx) {
  const AVOutputFormat *fmt = avf_ctx->oformat;
  enum AVCodecID codec_id;
  const AVCodec *codec;
  enum AVPixelFormat best;
  const enum AVPixelFormat *p;
  int ret;

  codec_id = av_guess_codec(fmt, NULL, filename, NULL, AVMEDIA_TYPE_VIDEO);
  if (AV_CODEC_ID_NONE == codec_id) {
    av_log(NULL, AV_LOG_ERROR, "Cannot guess codec from filename '%s'\n", filename);
    return AVERROR_ENCODER_NOT_FOUND;
  }
  codec = avcodec_find_encoder(codec_id);
  if (NULL == codec) {
    av_log(NULL, AV_LOG_ERROR, "Cannot get encoder '%s'\n", avcodec_get_name(codec_id));
    return AVERROR_ENCODER_NOT_FOUND;
  }
  av_log(NULL, AV_LOG_DEBUG, "Output format %s, encoder %s\n", fmt->name, codec->name);

  *avc_ctx = avcodec_alloc_context3(codec);
  if (NULL == *avc_ctx) {
    av_log(NULL, AV_LOG_ERROR, "Could not alloc an encoding context\n");
    return AVERROR(ENOMEM);
  }

  // choose best pixel format
  ret = avcodec_get_supported_config(*avc_ctx, NULL, AV_CODEC_CONFIG_PIX_FORMAT, 0, (const void **)&p, NULL);
  if (ret < 0)
    return ret;
  if (NULL == p) { // all possible pixel formats are supported
    best = (enum AVPixelFormat)frame->format;
  } else {
    best = avcodec_find_best_pix_fmt_of_list(p, (enum AVPixelFormat)frame->format, 0, NULL);
    if (AV_PIX_FMT_NONE == best) {
      av_log(NULL, AV_LOG_ERROR, "Could not find a suitable pixel format for '%s'\n",
             avcodec_get_name(fmt->video_codec));
      avcodec_free_context(avc_ctx);
      return AVERROR_ENCODER_NOT_FOUND;
    }
  }
  (*avc_ctx)->pix_fmt = best;
  av_log(NULL, AV_LOG_DEBUG, "Choose pixel format %s for encoding\n", av_get_pix_fmt_name((*avc_ctx)->pix_fmt));
  (*avc_ctx)->width = frame->width;
  (*avc_ctx)->height = frame->height;
  (*avc_ctx)->time_base = (AVRational){1, 25};
  (*avc_ctx)->framerate = (AVRational){25, 1};

  /* Some formats want stream headers to be separate. */
  if (fmt->flags & AVFMT_GLOBALHEADER)
    (*avc_ctx)->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  (*avc_ctx)->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

  ret = avcodec_open2(*avc_ctx, codec, NULL);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "Cannot open codec!\n");
    avcodec_free_context(avc_ctx);
    return ret;
  }

  /* copy the stream parameters to the muxer */
  ret = avcodec_parameters_from_context(avf_ctx->streams[0]->codecpar, *avc_ctx);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "Could not copy the stream parameters\n");
    avcodec_free_context(avc_ctx);
    return ret;
  }
  return 0;
}

static AVFrame *convert_frame(AVCodecContext *codec_ctx, const AVFrame *frame) {
  AVFrame *oframe;
  struct SwsContext *sws_ctx;
  uint8_t *buffer;
  int buffer_size;

  oframe = av_frame_alloc();
  oframe->format = codec_ctx->pix_fmt;
  oframe->width = codec_ctx->width;
  oframe->height = codec_ctx->height;
  buffer_size = av_image_get_buffer_size((enum AVPixelFormat)oframe->format, oframe->width, oframe->height, 1);
  buffer = (uint8_t *)av_malloc(buffer_size);
  av_image_fill_arrays(oframe->data, oframe->linesize, buffer, (enum AVPixelFormat)oframe->format, oframe->width,
                       oframe->height, 1);

  sws_ctx = sws_getContext(frame->width, frame->height, (enum AVPixelFormat)frame->format, oframe->width,
                           oframe->height, (enum AVPixelFormat)oframe->format,
                           SWS_SPLINE | SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP | SWS_ACCURATE_RND, 0, 0, 0);
  if (NULL == sws_ctx) {
    av_log(NULL, AV_LOG_ERROR, "Cannot get sws_ctx.\n");
    av_frame_free(&oframe);
    return NULL;
  }
  sws_scale(sws_ctx, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height, oframe->data,
            oframe->linesize);

  sws_freeContext(sws_ctx);
  av_frame_copy_props(oframe, frame);
  return oframe;
}

static int encode_frame(AVFrame *frame, AVFormatContext *avf_ctx, AVCodecContext *avc_ctx) {
  AVStream *st = avf_ctx->streams[0];
  AVFrame *oframe;
  AVPacket *pkt;
  int ret = 0;

  pkt = av_packet_alloc();
  if (NULL == pkt) {
    return AVERROR(ENOMEM);
  }

  if (avc_ctx->pix_fmt == frame->format) {
    oframe = frame;
  } else {
    oframe = convert_frame(avc_ctx, frame);
    if (NULL == oframe) {
      return AVERROR_BUG;
    }
  }

  /* send the frame to the encoder */
  ret = avcodec_send_frame(avc_ctx, oframe);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "Error sending a frame for encoding\n");
    goto fail;
  }

  while (ret >= 0) {
    ret = avcodec_receive_packet(avc_ctx, pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      break;
    else if (ret < 0) {
      av_log(NULL, AV_LOG_ERROR, "Error encoding a frame: %s\n", ffmpeg_error_string(ret).c_str());
      break;
    }

    /* rescale output packet timestamp values from codec to stream timebase */
    av_packet_rescale_ts(pkt, avc_ctx->time_base, st->time_base);
    pkt->stream_index = st->index;

    /* Write the compressed frame to the media file. */
    ret = av_interleaved_write_frame(avf_ctx, pkt);
    /* pkt is now blank (av_interleaved_write_frame() takes ownership of
     * its contents and resets pkt), so that no unreferencing is necessary.
     * This would be different if one used av_write_frame(). */
    if (ret < 0) {
      av_log(NULL, AV_LOG_ERROR, "Error while writing output packet: %s\n", ffmpeg_error_string(ret).c_str());
      break;
    }
  }

fail:
  av_packet_free(&pkt);
  if (oframe != frame) {
    av_frame_free(&oframe);
  }

  return ret == AVERROR_EOF ? 1 : 0;
}

int save_frame(AVFrame *frame, const std::filesystem::path &filename, std::string_view format) {
  AVFormatContext *avf_ctx;
  AVCodecContext *avc_ctx;
  AVDictionary *options = NULL;
  int ret;

  (void)format;

  const std::string filename_utf8 = filename.u8string();
  const char *filename_cstr = filename_utf8.c_str();

  ret = create_format_context(filename_cstr, &avf_ctx);
  if (ret < 0)
    return ret;
  avf_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

  ret = create_codec_context(frame, filename_cstr, avf_ctx, &avc_ctx);
  if (ret < 0)
    return ret;

  if (AV_LOG_INFO > av_log_get_level()) {
    av_dump_format(avf_ctx, 0, filename_cstr, 1);
  }

  ret = avio_open(&(avf_ctx->pb), filename_cstr, AVIO_FLAG_WRITE);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "Could not open '%s': %s\n", filename_cstr, ffmpeg_error_string(ret).c_str());
    return ret;
  }

  if (strcmp(avf_ctx->oformat->name, "image2") == 0) {
    // remove the warning from image2 muxer about writing a single image
    av_dict_set(&options, "update", "1", 0);
  }
  ret = avformat_write_header(avf_ctx, &options);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file: %s\n", ffmpeg_error_string(ret).c_str());
    return ret;
  }
  ret = encode_frame(frame, avf_ctx, avc_ctx);
  if (ret < 0)
    return ret;
  av_write_trailer(avf_ctx);

  avcodec_free_context(&avc_ctx);
  avio_closep(&(avf_ctx->pb));
  avformat_free_context(avf_ctx);
  return 0;
}

}; // namespace EYEQ
