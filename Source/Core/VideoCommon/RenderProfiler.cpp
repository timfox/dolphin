// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/RenderProfiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>

#include <fmt/format.h>

#include "Common/HookableEvent.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VideoEvents.h"
#include "VideoCommon/XFMemory.h"

namespace RenderProfiler
{
namespace
{
using Clock = std::chrono::steady_clock;

std::atomic<bool> s_enabled{false};
std::atomic<bool> s_capture_requested{false};

// Video thread state.
FrameSample s_current;
// Written by the video thread only; read by the UI through IsCapturePending().
std::atomic<bool> s_capturing{false};
FrameCapture s_capture;
const void* s_last_pipeline = nullptr;
Clock::time_point s_frame_begin{};
Clock::time_point s_last_frame_end{};
bool s_have_last_frame_end = false;
bool s_frame_began = false;
u64 s_frame_number = 0;
int s_textures_uploaded_base = 0;
int s_shaders_created_base = 0;

// Shared with readers.
std::mutex s_mutex;
std::deque<FrameSample> s_history;
std::optional<FrameCapture> s_finished_capture;

// GX primitive commands in the batch that hasn't been flushed yet.
u32 s_batch_gx_commands = 0;
u8 s_batch_gx_primitive_mask = 0;

Common::EventHook s_before_frame_event;
Common::EventHook s_after_frame_event;

double ToMs(Clock::duration d)
{
  return std::chrono::duration<double, std::milli>(d).count();
}

int ShadersCreated()
{
  return g_stats.num_pixel_shaders_created + g_stats.num_vertex_shaders_created;
}

void ResetCurrent()
{
  s_current = {};
  s_textures_uploaded_base = g_stats.num_textures_uploaded;
  s_shaders_created_base = ShadersCreated();
}

// Whether the hooks should record anything. A frame capture records even while the rolling
// history is disabled.
bool IsRecording()
{
  return s_enabled.load(std::memory_order_relaxed) || s_capturing;
}

void OnBeforeFrame()
{
  const bool enabled = s_enabled.load(std::memory_order_relaxed);
  if (!s_capturing && s_capture_requested.load(std::memory_order_relaxed))
  {
    // Counters gathered while the history was off are stale.
    if (!enabled)
      ResetCurrent();
    s_capture = {};
    s_capturing = true;
    s_capture_requested.store(false, std::memory_order_relaxed);
  }

  if (!IsRecording())
  {
    s_have_last_frame_end = false;
    return;
  }

  s_frame_begin = Clock::now();
  s_frame_began = true;
}

void OnAfterFrame()
{
  if (!IsRecording())
  {
    s_have_last_frame_end = false;
    return;
  }

  const auto now = Clock::now();
  s_current.frame_number = ++s_frame_number;
  s_current.frame_ms = s_have_last_frame_end ? ToMs(now - s_last_frame_end) : 0.0;
  s_current.gpu_thread_ms = s_frame_began ? ToMs(now - s_frame_begin) : 0.0;
  s_current.stats = g_stats.this_frame;
  s_current.textures_uploaded =
      static_cast<u32>(std::max(0, g_stats.num_textures_uploaded - s_textures_uploaded_base));
  s_current.shaders_created =
      static_cast<u32>(std::max(0, ShadersCreated() - s_shaders_created_base));
  s_last_frame_end = now;
  s_have_last_frame_end = true;
  s_frame_began = false;

  {
    std::lock_guard lk(s_mutex);
    if (s_enabled.load(std::memory_order_relaxed))
    {
      s_history.push_back(s_current);
      while (s_history.size() > HISTORY_SIZE)
        s_history.pop_front();
    }

    if (s_capturing)
    {
      s_capture.frame_number = s_current.frame_number;
      s_capture.summary = s_current;
      s_finished_capture = std::move(s_capture);
      s_capture = {};
      s_capturing = false;
    }
  }

  ResetCurrent();
}

const char* PrimitiveName(PrimitiveType primitive)
{
  switch (primitive)
  {
  case PrimitiveType::Points:
    return "Points";
  case PrimitiveType::Lines:
    return "Lines";
  case PrimitiveType::Triangles:
    return "Triangles";
  case PrimitiveType::TriangleStrip:
    return "Triangle strip";
  }
  return "?";
}

std::string DescribeGXPrimitives(u8 mask)
{
  static constexpr const char* names[] = {"Quads",          "Quads (alt)", "Triangles",
                                          "Triangle strip", "Triangle fan", "Lines",
                                          "Line strip",     "Points"};
  std::string out;
  for (u32 i = 0; i < 8; ++i)
  {
    if (!(mask & (1u << i)))
      continue;
    if (!out.empty())
      out += ", ";
    out += names[i];
  }
  return out;
}

std::string DescribeTextures(BitSet32 used_textures)
{
  std::string out;
  for (const u32 i : used_textures)
  {
    const TexUnit& unit = bpmem.tex.GetUnit(i);
    if (!out.empty())
      out += ", ";
    out += fmt::format("T{} {}x{} {} @{:08x}", i, unit.texImage0.width + 1,
                       unit.texImage0.height + 1, unit.texImage0.format,
                       unit.texImage3.image_base << 5);
  }
  return out.empty() ? std::string("none") : out;
}

std::string DescribeBlend()
{
  const auto& bm = bpmem.blendmode;
  std::string out;
  if (bm.logic_op_enable)
    out = fmt::format("Logic {}", bm.logic_mode);
  else if (bm.subtract)
    out = "Subtract";
  else if (bm.blend_enable)
    out = fmt::format("{} / {}", bm.src_factor, bm.dst_factor);
  else
    out = "Off";
  if (!bm.color_update)
    out += ", no color write";
  if (!bm.alpha_update)
    out += ", no alpha write";
  return out;
}

std::string DescribeDepth()
{
  const auto& z = bpmem.zmode;
  if (!z.test_enable)
    return "Off";
  return fmt::format("{}{}{}", z.func, z.update_enable ? ", write" : ", read-only",
                     bpmem.genMode.zfreeze ? ", zfreeze" : "");
}

std::string DescribeAlphaTest()
{
  const auto& at = bpmem.alpha_test;
  switch (at.TestResult())
  {
  case AlphaTestResult::Pass:
    return "Always pass";
  case AlphaTestResult::Fail:
    return "Always fail";
  default:
    return fmt::format("{} {} {} {} {}", at.comp0, at.ref0, at.logic, at.comp1, at.ref1);
  }
}
}  // namespace

void Init()
{
  auto& events = GetVideoEvents();
  s_before_frame_event = events.before_frame_event.Register(&OnBeforeFrame);
  s_after_frame_event =
      events.after_frame_event.Register([](Core::System&) { OnAfterFrame(); });
}

void Shutdown()
{
  s_before_frame_event.reset();
  s_after_frame_event.reset();
}

void SetEnabled(bool enabled)
{
  s_enabled.store(enabled, std::memory_order_relaxed);
}

bool IsEnabled()
{
  return s_enabled.load(std::memory_order_relaxed);
}

void ClearHistory()
{
  std::lock_guard lk(s_mutex);
  s_history.clear();
}

std::vector<FrameSample> GetHistory()
{
  std::lock_guard lk(s_mutex);
  return {s_history.begin(), s_history.end()};
}

std::optional<FrameSample> GetLatest()
{
  std::lock_guard lk(s_mutex);
  if (s_history.empty())
    return std::nullopt;
  return s_history.back();
}

void RequestCapture()
{
  s_capture_requested.store(true, std::memory_order_relaxed);
}

bool IsCapturePending()
{
  return s_capture_requested.load(std::memory_order_relaxed) || s_capturing;
}

std::optional<FrameCapture> TakeCapture()
{
  std::lock_guard lk(s_mutex);
  std::optional<FrameCapture> result = std::move(s_finished_capture);
  s_finished_capture.reset();
  return result;
}

void OnDraw(PrimitiveType primitive, u32 num_vertices, u32 num_indices, BitSet32 used_textures,
            const void* pipeline, bool culled)
{
  const u32 gx_commands = s_batch_gx_commands;
  const u8 gx_mask = s_batch_gx_primitive_mask;
  s_batch_gx_commands = 0;
  s_batch_gx_primitive_mask = 0;

  if (!IsRecording())
    return;

  const u32 tev_stages = bpmem.genMode.numtevstages + 1;
  const bool pipeline_changed = !culled && pipeline != s_last_pipeline;
  if (!culled)
    s_last_pipeline = pipeline;

  s_current.tev_stage_histogram[tev_stages - 1]++;
  if (used_textures.Count() != 0)
    s_current.textured_draws++;
  if (bpmem.genMode.numindstages != 0)
    s_current.indirect_draws++;
  if (bpmem.blendmode.blend_enable || bpmem.blendmode.logic_op_enable ||
      bpmem.blendmode.subtract)
    s_current.blended_draws++;
  if (bpmem.fog.c_proj_fsel.fsel != FogType::Off)
    s_current.fog_draws++;
  if (culled)
  {
    s_current.culled_draws++;
  }
  else
  {
    s_current.vertices += num_vertices;
    s_current.indices += num_indices;
  }
  if (pipeline_changed)
    s_current.pipeline_changes++;

  if (!s_capturing)
    return;

  DrawRecord& draw = s_capture.draws.emplace_back();
  draw.index = static_cast<u32>(s_capture.draws.size() - 1);
  draw.primitive = primitive;
  draw.num_vertices = num_vertices;
  draw.num_indices = num_indices;
  draw.gx_commands = gx_commands;
  draw.gx_primitives = DescribeGXPrimitives(gx_mask);
  draw.tev_stages = tev_stages;
  draw.indirect_stages = bpmem.genMode.numindstages;
  draw.tex_gens = bpmem.genMode.numtexgens;
  draw.color_channels = bpmem.genMode.numcolchans;
  draw.perspective = xfmem.projection.type == ProjectionType::Perspective;
  draw.culled = culled;
  draw.pipeline_changed = pipeline_changed;
  draw.textures = DescribeTextures(used_textures);
  draw.blend = DescribeBlend();
  draw.depth = DescribeDepth();
  draw.cull = fmt::format("{}", bpmem.genMode.cull_mode);
  draw.alpha_test = DescribeAlphaTest();
  draw.fog = fmt::format("{}", bpmem.fog.c_proj_fsel.fsel);
}

void OnGXPrimitive(u8 gx_primitive)
{
  if (!IsRecording())
    return;
  s_batch_gx_commands++;
  s_batch_gx_primitive_mask |= static_cast<u8>(1u << (gx_primitive & 7));
}

void OnEFBCopy(u32 dst_address, EFBCopyFormat format, u32 width, u32 height, bool is_depth,
               bool is_xfb, bool to_ram, bool to_vram, bool half_scale)
{
  if (!IsRecording())
    return;

  if (is_xfb)
    s_current.xfb_copies++;
  else if (to_ram)
    s_current.efb_copies_to_ram++;
  else
    s_current.efb_copies_to_texture++;
  if (is_depth)
    s_current.depth_copies++;
  s_current.efb_copy_pixels += width * height;

  if (!s_capturing)
    return;

  s_capture.copies.push_back({
      .after_draw = static_cast<u32>(s_capture.draws.size()),
      .dst_address = dst_address,
      .format = format,
      .width = width,
      .height = height,
      .is_depth = is_depth,
      .is_xfb = is_xfb,
      .to_ram = to_ram,
      .to_vram = to_vram,
      .half_scale = half_scale,
  });
}

const char* GetPrimitiveName(PrimitiveType primitive)
{
  return PrimitiveName(primitive);
}

std::string GetCopyFormatName(EFBCopyFormat format)
{
  return fmt::format("{}", format);
}
}  // namespace RenderProfiler
