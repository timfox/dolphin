// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

// GxBake's asset encoders for the formats the Xash3D GameCube port reads, ported from the Python
// tool (core/images.py, core/gct.py, core/media.py) with byte-identical output.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "GxBake/Resample.h"

namespace GxBake
{
constexpr int SKY_CMPR_SIZE = 128;
constexpr uint8_t GCT_FLAG_ALPHA = 1;

struct RgbaImage
{
  int width = 0;
  int height = 0;
  std::vector<uint8_t> pixels;  // width * height * 4
};

struct ImageOptions
{
  int max_edge = 256;
  bool mipmaps = true;
  enum class Alpha
  {
    Auto,
    On,
    Off
  } alpha = Alpha::Auto;
  int alpha_threshold = 128;
  Filter resample = Filter::Lanczos;
};

// One texture in a GCWT pack (.gct).
struct GctEntry
{
  std::string name;  // at most 16 bytes on disk
  uint16_t width = 0;
  uint16_t height = 0;
  uint8_t levels = 0;
  uint8_t flags = 0;
  uint16_t tag = 0;
  std::vector<uint8_t> data;
};

// images.encode_image: RGBA to a power-of-two CMPR mip chain. `name` is the file stem.
GctEntry EncodeImage(const RgbaImage& image, const ImageOptions& options, const std::string& name);

// images.sky_side_cmpr: one 128x128 sky side, raw CMPR with no mips.
std::vector<uint8_t> EncodeSkySide(const RgbaImage& image, int size = SKY_CMPR_SIZE);

// gct.write_gct / gct.read_gct.
std::vector<uint8_t> WriteGct(const std::vector<GctEntry>& entries);
std::optional<std::vector<GctEntry>> ReadGct(const std::vector<uint8_t>& data);

// media.wav_to_pcm48k + media._write_gcpcm: a PCM WAV to a .gcpcm stream (48 kHz stereo
// s16 big-endian samples). Returns nothing for WAVs the Python tool would hand to ffmpeg.
std::optional<std::vector<uint8_t>> WavToGcpcm(const std::vector<uint8_t>& wav);

// Image decoders for the formats baked without Python: PNG is decoded by the caller (Dolphin
// links libspng); TGA and BMP are read here, matching Pillow's convert("RGBA").
std::optional<RgbaImage> DecodeTga(const std::vector<uint8_t>& data);
std::optional<RgbaImage> DecodeBmp(const std::vector<uint8_t>& data);

// skybox._is_cross: whether a 4:3 or 3:4 image is a cubemap cross (its empty cells are flat).
bool LooksLikeCross(const RgbaImage& image);

std::string LowerAscii(std::string s);
}  // namespace GxBake
