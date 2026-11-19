// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include <gtest/gtest.h>

#include "Core/System.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/RenderProfiler.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VideoEvents.h"

namespace
{
class RenderProfilerTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    std::memset(&bpmem, 0, sizeof(bpmem));
    g_stats.this_frame = {};
    RenderProfiler::Init();
    RenderProfiler::ClearHistory();
    RenderProfiler::SetEnabled(true);
  }

  void TearDown() override
  {
    RenderProfiler::SetEnabled(false);
    RenderProfiler::TakeCapture();
    RenderProfiler::Shutdown();
  }

  static void BeginFrame() { GetVideoEvents().before_frame_event.Trigger(); }
  static void EndFrame()
  {
    GetVideoEvents().after_frame_event.Trigger(Core::System::GetInstance());
  }

  static void Draw(u32 tev_stages, u32 vertices, const void* pipeline, bool culled = false)
  {
    bpmem.genMode.numtevstages = tev_stages - 1;
    RenderProfiler::OnGXPrimitive(0);  // GX_DRAW_QUADS
    RenderProfiler::OnDraw(PrimitiveType::Triangles, vertices, vertices * 3 / 2, BitSet32{},
                           pipeline, culled);
  }
};
}  // namespace

TEST_F(RenderProfilerTest, CountsDrawsAndCopiesPerFrame)
{
  int pipeline_a = 0;
  int pipeline_b = 0;

  BeginFrame();
  Draw(1, 4, &pipeline_a);
  Draw(3, 8, &pipeline_a);
  Draw(3, 6, &pipeline_b);
  Draw(2, 100, &pipeline_b, true);
  RenderProfiler::OnEFBCopy(0x200000, EFBCopyFormat::RGB565, 64, 64, false, false, true, true,
                            false);
  RenderProfiler::OnEFBCopy(0x300000, EFBCopyFormat::XFB, 640, 480, false, true, true, true,
                            false);
  EndFrame();

  const auto sample = RenderProfiler::GetLatest();
  ASSERT_TRUE(sample.has_value());
  EXPECT_EQ(sample->vertices, 18u);
  EXPECT_EQ(sample->culled_draws, 1u);
  EXPECT_EQ(sample->pipeline_changes, 2u);
  EXPECT_EQ(sample->tev_stage_histogram[0], 1u);
  EXPECT_EQ(sample->tev_stage_histogram[1], 1u);
  EXPECT_EQ(sample->tev_stage_histogram[2], 2u);
  EXPECT_EQ(sample->efb_copies_to_ram, 1u);
  EXPECT_EQ(sample->xfb_copies, 1u);
  EXPECT_EQ(sample->efb_copy_pixels, 64u * 64u + 640u * 480u);

  // The next frame starts from zero.
  BeginFrame();
  Draw(1, 4, &pipeline_b);
  EndFrame();
  EXPECT_EQ(RenderProfiler::GetLatest()->vertices, 4u);
  EXPECT_EQ(RenderProfiler::GetLatest()->pipeline_changes, 0u);
  EXPECT_EQ(RenderProfiler::GetHistory().size(), 2u);
}

TEST_F(RenderProfilerTest, CapturesOneFrameInOrder)
{
  int pipeline = 0;

  RenderProfiler::RequestCapture();
  EXPECT_TRUE(RenderProfiler::IsCapturePending());

  BeginFrame();
  Draw(2, 4, &pipeline);
  RenderProfiler::OnEFBCopy(0x200000, EFBCopyFormat::RGBA8, 32, 32, true, false, false, true,
                            false);
  Draw(1, 4, &pipeline);
  EndFrame();

  EXPECT_FALSE(RenderProfiler::IsCapturePending());
  const auto capture = RenderProfiler::TakeCapture();
  ASSERT_TRUE(capture.has_value());
  ASSERT_EQ(capture->draws.size(), 2u);
  ASSERT_EQ(capture->copies.size(), 1u);
  EXPECT_EQ(capture->draws[0].tev_stages, 2u);
  EXPECT_EQ(capture->draws[0].gx_primitives, "Quads");
  EXPECT_TRUE(capture->draws[0].pipeline_changed);
  EXPECT_FALSE(capture->draws[1].pipeline_changed);
  EXPECT_EQ(capture->copies[0].after_draw, 1u);
  EXPECT_TRUE(capture->copies[0].is_depth);

  // Only one frame is captured.
  BeginFrame();
  Draw(1, 4, &pipeline);
  EndFrame();
  EXPECT_FALSE(RenderProfiler::TakeCapture().has_value());
}

TEST_F(RenderProfilerTest, RecordsNothingWhileDisabled)
{
  RenderProfiler::SetEnabled(false);
  int pipeline = 0;
  BeginFrame();
  Draw(1, 4, &pipeline);
  EndFrame();
  EXPECT_TRUE(RenderProfiler::GetHistory().empty());
}

TEST_F(RenderProfilerTest, HintsFlagStalls)
{
  RenderProfiler::FrameSample sample;
  sample.frame_ms = 20.0;
  sample.efb_copies_to_ram = 2;
  sample.shaders_created = 1;
  const auto hints = RenderProfiler::GetHints(sample);
  ASSERT_EQ(hints.size(), 3u);
  for (const auto& hint : hints)
    EXPECT_EQ(hint.severity, RenderProfiler::Hint::Severity::Warning);

  EXPECT_TRUE(RenderProfiler::GetHints(RenderProfiler::FrameSample{}).empty());
}
