// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

// Source engine content, ported from the Python tool with byte-identical output:
//
//   .mdl (IDST v44-49) + .vvd + .vtx   core/source_mdl.py: bones, LOD 0 of each bodypart's first
//                                       model, family-0 skins through .vmt/.vtf, the sequences
//                                       stored in the .mdl, helper bones for blended weights
//   .bsp (VBSP)                         core/source_map.py: the map rebuilt as BSP30

#pragma once

#include <cstdint>
#include <filesystem>
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
}  // namespace GxBake
