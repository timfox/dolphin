// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "GxBake/AssetBaker.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Image.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "GxBake/Config.h"
#include "GxBake/Encode.h"
#include "GxBake/GoldSrc.h"
#include "GxBake/Md5.h"
#include "GxBake/Process.h"

namespace fs = std::filesystem;

namespace GxBake
{
namespace
{
// Bumped when the baker's outputs change for the same inputs, so old caches are rebuilt.
constexpr std::string_view CACHE_VERSION = "1";

// gxbake's IMAGE_EXTENSIONS (core/images.py).
const std::set<std::string> IMAGE_EXTS = {
    ".png",  ".tga",  ".bmp", ".jpg", ".jpeg", ".gif", ".webp", ".dds", ".tif",
    ".tiff", ".psd",  ".pcx", ".qoi", ".ppm",  ".vtf", ".vtex_c", ".wal", ".blp",
    ".ftc",  ".ftu",  ".rgb", ".rgba", ".sgi", ".bw",  ".hdr"};
const std::set<std::string> NATIVE_IMAGE_EXTS = {".png", ".tga", ".bmp"};

// Everything else gxbake converts and that is only a source (it never goes on the disc as is):
// video, audio, 3D model and game formats from core/media.py, models.py, rose.py, ffxi.py,
// ff7.py, particles.py, panda.py and dmx_model.py.
const std::set<std::string> PYTHON_SOURCE_EXTS = {
    ".3ds",    ".3mf",     ".ac",     ".aif",       ".aiff",    ".amf",   ".ase",    ".avi",
    ".b3d",    ".bgw",     ".bik",    ".blend",     ".chr",     ".cob",   ".csm",    ".dae",
    ".dxf",    ".fbx",     ".ff7mdl", ".flac",      ".glb",     ".gltf",  ".hmp",    ".hrc",
    ".iqm",    ".irr",     ".irrmesh", ".lwo",      ".lws",     ".lxo",   ".m4a",    ".md2",
    ".md3",    ".md5mesh", ".mdc",    ".mkv",       ".mov",     ".mp3",   ".mp4",    ".mpeg",
    ".mpg",    ".ms3d",    ".ndo",    ".nff",       ".obj",     ".off",   ".ogex",   ".ogg",
    ".opus",   ".pcf",     ".ply",    ".q3o",       ".q3s",     ".rosemdl", ".sib",  ".smd",
    ".spw",    ".stl",     ".ter",    ".usd",       ".usda",    ".usdc",  ".usdz",   ".vmdl_c",
    ".vmesh_c", ".voc",    ".vpcf_c", ".vsnd_c",    ".webm",    ".wma",   ".wmv",    ".x",
    ".x3d",    ".xgl",     ".zgl",    ".zms",       ".zsc",     ".dmx",   ".egg",    ".bam",
    ".pz",     ".pandactor", ".ffximdl", ".vmap_c", ".vwrld_c", ".md5anim", ".bvh"};

// GoldSrc runtime files: they stay on the disc and gxbake also derives packs from them
// (world texture packs, lean gc_studio models, downsampled sprites).
const std::set<std::string> GOLDSRC_EXTS = {".bsp", ".wad", ".mdl", ".spr", ".sp2"};

constexpr std::array<std::string_view, 6> SKY_SIDES = {"rt", "bk", "lf", "ft", "up", "dn"};

// skybox._FACE_NAMES keys.
const std::map<std::string, std::string, std::less<>> FACE_NAMES = {
    {"right", "right"}, {"left", "left"}, {"front", "front"}, {"back", "back"},
    {"up", "up"},       {"top", "up"},    {"down", "down"},   {"bottom", "down"},
    {"px", "right"},    {"nx", "left"},   {"py", "up"},       {"ny", "down"},
    {"pz", "front"},    {"nz", "back"},   {"posx", "right"},  {"negx", "left"},
    {"posy", "up"},     {"negy", "down"}, {"posz", "front"},  {"negz", "back"}};

std::mutex s_bake_mutex;

enum class Kind
{
  Passthrough,
  NativeImage,
  NativeSky,
  NativeWav,
  NativeGoldSrc,  // a .spr/.wad that stays on the disc, plus a native derived output
  Python,
  PythonAndPassthrough,
};

struct Source
{
  std::string rel;     // relative to assets/, '/' separated
  std::string host;    // full host path
  std::string folder;  // relative folder ("" for the root)
  std::string gamedir; // first folder ("" for files in the root)
  std::string stem;    // lowercase stem
  std::string ext;     // lowercase extension with the dot
  std::string name_lower;
  u64 size = 0;
  s64 mtime = 0;
  Kind kind = Kind::Passthrough;
  std::string sky_output;  // NativeSky: media/sky_<name><side>.cmp

