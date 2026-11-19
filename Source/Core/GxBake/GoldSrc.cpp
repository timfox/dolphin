// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

#include "GxBake/GoldSrc.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "GxBake/Cmpr.h"
#include "GxBake/Encode.h"
#include "GxBake/Md5.h"
#include "GxBake/Resample.h"

namespace GxBake
{
namespace
{
bool Has(const std::vector<uint8_t>& d, int64_t offset, int64_t size)
{
  return offset >= 0 && size >= 0 && offset + size <= static_cast<int64_t>(d.size());
}

int32_t S32(const std::vector<uint8_t>& d, size_t p)
{
  return static_cast<int32_t>(d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) |
                              (static_cast<uint32_t>(d[p + 3]) << 24));
}

uint32_t U32(const std::vector<uint8_t>& d, size_t p)
{
  return static_cast<uint32_t>(S32(d, p));
}

int16_t S16(const std::vector<uint8_t>& d, size_t p)
{
  return static_cast<int16_t>(d[p] | (d[p + 1] << 8));
}

uint16_t U16(const std::vector<uint8_t>& d, size_t p)
{
  return static_cast<uint16_t>(d[p] | (d[p + 1] << 8));
}

void Put32(std::vector<uint8_t>* out, uint32_t v)
{
  for (int i = 0; i < 4; i++)
    out->push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void Put16(std::vector<uint8_t>* out, uint16_t v)
{
  out->push_back(static_cast<uint8_t>(v));
  out->push_back(static_cast<uint8_t>(v >> 8));
}

// Python floor division, for negative sprite origins.
int32_t FloorDiv(int32_t a, int32_t b)
{
  int32_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0)))
    q--;
  return q;
}

