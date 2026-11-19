// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "Common/FileUtil.h"
#include "Common/Image.h"
#include "GxBake/AssetBaker.h"
#include "GxBake/Encode.h"
#include "GxBake/GoldSrc.h"
#include "GxBake/Md5.h"
#include "GxBake/Quake.h"
#include "GxBake/Studio.h"

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

// GoldSrc fixtures, built the same way as the Python script the reference hashes came from.
void Put(std::vector<u8>* out, u32 v, int bytes = 4)
{
  for (int i = 0; i < bytes; i++)
    out->push_back(static_cast<u8>(v >> (8 * i)));
}

std::vector<u8> GsPalette(int seed)
{
  std::vector<u8> pal;
  for (int i = 0; i < 256; i++)
  {
    pal.push_back(static_cast<u8>((i * 3 + seed) & 255));
    pal.push_back(static_cast<u8>((i * 5 + 17) & 255));
    pal.push_back(static_cast<u8>((i * 11 + 101 + seed) & 255));
  }
  return pal;
}

std::vector<u8> GsIndices(int w, int h, int seed)
{
  std::vector<u8> idx;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      idx.push_back(static_cast<u8>((x * 7 + y * 13 + seed) & 255));
  return idx;
}

void Append(std::vector<u8>* out, const std::vector<u8>& more)
{
  out->insert(out->end(), more.begin(), more.end());
}

std::vector<u8> GsName(const std::string& name)
{
  std::vector<u8> raw(16, 0);
  std::copy(name.begin(), name.end(), raw.begin());
  return raw;
}

std::vector<u8> GsMiptex(const std::string& name, u32 w, u32 h, int seed)
{
  const u32 n = w * h;
  std::vector<u8> out = GsName(name);
  for (u32 v : {w, h, 40u, 40 + n, 40 + n + n / 4, 40 + n + n / 4 + n / 16})
    Put(&out, v);
  Append(&out, GsIndices(w, h, seed));
  out.resize(out.size() + n / 4 + n / 16 + n / 64, 0);
  Put(&out, 256, 2);
  Append(&out, GsPalette(seed));
  out.push_back(0);
  out.push_back(0);
  return out;
}

std::vector<u8> GsQpic(u32 w, u32 h, int seed)
{
  std::vector<u8> out;
  Put(&out, w);
  Put(&out, h);
  Append(&out, GsIndices(w, h, seed));
  Put(&out, 256, 2);
  Append(&out, GsPalette(seed));
  return out;
}

std::vector<u8> GsFont(u32 h, int seed)
{
  std::vector<u8> out;
  for (u32 v : {0u, h, 16u, h / 16 ? h / 16 : 1})
    Put(&out, v);
  for (u32 i = 0; i < 256; i++)
  {
    Put(&out, i * 3, 2);
    Put(&out, 8, 2);
  }
  Append(&out, GsIndices(256, h, seed));
  Put(&out, 256, 2);
  Append(&out, GsPalette(seed));
  return out;
}

std::vector<u8> GsWad()
{
  const std::vector<std::tuple<std::string, u8, std::vector<u8>>> lumps = {
      {"BRICK_A", 0x43, GsMiptex("BRICK_A", 64, 32, 1)},
      {"ODD", 0x43, GsMiptex("odd", 24, 20, 2)},
      {"{FENCE", 0x43, GsMiptex("{FENCE", 40, 12, 3)},
      {"BRICK_A", 0x43, GsMiptex("brick_a", 8, 8, 9)},
      {"HUD_BAR", 0x42, GsQpic(30, 10, 4)},
      {"CONCHARS", 0x46, GsFont(64, 5)},
      {"ODD", 0x42, GsQpic(8, 8, 6)}};
  std::vector<u8> data = {'W', 'A', 'D', '3'};
  Put(&data, static_cast<u32>(lumps.size()));
  Put(&data, 0);
  std::vector<u8> table;
  for (const auto& [name, kind, body] : lumps)
  {
    Put(&table, static_cast<u32>(data.size()));
    Put(&table, static_cast<u32>(body.size()));
    Put(&table, static_cast<u32>(body.size()));
    table.push_back(kind);
    table.push_back(0);
    Put(&table, 0, 2);
    Append(&table, GsName(name));
    Append(&data, body);
  }
  const u32 table_offset = static_cast<u32>(data.size());
  for (int i = 0; i < 4; i++)
    data[8 + i] = static_cast<u8>(table_offset >> (8 * i));
  Append(&data, table);
  return data;
}