  std::string Fingerprint() const { return fmt::format("{}:{}", size, mtime); }
};

struct UnitState
{
  std::string fingerprint;
  std::vector<std::string> outputs;  // disc paths under out/
};

std::string Lower(std::string s)
{
  return LowerAscii(std::move(s));
}

std::string Join(const std::string& dir, const std::string& rel)
{
  return dir.empty() ? rel : dir + "/" + rel;
}

std::vector<std::string> SplitPath(const std::string& rel)
{
  std::vector<std::string> parts;
  size_t start = 0;
  while (true)
  {
    const size_t slash = rel.find('/', start);
    parts.push_back(rel.substr(start, slash - start));
    if (slash == std::string::npos)
      break;
    start = slash + 1;
  }
  return parts;
}

std::string Md5Hex(std::string_view s)
{
  Md5 md5;
  md5.Update(s.data(), s.size());
  return md5.HexDigest();
}

std::optional<std::vector<u8>> ReadFile(const std::string& path)
{
  std::ifstream f;
  File::OpenFStream(f, path, std::ios::binary);
  if (!f)
    return std::nullopt;
  return std::vector<u8>(std::istreambuf_iterator<char>(f), {});
}

bool WriteFileAtomic(const std::string& path, const std::vector<u8>& data)
{
  File::CreateFullPath(path);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f;
    File::OpenFStream(f, tmp, std::ios::binary | std::ios::trunc);
    if (!f)
      return false;
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
    if (!f)
      return false;
  }
  if (File::Rename(tmp, path))
    return true;
  // A reader may hold the file open on some platforms; fall back to overwriting it.
  const bool ok = File::Copy(tmp, path, true);
  File::Delete(tmp, File::IfAbsentBehavior::NoConsoleWarning);
  return ok;
}

// --- GoldSrc maps: the WADs and sky a map reads (gct.collect_wads, project._bsp_sky) -----------

// A map's gamedir: the folder above maps/, or the map's own folder.
fs::path MapGamedir(const fs::path& bsp)
{
  const fs::path parent = bsp.parent_path();
  return Lower(PathToString(parent.filename())) == "maps" ? parent.parent_path() : parent;
}

// gct.wad_search_roots: the gamedir, its dod/valve siblings and the disc builder's env vars.
std::vector<fs::path> WadSearchRoots(const fs::path& gamedir)
{
  std::vector<fs::path> roots;
  std::set<fs::path> seen;
  const auto add = [&](const fs::path& path) {
    std::error_code ec;
    const fs::path resolved = fs::weakly_canonical(path, ec);
    if (ec || !fs::is_directory(resolved, ec) || !seen.insert(resolved).second)
      return;
    roots.push_back(resolved);
  };
  add(gamedir);
  for (const fs::path& name : {gamedir.filename(), fs::path("dod"), fs::path("valve")})
    add(gamedir.parent_path() / name);
  for (const char* env : {"DODGC_FULL_DOD", "DODGC_WT_WAD_ROOT", "HL_VALVE_DIR"})
  {
    const char* raw = std::getenv(env);
    std::string value = raw ? raw : "";
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
      value.pop_back();
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
      value.erase(value.begin());
    if (!value.empty())
      add(StringToPath(value));
  }
  return roots;
}

// sorted(dir.glob("*.wad"))
std::vector<fs::path> WadsIn(const fs::path& dir)
{
  std::vector<fs::path> wads;
  std::error_code ec;
  for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
       it.increment(ec))
  {
    std::string name = PathToString(it->path().filename());
#ifdef _WIN32
    name = Lower(name);
#endif
    if (name.ends_with(".wad") && it->is_regular_file(ec))
      wads.push_back(it->path());
  }
  std::ranges::sort(wads, {}, [](const fs::path& p) { return PathToString(p); });
  return wads;
}

std::string FileFingerprint(const fs::path& path)
{
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  const auto time = fs::last_write_time(path, ec);
  return fmt::format("{}={}:{};", PathToString(path), size,
                     std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch())
                         .count());
}

// Everything a map's outputs depend on besides the map: every WAD it could read and its sky.
std::string MapDependencies(const fs::path& bsp)
{
  const fs::path gamedir = MapGamedir(bsp);
  std::string fp;
  for (const fs::path& root : WadSearchRoots(gamedir))
  {
    for (const fs::path& wad : WadsIn(root))
      fp += FileFingerprint(wad);
  }
  std::error_code ec;
  for (auto it = fs::directory_iterator(gamedir / "gfx" / "env", ec);
       !ec && it != fs::directory_iterator(); it.increment(ec))
  {
    fp += FileFingerprint(it->path());
  }
  return fp;
}

// gct.collect_wads: every WAD in the gamedir, then the map's own WADs (plus the stock ones and
// dod_*.wad) from the search roots, first WAD of a name wins.
WadIndex CollectWads(const BspInfo& info, const fs::path& gamedir, std::vector<std::string>* loaded)
{
  WadIndex index;
  std::set<std::string> have;
  const auto ingest = [&](const fs::path& wad) {
    auto data = ReadFile(PathToString(wad));
    if (!data || index.Ingest(std::make_shared<const std::vector<u8>>(std::move(*data))) == 0)
      return false;
    loaded->push_back(PathToString(wad));
    return true;
  };
  for (const fs::path& wad : WadsIn(gamedir))
  {
    if (ingest(wad))
      have.insert(Lower(PathToString(wad.filename())));
  }
  std::set<std::string> wanted = {"halflife.wad", "hlbasics.wad", "decals.wad"};
  for (const std::string& w : info.wads)
    wanted.insert(Lower(w));
  for (const fs::path& root : WadSearchRoots(gamedir))
  {
    for (const fs::path& wad : WadsIn(root))
    {
      const std::string key = Lower(PathToString(wad.filename()));
      if (have.contains(key) || (!wanted.contains(key) && !key.starts_with("dod_")))
        continue;
      if (ingest(wad))
        have.insert(key);
    }
  }
  return index;
}

std::optional<RgbaImage> LoadNativeImage(const std::string& ext, const std::vector<u8>& data)
{
  if (ext == ".tga")
    return DecodeTga(data);
  if (ext == ".bmp")
    return DecodeBmp(data);
  RgbaImage img;
  Common::UniqueBuffer<u8> buffer;
  u32 w = 0, h = 0;
  if (!Common::LoadPNG(data, &buffer, &w, &h))
    return std::nullopt;
  img.width = static_cast<int>(w);
  img.height = static_cast<int>(h);
  img.pixels.assign(buffer.data(), buffer.data() + buffer.size());
  return img;
}

