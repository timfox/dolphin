// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

// GoldSrc (Half-Life) runtime files, ported from the Python tool so they bake without it:
//
//   .spr  Half-Life (v2) sprites: lean HUD downsample   (core/sprite.py lean_hl_spr_bytes_from)
//   .wad  WAD3 texture pack: every miptex, qpic and font (core/gct.py build_wad_texture_pack)
//   .bsp  BSP v30 world texture pack, from the map and the WADs it uses
//         (core/gct.py build_world_texture_pack, collect_wads)
//   .mdl  GoldSrc studio model: lean mesh and downsampled skin companion (core/studio.py)
//
// Outputs are byte-identical to the Python tool's, and the CMPR cache uses the same keys and file
// layout (gct.py wt_encode_cached), so a cache folder can be shared with it. Quake WAD2 files
// and v29 maps go through here too, with Quake's palette (Quake.h).

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace GxBake
{
// Nearest-neighbour downsample of a single-frame Half-Life sprite by |scale|. For alpha-tested
// sprites, any non-transparent texel in a block is kept so thin crosshair strokes survive. Empty
// when the sprite can't be leaned (not v2, several frames, or a size not divisible by |scale|).
std::optional<std::vector<uint8_t>> LeanHlSprite(const std::vector<uint8_t>& data, int scale = 2);

bool IsWad3(const std::vector<uint8_t>& data);
// Quake's WAD2: its miptex have no palette, so they need Quake's (QuakePalette in Quake.h).
bool IsWad2(const std::vector<uint8_t>& data);

// A GCWT pack of every miptex, picture and font in a WAD3 file (or every miptex of a WAD2 file,
// coloured by |quake_palette|), or empty when it has none. |cache_dir| (may be empty) holds
// encoded CMPR chains, shared with the Python tool's --cache.
std::optional<std::vector<uint8_t>> BuildWadTexturePack(const std::vector<uint8_t>& wad,
                                                        const std::string& cache_dir,
                                                        const uint8_t* quake_palette = nullptr);

// gc_studio/<name>.mdl: the model cut before its embedded textures, so it fits the engine's studio
// BSS. Empty when the file isn't a GoldSrc studio model (Quake and Source models are first baked
// into one, see Studio.h).
std::optional<std::vector<uint8_t>> LeanStudioMesh(const std::vector<uint8_t>& mdl);

// gc_studio/<name>t.mdl: the model's skins, downsampled until neither side exceeds |max_edge|.
// Empty when the model has no (readable) embedded skins.
std::optional<std::vector<uint8_t>> StudioSkinCompanion(const std::vector<uint8_t>& mdl,
                                                        int max_edge = 64);

// What the baker needs from a GoldSrc (v30) or Quake (v29) map. Empty for Source maps.
struct BspInfo
{
  int version = 30;               // 29: Quake, whose miptex need Quake's palette and no WADs
  std::vector<std::string> wads;  // worldspawn "wad" basenames, in order, without duplicates
  std::string sky;                // worldspawn "skyname", lower-cased, or empty
};
std::optional<BspInfo> ReadBspInfo(const std::vector<uint8_t>& bsp);

// Miptex found in WAD files, first WAD (and first lump) of a name wins (gct.collect_wads).
class WadIndex
{
public:
  // Indexes every uncompressed miptex lump of a WAD2/WAD3 file. Returns how many names were new.
  int Ingest(std::shared_ptr<const std::vector<uint8_t>> wad);

  struct Lump
  {
    std::shared_ptr<const std::vector<uint8_t>> wad;
    int32_t offset;
  };
  const Lump* Find(const std::string& name) const;

private:
  std::map<std::string, Lump> m_lumps;
};

// media/wt_<map>.gct: each world texture of the map (liquids and sky excluded) as CMPR with mips,
// from the map itself or else |wads|. Names found in neither are added to |missing|. Empty when
// the map has no world textures. A Quake (v29) map's miptex are coloured by |quake_palette|.
std::optional<std::vector<uint8_t>> BuildWorldTexturePack(const std::vector<uint8_t>& bsp,
                                                          const WadIndex& wads,
                                                          const std::string& cache_dir,
                                                          std::vector<std::string>* missing,
                                                          const uint8_t* quake_palette = nullptr);
}  // namespace GxBake
