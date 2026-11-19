// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

// Valve Texture Format (.vtf) and material (.vmt) reading for Source content, ported from the
// Python tool (core/vtf.py): the largest mip, first frame and face, decoded to RGBA exactly like
// the Python tool (DXT1/3/5, ATI1N/ATI2N and the uncompressed formats).

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "GxBake/Encode.h"

namespace GxBake
{
// vtf.read_vtf(data): RGBA of mip 0, frame 0, face 0. On failure the reason goes to |error|.
std::optional<RgbaImage> ReadVtf(const std::vector<uint8_t>& data, std::string* error);

// vtf.parse_vmt: (shader, lower-cased key -> value) of a .vmt's top-level block.
std::pair<std::string, std::map<std::string, std::string>> ParseVmt(const std::string& text);

// vtf._ci_file: |root|/|rel| (Latin-1, forward slashes), else matched part by part ignoring case.
std::optional<std::filesystem::path> FindFileNoCase(const std::filesystem::path& root,
                                                    const std::string& rel);

// vtf.find_materials_root: the nearest ancestor's materials/ folder.
std::optional<std::filesystem::path> FindMaterialsRoot(const std::filesystem::path& start);

// vtf.resolve_material: <dir>/<name>.vmt under |root| (case-insensitive) and its $basetexture
// .vtf, else <dir>/<name>.vtf. |read| (may be null) receives each .vmt read.
struct ResolvedMaterial
{
  std::optional<std::filesystem::path> vtf;
  std::map<std::string, std::string> params;
};
ResolvedMaterial ResolveMaterial(const std::string& name, const std::vector<std::string>& dirs,
                                 const std::filesystem::path& root,
                                 std::vector<std::filesystem::path>* read = nullptr);
}  // namespace GxBake
