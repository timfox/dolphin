// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Render profiler for the devkit editor.
//
// Collects a rolling history of per-frame GX statistics (frame time, draw calls, primitives, TEV
// usage, EFB/XFB copies, ...) and can capture a full per-draw breakdown of a single frame. All
// recording hooks are called from the video thread and cost nothing while the profiler is
// disabled; the getters are safe to call from any thread (the UI polls them).

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "VideoCommon/Statistics.h"

enum class EFBCopyFormat;
enum class PrimitiveType : u32;

namespace RenderProfiler
{
// GX supports 1..16 TEV stages per draw.
constexpr std::size_t MAX_TEV_STAGES = 16;
// Number of frames kept in the rolling history.
constexpr std::size_t HISTORY_SIZE = 300;

struct FrameSample
{
  u64 frame_number = 0;
  // Host time since the previous frame ended, in milliseconds.
  double frame_ms = 0.0;
  // Host time the video thread spent between the first draw and the end of the frame.
  double gpu_thread_ms = 0.0;

  Statistics::ThisFrame stats;

  u32 efb_copies_to_texture = 0;
  u32 efb_copies_to_ram = 0;
  u32 depth_copies = 0;
  u32 xfb_copies = 0;
  u32 efb_copy_pixels = 0;

  // Totals over the draws that weren't culled.
  u32 vertices = 0;
  u32 indices = 0;

  u32 textured_draws = 0;
  u32 indirect_draws = 0;
  u32 blended_draws = 0;
  u32 fog_draws = 0;
  u32 culled_draws = 0;
  u32 pipeline_changes = 0;
  u32 textures_uploaded = 0;
  u32 shaders_created = 0;

  // Number of draws that used N+1 TEV stages.
  std::array<u32, MAX_TEV_STAGES> tev_stage_histogram{};
};

struct DrawRecord
{
  u32 index = 0;
  PrimitiveType primitive{};
  u32 num_vertices = 0;
  u32 num_indices = 0;
  u32 tev_stages = 0;
  u32 indirect_stages = 0;
  u32 tex_gens = 0;
  u32 color_channels = 0;
  bool perspective = true;
  bool culled = false;
  bool pipeline_changed = false;
  // GX primitive commands merged into this draw, and their types (e.g. "Quads, Triangles").
  u32 gx_commands = 0;
  std::string gx_primitives;
  // Pre-formatted descriptions of the GX state at draw time.
  std::string textures;
  std::string blend;
  std::string depth;
  std::string cull;
  std::string alpha_test;
  std::string fog;
};

struct CopyRecord
{
  // Number of draws issued in the frame before this copy.
  u32 after_draw = 0;
  u32 dst_address = 0;
  EFBCopyFormat format{};
  u32 width = 0;
  u32 height = 0;
  bool is_depth = false;
  bool is_xfb = false;
  bool to_ram = false;
  bool to_vram = false;
  bool half_scale = false;
};

struct FrameCapture
{
  u64 frame_number = 0;
  FrameSample summary;
  std::vector<DrawRecord> draws;
  std::vector<CopyRecord> copies;
};

void Init();
void Shutdown();

// Recording only happens while enabled (a profiler panel is visible).
void SetEnabled(bool enabled);
bool IsEnabled();
// Drops the collected history.
void ClearHistory();

// Oldest first.
std::vector<FrameSample> GetHistory();
std::optional<FrameSample> GetLatest();

// Captures the next complete frame. The result becomes available through TakeCapture().
void RequestCapture();
bool IsCapturePending();
std::optional<FrameCapture> TakeCapture();

const char* GetPrimitiveName(PrimitiveType primitive);
std::string GetCopyFormatName(EFBCopyFormat format);

// Hooks, called from the video thread.
// |pipeline| is only compared against the previous draw's to count pipeline changes.
void OnDraw(PrimitiveType primitive, u32 num_vertices, u32 num_indices, BitSet32 used_textures,
            const void* pipeline, bool culled);
// A GX primitive command (OpcodeDecoder::Primitive) was added to the current batch.
void OnGXPrimitive(u8 gx_primitive);
void OnEFBCopy(u32 dst_address, EFBCopyFormat format, u32 width, u32 height, bool is_depth,
               bool is_xfb, bool to_ram, bool to_vram, bool half_scale);
}  // namespace RenderProfiler
