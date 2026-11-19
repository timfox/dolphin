// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

// Quake and Quake II files, ported from the Python tool (core/quake.py) with byte-identical
// output:
//
//   gfx/palette.lmp   Quake's palette, loose or inside a .pak near the content
//   .spr (IDSP v1)    Quake sprites, given Half-Life's header and Quake's palette
//   .sp2 (IDS2)       Quake II sprites, each frame read from its PCX
//   .mdl (IDPO v6)    Quake alias models, first frame and first skin (see Studio.h)
//
// WAD2 files and v29 maps go through GoldSrc.h with the palette found here.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GxBake
{
struct ImportedModel;

// archives.read_member for a Quake PACK file: one file by case-insensitive name.
std::optional<std::vector<uint8_t>> ReadPakMember(const std::filesystem::path& pak,
                                                  std::string_view name);

// quake._find_in_tree: |rel| loose under |start|'s folder or up to five of its parents (or their
// |subdirs|), else inside a .pak there. |found| (may be null) receives the file or .pak read.
std::optional<std::vector<uint8_t>> FindInTree(const std::filesystem::path& start,
                                               std::string_view rel,
                                               const std::vector<std::string>& subdirs,
                                               std::filesystem::path* found = nullptr);

// quake.quake_palette: gfx/palette.lmp (768 bytes) near |start|, or empty.
std::optional<std::vector<uint8_t>> QuakePalette(const std::filesystem::path& start,
                                                 std::filesystem::path* found = nullptr);

// quake.GREY_PALETTE, used (with a warning) when Quake's palette can't be found.
const std::vector<uint8_t>& GreyPalette();

bool IsQuakeSprite(const std::vector<uint8_t>& data);   // IDSP v1
bool IsQuake2Sprite(const std::vector<uint8_t>& data);  // IDS2

// quake.quake_spr_to_hl: the same frames with Half-Life's v2 header and |palette|.
std::optional<std::vector<uint8_t>> QuakeSpriteToHl(const std::vector<uint8_t>& data,
                                                    const std::vector<uint8_t>& palette);

// quake.quake2_sp2_to_hl: each frame's PCX (found near |path|) as a v2 frame. On failure, the
// reason goes to |error|. |deps| (may be null) receives every file read.
std::optional<std::vector<uint8_t>> Quake2SpriteToHl(const std::filesystem::path& path,
                                                     const std::vector<uint8_t>& data,
                                                     std::string* error,
                                                     std::vector<std::filesystem::path>* deps);

bool IsAliasModel(const std::vector<uint8_t>& data);  // IDPO v6

// quake.import_alias: the first frame and first skin of a Quake alias model, coloured by
// |palette|. |name| is the file stem. |skin_max_edge| is ModelOptions.skin_max_edge.
std::optional<ImportedModel> ImportAliasModel(const std::vector<uint8_t>& data,
                                              const std::string& name,
                                              const std::vector<uint8_t>& palette,
                                              bool palette_found, int skin_max_edge = 256);
}  // namespace GxBake
