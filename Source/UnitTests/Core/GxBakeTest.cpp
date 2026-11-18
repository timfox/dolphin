// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Common/FileUtil.h"
#include "Common/Image.h"
#include "GxBake/AssetBaker.h"
#include "GxBake/Encode.h"
#include "GxBake/Md5.h"

namespace
{
std::string Md5Of(const std::vector<u8>& data)
{
  GxBake::Md5 md5;
  md5.Update(data.data(), data.size());
  return md5.HexDigest();
}

// The pattern the reference hashes below were made from with the Python tool:
// r = (7x + 3y) & 255, g = (x*x + y) & 255, b = 11y & 255, a = 0 where (x + y) % 5 == 0.
GxBake::RgbaImage Pattern(int w, int h)
{
  GxBake::RgbaImage img;
  img.width = w;
  img.height = h;
  img.pixels.resize(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; y++)
  {
    for (int x = 0; x < w; x++)
    {
      u8* p = &img.pixels[(static_cast<size_t>(y) * w + x) * 4];
      p[0] = static_cast<u8>((x * 7 + y * 3) & 255);
      p[1] = static_cast<u8>((x * x + y) & 255);
      p[2] = static_cast<u8>((y * 11) & 255);
      p[3] = (x + y) % 5 == 0 ? 0 : 255;
    }
  }
  return img;
}

std::vector<u8> MonoWav(int frames)
{
  std::vector<u8> wav = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                         16,  0,   0,   0,   1, 0, 1, 0, 0x22, 0x56, 0, 0, 0x44, 0xAC, 0, 0,
                         2,   0,   16,  0,   'd', 'a', 't', 'a'};
  const u32 bytes = static_cast<u32>(frames) * 2;
  for (int i = 0; i < 4; i++)
    wav.push_back(static_cast<u8>(bytes >> (8 * i)));
  for (int i = 0; i < frames; i++)
  {
    const s16 v = static_cast<s16>((i * 37) % 2000 - 1000);
    wav.push_back(static_cast<u8>(v));
    wav.push_back(static_cast<u8>(static_cast<u16>(v) >> 8));
  }
  const u32 riff = static_cast<u32>(wav.size() - 8);
  for (int i = 0; i < 4; i++)
    wav[4 + i] = static_cast<u8>(riff >> (8 * i));
  return wav;
}

void WritePng(const std::string& path, const GxBake::RgbaImage& img)
{
  File::CreateFullPath(path);
  ASSERT_TRUE(Common::SavePNG(path, img.pixels.data(), Common::ImageByteFormat::RGBA, img.width,
                              img.height, img.width * 4));
}
}  // namespace

TEST(GxBake, Md5)
{
  EXPECT_EQ(Md5Of({'a', 'b', 'c'}), "900150983cd24fb0d6963f7d28e17f72");
}

// Reference outputs from GxBake 0.1.0 (Python): write_gct([encode_image(...)]), sky_side_cmpr,
// and the ffmpeg-free wav_to_pcm48k path.
TEST(GxBake, MatchesPythonTool)
{
  EXPECT_EQ(Md5Of(GxBake::WriteGct({GxBake::EncodeImage(Pattern(40, 24), {}, "Test")})),
            "07b943d36a29c673b58634bc71b331d3");
  EXPECT_EQ(Md5Of(GxBake::WriteGct(
                {GxBake::EncodeImage(Pattern(300, 70), {}, "Big_Name_Longer_Than_15")})),
            "5f0af20e71ee3253c616173a78c8a481");
  EXPECT_EQ(Md5Of(GxBake::EncodeSkySide(Pattern(100, 100))), "f5040692dada5e9f58718228a92b2bd8");
  const auto pcm = GxBake::WavToGcpcm(MonoWav(1000));
  ASSERT_TRUE(pcm.has_value());
  EXPECT_EQ(Md5Of(*pcm), "835f305643450fda0f071646a26a0848");
}

TEST(GxBake, GctRoundTrip)
{
  const GxBake::GctEntry entry = GxBake::EncodeImage(Pattern(64, 32), {}, "Wall");
  const auto parsed = GxBake::ReadGct(GxBake::WriteGct({entry, entry}));
  ASSERT_TRUE(parsed.has_value());
  ASSERT_EQ(parsed->size(), 2u);
  EXPECT_EQ((*parsed)[1].name, "wall");
  EXPECT_EQ((*parsed)[1].data, entry.data);
}

TEST(GxBake, BakesAndUpdatesLive)
{
  const std::string root = File::CreateTempDir() + "/";
  const std::string assets = root + "assets/";
  WritePng(assets + "valve/textures/wall.png", Pattern(64, 64));
  File::CreateFullPath(assets + "valve/music/");
  {
    const auto wav = MonoWav(500);
    File::WriteStringToFile(assets + "valve/music/theme.wav",
                            std::string(wav.begin(), wav.end()));
  }
  File::WriteStringToFile(assets + "valve/autoexec.cfg", "echo hi\n");

  GxBake::AssetBaker baker(assets);
  auto files = baker.Bake();
  auto find = [&](const std::string& disc_path) {
    return std::ranges::find(files, disc_path, &GxBake::BakedFile::disc_path);
  };
  ASSERT_NE(find("valve/media/textures.gct"), files.end());
  ASSERT_NE(find("valve/media/theme.gcpcm"), files.end());
  ASSERT_NE(find("valve/autoexec.cfg"), files.end());
  const std::string pack = find("valve/media/textures.gct")->host_path;
  std::string before;
  ASSERT_TRUE(File::ReadFileToString(pack, before));

  // A same-size edit is applied to the running disc.
  GxBake::RgbaImage edited = Pattern(64, 64);
  std::ranges::fill(edited.pixels, u8{200});
  WritePng(assets + "valve/textures/wall.png", edited);
  auto updated = baker.BakeLive();
  ASSERT_EQ(updated.size(), 1u);
  EXPECT_EQ(updated[0], "valve/media/textures.gct");
  std::string after;
  ASSERT_TRUE(File::ReadFileToString(pack, after));
  EXPECT_EQ(after.size(), before.size());
  EXPECT_NE(after, before);

  // A new texture changes the pack's size, so it waits for the next boot.
  WritePng(assets + "valve/textures/floor.png", Pattern(32, 32));
  EXPECT_TRUE(baker.BakeLive().empty());
  std::string unchanged;
  ASSERT_TRUE(File::ReadFileToString(pack, unchanged));
  EXPECT_EQ(unchanged, after);

  files = baker.Bake();
  const auto entries =
      GxBake::ReadGct([&] {
        std::string s;
        File::ReadFileToString(find("valve/media/textures.gct")->host_path, s);
        return std::vector<u8>(s.begin(), s.end());
      }());
  ASSERT_TRUE(entries.has_value());
  EXPECT_EQ(entries->size(), 2u);

  // Removing a source removes its output.
  File::Delete(assets + "valve/music/theme.wav");
  files = baker.Bake();
  EXPECT_EQ(find("valve/media/theme.gcpcm"), files.end());

  File::DeleteDirRecursively(root);
}