// A 16-byte lump/miptex name as the Python tool reads it: up to the first NUL, latin-1,
// lower-cased (str.lower() also folds latin-1 capitals).
std::string LumpName(const uint8_t* raw)
{
  std::string name;
  for (int i = 0; i < 16 && raw[i] != 0; i++)
  {
    uint8_t c = raw[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
      c += 0x20;
    name.push_back(static_cast<char>(c));
  }
  return name;
}

int BitLength(uint32_t v)
{
  int n = 0;
  while (v)
  {
    n++;
    v >>= 1;
  }
  return n;
}

struct Miptex
{
  uint32_t width = 0;
  uint32_t height = 0;
  const uint8_t* pixels = nullptr;
  const uint8_t* palette = nullptr;  // 768 bytes
};

// goldsrc.wt_read_miptex
std::optional<Miptex> ReadMiptex(const std::vector<uint8_t>& data, int64_t offset)
{
  if (!Has(data, offset, 28))
    return std::nullopt;
  const uint32_t w = U32(data, offset + 16);
  const uint32_t h = U32(data, offset + 20);
  const uint32_t m0 = U32(data, offset + 24);
  if (!m0 || !w || !h || static_cast<uint64_t>(w) * h > (1u << 20))
    return std::nullopt;
  const int64_t n = static_cast<int64_t>(w) * h;
  const int64_t end = offset + m0 + n + n / 4 + n / 16 + n / 64;
  if (end + 2 > static_cast<int64_t>(data.size()))
    return std::nullopt;
  if (U16(data, end) != 256 || end + 2 + 768 > static_cast<int64_t>(data.size()))
    return std::nullopt;
  return Miptex{w, h, data.data() + offset + m0, data.data() + end + 2};
}

// gct.wt_cache_key
std::string CacheKey(uint32_t w, uint32_t h, const uint8_t* pixels, const uint8_t* palette,
                     bool alpha)
{
  Md5 md5;
  const uint8_t a = alpha ? 1 : 0;
  md5.Update(&a, 1);
  std::vector<uint8_t> dims;
  Put32(&dims, w);
  Put32(&dims, h);
  md5.Update(dims.data(), dims.size());
  md5.Update(pixels, static_cast<size_t>(w) * h);
  md5.Update(palette, 768);
  md5.Update("2", 1);
  return md5.HexDigest();
}

struct Encoded
{
  std::string key;
  uint16_t width = 0;
  uint16_t height = 0;
  uint8_t levels = 0;
  std::vector<uint8_t> body;
};

// gct.wt_encode: palette texture to a CMPR mip chain with power-of-two sides.
Encoded Encode(uint32_t w, uint32_t h, const uint8_t* pixels, const uint8_t* palette, bool alpha)
{
  const size_t n = static_cast<size_t>(w) * h;
  std::vector<uint8_t> rgb(n * 3), opaque(n);
  for (size_t i = 0; i < n; i++)
  {
    std::memcpy(&rgb[i * 3], palette + pixels[i] * 3, 3);
    opaque[i] = alpha ? pixels[i] != 255 : 1;
  }
  const int tw = 1 << std::max(3, BitLength(w - 1));
  const int th = 1 << std::max(3, BitLength(h - 1));
  if (tw != static_cast<int>(w) || th != static_cast<int>(h))
  {
    rgb = Resize(rgb, w, h, 3, tw, th, alpha ? Filter::Nearest : Filter::Bicubic);
    opaque = ResizeMaskNearest(opaque, w, h, tw, th);
  }
  Encoded e;
  e.width = static_cast<uint16_t>(tw);
  e.height = static_cast<uint16_t>(th);
  e.levels = static_cast<uint8_t>(EncodeMipChain(rgb, opaque, tw, th, true, 8, &e.body));
  return e;
}

std::optional<std::vector<uint8_t>> ReadAll(const std::filesystem::path& path)
{
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return std::nullopt;
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

// gct.wt_encode_cached: <cache>/<key>.bin holds "<HHH" width, height, levels, then the chain.
Encoded EncodeCached(uint32_t w, uint32_t h, const uint8_t* pixels, const uint8_t* palette,
                     bool alpha, const std::string& cache_dir)
{
  const std::string key = CacheKey(w, h, pixels, palette, alpha);
  std::filesystem::path item;
  if (!cache_dir.empty())
  {
    item = std::filesystem::path(reinterpret_cast<const char8_t*>(cache_dir.c_str())) / (key + ".bin");
    if (const auto cached = ReadAll(item); cached && cached->size() >= 6)
    {
      Encoded e;
      e.key = key;
      e.width = U16(*cached, 0);
      e.height = U16(*cached, 2);
      e.levels = static_cast<uint8_t>(U16(*cached, 4));
      e.body.assign(cached->begin() + 6, cached->end());
      return e;
    }
  }
  Encoded e = Encode(w, h, pixels, palette, alpha);
  e.key = key;
  if (!item.empty())
  {
    std::error_code ec;
    std::filesystem::create_directories(item.parent_path(), ec);
    std::vector<uint8_t> out;
    Put16(&out, e.width);
    Put16(&out, e.height);
    Put16(&out, e.levels);
    out.insert(out.end(), e.body.begin(), e.body.end());
    const auto tmp = item.string() + ".tmp";
    {
      std::ofstream f(tmp, std::ios::binary);
      f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    }
    std::filesystem::rename(tmp, item, ec);
  }
  return e;
}

GctEntry MakeEntry(const std::string& name, uint32_t w, uint32_t h, const uint8_t* pixels,
                   const uint8_t* palette, bool alpha, const std::string& cache_dir)
{
  Encoded e = EncodeCached(w, h, pixels, palette, alpha, cache_dir);
  GctEntry entry;
  entry.name = name;
  entry.width = e.width;
  entry.height = e.height;
  entry.levels = e.levels;
  entry.flags = alpha ? GCT_FLAG_ALPHA : 0;
  entry.tag = static_cast<uint16_t>(std::stoul(e.key.substr(0, 4), nullptr, 16));
  entry.data = std::move(e.body);
  return entry;
}

struct WadLump
{
  std::string name;
  int8_t kind;
  int32_t offset;
  int32_t disk_size;
  bool compressed;
};

std::optional<std::vector<WadLump>> WadDirectory(const std::vector<uint8_t>& data)
{
  if (!Has(data, 0, 12))
    return std::nullopt;
  const int32_t count = S32(data, 4);
  const int32_t table = S32(data, 8);
  if (count < 0 || !Has(data, table, static_cast<int64_t>(count) * 32))
    return std::nullopt;
  std::vector<WadLump> lumps;
  for (int32_t i = 0; i < count; i++)
  {
    const size_t p = static_cast<size_t>(table) + static_cast<size_t>(i) * 32;
    lumps.push_back({LumpName(&data[p + 16]), static_cast<int8_t>(data[p + 12]), S32(data, p),
                     S32(data, p + 4), data[p + 13] != 0});
  }
  return lumps;
}

struct Picture
{
  uint32_t width;
  uint32_t height;
  const uint8_t* pixels;
  const uint8_t* palette;
};

// goldsrc.read_wad_picture: a qpic (0x42) or font (0x46) lump.
std::optional<Picture> ReadPicture(const uint8_t* lump, int64_t size, int kind)
{
  const std::vector<uint8_t> head(lump, lump + std::min<int64_t>(size, 16));
  int64_t p;
  int32_t w, h;
  if (kind == 0x42)
  {
    if (size < 8)
      return std::nullopt;
    w = S32(head, 0);
    h = S32(head, 4);
    p = 8;
  }
  else
  {
    if (size < 16 + 1024)
      return std::nullopt;
    w = S32(head, 0);
    h = S32(head, 4);
    p = 16 + 1024;
    if (w <= 0)
      w = 256;  // fonts are 256 wide
  }
  if (!(w > 0 && w <= 4096) || !(h > 0 && h <= 4096))
    return std::nullopt;
  const int64_t n = static_cast<int64_t>(w) * h;
  if (p + n + 2 + 768 > size)
    return std::nullopt;
  if ((lump[p + n] | (lump[p + n + 1] << 8)) != 256)
    return std::nullopt;
  return Picture{static_cast<uint32_t>(w), static_cast<uint32_t>(h), lump + p, lump + p + n + 2};
}
}  // namespace

std::optional<std::vector<uint8_t>> LeanHlSprite(const std::vector<uint8_t>& d, int scale)
{
  if (d.size() < 44 || std::memcmp(d.data(), "IDSP", 4) != 0 || S32(d, 4) != 2)
    return std::nullopt;
  const int32_t type = S32(d, 8);
  const int32_t tex = S32(d, 12);
  const int32_t face = S32(d, 32);
  const int32_t sync = S32(d, 36);
  const int ncolors = S16(d, 40);
  if (ncolors <= 0 || ncolors > 256)
    return std::nullopt;
  const size_t pal_off = 42;
  const size_t frame_off = pal_off + static_cast<size_t>(ncolors) * 3;
  if (frame_off + 20 > d.size() || S32(d, frame_off) != 0)
    return std::nullopt;
  const int32_t ox = S32(d, frame_off + 4), oy = S32(d, frame_off + 8);
  const int32_t w = S32(d, frame_off + 12), h = S32(d, frame_off + 16);
  const size_t pix_off = frame_off + 20;
  if (w <= 0 || h <= 0 || pix_off + static_cast<size_t>(w) * h > d.size())
    return std::nullopt;
  if (scale < 1 || w % scale || h % scale)
    return std::nullopt;

  const int nw = w / scale, nh = h / scale;
  const uint8_t* pix = d.data() + pix_off;
  std::vector<uint8_t> out_pix(static_cast<size_t>(nw) * nh);
  // texFormat 3 = SPR_ALPHATEST: index 255 is transparent.
  const bool preserve_ink = tex == 3;
  for (int y = 0; y < nh; y++)
  {
    const int sy = y * scale;
    for (int x = 0; x < nw; x++)
    {
      const int sx = x * scale;
      uint8_t sample = pix[static_cast<size_t>(sy) * w + sx];
      if (preserve_ink && sample == 255)
      {
        for (int dy = 0; dy < scale && sample == 255; dy++)
        {
          for (int dx = 0; dx < scale; dx++)
          {
            const uint8_t p = pix[static_cast<size_t>(sy + dy) * w + sx + dx];
            if (p != 255)
            {
              sample = p;
              break;
            }
          }
        }
      }
      out_pix[static_cast<size_t>(y) * nw + x] = sample;
    }
  }

  const float radius = static_cast<float>(std::sqrt((nw * 0.5) * (nw * 0.5) + (nh * 0.5) * (nh * 0.5)));
  uint32_t radius_bits;
  std::memcpy(&radius_bits, &radius, 4);

  std::vector<uint8_t> out = {'I', 'D', 'S', 'P'};
  Put32(&out, 2);
  Put32(&out, static_cast<uint32_t>(type));
  Put32(&out, static_cast<uint32_t>(tex));
  Put32(&out, radius_bits);
  Put32(&out, static_cast<uint32_t>(nw));
  Put32(&out, static_cast<uint32_t>(nh));
  Put32(&out, 1);
  Put32(&out, static_cast<uint32_t>(face));
  Put32(&out, static_cast<uint32_t>(sync));
  Put16(&out, static_cast<uint16_t>(ncolors));
  out.insert(out.end(), d.begin() + pal_off, d.begin() + frame_off);
  Put32(&out, 0);
  Put32(&out, static_cast<uint32_t>(FloorDiv(ox, scale)));
  Put32(&out, static_cast<uint32_t>(FloorDiv(oy, scale)));
  Put32(&out, static_cast<uint32_t>(nw));
  Put32(&out, static_cast<uint32_t>(nh));
  out.insert(out.end(), out_pix.begin(), out_pix.end());
  return out;
}

bool SpriteNeedsPython(const std::vector<uint8_t>& d)
{
  if (d.size() >= 4 && std::memcmp(d.data(), "IDS2", 4) == 0)
    return true;  // Quake II
  return d.size() >= 8 && std::memcmp(d.data(), "IDSP", 4) == 0 && S32(d, 4) == 1;  // Quake
}

bool IsWad3(const std::vector<uint8_t>& d)
{
  return d.size() >= 4 && std::memcmp(d.data(), "WAD3", 4) == 0;
}

std::optional<std::vector<uint8_t>> BuildWadTexturePack(const std::vector<uint8_t>& data,
                                                        const std::string& cache_dir)
{
  if (!IsWad3(data))
    return std::nullopt;
  const auto lumps = WadDirectory(data);
  if (!lumps)
    return std::nullopt;

  // goldsrc.wt_ingest_wad_file(kinds=(0x43, 0x44)): first lump of a name wins.
  std::vector<std::pair<std::string, int32_t>> miptex;
  std::set<std::string> miptex_names;
  for (const WadLump& l : *lumps)
  {
    if ((l.kind != 0x43 && l.kind != 0x44) || l.compressed)
      continue;
    if (miptex_names.insert(l.name).second)
      miptex.emplace_back(l.name, l.offset);
  }

  std::vector<GctEntry> entries;
  for (const auto& [name, offset] : miptex)
  {
    const auto mip = ReadMiptex(data, offset);
    if (!mip)
      continue;
    entries.push_back(MakeEntry(name, mip->width, mip->height, mip->pixels, mip->palette,
                                name.starts_with("{"), cache_dir));
  }

  // goldsrc.wad_pictures: HUD pictures and console fonts (gfx.wad, fonts.wad).
  std::set<std::string> picture_names;
  for (const WadLump& l : *lumps)
  {
    const int kind = static_cast<uint8_t>(l.kind);
    if ((kind != 0x42 && kind != 0x46) || l.compressed || picture_names.contains(l.name))
      continue;
    // Python slices data[offset:offset + disk_size], which clamps to the file.
    if (l.offset < 0 || l.disk_size < 0 || static_cast<size_t>(l.offset) > data.size())
      continue;
    const int64_t size =
        std::min<int64_t>(l.disk_size, static_cast<int64_t>(data.size()) - l.offset);
    const auto pic = ReadPicture(data.data() + l.offset, size, kind);
    if (!pic)
      continue;
    picture_names.insert(l.name);
    if (miptex_names.contains(l.name))
      continue;
    const size_t n = static_cast<size_t>(pic->width) * pic->height;
    const bool alpha = std::find(pic->pixels, pic->pixels + n, 255) != pic->pixels + n;
    entries.push_back(
        MakeEntry(l.name, pic->width, pic->height, pic->pixels, pic->palette, alpha, cache_dir));
  }

  if (entries.empty())
    return std::vector<uint8_t>{};
  return WriteGct(entries);
}
}  // namespace GxBake
