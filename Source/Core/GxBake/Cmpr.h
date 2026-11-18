// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

// GX CMPR (S3TC/DXT1, big-endian, 8x8 tiles of 2x2 blocks) encoders, ported from GxBake's
// core/cmpr.py and core/gct.py. They follow numpy's float32 arithmetic step by step so the output
// is byte-identical to the Python tool and the Xash3D GameCube disc builder.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace GxBake
{
// gx_cmpr_encode: brightest/darkest endpoints, used for sky sides. rgb is H*W*3, H and W
// multiples of 8.
std::vector<uint8_t> CmprEncodeSimple(const uint8_t* rgb, int width, int height);

// gx_cmpr_encode_blocks: principal-axis endpoints, 1-bit alpha for blocks with transparent
// texels. opaque is H*W (0/1).
std::vector<uint8_t> CmprEncodeBlocks(const uint8_t* rgb, const uint8_t* opaque, int width,
                                      int height);

// encode_mip_chain: a coverage-weighted box-filtered mip chain. Returns the level count.
int EncodeMipChain(const std::vector<uint8_t>& rgb, const std::vector<uint8_t>& opaque, int width,
                   int height, bool mipmaps, int min_edge, std::vector<uint8_t>* out);

// Decode one CMPR level to RGBA (for previews and tests).
std::vector<uint8_t> CmprDecode(const uint8_t* data, size_t size, int width, int height);

size_t CmprSize(int width, int height);
}  // namespace GxBake