// Width and height from the header alone, for the panorama checks.
std::optional<std::pair<int, int>> PeekImageSize(const std::string& host, const std::string& ext)
{
  std::ifstream f;
  File::OpenFStream(f, host, std::ios::binary);
  std::array<u8, 32> h{};
  f.read(reinterpret_cast<char*>(h.data()), h.size());
  if (f.gcount() < 26)
    return std::nullopt;
  if (ext == ".png")
  {
    return std::pair<int, int>((h[16] << 24) | (h[17] << 16) | (h[18] << 8) | h[19],
                               (h[20] << 24) | (h[21] << 16) | (h[22] << 8) | h[23]);
  }
  if (ext == ".tga")
    return std::pair<int, int>(h[12] | (h[13] << 8), h[14] | (h[15] << 8));
  if (ext == ".bmp")
  {
    const int w = h[18] | (h[19] << 8) | (h[20] << 16) | (h[21] << 24);
    const int hh = h[22] | (h[23] << 8) | (h[24] << 16) | (h[25] << 24);
    return std::pair<int, int>(w, std::abs(hh));
  }
  return std::nullopt;
}

struct PythonTool
{
  std::vector<std::string> command;
  bool resolved = false;
};

PythonTool s_python;

bool IsOnPath(const std::string& exe)
{
  const char* path = std::getenv("PATH");
  if (!path)
    return false;
#ifdef _WIN32
  constexpr char sep = ';';
  const std::vector<std::string> names = {exe + ".exe", exe + ".cmd", exe + ".bat"};
#else
  constexpr char sep = ':';
  const std::vector<std::string> names = {exe};
#endif
  std::stringstream ss(path);
  std::string dir;
  while (std::getline(ss, dir, sep))
  {
    for (const std::string& n : names)
    {
      if (!dir.empty() && File::IsFile(dir + "/" + n))
        return true;
    }
  }
  return false;
}

// The command that runs the Python tool, or empty when it is not installed.
const std::vector<std::string>& ResolvePython(const std::string& log_path)
{
  const std::string configured = Config::Get(Config::GXBAKE_PYTHON_COMMAND);
  if (!configured.empty())
  {
    s_python.command = SplitCommandLine(configured);
    s_python.resolved = true;
    return s_python.command;
  }
  if (s_python.resolved)
    return s_python.command;
  s_python.resolved = true;
  std::vector<std::vector<std::string>> candidates;
  if (IsOnPath("gxbake"))
    candidates.push_back({"gxbake"});
  candidates.push_back({"python3", "-m", "gxbake"});
  candidates.push_back({"python", "-m", "gxbake"});
#ifdef _WIN32
  candidates.push_back({"py", "-m", "gxbake"});
#endif
  for (const auto& c : candidates)
  {
    std::vector<std::string> probe = c;
    probe.push_back("--version");
    if (RunProcess(probe, log_path) == 0)
    {
      s_python.command = c;
      INFO_LOG_FMT(DISCIO, "GxBake: using the Python tool for other formats ({})",
                   fmt::join(c, " "));
      return s_python.command;
    }
  }
  return s_python.command;
}

// images.find_skybox_sides: the six <name>{rt,bk,lf,ft,up,dn} images of a GoldSrc sky.
std::optional<std::pair<std::string, std::map<std::string, const Source*>>>
FindSkySides(const Source& src, const std::vector<const Source*>& siblings)
{
  for (std::string_view sfx : SKY_SIDES)
  {
    if (src.stem.size() <= sfx.size() || !src.stem.ends_with(sfx))
      continue;
    const std::string name = src.stem.substr(0, src.stem.size() - sfx.size());
    std::map<std::string, const Source*> sides;
    for (std::string_view side : SKY_SIDES)
    {
      for (const Source* s : siblings)
      {
        if (s->stem == name + std::string(side) && IMAGE_EXTS.contains(s->ext))
        {
          sides[std::string(side)] = s;
          break;
        }
      }
    }
    if (sides.size() == 6)
      return std::make_pair(name, sides);
  }
  return std::nullopt;
}

bool IsSeparator(char c)
{
  return c == '_' || c == '-' || c == '.' || c == ' ';
}

