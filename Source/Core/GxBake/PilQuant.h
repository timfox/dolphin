// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace GxBake
{
// Pillow's Image.quantize(colors, method=Quantize.MEDIANCUT, kmeans=0) on an RGB image of
// |count| pixels: one palette index per pixel and the palette (3 bytes per entry, as many
// entries as the quantizer produced, at most |colors|).
bool MedianCutQuantize(const uint8_t* rgb, size_t count, int colors, std::vector<uint8_t>* indices,
                       std::vector<uint8_t>* palette);
}  // namespace GxBake
