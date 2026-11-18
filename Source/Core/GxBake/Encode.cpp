// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

#include "GxBake/Encode.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstring>

#include "GxBake/Cmpr.h"
#include "GxBake/Md5.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

namespace GxBake
{
namespace
{
int BitLength(int v)
{
  int n = 0;
  while (v > 0)
  {
    n++;
    v >>= 1;
  }
  return n;
}

// images._target_size: power-of-two sides, the long side clamped to max_edge.
void TargetSize(int w, int h, int max_edge, int* tw, int* th)
{
  *tw = 1 << std::max(3, BitLength(w - 1));
  *th = 1 << std::max(3, BitLength(h - 1));
  while (std::max(*tw, *th) > std::max(8, max_edge))
  {
    *tw = std::max(8, *tw / 2);
    *th = std::max(8, *th / 2);
  }
}

void Put32BE(std::vector<uint8_t>* out, uint32_t v)
{
  for (int i = 3; i >= 0; i--)
    out->push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void Put16BE(std::vector<uint8_t>* out, uint16_t v)
{
  out->push_back(static_cast<uint8_t>(v >> 8));
  out->push_back(static_cast<uint8_t>(v));
}

void Put32LE(std::vector<uint8_t>* out, uint32_t v)
{
  for (int i = 0; i < 4; i++)
    out->push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void Put16LE(std::vector<uint8_t>* out, uint16_t v)
{
  out->push_back(static_cast<uint8_t>(v));
  out->push_back(static_cast<uint8_t>(v >> 8));
}

uint32_t Get32BE(const uint8_t* p)
{
  return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

uint16_t Get16BE(const uint8_t* p)
{
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t Get32LE(const uint8_t* p)
{
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t Get16LE(const uint8_t* p)
{
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
}  // namespace

std::string LowerAscii(std::string s)
{
  for (char& c : s)
  {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

GctEntry EncodeImage(const RgbaImage& image, const ImageOptions& options, const std::string& name)
{
  const int w = image.width, h = image.height;
  int tw, th;
  TargetSize(w, h, options.max_edge, &tw, &th);

  bool use_alpha = options.alpha == ImageOptions::Alpha::On;
  if (options.alpha == ImageOptions::Alpha::Auto)
  {
    for (size_t i = 0; i < static_cast<size_t>(w) * h && !use_alpha; i++)
      use_alpha = image.pixels[i * 4 + 3] < 128;
  }

  std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
  std::vector<uint8_t> opaque(static_cast<size_t>(w) * h);
  for (size_t i = 0; i < opaque.size(); i++)
  {
    std::memcpy(&rgb[i * 3], &image.pixels[i * 4], 3);
    opaque[i] = use_alpha ? image.pixels[i * 4 + 3] >= options.alpha_threshold : 1;
  }
  if (tw != w || th != h)
  {
    rgb = Resize(rgb, w, h, 3, tw, th, options.resample);
    opaque = ResizeMaskNearest(opaque, w, h, tw, th);
  }

  GctEntry entry;
  std::string stem = name.substr(0, 15);
  entry.name = LowerAscii(stem);
  entry.width = static_cast<uint16_t>(tw);
  entry.height = static_cast<uint16_t>(th);
  entry.levels =
      static_cast<uint8_t>(EncodeMipChain(rgb, opaque, tw, th, options.mipmaps, 8, &entry.data));
  entry.flags = use_alpha ? GCT_FLAG_ALPHA : 0;

  // gct.wt_cache_key(tw, th, rgb, b"", alpha): md5(alpha byte, <II size, pixels, b"2").
  Md5 md5;
  const uint8_t alpha_byte = use_alpha ? 1 : 0;
  md5.Update(&alpha_byte, 1);
  uint8_t size_le[8];
  for (int i = 0; i < 4; i++)
  {
    size_le[i] = static_cast<uint8_t>(static_cast<uint32_t>(tw) >> (8 * i));
    size_le[4 + i] = static_cast<uint8_t>(static_cast<uint32_t>(th) >> (8 * i));
  }
  md5.Update(size_le, 8);
  md5.Update(rgb.data(), rgb.size());
  md5.Update("2", 1);
  entry.tag = static_cast<uint16_t>(std::stoul(md5.HexDigest().substr(0, 4), nullptr, 16));
  return entry;
}

std::vector<uint8_t> EncodeSkySide(const RgbaImage& image, int size)
{
  std::vector<uint8_t> rgb(static_cast<size_t>(image.width) * image.height * 3);
  for (size_t i = 0; i < static_cast<size_t>(image.width) * image.height; i++)
    std::memcpy(&rgb[i * 3], &image.pixels[i * 4], 3);
  if (image.width != size || image.height != size)
    rgb = Resize(rgb, image.width, image.height, 3, size, size, Filter::Lanczos);
  return CmprEncodeSimple(rgb.data(), size, size);
}

std::vector<uint8_t> WriteGct(const std::vector<GctEntry>& entries)
{
  std::vector<uint8_t> table, blob;
  for (const GctEntry& e : entries)
  {
    char name[16] = {};
    std::memcpy(name, e.name.data(), std::min<size_t>(16, e.name.size()));
    table.insert(table.end(), name, name + 16);
    Put16BE(&table, e.width);
    Put16BE(&table, e.height);
    table.push_back(e.levels);
    table.push_back(e.flags);
    Put16BE(&table, e.tag);
    Put32BE(&table, static_cast<uint32_t>(blob.size()));
    blob.insert(blob.end(), e.data.begin(), e.data.end());
    blob.resize((blob.size() + 31) & ~size_t(31), 0);
  }
  std::vector<uint8_t> out = {'G', 'C', 'W', 'T'};
  Put32BE(&out, static_cast<uint32_t>(entries.size()));
  Put32BE(&out, static_cast<uint32_t>(blob.size()));
  out.insert(out.end(), table.begin(), table.end());
  out.resize((out.size() + 31) & ~size_t(31), 0);
  out.insert(out.end(), blob.begin(), blob.end());
  return out;
}

std::optional<std::vector<GctEntry>> ReadGct(const std::vector<uint8_t>& data)
{
  if (data.size() < 12 || std::memcmp(data.data(), "GCWT", 4) != 0)
    return std::nullopt;
  const uint32_t count = Get32BE(&data[4]);
  const size_t first = 12 + static_cast<size_t>(count) * 28;
  const size_t blob_start = (first + 31) & ~size_t(31);
  if (first > data.size())
    return std::nullopt;
  std::vector<GctEntry> out;
  for (uint32_t i = 0; i < count; i++)
  {
    const uint8_t* row = &data[12 + i * 28];
    GctEntry e;
    e.name.assign(reinterpret_cast<const char*>(row), strnlen(reinterpret_cast<const char*>(row), 16));
    e.width = Get16BE(row + 16);
    e.height = Get16BE(row + 18);
    e.levels = row[20];
    e.flags = row[21];
    e.tag = Get16BE(row + 22);
    const size_t offset = blob_start + Get32BE(row + 24);
    size_t size = 0;
    for (int lv = 0; lv < e.levels; lv++)
      size += CmprSize(std::max(8, e.width >> lv), std::max(8, e.height >> lv));
    if (offset > data.size())
      return std::nullopt;
    size = std::min(size, data.size() - offset);
    e.data.assign(data.begin() + offset, data.begin() + offset + size);
    out.push_back(std::move(e));
  }
  return out;
}

std::optional<std::vector<uint8_t>> WavToGcpcm(const std::vector<uint8_t>& wav)
{
  // Python's wave module: RIFF/WAVE, a PCM (or extensible PCM) fmt chunk, then the data chunk.
  if (wav.size() < 12 || std::memcmp(wav.data(), "RIFF", 4) != 0 ||
      std::memcmp(&wav[8], "WAVE", 4) != 0)
  {
    return std::nullopt;
  }
  int channels = 0, width = 0;
  uint32_t rate = 0;
  bool have_fmt = false;
  const uint8_t* samples = nullptr;
  size_t data_size = 0;
  size_t pos = 12;
  while (pos + 8 <= wav.size())
  {
    const uint32_t size = Get32LE(&wav[pos + 4]);
    const uint8_t* body = &wav[pos + 8];
    const size_t avail = wav.size() - (pos + 8);
    if (std::memcmp(&wav[pos], "fmt ", 4) == 0)
    {
      if (size < 16 || avail < 16)
        return std::nullopt;
      const uint16_t tag = Get16LE(body);
      if (tag != 1)
      {
        // WAVE_FORMAT_EXTENSIBLE is read only with the PCM sub-format GUID.
        static constexpr uint8_t pcm_guid[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                                 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        if (tag != 0xFFFE || size < 40 || avail < 40 || std::memcmp(body + 24, pcm_guid, 16) != 0)
          return std::nullopt;
      }
      channels = Get16LE(body + 2);
      rate = Get32LE(body + 4);
      width = (Get16LE(body + 14) + 7) / 8;
      have_fmt = true;
    }
    else if (std::memcmp(&wav[pos], "data", 4) == 0)
    {
      if (!have_fmt)
        return std::nullopt;
      samples = body;
      data_size = std::min<size_t>(size, avail);
      break;
    }
    pos += 8 + size + (size & 1);
  }
  if (!samples || channels < 1 || rate == 0 || width < 1 || width > 4)
    return std::nullopt;

  const size_t frame_bytes = static_cast<size_t>(channels) * width;
  const size_t frames = data_size / frame_bytes;
  if (frames == 0)
    return std::nullopt;
  auto sample = [&](size_t frame, int ch) -> double {
    const uint8_t* p = samples + frame * frame_bytes + static_cast<size_t>(ch) * width;
    switch (width)
    {
    case 1:
      return (static_cast<double>(p[0]) - 128) / 128;
    case 2:
      return static_cast<int16_t>(Get16LE(p)) / 32768.0;
    case 3:
    {
      int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
      if (v >= 1 << 23)
        v -= 1 << 24;
      return static_cast<double>(v) / (1 << 23);
    }
    default:
      return static_cast<int32_t>(Get32LE(p)) / 2147483648.0;
    }
  };

  constexpr uint32_t out_rate = 48000;
  const double ratio = static_cast<double>(frames) * out_rate / rate;
  const size_t out_n = std::max<size_t>(1, static_cast<size_t>(std::nearbyint(ratio)));
  const double step = static_cast<double>(rate) / out_rate;

  std::vector<uint8_t> out = {'G', 'C', 'P', 'A'};
  Put32LE(&out, out_rate);
  Put16LE(&out, 2);
  Put16LE(&out, 2);
  Put32LE(&out, static_cast<uint32_t>(out_n * 4));
  out.reserve(out.size() + out_n * 4);
  for (size_t i = 0; i < out_n; i++)
  {
    const double t = static_cast<double>(i) * step;
    for (int c = 0; c < 2; c++)
    {
      const int src_c = channels == 1 ? 0 : c;
      // np.interp over the integer grid 0..frames-1.
      double v;
      if (t >= static_cast<double>(frames - 1))
      {
        v = sample(frames - 1, src_c);
      }
      else
      {
        const size_t j = static_cast<size_t>(t);
        const double y0 = sample(j, src_c);
        if (static_cast<double>(j) == t)
        {
          v = y0;
        }
        else
        {
          const double slope = (sample(j + 1, src_c) - y0) / 1.0;
          const double d = t - static_cast<double>(j);
          const double p = slope * d;
          v = p + y0;
        }
      }
      v = std::clamp(v, -1.0, 32767.0 / 32768.0);
      const auto s = static_cast<int16_t>(std::nearbyint(v * 32768));
      Put16BE(&out, static_cast<uint16_t>(s));
    }
  }
  return out;
}

std::optional<RgbaImage> DecodeTga(const std::vector<uint8_t>& data)
{
  if (data.size() < 18)
    return std::nullopt;
  const uint8_t id_len = data[0], cmap_type = data[1], type = data[2];
  const int w = Get16LE(&data[12]), h = Get16LE(&data[14]);
  const int depth = data[16], flags = data[17];
  if (cmap_type > 1 || w <= 0 || h <= 0)
    return std::nullopt;
  const int base = type & 7;
  const bool rle = (type & 8) != 0;
  if (!((base == 2 && (depth == 24 || depth == 32)) || (base == 3 && depth == 8) ||
        (base == 1 && depth == 8 && cmap_type == 1)))
  {
    return std::nullopt;  // 16-bit and 1-bit TGAs are left to the Python tool
  }
  // A version-2 TGA's extension area can mark the alpha channel as unused; let Pillow decide.
  if (depth == 32 && data.size() >= 26 &&
      std::memcmp(&data[data.size() - 18], "TRUEVISION-XFILE.", 17) == 0 &&
      Get32LE(&data[data.size() - 26]) != 0)
  {
    return std::nullopt;
  }

  size_t pos = 18 + id_len;
  std::vector<std::array<uint8_t, 4>> palette;
  if (cmap_type)
  {
    const int start = Get16LE(&data[3]), count = Get16LE(&data[5]), map_depth = data[7];
    if (map_depth != 24)
      return std::nullopt;  // 16- and 32-bit colour maps carry alpha; left to the Python tool
    const int bpp = map_depth / 8;
    palette.assign(256, {0, 0, 0, 255});
    for (int i = 0; i < count; i++)
    {
      if (pos + bpp > data.size())
        return std::nullopt;
      if (start + i < 256)
        palette[start + i] = {data[pos + 2], data[pos + 1], data[pos], 255};
      pos += bpp;
    }
  }

  const int bpp = depth / 8;
  std::vector<uint8_t> raw(static_cast<size_t>(w) * h * bpp);
  if (!rle)
  {
    if (pos + raw.size() > data.size())
      return std::nullopt;
    std::memcpy(raw.data(), &data[pos], raw.size());
  }
  else
  {
    size_t o = 0;
    while (o < raw.size())
    {
      if (pos >= data.size())
        return std::nullopt;
      const uint8_t head = data[pos++];
      const size_t n = (head & 0x7F) + 1;
      if (head & 0x80)
      {
        if (pos + bpp > data.size())
          return std::nullopt;
        for (size_t i = 0; i < n && o < raw.size(); i++, o += bpp)
          std::memcpy(&raw[o], &data[pos], bpp);
        pos += bpp;
      }
      else
      {
        const size_t bytes = std::min(n * bpp, raw.size() - o);
        if (pos + bytes > data.size())
          return std::nullopt;
        std::memcpy(&raw[o], &data[pos], bytes);
        pos += n * bpp;
        o += bytes;
      }
    }
  }

  RgbaImage img;
  img.width = w;
  img.height = h;
  img.pixels.resize(static_cast<size_t>(w) * h * 4);
  const bool top_down = (flags & 0x20) != 0;
  const bool right_to_left = (flags & 0x10) != 0;
  for (int y = 0; y < h; y++)
  {
    const int sy = top_down ? y : h - 1 - y;
    for (int x = 0; x < w; x++)
    {
      const int sx = right_to_left ? w - 1 - x : x;
      const uint8_t* p = &raw[(static_cast<size_t>(sy) * w + sx) * bpp];
      uint8_t* d = &img.pixels[(static_cast<size_t>(y) * w + x) * 4];
      if (base == 2)
      {
        d[0] = p[2];
        d[1] = p[1];
        d[2] = p[0];
        d[3] = depth == 32 ? p[3] : 255;
      }
      else if (base == 3)
      {
        d[0] = d[1] = d[2] = p[0];
        d[3] = 255;
      }
      else
      {
        std::memcpy(d, palette[p[0]].data(), 4);
      }
    }
  }
  return img;
}

std::optional<RgbaImage> DecodeBmp(const std::vector<uint8_t>& data)
{
  if (data.size() < 54 || data[0] != 'B' || data[1] != 'M')
    return std::nullopt;
  const uint32_t offset = Get32LE(&data[10]);
  const uint32_t header = Get32LE(&data[14]);
  if (header < 40)
    return std::nullopt;
  const int32_t w = static_cast<int32_t>(Get32LE(&data[18]));
  int32_t h = static_cast<int32_t>(Get32LE(&data[22]));
  const int bits = Get16LE(&data[28]);
  const uint32_t compression = Get32LE(&data[30]);
  if (w <= 0 || h == 0 || compression != 0 || (bits != 8 && bits != 24 && bits != 32))
    return std::nullopt;  // RLE, bitfields and low bit depths are left to the Python tool
  const bool top_down = h < 0;
  h = std::abs(h);

  std::vector<std::array<uint8_t, 4>> palette;
  if (bits == 8)
  {
    uint32_t colors = Get32LE(&data[46]);
    if (colors == 0 || colors > 256)
      colors = 256;
    const size_t pal_pos = 14 + header;
    palette.assign(256, {0, 0, 0, 255});
    for (uint32_t i = 0; i < colors; i++)
    {
      if (pal_pos + i * 4 + 3 > data.size())
        return std::nullopt;
      const uint8_t* p = &data[pal_pos + i * 4];
      palette[i] = {p[2], p[1], p[0], 255};
    }
  }
  const size_t stride = ((static_cast<size_t>(w) * bits + 31) / 32) * 4;
  if (offset + stride * h > data.size())
    return std::nullopt;

  RgbaImage img;
  img.width = w;
  img.height = h;
  img.pixels.resize(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; y++)
  {
    const int sy = top_down ? y : h - 1 - y;
    const uint8_t* row = &data[offset + stride * sy];
    for (int x = 0; x < w; x++)
    {
      uint8_t* d = &img.pixels[(static_cast<size_t>(y) * w + x) * 4];
      if (bits == 8)
      {
        std::memcpy(d, palette[row[x]].data(), 4);
      }
      else
      {
        const uint8_t* p = &row[x * (bits / 8)];
        d[0] = p[2];
        d[1] = p[1];
        d[2] = p[0];
        d[3] = 255;
      }
    }
  }
  return img;
}

bool LooksLikeCross(const RgbaImage& image)
{
  const int w = image.width, h = image.height;
  // (column, row) of the six faces in a horizontal and a vertical cross.
  static constexpr int hcross[6][2] = {{0, 1}, {1, 1}, {2, 1}, {3, 1}, {1, 0}, {1, 2}};
  static constexpr int vcross[6][2] = {{1, 0}, {0, 1}, {1, 1}, {2, 1}, {1, 2}, {1, 3}};
  const bool horizontal = 3 * w == 4 * h;
  if (!horizontal && 4 * w != 3 * h)
    return false;
  const auto& cells = horizontal ? hcross : vcross;
  const int cols = horizontal ? 4 : 3, rows = horizontal ? 3 : 4;
  const int cw = w / cols, ch = h / rows;
  for (int c = 0; c < cols; c++)
  {
    for (int r = 0; r < rows; r++)
    {
      bool used = false;
      for (const auto& cell : cells)
        used |= cell[0] == c && cell[1] == r;
      if (used)
        continue;
      const int y0 = r * ch + ch / 8, y1 = (r + 1) * ch - ch / 8;
      const int x0 = c * cw + cw / 8, x1 = (c + 1) * cw - cw / 8;
      if (y1 <= y0 || x1 <= x0)
        continue;
      for (int k = 0; k < 3; k++)
      {
        int lo = 255, hi = 0;
        for (int y = y0; y < y1; y++)
        {
          for (int x = x0; x < x1; x++)
          {
            const int v = image.pixels[(static_cast<size_t>(y) * w + x) * 4 + k];
            lo = std::min(lo, v);
            hi = std::max(hi, v);
          }
        }
        if (hi - lo > 8)
          return false;
      }
    }
  }
  return true;
}
}  // namespace GxBake
