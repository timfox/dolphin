// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Bakes a game folder's assets/ tree into GameCube-native files with GxBake, so a game booted
// from an extracted folder (sys/ + files/) sees converted textures, skies and audio without the
// developer running a converter.
//
//   <game>/sys/main.dol
//   <game>/files/...          the disc, as before
//   <game>/assets/valve/...   GxBake sources; outputs are added under files/valve/
//
// PNG/TGA/BMP textures and skies, PCM WAV audio, GoldSrc content (Half-Life sprites, WAD3
// texture packs, BSP v30 world texture packs and skies, studio models) and Quake content (sprites,
// WAD2 files, v29 maps, alias models) and Source models and maps are converted natively,
// byte-identical to the Python tool. Everything else GxBake reads (other model formats, Source
// textures, Source 2 assets, other image and
// audio formats) is handed to the GxBake Python tool when it is installed. Results are
// cached per source file, so only what changed is converted again.

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace GxBake
{
struct BakedFile
{
  std::string disc_path;  // relative to the partition's files/ folder, '/' separated
  std::string host_path;
};

class AssetBaker
{
public:
  explicit AssetBaker(std::string assets_dir);
  ~AssetBaker();

  AssetBaker(const AssetBaker&) = delete;
  AssetBaker& operator=(const AssetBaker&) = delete;

  // Brings the cache up to date and returns every file to add to the disc.
  std::vector<BakedFile> Bake();

  // Re-bakes sources that changed since the last bake, for a game that is running from the
  // baked files. Only outputs whose size is unchanged are replaced (the disc's file table is
  // fixed while it runs); anything else is left for the next boot. Returns the disc paths that
  // were updated in place.
  std::vector<std::string> BakeLive();

  const std::string& GetAssetsDir() const { return m_assets_dir; }

  // <root>/assets/ for a DirectoryBlob root, if it exists and baking is enabled.
  static std::string FindAssetsDir(const std::string& partition_root);

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
  std::string m_assets_dir;
};

// Polls an assets folder while a game runs from it and re-bakes in place.
class AssetWatcher
{
public:
  explicit AssetWatcher(std::shared_ptr<AssetBaker> baker);
  ~AssetWatcher();

  AssetWatcher(const AssetWatcher&) = delete;
  AssetWatcher& operator=(const AssetWatcher&) = delete;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
}  // namespace GxBake