// A GoldSrc studio model: 300 bytes standing in for bones and meshes, then the skins.
std::vector<u8> GsStudio(const std::vector<std::tuple<std::string, u32, u32, u32, int>>& textures,
                         u32 skinref, u32 families)
{
  std::vector<u8> body;
  for (int i = 0; i < 300; i++)
    body.push_back(static_cast<u8>((i * 31) & 255));
  const u32 nt = static_cast<u32>(textures.size());
  const u32 texidx = 244 + 300;
  const u32 skinidx = texidx + nt * 80;
  std::vector<u8> skin;
  for (u32 i = 0; i < skinref * families; i++)
    Put(&skin, (i * 3) % nt, 2);
  u32 cursor = skinidx + static_cast<u32>(skin.size());
  std::vector<u8> table, blobs;
  for (const auto& [name, flags, w, h, seed] : textures)
  {
    std::vector<u8> raw_name(64, 0);
    std::copy(name.begin(), name.end(), raw_name.begin());
    Append(&table, raw_name);
    for (u32 v : {flags, w, h, cursor})
      Put(&table, v);
    const size_t before = blobs.size();
    Append(&blobs, GsIndices(w, h, seed));
    Append(&blobs, GsPalette(seed));
    cursor += static_cast<u32>(blobs.size() - before);
  }
  std::vector<u8> data(244, 0);
  std::memcpy(data.data(), "IDST", 4);
  data[4] = 10;
  const std::string model_name = "models/test.mdl";
  std::copy(model_name.begin(), model_name.end(), data.begin() + 8);
  data[140] = 3;
  data[164] = 2;
  const u32 fields[] = {nt, texidx, skinidx + static_cast<u32>(skin.size()), skinref, families,
                        skinidx};
  for (int f = 0; f < 6; f++)
    for (int i = 0; i < 4; i++)
      data[180 + f * 4 + i] = static_cast<u8>(fields[f] >> (8 * i));
  Append(&data, body);
  Append(&data, table);
  Append(&data, skin);
  Append(&data, blobs);
  const u32 length = static_cast<u32>(data.size());
  for (int i = 0; i < 4; i++)
    data[72 + i] = static_cast<u8>(length >> (8 * i));
  return data;
}

std::vector<u8> GsRifle()
{
  return GsStudio({{"stock.bmp", 0x43, 200, 100, 1},
                   {"chrome.bmp", 0x2, 64, 64, 2},
                   {"mask.bmp", 0x40, 96, 130, 3}},
                  3, 2);
}

// A BSP v30 map that uses "brick" from its own texture lump and "odd" from test.wad.
std::vector<u8> GsMap()
{
  const std::string ents = "{\n\"classname\" \"worldspawn\"\n\"wad\" \"\\\\hl\\\\valve\\\\test.wad\"\n}\n";
  std::vector<u8> textures;
  Put(&textures, 2);
  const std::vector<u8> brick = GsMiptex("brick", 16, 16, 21);
  Put(&textures, 12);
  Put(&textures, 12 + static_cast<u32>(brick.size()));
  Append(&textures, brick);
  Append(&textures, GsName("ODD"));
  for (u32 v : {24u, 20u, 0u, 0u, 0u, 0u})
    Put(&textures, v);

  std::vector<u8> data;
  Put(&data, 30);
  data.resize(4 + 15 * 8, 0);
  const u32 ent_offset = static_cast<u32>(data.size());
  data.insert(data.end(), ents.begin(), ents.end());
  const u32 tex_offset = static_cast<u32>(data.size());
  Append(&data, textures);
  for (int i = 0; i < 4; i++)
  {
    data[4 + i] = static_cast<u8>(ent_offset >> (8 * i));
    data[8 + i] = static_cast<u8>(ents.size() >> (8 * i));
    data[20 + i] = static_cast<u8>(tex_offset >> (8 * i));
    data[24 + i] = static_cast<u8>(textures.size() >> (8 * i));
  }
  return data;
}

std::vector<u8> GsSprite(u32 w, u32 h, u32 tex, s32 ox, s32 oy, int seed)
{
  std::vector<u8> out = {'I', 'D', 'S', 'P'};
  Put(&out, 2);
  Put(&out, 2);
  Put(&out, tex);
  Put(&out, 0x3F800000);  // 1.0f
  for (u32 v : {w, h, 1u, 0u, 0u})
    Put(&out, v);
  Put(&out, 256, 2);
  Append(&out, GsPalette(seed));
  for (u32 v : {0u, static_cast<u32>(ox), static_cast<u32>(oy), w, h})
    Put(&out, v);
  std::vector<u8> px = GsIndices(w, h, seed);
  if (tex == 3)
  {
    for (size_t i = 0; i < px.size(); i++)
    {
      if ((i * 7) % 5)
        px[i] = 255;
    }
  }
  Append(&out, px);
  return out;
}

