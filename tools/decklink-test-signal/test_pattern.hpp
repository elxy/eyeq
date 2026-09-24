#pragma once

#include <cstdint>

#include "output/pixel_format.hpp"

namespace EYEQ {

// Deterministic CPU test-signal generator for the standalone DeckLink
// test-signal tool. This is a signal generator, not a color-management
// reference: it emits 100% RGB bars and integer code-value steps and performs
// only the RGB -> Y'CbCr 10-bit quantization that filling a v210 frame needs.
// It applies no gamma, transfer function, tone mapping or any other conversion.
//
// The layout's pixel_format selects the wire packing:
//   R210: R10G10B10 words, no color conversion.
//   V210: RGB is quantized to Y'CbCr 10-bit using encoding.primaries and
//         encoding.range, then packed six pixels per four little-endian words.
//
// The row stride is taken from layout.row_bytes; it is never recomputed. The
// same (frame_index, layout, encoding) always produces byte-identical output.
// frame_index only moves the bottom motion/gray-code band, so the upper bars and
// the grayscale staircase stay stable across frames.
void RenderTestPattern(uint64_t frame_index, const BufferLayout &layout, const ColorEncoding &encoding, uint8_t *dst);

} // namespace EYEQ
