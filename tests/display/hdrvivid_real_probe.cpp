// V2-4 real-material probe (not a ctest).
//
// Decodes one frame from a real file, maps it through the real single
// FrameRenderer into the common display-linear target and prints the result
// statistics plus the dynamic-HDR metadata that was actually seen. It exists to
// document, per format, what real material does on this machine; it makes no
// correctness claim about Dolby display management.
//
// usage: hdrvivid_real_probe <file> [--hdrvivid] [--frame N]

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/frame.h>
#include <libavutil/hdr_dynamic_vivid_metadata.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
}
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#include "display/frame_renderer.hpp"
#include "display/hdrvivid_curve.hpp"
#include "display/render.hpp"
#include "display/render_context.hpp"

namespace {

std::shared_ptr<AVFrame> DecodeFrame(const std::string &path, int wanted, AVFormatContext **out_fmt,
                                     AVStream **out_stream, size_t *vivid_bytes, size_t *dovi_bytes) {
  AVFormatContext *fmt = nullptr;
  if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
    throw std::runtime_error("cannot open " + path);
  }
  if (avformat_find_stream_info(fmt, nullptr) < 0) {
    throw std::runtime_error("find_stream_info failed");
  }
  const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (vs < 0) {
    throw std::runtime_error("no video stream");
  }
  const AVCodec *dec = avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
  AVCodecContext *ctx = avcodec_alloc_context3(dec);
  avcodec_parameters_to_context(ctx, fmt->streams[vs]->codecpar);
  if (avcodec_open2(ctx, dec, nullptr) < 0) {
    throw std::runtime_error("avcodec_open2 failed");
  }
  std::shared_ptr<AVFrame> result;
  AVFrame *frame = av_frame_alloc();
  AVPacket *pkt = av_packet_alloc();
  int index = 0;
  while (result == nullptr && av_read_frame(fmt, pkt) >= 0) {
    if (pkt->stream_index != vs) {
      av_packet_unref(pkt);
      continue;
    }
    if (avcodec_send_packet(ctx, pkt) >= 0) {
      while (avcodec_receive_frame(ctx, frame) == 0) {
        if (index++ == wanted) {
          *vivid_bytes = 0;
          *dovi_bytes = 0;
          for (int i = 0; i < frame->nb_side_data; ++i) {
            if (frame->side_data[i]->type == AV_FRAME_DATA_DYNAMIC_HDR_VIVID) {
              *vivid_bytes = static_cast<size_t>(frame->side_data[i]->size);
            }
            if (frame->side_data[i]->type == AV_FRAME_DATA_DOVI_METADATA) {
              *dovi_bytes = static_cast<size_t>(frame->side_data[i]->size);
            }
          }
          AVFrame *copy = av_frame_alloc();
          av_frame_ref(copy, frame);
          copy->opaque = fmt->streams[vs];
          result = std::shared_ptr<AVFrame>(copy, [](AVFrame *f) { av_frame_free(&f); });
          break;
        }
        av_frame_unref(frame);
      }
    }
    av_packet_unref(pkt);
  }
  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&ctx);
  if (!result) {
    avformat_close_input(&fmt);
    throw std::runtime_error("no frame decoded");
  }
  *out_fmt = fmt;
  *out_stream = fmt->streams[vs];
  return result;
}

void PrintVividSummary(const AVFrame *frame) {
  const AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DYNAMIC_HDR_VIVID);
  if (!sd || sd->size < static_cast<int>(sizeof(AVDynamicHDRVivid))) {
    std::printf("  vivid side data: absent\n");
    return;
  }
  const auto *v = reinterpret_cast<const AVDynamicHDRVivid *>(sd->data);
  std::printf("  vivid side data: %zu bytes, system_start_code=%u num_windows=%u\n", sd->size, v->system_start_code,
              v->num_windows);
  if (v->num_windows >= 1) {
    const AVHDRVividColorTransformParams &p = v->params[0];
    std::printf("    maximum_maxrgb=%.4f average_maxrgb=%.4f tone_mapping_mode=%d\n", av_q2d(p.maximum_maxrgb),
                av_q2d(p.average_maxrgb), p.tone_mapping_mode_flag);
  } else {
    std::printf("    -> no processing window; the CUVA curve cannot be solved from this frame\n");
  }
}