// Quake fixtures: miptex without palettes, a v1 sprite and an alias model.
std::vector<u8> QkMiptex(const std::string& name, u32 w, u32 h, int seed)
{
  const u32 n = w * h;
  std::vector<u8> out = GsName(name);
  for (u32 v : {w, h, 40u, 40 + n, 40 + n + n / 4, 40 + n + n / 4 + n / 16})
    Put(&out, v);
  Append(&out, GsIndices(w, h, seed));
  out.resize(out.size() + n / 4 + n / 16 + n / 64, 0);
  return out;
}

std::vector<u8> QkWad2()
{
  const std::vector<std::pair<std::string, std::vector<u8>>> lumps = {
      {"brick", QkMiptex("brick", 64, 32, 31)},
      {"{grate", QkMiptex("{grate", 16, 16, 32)},
      {"odd", QkMiptex("odd", 24, 40, 33)}};
  std::vector<u8> data = {'W', 'A', 'D', '2'};
  Put(&data, static_cast<u32>(lumps.size()));
  Put(&data, 0);
  std::vector<u8> table;
  for (const auto& [name, body] : lumps)
  {
    Put(&table, static_cast<u32>(data.size()));
    Put(&table, static_cast<u32>(body.size()));
    Put(&table, static_cast<u32>(body.size()));
    table.push_back(0x44);
    table.push_back(0);
    Put(&table, 0, 2);
    Append(&table, GsName(name));
    Append(&data, body);
  }
  const u32 table_offset = static_cast<u32>(data.size());
  for (int i = 0; i < 4; i++)
    data[8 + i] = static_cast<u8>(table_offset >> (8 * i));
  Append(&data, table);
  return data;
}

// A Quake (v29) map: two world textures, a liquid and a sky, all in its own texture lump.
std::vector<u8> QkMap()
{
  const std::string ents = "{\n\"classname\" \"worldspawn\"\n\"message\" \"test\"\n}\n";
  const std::vector<std::vector<u8>> mips = {QkMiptex("wall", 32, 32, 41),
                                             QkMiptex("*water", 16, 16, 42),
                                             QkMiptex("sky1", 32, 16, 43),
                                             QkMiptex("floor", 48, 24, 44)};
  std::vector<u8> textures;
  Put(&textures, static_cast<u32>(mips.size()));
  u32 offset = 4 + 4 * static_cast<u32>(mips.size());
  for (const auto& m : mips)
  {
    Put(&textures, offset);
    offset += static_cast<u32>(m.size());
  }
  for (const auto& m : mips)
    Append(&textures, m);
  std::vector<u8> data;
  Put(&data, 29);
  data.resize(4 + 15 * 8, 0);
  const u32 ent_offset = static_cast<u32>(data.size());
  data.insert(data.end(), ents.begin(), ents.end());
  const u32 tex_offset = static_cast<u32>(data.size());
  Append(&data, textures);
  for (int i = 0; i < 4; i++)
  {
    data[4 + i] = static_cast<u8>(ent_offset >> (8 * i));
    data[8 + i] = static_cast<u8>(ents.size() >> (8 * i));
    data[20 + i] = static_cast<u8>(tex_offset >> (8 * i));
    data[24 + i] = static_cast<u8>(textures.size() >> (8 * i));
  }
  return data;
}

std::vector<u8> QkSprite()
{
  std::vector<u8> out = {'I', 'D', 'S', 'P'};
  Put(&out, 1);
  Put(&out, 2);
  Put(&out, 0x41A00000);  // radius 20.0f
  for (u32 v : {32u, 16u, 1u, 0u, 0u})
    Put(&out, v);
  for (u32 v : {0u, static_cast<u32>(-16), 8u, 32u, 16u})
    Put(&out, v);
  std::vector<u8> px = GsIndices(32, 16, 51);
  for (size_t i = 0; i < px.size(); i += 3)
    px[i] = 255;
  Append(&out, px);
  return out;
}