// skybox.find_faces "view" scheme: <base>_right/left/front/back/up/down (or px/nx...).
bool IsViewFace(const Source& src, const std::vector<const Source*>& siblings)
{
  std::vector<std::string> tokens;
  for (const auto& [token, face] : FACE_NAMES)
    tokens.push_back(token);
  std::stable_sort(tokens.begin(), tokens.end(),
                   [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
  for (const std::string& token : tokens)
  {
    if (src.stem.size() <= token.size() || !src.stem.ends_with(token))
      continue;
    std::string base = src.stem.substr(0, src.stem.size() - token.size());
    if (!base.empty() && IsSeparator(base.back()))
      base.pop_back();
    if (base.empty())
      continue;
    std::set<std::string> found;
    for (const Source* s : siblings)
    {
      if (!IMAGE_EXTS.contains(s->ext) || !s->stem.starts_with(base))
        continue;
      std::string rest = s->stem.substr(base.size());
      if (!rest.empty() && IsSeparator(rest[0]))
        rest = rest.substr(1);
      if (rest.empty() || !std::ranges::all_of(rest, [](char c) { return c >= 'a' && c <= 'z'; }))
        continue;
      const auto it = FACE_NAMES.find(rest);
      if (it != FACE_NAMES.end())
        found.insert(it->second);
    }
    if (found.size() == 6)
      return true;
  }
  return false;
}

// skybox.looks_like_panorama, for the formats read natively.
bool LooksLikePanorama(const Source& src)
{
  const auto size = PeekImageSize(src.host, src.ext);
  if (!size)
    return false;
  const auto [w, h] = *size;
  if (w == 2 * h && w >= 1024)
    return true;
  if ((w == 6 * h && h >= 128) || (h == 6 * w && w >= 128))
    return true;
  if ((3 * w == 4 * h || 4 * w == 3 * h) && std::max(w, h) >= 512)
  {
    const auto data = ReadFile(src.host);
    const auto img = data ? LoadNativeImage(src.ext, *data) : std::nullopt;
    return img && LooksLikeCross(*img);
  }
  return false;
}

bool InSoundFolder(const std::string& rel)
{
  const auto parts = SplitPath(rel);
  for (size_t i = 0; i + 1 < parts.size(); i++)
  {
    if (Lower(parts[i]) == "sound")
      return true;
  }
  return false;
}

bool ComparePathParts(const std::string& a, const std::string& b)
{
  return SplitPath(a) < SplitPath(b);
}
}  // namespace

struct AssetBaker::Impl
{
  std::string assets_dir;  // with a trailing slash
  std::string cache_dir;   // with a trailing slash
  std::map<std::string, UnitState> state;
  std::vector<std::string> warned;

  std::string OutPath(const std::string& disc_path) const { return cache_dir + "out/" + disc_path; }
  std::string StatePath() const { return cache_dir + "state.txt"; }

  void LoadState()
  {
    state.clear();
    std::string text;
    if (!File::ReadFileToString(StatePath(), text))
      return;
    std::stringstream ss(text);
    std::string line;
    bool first = true;
    while (std::getline(ss, line))
    {
      if (first)
      {
        first = false;
        if (line != fmt::format("gxbake-cache {}", CACHE_VERSION))
          return;
        continue;
      }
      const auto cols = SplitString(line, '\t');
      if (cols.size() < 2)
        continue;
      UnitState& u = state[cols[0]];
      u.fingerprint = cols[1];
      u.outputs.assign(cols.begin() + 2, cols.end());
    }
  }

  void SaveState() const
  {
    std::string text = fmt::format("gxbake-cache {}\n", CACHE_VERSION);
    for (const auto& [key, u] : state)
    {
      text += key + "\t" + u.fingerprint;
      for (const std::string& o : u.outputs)
        text += "\t" + o;
      text += "\n";
    }
    File::CreateFullPath(StatePath());
    File::WriteStringToFile(StatePath() + ".tmp", text);
    File::Rename(StatePath() + ".tmp", StatePath());
  }

  std::vector<Source> Scan() const
  {
    std::vector<Source> out;
    std::error_code ec;
    const fs::path root = StringToPath(assets_dir);
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
    {
      if (ec)
        break;
      const std::string name = PathToString(it->path().filename());
      if (name.starts_with("."))
      {
        if (it->is_directory(ec))
          it.disable_recursion_pending();
        continue;
      }
      if (!it->is_regular_file(ec))
        continue;
      const std::string lower = Lower(name);
      if (lower == "gxbake-manifest.json" || lower == "thumbs.db" || lower == "desktop.ini" ||
          lower.ends_with(".tmp"))
      {
        continue;
      }
      Source s;
      s.rel = PathToString(it->path().lexically_relative(root).generic_u8string());
      s.host = PathToString(it->path());
      const size_t slash = s.rel.rfind('/');
      s.folder = slash == std::string::npos ? "" : s.rel.substr(0, slash);
      const size_t first = s.rel.find('/');
      s.gamedir = first == std::string::npos ? "" : s.rel.substr(0, first);
      s.name_lower = lower;
      const size_t dot = lower.rfind('.');
      s.ext = dot == std::string::npos || dot == 0 ? "" : lower.substr(dot);
      s.stem = dot == std::string::npos || dot == 0 ? lower : lower.substr(0, dot);
      s.size = it->file_size(ec);
      const auto t = it->last_write_time(ec);
      s.mtime = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
      out.push_back(std::move(s));
    }
    std::ranges::sort(out, ComparePathParts, &Source::rel);
    return out;
  }

  void Classify(std::vector<Source>* sources)
  {
    std::map<std::string, std::vector<const Source*>> folders;
    for (const Source& s : *sources)
      folders[s.folder].push_back(&s);

    for (Source& s : *sources)
    {
      const auto& siblings = folders[s.folder];
      if (NATIVE_IMAGE_EXTS.contains(s.ext))
      {
        if (const auto sky = FindSkySides(s, siblings))
        {
          const bool all_native = std::ranges::all_of(
              sky->second, [](const auto& kv) { return NATIVE_IMAGE_EXTS.contains(kv.second->ext); });
          if (all_native)
          {
            s.kind = Kind::NativeSky;
            const std::string side = s.stem.substr(sky->first.size());
            s.sky_output = fmt::format("media/sky_{}{}.cmp", sky->first, side);
          }
          else
          {
            s.kind = Kind::Python;
          }
        }
        else if (IsViewFace(s, siblings) || LooksLikePanorama(s))
        {
          s.kind = Kind::Python;
        }
        else
        {
          s.kind = Kind::NativeImage;
        }
      }
      else if (s.ext == ".wav")
      {
        s.kind = InSoundFolder(s.rel) ? Kind::Passthrough : Kind::NativeWav;
      }
      else if (IMAGE_EXTS.contains(s.ext) || PYTHON_SOURCE_EXTS.contains(s.ext))
      {
        s.kind = Kind::Python;
      }
      else if (s.ext == ".spr" || s.ext == ".wad" || s.ext == ".bsp" || s.ext == ".mdl")
      {
        // Half-Life sprites, WAD3 files, BSP v30 maps and studio models bake natively; Quake and
        // Source ones turn out to need the Python tool once their header is read.
        s.kind = Kind::NativeGoldSrc;
      }
      else if (GOLDSRC_EXTS.contains(s.ext))
      {
        s.kind = Kind::PythonAndPassthrough;
      }
      else
      {
        s.kind = Kind::Passthrough;
      }
    }
  }

  void Warn(const std::string& msg)
  {
    if (std::ranges::find(warned, msg) != warned.end())
      return;
    warned.push_back(msg);
    WARN_LOG_FMT(DISCIO, "GxBake: {}", msg);
  }

  // Writes one output. In live mode, a file whose size would change is left for the next boot.
  bool Commit(const std::string& disc_path, const std::vector<u8>& data, bool live,
              std::vector<std::string>* updated)
  {
    const std::string path = OutPath(disc_path);
    if (live)
    {
      if (!File::IsFile(path) || File::GetSize(path) != data.size())
        return false;
      const auto old = ReadFile(path);
      if (old && *old == data)
        return true;
    }
    if (!WriteFileAtomic(path, data))
    {
      Warn(fmt::format("could not write {}", path));
      return false;
    }
    if (live && updated)
      updated->push_back(disc_path);
    return true;
  }

  void RemoveOutputs(const std::vector<std::string>& outputs, const std::set<std::string>& keep)
  {
    for (const std::string& o : outputs)
    {
      if (!keep.contains(o))
        File::Delete(OutPath(o), File::IfAbsentBehavior::NoConsoleWarning);
    }
  }

  std::vector<std::string> Run(bool live, std::vector<BakedFile>* files);
};

std::vector<std::string> AssetBaker::Impl::Run(bool live, std::vector<BakedFile>* files)
{
  std::vector<std::string> updated;
  File::CreateFullPath(cache_dir);
  LoadState();
  std::vector<Source> sources = Scan();

  // The Python tool is looked up only when something actually needs it.
  const bool want_python = Config::Get(Config::GXBAKE_PYTHON_FALLBACK);
  const std::vector<std::string>* python_cmd = nullptr;
  bool python_resolved = false;
  auto python = [&]() -> const std::vector<std::string>* {
    if (!python_resolved && want_python)
    {
      const auto& cmd = ResolvePython(cache_dir + "python-probe.log");
      python_cmd = cmd.empty() ? nullptr : &cmd;
    }
    python_resolved = true;
    return python_cmd;
  };
  Classify(&sources);

  std::map<std::string, UnitState> next;
  auto carry = [&](const std::string& key) {
    if (const auto it = state.find(key); it != state.end())
      next[key] = it->second;
  };

  // Image entries for each gamedir's media/textures.gct, keyed by source path.
  struct PackEntry
  {
    std::string rel;
    std::string entry_file;
    std::string fingerprint;
  };
  std::map<std::string, std::vector<PackEntry>> packs;
  int converted = 0;

  // --- Native textures, sky sides and audio ---------------------------------------------------
  for (Source& s : sources)
  {
    if (s.kind == Kind::NativeImage)
    {
      const std::string key = "img:" + s.rel;
      const std::string entry_file = cache_dir + "entries/" + Md5Hex(s.rel) + ".gct";
      const auto it = state.find(key);
      if (it != state.end() && it->second.fingerprint == s.Fingerprint() &&
          File::IsFile(entry_file))
      {
        next[key] = it->second;
        packs[s.gamedir].push_back({s.rel, entry_file, s.Fingerprint()});
        continue;
      }
      const auto data = ReadFile(s.host);
      const auto img = data ? LoadNativeImage(s.ext, *data) : std::nullopt;
      if (!img)
      {
        if (want_python)
        {
          s.kind = Kind::Python;
          continue;
        }
        Warn(fmt::format("{}: cannot read this image without the GxBake Python tool", s.rel));
        continue;
      }
      const GctEntry entry = EncodeImage(*img, ImageOptions{}, s.stem);
      if (!WriteFileAtomic(entry_file, WriteGct({entry})))
        continue;
      converted++;
      next[key] = {s.Fingerprint(), {}};
      packs[s.gamedir].push_back({s.rel, entry_file, s.Fingerprint()});
    }
    else if (s.kind == Kind::NativeSky || s.kind == Kind::NativeWav)
    {
      const std::string key = (s.kind == Kind::NativeSky ? "sky:" : "wav:") + s.rel;
      const std::string out_rel =
          Join(s.gamedir, s.kind == Kind::NativeSky ? s.sky_output : "media/" + s.stem + ".gcpcm");
      const auto it = state.find(key);
      if (it != state.end() && it->second.fingerprint == s.Fingerprint() &&
          it->second.outputs == std::vector<std::string>{out_rel} &&
          File::IsFile(OutPath(out_rel)))
      {
        next[key] = it->second;
        continue;
      }
      const auto data = ReadFile(s.host);
      std::optional<std::vector<u8>> baked;
      if (data && s.kind == Kind::NativeSky)
      {
        if (const auto img = LoadNativeImage(s.ext, *data))
          baked = EncodeSkySide(*img);
      }
      else if (data)
      {
        baked = WavToGcpcm(*data);
      }
      if (!baked)
      {
        if (want_python)
        {
          s.kind = Kind::Python;
          continue;
        }
        Warn(fmt::format("{}: needs the GxBake Python tool (or ffmpeg) to convert", s.rel));
        continue;
      }
      if (!Commit(out_rel, *baked, live, &updated))
      {
        carry(key);  // left for the next boot
        continue;
      }
      converted++;
      if (it != state.end())
        RemoveOutputs(it->second.outputs, {out_rel});
      next[key] = {s.Fingerprint(), {out_rel}};
    }
  }

  // --- GoldSrc sprites, WADs and maps --------------------------------------------------------
  for (Source& s : sources)
  {
    if (s.kind != Kind::NativeGoldSrc)
      continue;
    const std::string key = "goldsrc:" + s.rel;
    const bool map = s.ext == ".bsp";
    const std::string fp =
        map ? Md5Hex(s.Fingerprint() + ";" + MapDependencies(StringToPath(s.host))) :
              s.Fingerprint();
    const auto it = state.find(key);
    if (it != state.end() && it->second.fingerprint == fp &&
        std::ranges::all_of(it->second.outputs,
                            [&](const std::string& o) { return File::IsFile(OutPath(o)); }))
    {
      next[key] = it->second;
      continue;
    }
    const auto data = ReadFile(s.host);
    if (!data)
      continue;

    // disc path (relative to the gamedir) -> contents
    std::map<std::string, std::vector<u8>> baked;
    if (s.ext == ".spr")
    {
      if (SpriteNeedsPython(*data))
      {
        s.kind = Kind::PythonAndPassthrough;  // Quake sprite
        continue;
      }
      // A sprite that can't be leaned (several frames, odd size) is copied as is.
      baked["sprites/" + s.stem + ".spr"] = LeanHlSprite(*data).value_or(*data);
    }
    else if (s.ext == ".wad")
    {
      if (!IsWad3(*data))
      {
        s.kind = Kind::PythonAndPassthrough;  // Quake WAD2
        continue;
      }
      const auto pack = BuildWadTexturePack(*data, cache_dir + "python-cmpr");
      if (!pack)
      {
        Warn(fmt::format("{}: not a readable WAD3 file", s.rel));
        continue;
      }
      if (!pack->empty())
        baked["media/" + s.stem + ".gct"] = *pack;
    }
    else if (s.ext == ".mdl")
    {
      if (StudioNeedsPython(*data))
      {
        s.kind = Kind::PythonAndPassthrough;  // Source or Quake model
        continue;
      }
      // A sequence group (<name>01.mdl, "IDSQ") yields nothing; it still goes on the disc for
      // the engine to read with its model.
      const bool sequence_group = data->size() >= 4 && std::memcmp(data->data(), "IDSQ", 4) == 0;
      if (const auto lean = sequence_group ? std::nullopt : LeanStudioMesh(*data))
        baked["gc_studio/" + s.stem + ".mdl"] = *lean;
      if (const auto skins = sequence_group ? std::nullopt : StudioSkinCompanion(*data))
        baked["gc_studio/" + s.stem + "t.mdl"] = *skins;
    }
    else
    {
      const auto info = ReadBspInfo(*data);
      if (!info)
      {
        s.kind = Kind::PythonAndPassthrough;  // Quake or Source map
        continue;
      }
      const fs::path gamedir = MapGamedir(StringToPath(s.host));
      std::vector<std::string> loaded, missing;
      const WadIndex wads = CollectWads(*info, gamedir, &loaded);
      const auto pack = BuildWorldTexturePack(*data, wads, cache_dir + "python-cmpr", &missing);
      if (!pack)
      {
        Warn(fmt::format("{}: not a readable BSP v30 map", s.rel));
        continue;
      }
      if (!missing.empty())
      {
        std::string names;
        for (size_t i = 0; i < std::min<size_t>(8, missing.size()); i++)
          names += (i ? ", " : "") + missing[i];
        Warn(fmt::format("{}: {} textures unresolved: {} ({} WADs searched)", s.rel,
                         missing.size(), names, loaded.size()));
      }
      if (!pack->empty())
        baked["media/wt_" + s.stem + ".gct"] = *pack;

      // project._bsp_sky: gfx/env/<sky><side>.tga/bmp/png next to the map.
      if (!info->sky.empty())
      {
        std::map<std::string, std::vector<u8>> sky;
        for (std::string_view side : SKY_SIDES)
        {
          for (const char* ext : {".tga", ".bmp", ".png"})
          {
            const fs::path path =
                gamedir / "gfx" / "env" / StringToPath(info->sky + std::string(side) + ext);
            std::error_code ec;
            if (!fs::is_regular_file(path, ec))
              continue;
            const auto img_data = ReadFile(PathToString(path));
            const auto img = img_data ? LoadNativeImage(ext, *img_data) : std::nullopt;
            if (img)
              sky[fmt::format("media/sky_{}{}.cmp", info->sky, side)] = EncodeSkySide(*img);
            break;
          }
        }
        if (sky.size() == SKY_SIDES.size())
          baked.merge(sky);
      }
    }

    std::vector<std::string> outputs;
    bool deferred = false;
    for (const auto& [rel, contents] : baked)
    {
      const std::string out_rel = Join(s.gamedir, rel);
      if (!Commit(out_rel, contents, live, &updated))
      {
        deferred = true;
        continue;
      }
      outputs.push_back(out_rel);
    }
    if (deferred)
    {
      carry(key);  // left for the next boot
      continue;
    }
    converted++;
    if (it != state.end())
      RemoveOutputs(it->second.outputs, {outputs.begin(), outputs.end()});
    next[key] = {fp, outputs};
  }

  // --- Everything else, through the Python tool, one call per folder --------------------------
  std::map<std::string, std::vector<const Source*>> groups;
  std::map<std::string, std::string> folder_fp;
  for (const Source& s : sources)
  {
    folder_fp[s.folder] += s.name_lower + "=" + s.Fingerprint() + ";";
    if (s.kind == Kind::Python || s.kind == Kind::PythonAndPassthrough)
      groups[s.folder].push_back(&s);
  }
  for (const auto& [folder, members] : groups)
  {
    const std::string key = "py:" + folder;
    // A model can read textures and animation clips next to it, so a folder with models is
    // converted again when anything in it changes; a folder of images only when they change.
    const bool only_images = std::ranges::all_of(
        members, [](const Source* m) { return IMAGE_EXTS.contains(m->ext); });
    std::string member_fp;
    for (const Source* m : members)
      member_fp += m->name_lower + "=" + m->Fingerprint() + ";";
    const std::string fp = Md5Hex(only_images ? member_fp : folder_fp[folder]);
    const std::string gamedir = members.front()->gamedir;
    const auto it = state.find(key);

    auto add_entries = [&](const UnitState& u) {
      // Outputs starting with '#' are image entries for the pack: #<source rel>|<entry file>.
      for (const std::string& o : u.outputs)
      {
        if (!o.starts_with("#"))
          continue;
        const size_t bar = o.find('|');
        packs[gamedir].push_back({o.substr(1, bar - 1), o.substr(bar + 1), fp});
      }
    };

    bool cached = it != state.end() && it->second.fingerprint == fp;
    if (cached)
    {
      for (const std::string& o : it->second.outputs)
      {
        if (o.starts_with("#") ? !File::IsFile(o.substr(o.find('|') + 1)) :
                                 !File::IsFile(OutPath(o)))
        {
          cached = false;
        }
      }
    }
    if (cached)
    {
      next[key] = it->second;
      add_entries(it->second);
      continue;
    }
    if (!python())
    {
      for (const Source* s : members)
      {
        if (s->kind == Kind::Python)
          Warn(fmt::format("{}: install the GxBake Python tool to convert this file", s->rel));
      }
      continue;
    }

    const std::string tmp = cache_dir + "python-out/";
    File::DeleteDirRecursively(tmp);
    File::CreateFullPath(tmp);
    std::vector<std::string> args = *python();
    args.push_back("convert");
    for (const Source* s : members)
      args.push_back(s->host);
    for (const char* a : {"-o", tmp.c_str(), "--no-pack", "--cache"})
      args.push_back(a);
    args.push_back(cache_dir + "python-cmpr");
    INFO_LOG_FMT(DISCIO, "GxBake: converting {} file(s) in assets/{} with the Python tool",
                 members.size(), folder);
    const std::string log_path = cache_dir + "python.log";
    const auto code = RunProcess(args, log_path);
    if (code != 0)
    {
      Warn(fmt::format("the Python tool failed on assets/{} (see {})", folder, log_path));
      carry(key);
      if (it != state.end())
        add_entries(it->second);
      continue;
    }

    std::map<std::string, const Source*> image_stems;
    for (const Source* s : members)
    {
      if (IMAGE_EXTS.contains(s->ext))
        image_stems.emplace(s->stem, s);
    }
    UnitState u{fp, {}};
    std::set<std::string> keep;
    bool deferred = false;
    std::error_code ec;
    const fs::path tmp_root = StringToPath(tmp);
    for (auto oit = fs::recursive_directory_iterator(tmp_root, ec);
         oit != fs::recursive_directory_iterator(); oit.increment(ec))
    {
      if (ec || !oit->is_regular_file(ec))
        continue;
      const std::string rel =
          PathToString(oit->path().lexically_relative(tmp_root).generic_u8string());
      if (rel == "gxbake-manifest.json")
        continue;
      const auto data = ReadFile(PathToString(oit->path()));
      if (!data)
        continue;
      if (rel.starts_with("media/") && rel.ends_with(".gct") &&
          rel.find('/', 6) == std::string::npos)
      {
        const std::string stem = rel.substr(6, rel.size() - 10);
        if (const auto img = image_stems.find(stem); img != image_stems.end())
        {
          const std::string entry_file = cache_dir + "entries/py-" + Md5Hex(img->second->rel) +
                                         ".gct";
          WriteFileAtomic(entry_file, *data);
          u.outputs.push_back("#" + img->second->rel + "|" + entry_file);
          continue;
        }
      }
      const std::string out_rel = Join(gamedir, rel);
      if (!Commit(out_rel, *data, live, &updated))
      {
        deferred = true;
        continue;
      }
      keep.insert(out_rel);
      u.outputs.push_back(out_rel);
    }
    File::DeleteDirRecursively(tmp);
    converted += static_cast<int>(members.size());
    if (deferred)
    {
      carry(key);
      if (it != state.end())
        add_entries(it->second);
      continue;
    }
    if (it != state.end())
    {
      // Files this folder used to get from the Python tool may now be baked natively.
      for (const auto& [other_key, other] : next)
        keep.insert(other.outputs.begin(), other.outputs.end());
      RemoveOutputs(it->second.outputs, keep);
    }
    next[key] = u;
    add_entries(u);
  }

  // --- One media/textures.gct per gamedir -----------------------------------------------------
  for (auto& [gamedir, entries] : packs)
  {
    std::ranges::sort(entries, ComparePathParts, &PackEntry::rel);
    std::string fp_text;
    for (const PackEntry& e : entries)
      fp_text += e.rel + "=" + e.fingerprint + ";";
    const std::string key = "pack:" + gamedir;
    const std::string fp = Md5Hex(fp_text);
    const std::string out_rel = Join(gamedir, "media/textures.gct");
    const auto it = state.find(key);
    if (it != state.end() && it->second.fingerprint == fp && File::IsFile(OutPath(out_rel)))
    {
      next[key] = it->second;
      continue;
    }
    std::vector<GctEntry> all;
    for (const PackEntry& e : entries)
    {
      const auto data = ReadFile(e.entry_file);
      const auto parsed = data ? ReadGct(*data) : std::nullopt;
      if (parsed)
        all.insert(all.end(), parsed->begin(), parsed->end());
    }
    if (all.size() > 512)
      Warn(fmt::format("{} holds {} textures; the engine reads at most 512", out_rel, all.size()));
    if (!Commit(out_rel, WriteGct(all), live, &updated))
    {
      carry(key);
      continue;
    }
    next[key] = {fp, {out_rel}};
  }

  // Outputs of sources that are gone, unless something else produces the same file now.
  std::set<std::string> claimed;
  for (const auto& [key, u] : next)
    claimed.insert(u.outputs.begin(), u.outputs.end());
  for (const auto& [key, u] : state)
  {
    if (next.contains(key))
      continue;
    if (live)
    {
      next[key] = u;  // the running disc may still read them
      continue;
    }
    std::vector<std::string> plain;
    for (const std::string& o : u.outputs)
    {
      if (o.starts_with("#"))
        File::Delete(o.substr(o.find('|') + 1), File::IfAbsentBehavior::NoConsoleWarning);
      else
        plain.push_back(o);
    }
    RemoveOutputs(plain, claimed);
  }
  state = std::move(next);
  SaveState();

  if (converted > 0)
  {
    NOTICE_LOG_FMT(DISCIO, "GxBake: baked {} asset(s) from {}{}", converted, assets_dir,
                   live ? " (live)" : "");
  }

  if (files)
  {
    std::map<std::string, std::string> disc;
    for (const Source& s : sources)
    {
      if (s.kind == Kind::Passthrough || s.kind == Kind::PythonAndPassthrough ||
          s.kind == Kind::NativeGoldSrc)
      {
        disc[s.rel] = s.host;
      }
    }
    for (const auto& [key, u] : state)
    {
      for (const std::string& o : u.outputs)
      {
        if (!o.starts_with("#") && File::IsFile(OutPath(o)))
          disc[o] = OutPath(o);
      }
    }
    for (auto& [disc_path, host] : disc)
      files->push_back({disc_path, host});
  }
  return updated;
}

AssetBaker::AssetBaker(std::string assets_dir) : m_impl(std::make_unique<Impl>())
{
  if (!assets_dir.empty() && assets_dir.back() != '/')
    assets_dir += '/';
  m_assets_dir = assets_dir;
  m_impl->assets_dir = assets_dir;
  const std::string& user_cache = File::GetUserPath(D_CACHE_IDX);
  if (!user_cache.empty())
  {
    m_impl->cache_dir = user_cache + "GxBake/" + Md5Hex(assets_dir).substr(0, 16) + "/";
  }
  else
  {
    // No user folder (some dolphin-tool commands): keep the cache beside assets/, off the disc.
    m_impl->cache_dir = assets_dir + "../.gxbake-cache/";
  }
}

AssetBaker::~AssetBaker() = default;

std::vector<BakedFile> AssetBaker::Bake()
{
  std::lock_guard lk(s_bake_mutex);
  std::vector<BakedFile> files;
  m_impl->Run(false, &files);
  return files;
}

std::vector<std::string> AssetBaker::BakeLive()
{
  std::lock_guard lk(s_bake_mutex);
  return m_impl->Run(true, nullptr);
}

std::string AssetBaker::FindAssetsDir(const std::string& partition_root)
{
  if (!Config::Get(Config::GXBAKE_ENABLED))
    return {};
  std::string dir = partition_root;
  if (!dir.empty() && dir.back() != '/')
    dir += '/';
  dir += "assets/";
  return File::IsDirectory(dir) ? dir : std::string();
}

struct AssetWatcher::Impl
{
  std::shared_ptr<AssetBaker> baker;
  std::thread thread;
  std::mutex mutex;
  std::condition_variable cv;
  bool stop = false;

  std::string Signature() const
  {
    std::string sig;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             StringToPath(baker->GetAssetsDir()), fs::directory_options::skip_permission_denied,
             ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
    {
      if (ec)
        break;
      if (!it->is_regular_file(ec))
        continue;
      const auto t = it->last_write_time(ec).time_since_epoch().count();
      sig += fmt::format("{}|{}|{};", PathToString(it->path()), it->file_size(ec), t);
    }
    return Md5Hex(sig);
  }

  void Loop()
  {
    std::string last = Signature();
    std::unique_lock lk(mutex);
    while (!cv.wait_for(lk, std::chrono::seconds(1), [this] { return stop; }))
    {
      lk.unlock();
      const std::string sig = Signature();
      if (sig != last)
      {
        // Let editors finish writing before reading the file.
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        last = Signature();
        const auto updated = baker->BakeLive();
        for (const std::string& path : updated)
          NOTICE_LOG_FMT(DISCIO, "GxBake: updated {} in the running game", path);
      }
      lk.lock();
    }
  }
};

AssetWatcher::AssetWatcher(std::shared_ptr<AssetBaker> baker) : m_impl(std::make_unique<Impl>())
{
  m_impl->baker = std::move(baker);
  m_impl->thread = std::thread([this] { m_impl->Loop(); });
}

AssetWatcher::~AssetWatcher()
{
  {
    std::lock_guard lk(m_impl->mutex);
    m_impl->stop = true;
  }
  m_impl->cv.notify_all();
  m_impl->thread.join();
}
}  // namespace GxBake
