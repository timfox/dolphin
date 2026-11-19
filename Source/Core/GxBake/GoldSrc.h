// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

// GoldSrc (Half-Life) runtime files, ported from the Python tool so they bake without it:
//
//   .spr  Half-Life (v2) sprites: lean HUD downsample   (core/sprite.py lean_hl_spr_bytes_from)
//   .wad  WAD3 texture pack: every miptex, qpic and font (core/gct.py build_wad_texture_pack)
//
// Outputs are byte-identical to the Python tool's, and the CMPR cache uses the same keys and file
// layout (gct.py wt_encode_cached), so a cache folder can be shared with it. Quake sprites and
// WAD2 files need Quake's palette and are left to the Python tool.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace GxBake
{
// Nearest-neighbour downsample of a single-frame Half-Life sprite by |scale|. For alpha-tested
// sprites, any non-transparent texel in a block is kept so thin crosshair strokes survive. Empty
// when the sprite can't be leaned (not v2, several frames, or a size not divisible by |scale|).
std::optional<std::vector<uint8_t>> LeanHlSprite(const std::vector<uint8_t>& data, int scale = 2);

// True when the Python tool must convert this sprite first (Quake/Quake II sprites).
bool SpriteNeedsPython(const std::vector<uint8_t>& data);

// True for a WAD3 file (WAD2 needs Quake's palette and goes to the Python tool).
bool IsWad3(const std::vector<uint8_t>& data);

// A GCWT pack of every miptex, picture and font in a WAD3 file, or empty when it has none.
// |cache_dir| (may be empty) holds encoded CMPR chains, shared with the Python tool's --cache.
std::optional<std::vector<uint8_t>> BuildWadTexturePack(const std::vector<uint8_t>& wad,
                                                        const std::string& cache_dir);
}  // namespace GxBake
