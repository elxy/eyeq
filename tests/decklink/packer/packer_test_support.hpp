#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "gpu_test_device.hpp"
#include "output/frame_packer.hpp"
#include "pack_compare.hpp"
#include "pack_reference.hpp"

namespace eyeq_test {

using EYEQ::FramePacker;
using EYEQ::PackConfig;
using EYEQ::PackPoll;
using EYEQ::StagingSlot;

using Clock = std::chrono::steady_clock;

inline PackConfig MakeConfig(int width, int height, EYEQ::PixelFormat format, const EYEQ::ColorEncoding &encoding,
                             int extra_row_bytes) {
  PackConfig config;
  config.layout.width = width;
  config.layout.height = height;
  config.layout.pixel_format = format;
  config.layout.row_bytes = EYEQ::MinimalRowBytes(format, width) + extra_row_bytes;
  config.layout.image_bytes = static_cast<std::size_t>(config.layout.row_bytes) * static_cast<std::size_t>(height);
  config.output = encoding;
  return config;
}

inline EYEQ::ColorEncoding Sdr709() {
  EYEQ::ColorEncoding encoding;
  encoding.primaries = EYEQ::ColorPrimaries::BT709;
  encoding.transfer = EYEQ::ColorTransfer::SDR_BT1886;
  encoding.range = EYEQ::ColorRange::Video;
  encoding.chroma_loc = EYEQ::ChromaLocation::Left;
  return encoding;
}

inline EYEQ::ColorEncoding Hdr2020() {
  EYEQ::ColorEncoding encoding;
  encoding.primaries = EYEQ::ColorPrimaries::BT2020;
  encoding.transfer = EYEQ::ColorTransfer::PQ;
  encoding.range = EYEQ::ColorRange::Video;
  encoding.chroma_loc = EYEQ::ChromaLocation::Left;
  return encoding;
}

// Deterministic per-pixel RGB pattern with fine spatial variation (so any dropped
// column, half-pixel shift or missing filter would show up), some injected primary
// impulses and an alternating red/blue band. `half_precision` rounds every component
// to the nearest binary16 value so the RGBA16F texture stores exactly the value the
// reference is built from.
inline std::vector<float> MakePattern(int width, int height, uint32_t seed, bool half_precision) {
  const uint32_t scale = half_precision ? 1024u : 1048576u;
  uint32_t state = seed * 2654435761u + 12345u;
  auto next = [&]() -> uint32_t {
    state = state * 1664525u + 1013904223u;
    return (state >> 8) % (scale + 1);
  };
  std::vector<float> rgb(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      float *pixel = rgb.data() + (static_cast<std::size_t>(y) * width + x) * 3;
      pixel[0] = static_cast<float>(next()) / static_cast<float>(scale);
      pixel[1] = static_cast<float>(next()) / static_cast<float>(scale);
      pixel[2] = static_cast<float>(next()) / static_cast<float>(scale);
    }
  }
  auto set = [&](int x, int y, float r, float g, float b) {
    if (x >= width || y >= height) {
      return;
    }
    float *pixel = rgb.data() + (static_cast<std::size_t>(y) * width + x) * 3;
    pixel[0] = r;
    pixel[1] = g;
    pixel[2] = b;
  };
  set(0, 0, 1.0f, 0.0f, 0.0f);
  set(1, 0, 0.0f, 0.0f, 1.0f);
  set(2, 0, 1.0f, 1.0f, 1.0f);
  set(3, 0, 0.0f, 0.0f, 0.0f);
  if (width > 7 && height > 1) {
    for (int x = 4; x < std::min(width, 12); ++x) {
      if (x % 2 == 0) {
        set(x, 1, 1.0f, 0.0f, 0.0f);
      } else {
        set(x, 1, 0.0f, 0.0f, 1.0f);
      }
    }
  }
  if (half_precision) {
    for (float &value : rgb) {
      value = HalfToFloat(FloatToHalf(value));
    }
  }
  return rgb;
}