void PrintDoviSummary(const AVFrame *frame) {
  const AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA);
  if (!sd || sd->size < static_cast<int>(sizeof(AVDOVIMetadata))) {
    std::printf("  dovi side data: absent\n");
    return;
  }
  const auto *md = reinterpret_cast<const AVDOVIMetadata *>(sd->data);
  const AVDOVIRpuDataHeader *header = av_dovi_get_header(md);
  const AVDOVIDmData *l1 = av_dovi_find_level(md, 1);
  std::printf("  dovi side data: %zu bytes, rpu_profile=%u bl_bit_depth=%u disable_residual=%u\n", sd->size,
              header->vdr_rpu_profile, header->bl_bit_depth, header->disable_residual_flag);
  if (l1) {
    std::printf("    L1 min_pq=%u max_pq=%u avg_pq=%u\n", l1->l1.min_pq, l1->l1.max_pq, l1->l1.avg_pq);
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <file> [--hdrvivid] [--frame N]\n", argv[0]);
    return 2;
  }
  const std::string path = argv[1];
  bool vivid = false;
  int wanted = 0;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--hdrvivid") == 0) {
      vivid = true;
    } else if (std::strcmp(argv[i], "--frame") == 0 && i + 1 < argc) {
      wanted = std::atoi(argv[++i]);
    }
  }

  AVFormatContext *fmt = nullptr;
  AVStream *stream = nullptr;
  size_t vivid_bytes = 0;
  size_t dovi_bytes = 0;
  auto frame = DecodeFrame(path, wanted, &fmt, &stream, &vivid_bytes, &dovi_bytes);

  std::printf("file=%s\n", path.c_str());
  std::printf("  codec=%s %dx%d pix_fmt=%s\n", avcodec_get_name(stream->codecpar->codec_id), frame->width,
              frame->height, av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)));
  std::printf("  colorspace=%d primaries=%d trc=%d range=%d\n", frame->colorspace, frame->color_primaries,
              frame->color_trc, frame->color_range);
  PrintVividSummary(frame.get());
  PrintDoviSummary(frame.get());

  EYEQ::RenderContext context(EYEQ::LoggingLevel::INFO, nullptr, nullptr, 0);
  context.CreateHeadlessDevice();
  if (!context.gpu()) {
    std::fprintf(stderr, "FATAL: no headless Vulkan device\n");
    return 1;
  }

  EYEQ::FrameRenderer::Config cfg;
  cfg.mode = EYEQ::DisplayMode::Fill;
  cfg.enable_osd = false;
  cfg.present_dither = false;
  EYEQ::FrameRenderer renderer(context, cfg, nullptr);
  EYEQ::MappingTarget target;
  target.primaries = PL_COLOR_PRIM_BT_2020;
  target.peak_nits = 1000.0f;
  target.black_nits = 0.0f;
  target.sdr_white_nits = PL_COLOR_SDR_WHITE;
  renderer.SetMappingTarget(target, EYEQ::WorkPrecision::F32);
  renderer.SetHdrVividEnabled(vivid);
  renderer.SetValidateLinearResult(true);

  std::map<int, std::shared_ptr<AVFrame>> frames{{0, frame}};
  renderer.FeedFrames(frames);
  std::printf("  hdrvivid_frame_count=%llu\n", static_cast<unsigned long long>(renderer.hdrvivid_frame_count()));

  const struct pl_frame *src = renderer.GetSourceFrame(0);
  if (src) {
    std::printf("  mapped source: repr.sys=%d dovi=%s color(prim=%d trc=%d max_luma=%.1f)\n", src->repr.sys,
                src->repr.dovi ? "yes" : "no", src->color.primaries, src->color.transfer, src->color.hdr.max_luma);
  }

  EYEQ::DisplayState state{};
  state.ids = {0};
  state.scale = 1.0f;
  state.fill.id = 0;
  state.slide.left_id = 0;
  state.slide.right_id = 0;
  state.grid.cols = 1;
  state.grid.rows = 1;
  renderer.RenderScene(frame->width, frame->height, state);

  const struct pl_frame &result = renderer.GetLinearResult();
  std::printf("  common result: prim=%d trc=%d max_luma=%.1f min_luma=%.4f max_pq_y=%.4f avg_pq_y=%.4f dovi=%s\n",
              result.color.primaries, result.color.transfer, result.color.hdr.max_luma, result.color.hdr.min_luma,
              result.color.hdr.max_pq_y, result.color.hdr.avg_pq_y, result.repr.dovi ? "yes" : "no");
  std::printf("  mapping_pass_count=%llu hdrvivid_apply_count=%llu\n",
              static_cast<unsigned long long>(renderer.MappingPassCount(0)),
              static_cast<unsigned long long>(renderer.HdrVividApplyCount()));

  pl_tex tex = renderer.GetLinearResultTexture();
  std::vector<float> pixels(static_cast<size_t>(tex->params.w) * tex->params.h * 4);
  struct pl_tex_transfer_params transfer = {
      .tex = tex, .row_pitch = static_cast<size_t>(tex->params.w) * 4 * sizeof(float), .ptr = pixels.data()};
  if (!pl_tex_download(context.gpu(), &transfer)) {
    std::fprintf(stderr, "download failed\n");
    return 1;
  }
  float lo = 1e30f, hi = -1e30f;
  double sum = 0.0;
  size_t count = 0;
  bool finite = true;
  for (size_t i = 0; i < pixels.size(); i += 4) {
    const float v = pixels[i];
    finite = finite && std::isfinite(v);
    lo = std::min(lo, v);
    hi = std::max(hi, v);
    sum += v;
    ++count;
  }
  std::printf("  display-linear R: min=%.4f max=%.4f mean=%.4f finite=%s (PL_HDR_NORM, 1.0=203 nits)\n", lo, hi,
              sum / std::max<size_t>(count, 1), finite ? "yes" : "no");

  avformat_close_input(&fmt);
  return finite ? 0 : 1;
}