// A Quake alias model: a 37x23 skin (fitted to 64x32), on-seam vertices and two frames.
std::vector<u8> QkAlias()
{
  constexpr u32 sw = 37, sh = 23, nv = 8;
  std::vector<u8> out = {'I', 'D', 'P', 'O'};
  Put(&out, 6);
  for (u32 f : {0x3E800000u, 0x3F000000u, 0x3E000000u})  // scale 0.25, 0.5, 0.125
    Put(&out, f);
  for (u32 f : {0xC1200000u, 0xC1A00000u, 0xC0A00000u})  // origin -10, -20, -5
    Put(&out, f);
  Put(&out, 0x41F00000);  // radius 30
  for (u32 f : {0u, 0u, 0x41B00000u})
    Put(&out, f);
  for (u32 v : {1u, sw, sh, nv, 8u, 2u, 0u, 0u})
    Put(&out, v);
  Put(&out, 0x3F800000);
  Put(&out, 0);  // a single skin
  Append(&out, GsIndices(sw, sh, 61));
  for (u32 v = 0; v < nv; v++)
  {
    Put(&out, v % 3 == 0 ? 1 : 0);
    Put(&out, (v * 5 + 2) % (sw / 2));
    Put(&out, (v * 7 + 1) % sh);
  }
  const u32 tris[8][4] = {{1, 0, 1, 2}, {0, 2, 1, 3}, {1, 4, 5, 6}, {0, 5, 6, 7},
                          {1, 0, 4, 7}, {0, 1, 3, 5}, {1, 2, 6, 3}, {0, 0, 7, 2}};
  for (const auto& t : tris)
  {
    for (u32 v : t)
      Put(&out, v);
  }
  for (u32 f = 0; f < 2; f++)
  {
    Put(&out, 0);
    Put(&out, 0);
    Put(&out, 0);
    Append(&out, GsName("frame" + std::to_string(f)));
    for (u32 v = 0; v < nv; v++)
    {
      for (u32 k = 0; k < 3; k++)
        out.push_back(static_cast<u8>((v * 37 + k * 91 + f * 13 + 7) & 255));
      out.push_back(0);
    }
  }
  return out;
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

// Reference outputs from the Python tool (gxbake convert, default settings) for the same files.
TEST(GxBake, GoldSrcMatchesPythonTool)
{
  const auto pack = GxBake::BuildWadTexturePack(GsWad(), "");
  ASSERT_TRUE(pack.has_value());
  EXPECT_EQ(Md5Of(*pack), "b59d13bc1aaa92f544083bebd60e4fe0");

  const auto cross = GxBake::LeanHlSprite(GsSprite(64, 32, 3, -31, 15, 7));
  ASSERT_TRUE(cross.has_value());
  EXPECT_EQ(Md5Of(*cross), "753fc5b9abd4cab292d2c334e76c118a");

  // 33 pixels wide can't be halved, so the Python tool copies the sprite as is.
  const auto odd = GsSprite(33, 16, 0, 0, 0, 8);
  EXPECT_FALSE(GxBake::LeanHlSprite(odd).has_value());
  EXPECT_EQ(Md5Of(odd), "8ba273c3a8bd29df2e88c62e2b0c985e");

  const auto lean = GxBake::LeanStudioMesh(GsRifle());
  ASSERT_TRUE(lean.has_value());
  EXPECT_EQ(Md5Of(*lean), "b2464288ac9b8165916e0d03355ecfc1");
  const auto skins = GxBake::StudioSkinCompanion(GsRifle());
  ASSERT_TRUE(skins.has_value());
  EXPECT_EQ(Md5Of(*skins), "8144496dddc236e77216a18184c54067");
  const auto plain_skins = GxBake::StudioSkinCompanion(GsStudio({{"a.bmp", 0, 32, 16, 4}}, 0, 0));
  ASSERT_TRUE(plain_skins.has_value());
  EXPECT_EQ(Md5Of(*plain_skins), "49242b00db35a41a505c54281a656f2c");

  // A WAD2 file needs Quake's palette.
  EXPECT_FALSE(GxBake::BuildWadTexturePack(QkWad2(), "").has_value());
}

TEST(GxBake, QuakeMatchesPythonTool)
{
  // The palette the Python tool finds as gfx/palette.lmp next to the fixtures.
  const std::vector<u8> palette = GsPalette(71);
  if (const char* dir = std::getenv("GXBAKE_FIXTURE_DIR"))
  {
    // Writes the fixtures for `gxbake convert`, to refresh the reference hashes.
    const auto write = [&](const std::string& rel, const std::vector<u8>& data) {
      const std::string path = std::string(dir) + "/id1/" + rel;
      File::CreateFullPath(path);
      File::WriteStringToFile(path, std::string(data.begin(), data.end()));
    };
    write("gfx/palette.lmp", palette);
    write("sprites/q1.spr", QkSprite());
    write("quake.wad", QkWad2());
    write("maps/e1m1.bsp", QkMap());
    write("progs/thing.mdl", QkAlias());
  }

  const auto sprite = GxBake::QuakeSpriteToHl(QkSprite(), palette);
  ASSERT_TRUE(sprite.has_value());
  const auto lean = GxBake::LeanHlSprite(*sprite);
  ASSERT_TRUE(lean.has_value());
  EXPECT_EQ(Md5Of(*lean), "1c3f1c8f943bc4a54c7f5d5004f49d1f");

  const auto wad = GxBake::BuildWadTexturePack(QkWad2(), "", palette.data());
  ASSERT_TRUE(wad.has_value());
  EXPECT_EQ(Md5Of(*wad), "3da34373f95174d4900b485332219cd1");

  const auto info = GxBake::ReadBspInfo(QkMap());
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->version, 29);
  std::vector<std::string> missing;
  const auto world = GxBake::BuildWorldTexturePack(QkMap(), {}, "", &missing, palette.data());
  ASSERT_TRUE(world.has_value());
  EXPECT_TRUE(missing.empty());
  EXPECT_EQ(Md5Of(*world), "df7e855382215898a6fee9ae260afdc5");

  auto model = GxBake::ImportAliasModel(QkAlias(), "thing", palette, true);
  ASSERT_TRUE(model.has_value());
  ASSERT_EQ(model->warnings.size(), 1u);  // two frames, the first converted
  std::string error;
  const auto mdl = GxBake::BuildStudioMdl(*model, &error);
  ASSERT_TRUE(mdl.has_value()) << error;
  EXPECT_EQ(Md5Of(*mdl), "927af53891007d422eb3bd996b890fc0");
  EXPECT_EQ(Md5Of(*GxBake::LeanStudioMesh(*mdl)), "44ee5bb6516ba81310a7c313d0475eed");
  EXPECT_EQ(Md5Of(*GxBake::StudioSkinCompanion(*mdl)), "313976fdfa0a65e733cfc60156c2ddfe");
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
  const auto write_bytes = [](const std::string& path, const std::vector<u8>& data) {
    File::CreateFullPath(path);
    File::WriteStringToFile(path, std::string(data.begin(), data.end()));
  };
  write_bytes(assets + "valve/test.wad", GsWad());
  write_bytes(assets + "valve/sprites/cross.spr", GsSprite(64, 32, 3, -31, 15, 7));
  write_bytes(assets + "valve/maps/e2e.bsp", GsMap());
  write_bytes(assets + "valve/models/rifle.mdl", GsRifle());
  {
    std::vector<u8> sequences = GsRifle();
    std::memcpy(sequences.data(), "IDSQ", 4);
    write_bytes(assets + "valve/models/rifle01.mdl", sequences);
  }

  GxBake::AssetBaker baker(assets);
  auto files = baker.Bake();
  auto find = [&](const std::string& disc_path) {
    return std::ranges::find(files, disc_path, &GxBake::BakedFile::disc_path);
  };
  ASSERT_NE(find("valve/media/textures.gct"), files.end());
  ASSERT_NE(find("valve/media/theme.gcpcm"), files.end());
  ASSERT_NE(find("valve/autoexec.cfg"), files.end());
  // GoldSrc files stay on the disc and get native derived outputs, without the Python tool.
  ASSERT_NE(find("valve/test.wad"), files.end());
  ASSERT_NE(find("valve/media/test.gct"), files.end());
  ASSERT_NE(find("valve/sprites/cross.spr"), files.end());
  ASSERT_NE(find("valve/maps/e2e.bsp"), files.end());
  ASSERT_NE(find("valve/media/wt_e2e.gct"), files.end());
  ASSERT_NE(find("valve/models/rifle.mdl"), files.end());
  ASSERT_NE(find("valve/models/rifle01.mdl"), files.end());
  ASSERT_NE(find("valve/gc_studio/rifle.mdl"), files.end());
  ASSERT_NE(find("valve/gc_studio/riflet.mdl"), files.end());
  EXPECT_EQ(find("valve/gc_studio/rifle01.mdl"), files.end());  // sequence groups stay as they are
  {
    std::string wt;
    ASSERT_TRUE(File::ReadFileToString(find("valve/media/wt_e2e.gct")->host_path, wt));
    const auto world = GxBake::ReadGct(std::vector<u8>(wt.begin(), wt.end()));
    ASSERT_TRUE(world.has_value());
    ASSERT_EQ(world->size(), 2u);
    EXPECT_EQ((*world)[0].name, "brick");
    EXPECT_EQ((*world)[1].name, "odd");  // found in test.wad
  }
  {
    std::string lean;
    ASSERT_TRUE(File::ReadFileToString(find("valve/sprites/cross.spr")->host_path, lean));
    EXPECT_EQ(Md5Of(std::vector<u8>(lean.begin(), lean.end())),
              "753fc5b9abd4cab292d2c334e76c118a");
  }
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
