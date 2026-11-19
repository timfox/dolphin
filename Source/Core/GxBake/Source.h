// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

// Source engine content, ported from the Python tool with byte-identical output:
//
//   .mdl (IDST v44-49) + .vvd + .vtx   core/source_mdl.py: bones, LOD 0 of each bodypart's first
//                                       model, family-0 skins through .vmt/.vtf, the sequences
//                                       stored in the .mdl, helper bones for blended weights
//   .bsp (VBSP v19-21, 25)              core/source_map.py: the map rebuilt as BSP30, its
//                                       textures embedded from the pakfile, game folders or
//                                       VPKs, the sky's six sides read the same way

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "GxBake/Studio.h"

namespace GxBake
{
bool IsSourceModel(const std::vector<uint8_t>& data);  // IDST v44-49

// source_mdl.companion_files: the .vvd and the best .vtx next to |mdl|.
struct SourceModelFiles
{
  std::optional<std::filesystem::path> vvd, vtx;
};
SourceModelFiles SourceModelCompanions(const std::filesystem::path& mdl);

// Every file a Source model's import reads besides the .mdl: its .vvd and .vtx, and the .vmt
// and .vtf files its skins resolve to.
std::vector<std::filesystem::path> SourceModelDependencies(const std::filesystem::path& mdl,
                                                           const std::vector<uint8_t>& data);

struct SourceModelOptions
{
  double scale = 1.0;
  int skin_max_edge = 256;
  int alpha_threshold = 128;
  int anim_fps = 15;
  bool static_model = false;
  bool blend_bones = true;
};

// source_mdl.import_source. On failure the reason goes to |error|.
std::optional<ImportedModel> ImportSourceModel(const std::filesystem::path& path,
                                               const std::vector<uint8_t>& data,
                                               const SourceModelOptions& options,
                                               std::string* error);

bool IsSourceMap(const std::vector<uint8_t>& data);  // VBSP v17-29

struct SourceMapOptions
{
  int max_texture = 256;  // largest embedded miptex edge
  int min_texture = 32;
  double texel_density = 1.0;  // texels per world unit
  int disp_power = 2;          // displacement detail kept (2 = 5x5 vertices)
  double light_gamma = 2.2;
  double light_scale = 1.0;
  bool props = true;     // prop_static -> cycler_sprite entities
  bool overlays = true;  // info_overlay decals become faces
  bool hulls = true;     // clip hulls 1-3
  std::vector<std::filesystem::path> search;  // extra game folders or _dir.vpk files
  int vis_merge = 8;
};

struct SourceMapResult
{
  std::vector<uint8_t> bsp;
  std::vector<std::string> warnings;
  std::string sky;                             // worldspawn's skyname
  std::map<std::string, RgbaImage> sky_sides;  // "rt", "bk", ... that were found
  std::vector<std::string> props;              // prop_static models, sorted
};

// source_map.convert. On failure the reason goes to |error|.
std::optional<SourceMapResult> ConvertSourceMap(const std::filesystem::path& path,
                                                const std::vector<uint8_t>& data,
                                                const SourceMapOptions& options,
                                                std::string* error);

// Files on disk a Source map's conversion reads besides the map: the game folders' material
// files its textures and sky resolve to, and the VPKs it searches.
std::vector<std::filesystem::path> SourceMapDependencies(const std::filesystem::path& path,
                                                         const std::vector<uint8_t>& data);
}  // namespace GxBake
