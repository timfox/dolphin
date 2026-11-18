// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

// Image resizing that matches Pillow's Image.resize() bit for bit, so the native baker produces
// the same bytes as the Python GxBake tool (which resizes through Pillow).

#pragma once

#include <cstdint>
#include <vector>

namespace GxBake
{
enum class Filter
{
  Nearest,
  Bicubic,
  Lanczos,
};

// Resize an 8-bit image with `channels` interleaved channels (Pillow's "L"/"RGB" modes; RGB is
// resampled per channel exactly like Pillow's 4-byte RGB layout).
std::vector<uint8_t> Resize(const std::vector<uint8_t>& src, int width, int height, int channels,
                            int out_width, int out_height, Filter filter);

// Nearest-neighbour resize of a boolean mask (Pillow mode "1").
std::vector<uint8_t> ResizeMaskNearest(const std::vector<uint8_t>& src, int width, int height,
                                       int out_width, int out_height);
}  // namespace GxBake
