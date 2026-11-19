// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

// The studio model writer the Python tool's model importers share (core/models.py
// build_studio_mdl and core/skeleton.py), ported with byte-identical output. Quake alias models
// (Quake.h) and Source models (Source.h) are imported into an ImportedModel and baked here into a
// GoldSrc studio MDL (v10); GoldSrc.h then cuts it into the lean mesh and the skin companion.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "GxBake/Encode.h"

namespace GxBake
{
using Pose = std::array<double, 6>;  // x, y, z, roll, pitch, yaw relative to the parent

struct StudioMaterial
{
  std::string name;
  RgbaImage rgba;  // power-of-two skin texels
  bool masked = false;
  std::string texture;  // where the texels came from
};

struct StudioSubmesh
{
  int material = 0;
  std::vector<std::array<float, 3>> positions;  // in each vertex's bone space
  std::vector<std::array<float, 3>> normals;
  std::vector<std::array<float, 2>> uvs;  // top-left origin
  std::vector<std::array<uint32_t, 3>> tris;  // counter-clockwise
  std::vector<int32_t> bones;                  // studio bone per vertex (empty: bone 0)
};

struct StudioBone
{
  std::string name;
  int parent = -1;
  Pose pose{};
};

struct StudioSequence
{
  std::string name;
  double fps = 30.0;
  std::vector<std::vector<Pose>> frames;  // [frame][bone]
  bool loop = true;
};

struct ImportedModel
{
  std::string name;
  std::vector<StudioMaterial> materials;
  std::vector<StudioSubmesh> meshes;
  std::vector<std::string> warnings;
  std::vector<StudioBone> bones = {StudioBone{"root", -1, {}}};
  std::vector<StudioSequence> sequences;

  // ImportedModel.rest_frames: one frame of every bone's rest pose.
  std::vector<std::vector<Pose>> RestFrames() const;
};

// models._fit_skin: power-of-two sides (at least 8) no larger than |max_edge|, resized with
// Pillow's Lanczos filter.
RgbaImage FitSkin(const RgbaImage& rgba, int max_edge);

// models.quantize_skin: (indices, 768-byte palette). Masked skins keep index 255 for holes.
std::optional<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>
QuantizeSkin(const RgbaImage& rgba, bool masked, int alpha_threshold = 128);

// models.build_studio_mdl. May append warnings to |model| (and halve a sequence's frame rate
// when its animation doesn't fit the 16-bit offsets). On failure the reason goes to |error|.
std::optional<std::vector<uint8_t>> BuildStudioMdl(ImportedModel& model, std::string* error,
                                                   int alpha_threshold = 128);

// skeleton.py matrices: studio poses are R = Rz(yaw) Ry(pitch) Rx(roll) plus a translation.
using Mat4 = std::array<std::array<double, 4>, 4>;
Mat4 MatrixFromPose(const Pose& pose);
Pose PoseFromMatrix(const Mat4& m);
Mat4 Multiply(const Mat4& a, const Mat4& b);  // a @ b
std::vector<Mat4> WorldMatrices(const std::vector<Pose>& poses, const std::vector<int>& parents);

// models._seq_name and models._unique: a safe, lower-case sequence name of at most 31 bytes,
// numbered when |used| already holds it.
std::string SequenceName(const std::string& latin1);
std::string UniqueName(const std::string& name, std::vector<std::string>* used);

// Latin-1 bytes for a UTF-8 string, '?' for characters outside it (str.encode("latin-1",
// "replace")).
std::string ToLatin1(const std::string& utf8);
}  // namespace GxBake
