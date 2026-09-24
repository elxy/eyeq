#pragma once

#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace eyeq_test {

inline void GpuTestLogCallback(void *, pl_log_level level, const char *message) {
  if (level <= PL_LOG_WARN) {
    std::fprintf(stderr, "libplacebo: %s\n", message);
  }
}

// Owns a real Vulkan pl_gpu. Construction throws a clear std::runtime_error when no
// hardware device is available so an explicitly enabled GPU test fails loudly
// instead of being skipped.
class GpuDevice {
public:
  GpuDevice() {
    pl_log_params log_params{};
    log_params.log_cb = GpuTestLogCallback;
    log_params.log_level = PL_LOG_WARN;
    log_ = pl_log_create(PL_API_VER, &log_params);
    if (log_ == nullptr) {
      throw std::runtime_error("pl_log_create failed");
    }
    pl_vulkan_params vulkan_params{};
    vulkan_params.allow_software = false;
    vulkan_ = pl_vulkan_create(log_, &vulkan_params);
    if (vulkan_ == nullptr) {
      pl_log_destroy(&log_);
      throw std::runtime_error("no Vulkan hardware device available (allow_software=false)");
    }
    gpu_ = vulkan_->gpu;
  }

  ~GpuDevice() {
    if (vulkan_ != nullptr) {
      pl_vulkan_destroy(&vulkan_);
    }
    if (log_ != nullptr) {
      pl_log_destroy(&log_);
    }
  }

  GpuDevice(const GpuDevice &) = delete;
  GpuDevice &operator=(const GpuDevice &) = delete;

  pl_gpu gpu() const { return gpu_; }

private:
  pl_log log_ = nullptr;
  pl_vulkan vulkan_ = nullptr;
  pl_gpu gpu_ = nullptr;
};

// IEEE-754 binary16 conversion. The 16F test uploads half data and builds its
// reference input from HalfToFloat(FloatToHalf(v)), so the two sides always agree on
// the represented value regardless of rounding.
inline uint16_t FloatToHalf(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint32_t sign = bits >> 31;
  const uint32_t exponent = (bits >> 23) & 0xFFu;
  uint32_t mantissa = bits & 0x7FFFFFu;
  uint16_t result = 0;
  if (exponent == 0xFFu) {
    result = static_cast<uint16_t>(0x7C00u | (mantissa != 0 ? 0x200u : 0u));
  } else {
    const int32_t new_exponent = static_cast<int32_t>(exponent) - 127 + 15;
    if (new_exponent >= 31) {
      result = 0x7C00u;
    } else if (new_exponent <= 0) {
      if (new_exponent < -10) {
        result = 0;
      } else {
        mantissa = (mantissa | 0x800000u) >> (1 - new_exponent);
        result = static_cast<uint16_t>(mantissa >> 13);
        const uint32_t remainder = mantissa & 0x1FFFu;
        if (remainder > 0x1000u || (remainder == 0x1000u && (mantissa & 0x2000u) != 0)) {
          result = static_cast<uint16_t>(result + 1);
        }
      }
    } else {
      result = static_cast<uint16_t>((static_cast<uint32_t>(new_exponent) << 10) | (mantissa >> 13));
      const uint32_t remainder = mantissa & 0x1FFFu;
      if (remainder > 0x1000u || (remainder == 0x1000u && (mantissa & 0x2000u) != 0)) {
        result = static_cast<uint16_t>(result + 1);
      }
    }
  }
  return static_cast<uint16_t>((sign << 15) | result);
}

inline float HalfToFloat(uint16_t half) {
  const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16;
  uint32_t exponent = (half >> 10) & 0x1Fu;
  uint32_t mantissa = half & 0x3FFu;
  uint32_t bits = 0;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      exponent = 127 - 15 + 1;
      while ((mantissa & 0x400u) == 0) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x3FFu;
      bits = sign | (exponent << 23) | (mantissa << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
  }
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

} // namespace eyeq_test