// Uploads a row-major RGB pattern as a sampleable float texture. The host layout is
// chosen from the format's host_bits: RGBA32F/RGBA16F use 32-bit host floats (the
// driver stores 16F internally for rgba16f), while a 16-bit-host format uses halves.
inline pl_tex UploadSource(pl_gpu gpu, int width, int height, const std::vector<float> &rgb, bool half_precision) {
  pl_fmt format = pl_find_named_fmt(gpu, half_precision ? "rgba16f" : "rgba32f");
  if (format == nullptr) {
    throw std::runtime_error(std::string("required input format missing: ") + (half_precision ? "rgba16f" : "rgba32f"));
  }
  pl_tex_params params{};
  params.w = width;
  params.h = height;
  params.format = format;
  params.sampleable = true;
  params.host_writable = true;
  pl_tex texture = pl_tex_create(gpu, &params);
  if (texture == nullptr) {
    throw std::runtime_error("source texture creation failed");
  }

  const std::size_t texels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  pl_tex_transfer_params transfer{};
  transfer.tex = texture;
  if (format->host_bits[0] >= 32) {
    std::vector<float> data(texels * 4);
    for (std::size_t i = 0; i < texels; ++i) {
      data[i * 4 + 0] = rgb[i * 3 + 0];
      data[i * 4 + 1] = rgb[i * 3 + 1];
      data[i * 4 + 2] = rgb[i * 3 + 2];
      data[i * 4 + 3] = 1.0f;
    }
    transfer.ptr = data.data();
    transfer.row_pitch = static_cast<std::size_t>(width) * 4 * sizeof(float);
    if (!pl_tex_upload(gpu, &transfer)) {
      pl_tex_destroy(gpu, &texture);
      throw std::runtime_error("float32 source upload failed");
    }
  } else {
    std::vector<uint16_t> data(texels * 4);
    for (std::size_t i = 0; i < texels; ++i) {
      data[i * 4 + 0] = FloatToHalf(rgb[i * 3 + 0]);
      data[i * 4 + 1] = FloatToHalf(rgb[i * 3 + 1]);
      data[i * 4 + 2] = FloatToHalf(rgb[i * 3 + 2]);
      data[i * 4 + 3] = FloatToHalf(1.0f);
    }
    transfer.ptr = data.data();
    transfer.row_pitch = static_cast<std::size_t>(width) * 4 * sizeof(uint16_t);
    if (!pl_tex_upload(gpu, &transfer)) {
      pl_tex_destroy(gpu, &texture);
      throw std::runtime_error("float16 source upload failed");
    }
  }
  return texture;
}

// Submits one frame and polls it to Ready, copying image_bytes out before Reap. A
// Failed slot is aborted (retaining it until its GPU work is done) so a failure is
// never treated as success.
inline std::vector<uint8_t> RunOneFrame(FramePacker &packer, pl_tex src, uint64_t frame_id, int timeout_ms) {
  StagingSlot *slot = packer.Submit(src, frame_id);
  if (slot == nullptr) {
    throw std::runtime_error("Submit returned nullptr while a slot was expected");
  }
  const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));
  for (;;) {
    const PackPoll poll = packer.Poll(slot);
    if (poll == PackPoll::Ready) {
      break;
    }
    if (poll == PackPoll::Failed) {
      packer.Abort(slot);
      throw std::runtime_error("packer slot failed during readback");
    }
    if (Clock::now() >= deadline) {
      packer.Abort(slot);
      packer.Shutdown(1000);
      throw std::runtime_error("packer readback timed out");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const uint8_t *bytes = packer.Bytes(slot);
  const std::size_t size = packer.Size(slot);
  if (bytes == nullptr || size != packer.config().layout.image_bytes) {
    packer.Reap(slot);
    throw std::runtime_error("Ready slot did not expose the expected bytes");
  }
  std::vector<uint8_t> copy(bytes, bytes + size);
  packer.Reap(slot);
  return copy;
}

} // namespace eyeq_test
