// Real-GPU packing tests: they create a Vulkan libplacebo device, run the production
// FramePacker, and compare the read-back bytes against the independent CPU reference
// in pack_reference.hpp. No DeckLink device or SDK is involved.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <stdexcept>
#include <string>
#include <vector>

#include "gpu_test_device.hpp"
#include "packer_test_support.hpp"

using namespace EYEQ;
using namespace eyeq_test;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                   \
      std::abort();                                                                                                    \
    }                                                                                                                  \
  } while (0)

namespace {

void DescribeStats(const char *label, const CompareStats &stats) {
  std::printf("%s: exact=%lld within_one=%lld (boundary=%lld) absent=%lld mismatch=%lld\n", label, stats.exact,
              stats.within_one, stats.boundary, stats.absent_checked, stats.mismatch);
}

void RequireNoMismatch(const char *label, const CompareStats &stats) {
  DescribeStats(label, stats);
  if (stats.mismatch != 0) {
    throw std::runtime_error(std::string("packed bytes mismatch in ") + label);
  }
}

void VerifyFrame(GpuDevice &device, int width, int height, PixelFormat format, const ColorEncoding &encoding,
                 const std::vector<float> &rgb, bool half_precision, int extra_row_bytes, const char *label) {
  pl_tex source = UploadSource(device.gpu(), width, height, rgb, half_precision);
  const PackConfig config = MakeConfig(width, height, format, encoding, extra_row_bytes);
  FramePacker packer(device.gpu(), config);
  const std::vector<uint8_t> actual = RunOneFrame(packer, source, 100, 20000);
  CHECK(actual.size() == config.layout.image_bytes);
  RequireNoMismatch(
      label, CompareFrame(actual.data(), width, height, config.layout.row_bytes, rgb, format, encoding.primaries));
  CHECK(packer.Shutdown(5000) == PackDrain::Idle);
  pl_tex_destroy(device.gpu(), &source);
}

// Every format/encoding/precision combination on a small odd-width image with a
// 16-byte stride pad.
void TestSmallAllCombinations(GpuDevice &device) {
  const int width = 13;
  const int height = 5;
  for (bool half : {false, true}) {
    for (PixelFormat format : {PixelFormat::R210, PixelFormat::V210}) {
      for (int variant = 0; variant < 2; ++variant) {
        const ColorEncoding encoding = variant == 0 ? Sdr709() : Hdr2020();
        const std::vector<float> rgb = MakePattern(width, height, static_cast<uint32_t>(variant * 7 + 1), half);
        const std::string label =
            std::string(half ? "16F" : "32F") + "/" + ToString(format) + "/" + (variant == 0 ? "bt709" : "bt2020");
        VerifyFrame(device, width, height, format, encoding, rgb, half, 16, label.c_str());
      }
    }
  }
}

// Widths around the 6-pixel v210 group and the 48/64 boundaries, with +0/+4/+8 stride
// pads (not only +16).
void TestWidthsAndStrides(GpuDevice &device) {
  const int heights[] = {1, 3};
  for (int width : {1, 5, 6, 7, 8, 47, 48, 49, 63, 64, 65}) {
    for (PixelFormat format : {PixelFormat::R210, PixelFormat::V210}) {
      for (int extra : {0, 4, 8}) {
        for (int height : heights) {
          const std::vector<float> rgb = MakePattern(width, height, static_cast<uint32_t>(width + extra), false);
          const std::string label = std::string("w") + std::to_string(width) + "/" + ToString(format) + "/+" +
                                    std::to_string(extra) + "/h" + std::to_string(height);
          VerifyFrame(device, width, height, format, Sdr709(), rgb, false, extra, label.c_str());
        }
      }
    }
  }
}

// Finite values outside [0,1] are legalized by clamp on both sides.
void TestFiniteClampingOnGpu(GpuDevice &device) {
  const int width = 17;
  const int height = 2;
  std::vector<float> rgb(static_cast<std::size_t>(width) * height * 3);
  for (std::size_t i = 0; i < rgb.size(); ++i) {
    const float t = static_cast<float>(i % 31) / 31.0f;
    rgb[i] = t * 2.0f - 0.5f; // spans [-0.5, 1.5)
  }
  for (PixelFormat format : {PixelFormat::R210, PixelFormat::V210}) {
    VerifyFrame(device, width, height, format, Sdr709(), rgb, false, 0, "clamp-32f");
    VerifyFrame(device, width, height, format, Hdr2020(), rgb, false, 0, "clamp-32f-2020");
  }
}

// Grayscale steps spanning the legal luma codes, plus the primary/complement anchors.
void TestGrayAndPrimaryAnchors(GpuDevice &device) {
  const int width = 33;
  const int height = 1;
  std::vector<float> rgb(static_cast<std::size_t>(width) * 3);
  for (int x = 0; x < width; ++x) {
    const int code = 64 + (x * (940 - 64)) / (width - 1);
    const float gray = static_cast<float>((code - 64) / 876.0);
    rgb[x * 3 + 0] = gray;
    rgb[x * 3 + 1] = gray;
    rgb[x * 3 + 2] = gray;
  }
  VerifyFrame(device, width, height, PixelFormat::R210, Sdr709(), rgb, false, 0, "gray-steps-r210");
  VerifyFrame(device, width, height, PixelFormat::V210, Sdr709(), rgb, false, 0, "gray-steps-v210");
  VerifyFrame(device, width, height, PixelFormat::V210, Hdr2020(), rgb, false, 0, "gray-steps-v210-2020");

  const float primaries[8][3] = {{1, 1, 1}, {0, 0, 0}, {1, 0, 0}, {0, 1, 0},
                                 {0, 0, 1}, {1, 1, 0}, {0, 1, 1}, {1, 0, 1}};
  std::vector<float> anchors(8 * 3);
  for (int i = 0; i < 8; ++i) {
    anchors[i * 3 + 0] = primaries[i][0];
    anchors[i * 3 + 1] = primaries[i][1];
    anchors[i * 3 + 2] = primaries[i][2];
  }
  VerifyFrame(device, 8, 1, PixelFormat::R210, Sdr709(), anchors, false, 0, "primary-anchors-r210");
  VerifyFrame(device, 8, 1, PixelFormat::V210, Sdr709(), anchors, false, 0, "primary-anchors-v210");
  VerifyFrame(device, 8, 1, PixelFormat::V210, Hdr2020(), anchors, false, 0, "primary-anchors-v210-2020");
}

// Single-pixel chroma impulses at every column, so a dropped odd column or a
// half-pixel phase shift cannot hide behind the numeric tolerance.
void TestChromaPhaseImpulses(GpuDevice &device) {
  const int width = 9;
  for (PixelFormat format : {PixelFormat::R210, PixelFormat::V210}) {
    for (int impulse = 0; impulse < width; ++impulse) {
      std::vector<float> rgb(static_cast<std::size_t>(width) * 3, 0.0f);
      rgb[impulse * 3 + 0] = 1.0f;
      const std::string label = std::string("impulse-") + ToString(format) + "-x" + std::to_string(impulse);
      VerifyFrame(device, width, 1, format, Sdr709(), rgb, false, 0, label.c_str());
    }
  }
}

// Several frames through the same packer (shader reuse).
void TestMultipleFrames(GpuDevice &device) {
  const int width = 37;
  const int height = 4;
  const ColorEncoding encoding = Sdr709();
  for (PixelFormat format : {PixelFormat::R210, PixelFormat::V210}) {
    const PackConfig config = MakeConfig(width, height, format, encoding, 0);
    FramePacker packer(device.gpu(), config);
    float sum = 0.0f;
    for (uint32_t seed = 1; seed <= 5; ++seed) {
      const std::vector<float> rgb = MakePattern(width, height, seed, false);
      pl_tex source = UploadSource(device.gpu(), width, height, rgb, false);
      const std::vector<uint8_t> actual = RunOneFrame(packer, source, seed, 20000);
      RequireNoMismatch("multi-frame", CompareFrame(actual.data(), width, height, config.layout.row_bytes, rgb, format,
                                                    encoding.primaries));
      sum += static_cast<float>(actual[0]);
      pl_tex_destroy(device.gpu(), &source);
    }
    CHECK(sum > 0.0f);
    CHECK(packer.Shutdown(5000) == PackDrain::Idle);
  }
}

// The two encodings must produce different chroma for a saturated pattern.
void TestEncodingsDiffer(GpuDevice &device) {
  const int width = 17;
  const int height = 3;
  const std::vector<float> rgb = MakePattern(width, height, 99, false);
  pl_tex source = UploadSource(device.gpu(), width, height, rgb, false);
  std::vector<uint8_t> sdr_bytes;
  std::vector<uint8_t> hdr_bytes;
  for (int variant = 0; variant < 2; ++variant) {
    const ColorEncoding encoding = variant == 0 ? Sdr709() : Hdr2020();
    const PackConfig config = MakeConfig(width, height, PixelFormat::V210, encoding, 0);
    FramePacker packer(device.gpu(), config);
    std::vector<uint8_t> actual = RunOneFrame(packer, source, 5, 20000);
    if (variant == 0) {
      sdr_bytes = std::move(actual);
    } else {
      hdr_bytes = std::move(actual);
    }
    CHECK(packer.Shutdown(5000) == PackDrain::Idle);
  }
  CHECK(sdr_bytes != hdr_bytes);
  pl_tex_destroy(device.gpu(), &source);
}

// Full-frame UHD verification of the first and last frames of a short run.
void TestUhdFirstAndLast(GpuDevice &device) {
  const int width = 3840;
  const int height = 2160;
  constexpr int kFrames = 3;
  const ColorEncoding encoding = Sdr709();
  for (PixelFormat format : {PixelFormat::R210, PixelFormat::V210}) {
    const PackConfig config = MakeConfig(width, height, format, encoding, 0);
    FramePacker packer(device.gpu(), config);
    for (int frame = 0; frame < kFrames; ++frame) {
      const std::vector<float> rgb = MakePattern(width, height, static_cast<uint32_t>(frame + 1), true);
      pl_tex source = UploadSource(device.gpu(), width, height, rgb, true);
      const auto begin = Clock::now();
      const std::vector<uint8_t> actual = RunOneFrame(packer, source, static_cast<uint64_t>(frame), 60000);
      const double elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
      if (frame == 0 || frame == kFrames - 1) {
        const CompareStats stats =
            CompareFrame(actual.data(), width, height, config.layout.row_bytes, rgb, format, encoding.primaries);
        std::printf("UHD %s frame %d submit+readback=%.2f ms\n", ToString(format), frame, elapsed_ms);
        RequireNoMismatch("uhd", stats);
      }
      pl_tex_destroy(device.gpu(), &source);
    }
    CHECK(packer.Shutdown(10000) == PackDrain::Idle);
  }
}

// Short four-slot UHD benchmark. Warm-up (shader compile, first allocations) is
// excluded. Latency starts before Submit, and the success/failure and Shutdown results
// are checked. Reported bytes are the mapped host buffers and the logical carrier
// payload separately; driver physical residency is not measured.
void TestUhdShortBench(GpuDevice &device) {
  const int width = 3840;
  const int height = 2160;
  constexpr int kFrames = 16;
  constexpr int kRing = 2;
  const std::vector<float> rgb = MakePattern(width, height, 7, true);
  for (PixelFormat format : {PixelFormat::R210, PixelFormat::V210}) {
    const PackConfig config = MakeConfig(width, height, format, Sdr709(), 0);
    FramePacker packer(device.gpu(), config);
    pl_tex source = UploadSource(device.gpu(), width, height, rgb, true);
    for (int i = 0; i < 3; ++i) {
      (void)RunOneFrame(packer, source, static_cast<uint64_t>(i), 60000);
    }

    struct Pending {
      StagingSlot *slot = nullptr;
      Clock::time_point submitted;
    };
    std::deque<Pending> pending;
    std::vector<double> latencies;
    double submit_total_ms = 0.0;
    const auto begin = Clock::now();
    auto retire_oldest = [&]() {
      Pending oldest = pending.front();
      pending.pop_front();
      const auto deadline = Clock::now() + std::chrono::milliseconds(60000);
      PackPoll poll = PackPoll::NotReady;
      while (Clock::now() < deadline) {
        poll = packer.Poll(oldest.slot);
        if (poll != PackPoll::NotReady) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
      if (poll != PackPoll::Ready) {
        packer.Abort(oldest.slot);
        throw std::runtime_error("bench frame did not reach Ready within the wall-time bound");
      }
      latencies.push_back(std::chrono::duration<double, std::milli>(Clock::now() - oldest.submitted).count());
      packer.Reap(oldest.slot);
    };
    for (int i = 0; i < kFrames; ++i) {
      if (static_cast<int>(pending.size()) == kRing) {
        retire_oldest();
      }
      const auto submit_begin = Clock::now(); // start before Submit
      StagingSlot *slot = packer.Submit(source, static_cast<uint64_t>(i));
      if (slot == nullptr) {
        throw std::runtime_error("bench Submit returned nullptr");
      }
      submit_total_ms += std::chrono::duration<double, std::milli>(Clock::now() - submit_begin).count();
      pending.push_back(Pending{slot, submit_begin});
    }
    while (!pending.empty()) {
      retire_oldest();
    }
    const double wall_ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    std::sort(latencies.begin(), latencies.end());
    const double p50 = latencies[latencies.size() / 2];
    const double p95 = latencies[latencies.size() * 95 / 100];
    const std::size_t mapped_bytes = config.layout.image_bytes * kRing;
    const std::size_t carrier_bytes =
        static_cast<std::size_t>(config.layout.row_bytes / 4) * static_cast<std::size_t>(height) * 4 * kRing;
    std::printf("BENCH %s frames=%d wall=%.1fms amortized=%.3fms/frame submit_avg=%.3fms "
                "submit_to_ready_p50=%.3fms p95=%.3fms bytes_per_frame=%zu mapped_host_bytes=%zu "
                "carrier_payload_bytes=%zu\n",
                ToString(format), kFrames, wall_ms, wall_ms / kFrames, submit_total_ms / kFrames, p50, p95,
                config.layout.image_bytes, mapped_bytes, carrier_bytes);
    if (packer.Shutdown(10000) != PackDrain::Idle) {
      throw std::runtime_error("bench Shutdown did not reach Idle");
    }
    pl_tex_destroy(device.gpu(), &source);
  }
}

} // namespace

int main() {
  try {
    GpuDevice device;
    std::printf("frame_packer_gpu: device acquired (buf_transfer=%d)\n", device.gpu()->limits.buf_transfer);
    TestSmallAllCombinations(device);
    TestWidthsAndStrides(device);
    TestFiniteClampingOnGpu(device);
    TestGrayAndPrimaryAnchors(device);
    TestChromaPhaseImpulses(device);
    TestMultipleFrames(device);
    TestEncodingsDiffer(device);
    TestUhdFirstAndLast(device);
    TestUhdShortBench(device);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "frame_packer_gpu FAILED: %s\n", error.what());
    return 1;
  }
  std::printf("frame_packer_gpu: OK\n");
  return 0;
}
