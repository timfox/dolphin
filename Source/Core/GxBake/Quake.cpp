// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

#include "GxBake/Quake.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>

#include "GxBake/Studio.h"

namespace GxBake
{
namespace fs = std::filesystem;

namespace
{
using Bytes = std::vector<uint8_t>;

bool Has(const Bytes& d, int64_t offset, int64_t size)
{
  return offset >= 0 && size >= 0 && offset + size <= static_cast<int64_t>(d.size());
}

int32_t S32(const Bytes& d, size_t p)
{
  uint32_t v;
  std::memcpy(&v, d.data() + p, 4);
  return static_cast<int32_t>(v);
}

float F32(const Bytes& d, size_t p)
{
  float v;
  std::memcpy(&v, d.data() + p, 4);
  return v;
}

uint16_t U16(const Bytes& d, size_t p)
{
  return static_cast<uint16_t>(d[p] | (d[p + 1] << 8));
}

void Put32(Bytes* out, uint32_t v)
{
  for (int i = 0; i < 4; i++)
    out->push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void PutF32(Bytes* out, float v)
{
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  Put32(out, bits);
}

std::optional<Bytes> ReadAll(const fs::path& path)
{
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return std::nullopt;
  return Bytes(std::istreambuf_iterator<char>(f), {});
}

std::string Lower(std::string s)
{
  for (char& c : s)
  {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c + 32);
  }
  return s;
}

std::string Utf8(const fs::path& p)
{
  const auto u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
}

fs::path FromUtf8(std::string_view s)
{
  return fs::path(std::u8string(s.begin(), s.end()));
}

// quake._ancestors: the folder itself (or the file's folder) and up to |depth| parents.
std::vector<fs::path> Ancestors(const fs::path& start, int depth = 5)
{
  std::error_code ec;
  fs::path dir = fs::is_directory(start, ec) ? start : start.parent_path();
  std::vector<fs::path> out = {dir};
  for (int i = 0; i < depth; i++)
  {
    const fs::path parent = dir.parent_path();
    if (parent.empty() || parent == dir)
      break;
    out.push_back(parent);
    dir = parent;
  }
  return out;
}

// sorted(base.glob(pattern)) for "*.pak" or "*.PAK" (case-sensitive, like glob on Linux).
std::vector<fs::path> Paks(const fs::path& base, std::string_view ext)
{
  std::vector<fs::path> out;
  std::error_code ec;
  for (auto it = fs::directory_iterator(base, ec); !ec && it != fs::directory_iterator();
       it.increment(ec))
  {
    std::string name = Utf8(it->path().filename());
#ifdef _WIN32
    // Windows globbing ignores case, so both patterns match every .pak.
    if (Lower(name).ends_with(Lower(std::string(ext))))
#else
    if (name.ends_with(ext))
#endif
      out.push_back(it->path());
  }
  std::ranges::sort(out, {}, [](const fs::path& p) { return Utf8(p); });
  return out;
}

// --- PCX (Pillow's PcxImagePlugin, 8-bit with a palette) ---------------------------------------

struct Pcx
{
  int width = 0, height = 0;
  Bytes indices;
  Bytes palette;  // 768 bytes
};

std::optional<Pcx> DecodePcx(const Bytes& d, std::string* error)
{
  if (d.size() < 128 || d[0] != 10 || (d[1] != 0 && d[1] != 2 && d[1] != 3 && d[1] != 5))
  {
    *error = "not a PCX file";
    return std::nullopt;
  }
  const auto i16 = [&](size_t p) { return static_cast<int>(U16(d, p)); };
  const int x0 = i16(4), y0 = i16(6), x1 = i16(8) + 1, y1 = i16(10) + 1;
  if (x1 <= x0 || y1 <= y0)
  {
    *error = "bad PCX image size";
    return std::nullopt;
  }
  const int version = d[1], bits = d[3], planes = d[65];
  const int provided_stride = i16(66);
  // Only 8-bit images with a non-grey palette open in mode "P".
  bool p_mode = false;
  if (version == 5 && bits == 8 && planes == 1 && d.size() >= 769 && d[d.size() - 769] == 12)
  {
    const uint8_t* pal = d.data() + d.size() - 768;
    for (int i = 0; i < 256 && !p_mode; i++)
      p_mode = pal[i * 3] != i || pal[i * 3 + 1] != i || pal[i * 3 + 2] != i;
  }
  if (!p_mode)
  {
    *error = "not an 8-bit PCX";
    return std::nullopt;
  }
  Pcx pcx;
  pcx.width = x1 - x0;
  pcx.height = y1 - y0;
  int stride = (pcx.width * bits + 7) / 8;
  if (provided_stride != stride)
    stride += stride % 2;
  const size_t line = static_cast<size_t>(stride) * planes;
  Bytes raw;
  raw.reserve(line * pcx.height);
  size_t p = 128;
  while (raw.size() < line * pcx.height)
  {
    if (p >= d.size())
    {
      *error = "image file is truncated";
      return std::nullopt;
    }
    const uint8_t b = d[p++];
    if ((b & 0xC0) == 0xC0)
    {
      if (p >= d.size())
      {
        *error = "image file is truncated";
        return std::nullopt;
      }
      raw.insert(raw.end(), b & 0x3F, d[p++]);
    }
    else
    {
      raw.push_back(b);
    }
  }
  pcx.indices.resize(static_cast<size_t>(pcx.width) * pcx.height);
  for (int y = 0; y < pcx.height; y++)
  {
    std::memcpy(&pcx.indices[static_cast<size_t>(y) * pcx.width], &raw[y * line],
                static_cast<size_t>(pcx.width));
  }
  pcx.palette.assign(d.end() - 768, d.end());
  return pcx;
}

Bytes HlHeader(int32_t type, int32_t tex, float radius, int32_t w, int32_t h, int32_t nframes,
               float beam, int32_t sync, const Bytes& palette)
{
  Bytes out = {'I', 'D', 'S', 'P'};
  Put32(&out, 2);
  Put32(&out, static_cast<uint32_t>(type));
  Put32(&out, static_cast<uint32_t>(tex));
  PutF32(&out, radius);
  Put32(&out, static_cast<uint32_t>(w));
  Put32(&out, static_cast<uint32_t>(h));
  Put32(&out, static_cast<uint32_t>(nframes));
  PutF32(&out, beam);
  Put32(&out, static_cast<uint32_t>(sync));
  out.push_back(0);  // 256 colours
  out.push_back(1);
  Bytes pal(palette.begin(), palette.begin() + std::min<size_t>(768, palette.size()));
  out.insert(out.end(), pal.begin(), pal.end());
  return out;
}

constexpr int32_t SPR_ALPHATEST = 3;
constexpr int32_t SPR_VP_PARALLEL = 2;
}  // namespace

std::optional<Bytes> ReadPakMember(const fs::path& pak, std::string_view name)
{
  std::ifstream f(pak, std::ios::binary);
  if (!f)
    return std::nullopt;
  uint8_t head[12];
  if (!f.read(reinterpret_cast<char*>(head), 12) || std::memcmp(head, "PACK", 4) != 0)
    return std::nullopt;
  int32_t dirofs, dirlen;
  std::memcpy(&dirofs, head + 4, 4);
  std::memcpy(&dirlen, head + 8, 4);
  if (dirofs < 0 || dirlen < 0)
    return std::nullopt;
  Bytes table(static_cast<size_t>(dirlen));
  f.seekg(dirofs);
  if (!f.read(reinterpret_cast<char*>(table.data()), dirlen))
    return std::nullopt;
  std::string want = Lower(std::string(name));
  std::replace(want.begin(), want.end(), '\\', '/');
  for (size_t i = 0; i + 64 <= table.size(); i += 64)
  {
    const char* raw = reinterpret_cast<const char*>(&table[i]);
    const std::string member(raw, strnlen(raw, 56));
    if (Lower(member) != want)
      continue;
    const int32_t pos = S32(table, i + 56), size = S32(table, i + 60);
    if (pos < 0 || size < 0)
      return std::nullopt;
    Bytes data(static_cast<size_t>(size));
    f.clear();
    f.seekg(pos);
    f.read(reinterpret_cast<char*>(data.data()), size);
    data.resize(static_cast<size_t>(f.gcount()));
    return data;
  }
  return std::nullopt;
}

std::optional<Bytes> FindInTree(const fs::path& start, std::string_view rel,
                                const std::vector<std::string>& subdirs, fs::path* found)
{
  const std::vector<fs::path> ancestors = Ancestors(start);
  const auto bases = [&](const fs::path& d) {
    std::vector<fs::path> out = {d};
    for (const std::string& s : subdirs)
      out.push_back(d / FromUtf8(s));
    return out;
  };
  for (const fs::path& d : ancestors)
  {
    for (const fs::path& base : bases(d))
    {
      const fs::path p = base / FromUtf8(rel);
      std::error_code ec;
      if (!fs::is_regular_file(p, ec))
        continue;
      if (auto data = ReadAll(p))
      {
        if (found)
          *found = p;
        return data;
      }
    }
  }
  for (const fs::path& d : ancestors)
  {
    for (const fs::path& base : bases(d))
    {
      std::vector<fs::path> paks = Paks(base, ".pak");
      for (const fs::path& p : Paks(base, ".PAK"))
        paks.push_back(p);
      for (const fs::path& pak : paks)
      {
        if (auto data = ReadPakMember(pak, rel))
        {
          if (found)
            *found = pak;
          return data;
        }
      }
    }
  }
  return std::nullopt;
}

std::optional<Bytes> QuakePalette(const fs::path& start, fs::path* found)
{
  auto data = FindInTree(start, "gfx/palette.lmp", {"id1"}, found);
  if (!data || data->size() < 768)
    return std::nullopt;
  data->resize(768);
  return data;
}

const Bytes& GreyPalette()
{
  static const Bytes grey = [] {
    Bytes p;
    for (int i = 0; i < 256; i++)
      p.insert(p.end(), 3, static_cast<uint8_t>(i));
    return p;
  }();
  return grey;
}

bool IsQuakeSprite(const Bytes& d)
{
  return d.size() >= 8 && std::memcmp(d.data(), "IDSP", 4) == 0 && S32(d, 4) == 1;
}

bool IsQuake2Sprite(const Bytes& d)
{
  return d.size() >= 4 && std::memcmp(d.data(), "IDS2", 4) == 0;
}

std::optional<Bytes> QuakeSpriteToHl(const Bytes& d, const Bytes& palette)
{
  if (!IsQuakeSprite(d) || d.size() < 36)
    return std::nullopt;
  Bytes out = HlHeader(S32(d, 8), SPR_ALPHATEST, F32(d, 12), S32(d, 16), S32(d, 20), S32(d, 24),
                       F32(d, 28), S32(d, 32), palette);
  out.insert(out.end(), d.begin() + 36, d.end());
  return out;
}

std::optional<Bytes> Quake2SpriteToHl(const fs::path& path, const Bytes& d, std::string* error,
                                      std::vector<fs::path>* deps)
{
  if (!IsQuake2Sprite(d) || d.size() < 12)
  {
    *error = "not a Quake II sprite";
    return std::nullopt;
  }
  const int32_t nframes = S32(d, 8);
  struct Frame
  {
    int32_t ox, oy;
    Pcx pcx;
  };
  std::vector<Frame> frames;
  Bytes palette;
  for (int32_t i = 0; i < nframes; i++)
  {
    const size_t p = 12 + static_cast<size_t>(i) * 80;
    if (!Has(d, static_cast<int64_t>(p), 80))
    {
      *error = "truncated sprite";
      return std::nullopt;
    }
    const int32_t ox = S32(d, p + 8), oy = S32(d, p + 12);
    const char* raw = reinterpret_cast<const char*>(&d[p + 16]);
    const std::string name(raw, strnlen(raw, 64));  // Latin-1 in the file
    fs::path where;
    auto data = FindInTree(path, name, {"baseq2"}, &where);
    if (!data)
    {
      const size_t slash = name.find_last_of("/\\");
      data = FindInTree(path, slash == std::string::npos ? name : name.substr(slash + 1), {},
                        &where);
    }
    if (!data)
    {
      *error = "frame " + std::to_string(i) + ": " + name + " not found";
      return std::nullopt;
    }
    if (deps)
      deps->push_back(where);
    std::string why;
    auto pcx = DecodePcx(*data, &why);
    if (!pcx)
    {
      *error = "frame " + std::to_string(i) + ": " + name + " is not an 8-bit PCX";
      return std::nullopt;
    }
    if (palette.empty())
      palette = pcx->palette;
    frames.push_back({-ox, oy, std::move(*pcx)});
  }
  if (palette.empty())
  {
    *error = "sprite has no frames";
    return std::nullopt;
  }
  int w = 0, h = 0;
  for (const Frame& f : frames)
  {
    w = std::max(w, f.pcx.width);
    h = std::max(h, f.pcx.height);
  }
  const float radius = static_cast<float>(std::hypot(w / 2.0, h / 2.0));
  Bytes out = HlHeader(SPR_VP_PARALLEL, SPR_ALPHATEST, radius, w, h,
                       static_cast<int32_t>(frames.size()), 0.0f, 0, palette);
  for (const Frame& f : frames)
  {
    Put32(&out, 0);
    Put32(&out, static_cast<uint32_t>(f.ox));
    Put32(&out, static_cast<uint32_t>(f.oy));
    Put32(&out, static_cast<uint32_t>(f.pcx.width));
    Put32(&out, static_cast<uint32_t>(f.pcx.height));
    out.insert(out.end(), f.pcx.indices.begin(), f.pcx.indices.end());
  }
  return out;
}

bool IsAliasModel(const Bytes& d)
{
  return d.size() >= 8 && std::memcmp(d.data(), "IDPO", 4) == 0 && S32(d, 4) == 6;
}

std::optional<ImportedModel> ImportAliasModel(const Bytes& d, const std::string& name,
                                              const Bytes& palette, bool palette_found,
                                              int skin_max_edge)
{
  // quake.read_alias
  if (!IsAliasModel(d) || d.size() < 84)
    return std::nullopt;
  const double scale[3] = {F32(d, 8), F32(d, 12), F32(d, 16)};
  const double origin[3] = {F32(d, 20), F32(d, 24), F32(d, 28)};
  const int32_t numskins = S32(d, 48), sw = S32(d, 52), sh = S32(d, 56);
  const int32_t numverts = S32(d, 60), numtris = S32(d, 64), numframes = S32(d, 68);
  if (numskins < 0 || sw <= 0 || sh <= 0 || numverts <= 0 || numtris <= 0 || numframes <= 0)
    return std::nullopt;
  const int64_t skin_size = static_cast<int64_t>(sw) * sh;
  int64_t pos = 84;
  std::optional<int64_t> skin;
  for (int32_t i = 0; i < numskins; i++)
  {
    if (!Has(d, pos, 4))
      return std::nullopt;
    const int32_t group = S32(d, pos);
    pos += 4;
    if (group == 0)
    {
      if (!skin)
        skin = pos;
      pos += skin_size;
    }
    else
    {
      if (!Has(d, pos, 4))
        return std::nullopt;
      const int32_t n = S32(d, pos);
      pos += 4 + 4 * static_cast<int64_t>(n);
      if (!skin)
        skin = pos;
      pos += n * skin_size;
    }
  }
  if (!Has(d, pos, static_cast<int64_t>(numverts) * 12 + static_cast<int64_t>(numtris) * 16))
    return std::nullopt;
  const int64_t st_off = pos;
  pos += static_cast<int64_t>(numverts) * 12;
  const int64_t tri_off = pos;
  pos += static_cast<int64_t>(numtris) * 16;
  // The first frame (of the first frame group).
  if (!Has(d, pos, 4))
    return std::nullopt;
  const int32_t type = S32(d, pos);
  pos += 4;
  if (type != 0)
  {
    if (!Has(d, pos, 4))
      return std::nullopt;
    pos += 4 + 8 + 4 * static_cast<int64_t>(S32(d, pos));
  }
  const int64_t frame_name = pos + 8;
  pos += 24;
  if (!Has(d, pos, static_cast<int64_t>(numverts) * 4))
    return std::nullopt;
  std::vector<std::array<double, 3>> verts(numverts);
  for (int32_t v = 0; v < numverts; v++)
  {
    for (int k = 0; k < 3; k++)
      verts[v][k] = static_cast<double>(d[pos + v * 4 + k]) * scale[k] + origin[k];
  }

  ImportedModel model;
  model.name = name;
  if (!palette_found)
    model.warnings.push_back("Quake palette (gfx/palette.lmp) not found; greyscale skin");

  RgbaImage rgba;
  if (skin && Has(d, *skin, skin_size))
  {
    rgba.width = sw;
    rgba.height = sh;
    rgba.pixels.resize(static_cast<size_t>(skin_size) * 4);
    for (int64_t i = 0; i < skin_size; i++)
    {
      std::memcpy(&rgba.pixels[i * 4], &palette[d[*skin + i] * 3], 3);
      rgba.pixels[i * 4 + 3] = 255;
    }
  }
  else
  {
    rgba.width = rgba.height = 8;
    rgba.pixels.assign(8 * 8 * 4, 180);
  }

  // The frame count: every frame, and every frame of every group.
  {
    int64_t total = 0, p = tri_off + static_cast<int64_t>(numtris) * 16;
    for (int32_t f = 0; f < numframes && Has(d, p, 4); f++)
    {
      const int32_t t = S32(d, p);
      p += 4;
      int32_t count = 1;
      if (t != 0)
      {
        count = Has(d, p, 4) ? S32(d, p) : 0;
        p += 4 + 8 + 4 * static_cast<int64_t>(count);
      }
      total += count;
      p += static_cast<int64_t>(count) * (24 + static_cast<int64_t>(numverts) * 4);
    }
    if (total > 1)
    {
      const char* raw = reinterpret_cast<const char*>(&d[frame_name]);
      const std::string first(raw, strnlen(raw, 16));
      model.warnings.push_back(std::to_string(total) +
                               " vertex-animation frames: studio models can't morph, so frame 0 (" +
                               (first.empty() ? "unnamed" : first) + ") is converted");
    }
  }

  // Back-facing triangles shift on-seam vertices by half the skin width.
  std::map<std::pair<int32_t, int>, uint32_t> keys;
  std::vector<std::array<double, 3>> out_pos;
  std::vector<std::array<float, 2>> out_uv;
  std::vector<std::array<int64_t, 3>> faces;
  for (int32_t t = 0; t < numtris; t++)
  {
    const int64_t p = tri_off + static_cast<int64_t>(t) * 16;
    const int32_t front = S32(d, p);
    std::array<int64_t, 3> face{};
    for (int k = 0; k < 3; k++)
    {
      const int32_t v = S32(d, p + 4 + k * 4);
      if (v < 0 || v >= numverts)
        return std::nullopt;
      const int32_t onseam = S32(d, st_off + v * 12), s0 = S32(d, st_off + v * 12 + 4);
      const int32_t t0 = S32(d, st_off + v * 12 + 8);
      const int shift = onseam != 0 && !front ? 1 : 0;
      const auto key = std::pair{v, shift};
      auto it = keys.find(key);
      if (it == keys.end())
      {
        it = keys.emplace(key, static_cast<uint32_t>(out_pos.size())).first;
        out_pos.push_back(verts[v]);
        const int64_t s = static_cast<int64_t>(s0) + (shift ? sw / 2 : 0);
        out_uv.push_back({static_cast<float>((static_cast<double>(s) + 0.5) / sw),
                          static_cast<float>((static_cast<double>(t0) + 0.5) / sh)});
      }
      face[k] = it->second;
    }
    faces.push_back(face);
  }

  // Quake fronts are clockwise; the studio writer wants counter-clockwise. Check by volume.
  const auto cross = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return std::array<double, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                                 a[0] * b[1] - a[1] * b[0]};
  };
  const auto sub = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return std::array<double, 3>{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  };
  std::array<double, 3> cen{};
  for (const auto& p : out_pos)
  {
    for (int k = 0; k < 3; k++)
      cen[k] += p[k];
  }
  for (double& c : cen)
    c /= static_cast<double>(out_pos.size());
  double vol = 0.0;
  for (const auto& f : faces)
  {
    const auto a = sub(out_pos[f[0]], cen);
    const auto c = cross(sub(out_pos[f[1]], cen), sub(out_pos[f[2]], cen));
    vol += a[0] * c[0] + a[1] * c[1] + a[2] * c[2];
  }
  if (vol < 0)
  {
    for (auto& f : faces)
      std::swap(f[1], f[2]);
  }
  std::vector<std::array<double, 3>> nrm(out_pos.size(), {0.0, 0.0, 0.0});
  std::vector<std::array<double, 3>> fn;
  for (const auto& f : faces)
    fn.push_back(cross(sub(out_pos[f[1]], out_pos[f[0]]), sub(out_pos[f[2]], out_pos[f[0]])));
  for (int k = 0; k < 3; k++)
  {
    for (size_t i = 0; i < faces.size(); i++)
    {
      for (int j = 0; j < 3; j++)
        nrm[faces[i][k]][j] += fn[i][j];
    }
  }

  StudioSubmesh mesh;
  mesh.material = 0;
  for (size_t i = 0; i < out_pos.size(); i++)
  {
    const auto& n = nrm[i];
    const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    std::array<double, 3> unit = {0.0, 0.0, 1.0};
    if (len > 1e-9)
    {
      const double l = std::max(len, 1e-9);
      unit = {n[0] / l, n[1] / l, n[2] / l};
    }
    mesh.positions.push_back({static_cast<float>(out_pos[i][0]), static_cast<float>(out_pos[i][1]),
                              static_cast<float>(out_pos[i][2])});
    mesh.normals.push_back(
        {static_cast<float>(unit[0]), static_cast<float>(unit[1]), static_cast<float>(unit[2])});
  }
  mesh.uvs = std::move(out_uv);
  for (const auto& f : faces)
  {
    mesh.tris.push_back(
        {static_cast<uint32_t>(f[0]), static_cast<uint32_t>(f[1]), static_cast<uint32_t>(f[2])});
  }
  model.materials.push_back({name + "_skin", FitSkin(rgba, skin_max_edge), false, "skin 0"});
  model.meshes.push_back(std::move(mesh));
  model.sequences.push_back({"idle", 10.0, model.RestFrames(), true});
  return model;
}
}  // namespace GxBake
