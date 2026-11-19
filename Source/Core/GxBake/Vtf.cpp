// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

#include "GxBake/Vtf.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

namespace GxBake
{
namespace fs = std::filesystem;

namespace
{
using Bytes = std::vector<uint8_t>;

uint16_t U16(const Bytes& d, size_t p)
{
  return static_cast<uint16_t>(d[p] | (d[p + 1] << 8));
}

uint32_t U32(const Bytes& d, size_t p)
{
  uint32_t v;
  std::memcpy(&v, d.data() + p, 4);
  return v;
}

int64_t BytesPerPixel(int fmt)
{
  switch (fmt)
  {
  case 0:
  case 1:
  case 11:
  case 12:
  case 16:
  case 23:
  case 26:
    return 4;
  case 2:
  case 3:
  case 9:
  case 10:
    return 3;
  case 4:
  case 6:
  case 17:
  case 18:
  case 19:
  case 21:
  case 22:
    return 2;
  case 5:
  case 7:
  case 8:
    return 1;
  case 24:
  case 25:
    return 8;
  default:
    return 0;
  }
}

int64_t BlockBytes(int fmt)
{
  switch (fmt)
  {
  case 13:
  case 20:
  case 35:
    return 8;
  case 14:
  case 15:
  case 34:
    return 16;
  default:
    return 0;
  }
}

// vtf.level_bytes; -1 for an unsupported format.
int64_t LevelBytes(int fmt, int64_t w, int64_t h)
{
  if (const int64_t block = BlockBytes(fmt))
    return std::max<int64_t>(1, (w + 3) / 4) * std::max<int64_t>(1, (h + 3) / 4) * block;
  if (const int64_t bpp = BytesPerPixel(fmt))
    return w * h * bpp;
  return -1;
}

float HalfToFloat(uint16_t h)
{
  const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, mant = h & 1023;
  float v;
  if (exp == 0)
    v = std::ldexp(static_cast<float>(mant), -24);
  else if (exp == 31)
    v = mant ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
  else
    v = std::ldexp(static_cast<float>(mant | 1024), static_cast<int>(exp) - 25);
  return sign ? -v : v;
}

// (nblocks, 16 texels, C) in row-major block order -> cropped w x h image.
void BlocksToImage(const std::vector<std::array<uint8_t, 4>>& texels, int w, int h,
                   RgbaImage* out)
{
  const int bw = std::max(1, (w + 3) / 4);
  out->width = w;
  out->height = h;
  out->pixels.assign(static_cast<size_t>(w) * h * 4, 0);
  for (int y = 0; y < h; y++)
  {
    for (int x = 0; x < w; x++)
    {
      const size_t block = static_cast<size_t>(y / 4) * bw + x / 4;
      const auto& t = texels[block * 16 + (y % 4) * 4 + (x % 4)];
      std::memcpy(&out->pixels[(static_cast<size_t>(y) * w + x) * 4], t.data(), 4);
    }
  }
}

std::array<int, 3> Rgb565(uint16_t v)
{
  const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
  return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
}

// vtf._colour_block: 16 RGB texels (alpha 255, or 0 for a punch-through hole).
void ColourBlock(const uint8_t* b, bool four_colour, std::array<uint8_t, 4>* texels)
{
  const uint16_t c0 = static_cast<uint16_t>(b[0] | (b[1] << 8));
  const uint16_t c1 = static_cast<uint16_t>(b[2] | (b[3] << 8));
  const auto p0 = Rgb565(c0), p1 = Rgb565(c1);
  const bool opaque4 = four_colour || c0 > c1;
  std::array<std::array<int, 3>, 4> pal{p0, p1, {}, {}};
  for (int k = 0; k < 3; k++)
  {
    pal[2][k] = opaque4 ? (2 * p0[k] + p1[k]) / 3 : (p0[k] + p1[k]) / 2;
    pal[3][k] = opaque4 ? (p0[k] + 2 * p1[k]) / 3 : 0;
  }
  const uint32_t word = b[4] | (b[5] << 8) | (b[6] << 16) | (static_cast<uint32_t>(b[7]) << 24);
  for (int i = 0; i < 16; i++)
  {
    const int idx = (word >> (2 * i)) & 3;
    for (int k = 0; k < 3; k++)
      texels[i][k] = static_cast<uint8_t>(pal[idx][k]);
    texels[i][3] = idx == 3 && !opaque4 ? 0 : 255;
  }
}

// vtf._bc4_values
std::array<uint8_t, 16> Bc4Values(const uint8_t* b)
{
  const int a0 = b[0], a1 = b[1];
  std::array<int, 8> pal{a0, a1};
  if (a0 > a1)
  {
    for (int k = 1; k <= 6; k++)
      pal[1 + k] = ((7 - k) * a0 + k * a1) / 7;
  }
  else
  {
    for (int k = 1; k <= 4; k++)
      pal[1 + k] = ((5 - k) * a0 + k * a1) / 5;
    pal[6] = 0;
    pal[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; i++)
    bits |= static_cast<uint64_t>(b[2 + i]) << (8 * i);
  std::array<uint8_t, 16> out{};
  for (int i = 0; i < 16; i++)
    out[i] = static_cast<uint8_t>(pal[(bits >> (3 * i)) & 7]);
  return out;
}

std::optional<RgbaImage> DecodePixels(int fmt, const uint8_t* raw, int w, int h,
                                      std::string* error)
{
  RgbaImage img;
  const size_t nblocks =
      static_cast<size_t>(std::max(1, (w + 3) / 4)) * static_cast<size_t>(std::max(1, (h + 3) / 4));
  std::vector<std::array<uint8_t, 4>> texels(nblocks * 16);
  switch (fmt)
  {
  case 13:
  case 20:
    for (size_t i = 0; i < nblocks; i++)
      ColourBlock(raw + i * 8, false, &texels[i * 16]);
    BlocksToImage(texels, w, h, &img);
    return img;
  case 14:
    for (size_t i = 0; i < nblocks; i++)
    {
      const uint8_t* b = raw + i * 16;
      ColourBlock(b + 8, true, &texels[i * 16]);
      for (int k = 0; k < 8; k++)
      {
        texels[i * 16 + k * 2][3] = static_cast<uint8_t>((b[k] & 15) * 17);
        texels[i * 16 + k * 2 + 1][3] = static_cast<uint8_t>((b[k] >> 4) * 17);
      }
    }
    BlocksToImage(texels, w, h, &img);
    return img;
  case 15:
    for (size_t i = 0; i < nblocks; i++)
    {
      const uint8_t* b = raw + i * 16;
      ColourBlock(b + 8, true, &texels[i * 16]);
      const auto a = Bc4Values(b);
      for (int k = 0; k < 16; k++)
        texels[i * 16 + k][3] = a[k];
    }
    BlocksToImage(texels, w, h, &img);
    return img;
  case 35:
    for (size_t i = 0; i < nblocks; i++)
    {
      const auto a = Bc4Values(raw + i * 8);
      for (int k = 0; k < 16; k++)
        texels[i * 16 + k] = {a[k], a[k], a[k], 255};
    }
    BlocksToImage(texels, w, h, &img);
    return img;
  case 34:
    for (size_t i = 0; i < nblocks; i++)
    {
      const auto xs = Bc4Values(raw + i * 16), ys = Bc4Values(raw + i * 16 + 8);
      for (int k = 0; k < 16; k++)
      {
        const float x = static_cast<float>(xs[k]) / 127.5f - 1.0f;
        const float y = static_cast<float>(ys[k]) / 127.5f - 1.0f;
        const float z = std::sqrt(std::clamp(1.0f - x * x - y * y, 0.0f, 1.0f));
        const auto q = [](float v) {
          return static_cast<uint8_t>(std::clamp(v * 127.5f + 127.5f, 0.0f, 255.0f));
        };
        texels[i * 16 + k] = {q(x), q(y), q(z), 255};
      }
    }
    BlocksToImage(texels, w, h, &img);
    return img;
  default:
    break;
  }

  const size_t n = static_cast<size_t>(w) * h;
  img.width = w;
  img.height = h;
  img.pixels.assign(n * 4, 255);
  uint8_t* out = img.pixels.data();
  const auto expand = [](uint32_t v, uint32_t max) { return static_cast<uint8_t>(v * 255 / max); };
  for (size_t i = 0; i < n; i++)
  {
    uint8_t* o = out + i * 4;
    switch (fmt)
    {
    case 0:
    case 23:
    case 26:
      std::memcpy(o, raw + i * 4, 4);
      if (fmt == 26)
        o[3] = 255;
      break;
    case 1:
      o[0] = raw[i * 4 + 3], o[1] = raw[i * 4 + 2], o[2] = raw[i * 4 + 1], o[3] = raw[i * 4];
      break;
    case 11:
      o[0] = raw[i * 4 + 1], o[1] = raw[i * 4 + 2], o[2] = raw[i * 4 + 3], o[3] = raw[i * 4];
      break;
    case 12:
    case 16:
      o[0] = raw[i * 4 + 2], o[1] = raw[i * 4 + 1], o[2] = raw[i * 4], o[3] = raw[i * 4 + 3];
      if (fmt == 16)
        o[3] = 255;
      break;
    case 2:
    case 9:
    case 3:
    case 10:
      if (fmt == 2 || fmt == 9)
        std::memcpy(o, raw + i * 3, 3);
      else
        o[0] = raw[i * 3 + 2], o[1] = raw[i * 3 + 1], o[2] = raw[i * 3];
      if (fmt == 9 || fmt == 10)
        o[3] = o[0] == 0 && o[1] == 0 && o[2] == 255 ? 0 : 255;
      break;
    case 4:
    case 17:
    case 18:
    case 19:
    case 21:
    {
      const uint32_t v = static_cast<uint32_t>(raw[i * 2] | (raw[i * 2 + 1] << 8));
      if (fmt == 4)
      {
        o[0] = expand(v & 31, 31), o[1] = expand((v >> 5) & 63, 63), o[2] = expand(v >> 11, 31);
      }
      else if (fmt == 17)
      {
        o[2] = expand(v & 31, 31), o[1] = expand((v >> 5) & 63, 63), o[0] = expand(v >> 11, 31);
      }
      else if (fmt == 19)
      {
        o[2] = static_cast<uint8_t>((v & 15) * 17), o[1] = static_cast<uint8_t>(((v >> 4) & 15) * 17);
        o[0] = static_cast<uint8_t>(((v >> 8) & 15) * 17), o[3] = static_cast<uint8_t>((v >> 12) * 17);
      }
      else
      {
        o[2] = expand(v & 31, 31), o[1] = expand((v >> 5) & 31, 31);
        o[0] = expand((v >> 10) & 31, 31);
        o[3] = fmt == 18 ? 255 : static_cast<uint8_t>((v >> 15) * 255);
      }
      break;
    }
    case 5:
    case 7:
      o[0] = o[1] = o[2] = raw[i];
      break;
    case 6:
      o[0] = o[1] = o[2] = raw[i * 2];
      o[3] = raw[i * 2 + 1];
      break;
    case 8:
      o[3] = raw[i];
      break;
    case 22:
      o[0] = raw[i * 2], o[1] = raw[i * 2 + 1];
      break;
    case 24:
    case 25:
      for (int k = 0; k < 4; k++)
      {
        const uint16_t bits = static_cast<uint16_t>(raw[i * 8 + k * 2] | (raw[i * 8 + k * 2 + 1] << 8));
        float f;
        if (fmt == 24)
        {
          f = HalfToFloat(bits);
          if (k < 3)
            f = f / (1.0f + f);  // HDR -> LDR (Reinhard)
        }
        else
        {
          f = static_cast<float>(bits) / 65535.0f;
        }
        // numpy's clip passes NaN through, and casting it to uint8 is undefined; use 0.
        const float c = std::isnan(f) ? 0.0f : std::clamp(f, 0.0f, 1.0f);
        o[k] = static_cast<uint8_t>(c * 255.0f + 0.5f);
      }
      break;
    default:
      *error = "unsupported VTF format " + std::to_string(fmt);
      return std::nullopt;
    }
  }
  return img;
}

bool IsSpace(uint8_t c)
{
  return c == ' ' || (c >= '\t' && c <= '\r') || (c >= 0x1c && c <= 0x1f) || c == 0x85 ||
         c == 0xa0;
}

std::string Lower(std::string s)
{
  for (char& ch : s)
  {
    const uint8_t c = static_cast<uint8_t>(ch);
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
      ch = static_cast<char>(c + 32);
  }
  return s;
}

std::string Strip(const std::string& s, char c)
{
  const size_t a = s.find_first_not_of(c);
  if (a == std::string::npos)
    return {};
  return s.substr(a, s.find_last_not_of(c) - a + 1);
}

std::optional<Bytes> ReadAll(const fs::path& path)
{
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return std::nullopt;
  return Bytes(std::istreambuf_iterator<char>(f), {});
}

fs::path FromLatin1(const std::string& s)
{
  std::u8string u;
  for (const char ch : s)
  {
    const uint8_t c = static_cast<uint8_t>(ch);
    if (c < 0x80)
    {
      u += static_cast<char8_t>(c);
    }
    else
    {
      u += static_cast<char8_t>(0xC0 | (c >> 6));
      u += static_cast<char8_t>(0x80 | (c & 0x3F));
    }
  }
  return fs::path(u);
}

std::string FileName(const fs::path& p)
{
  const auto u8 = p.filename().u8string();
  return std::string(u8.begin(), u8.end());
}

// vtf._ci_file: root/rel, else matched part by part ignoring case.
std::optional<fs::path> CiFile(const fs::path& root, const std::string& rel)
{
  std::error_code ec;
  const fs::path direct = root / FromLatin1(rel);
  if (fs::is_regular_file(direct, ec))
    return direct;
  fs::path cur = root;
  size_t start = 0;
  while (start <= rel.size())
  {
    size_t slash = rel.find('/', start);
    if (slash == std::string::npos)
      slash = rel.size();
    const std::string part = rel.substr(start, slash - start);
    start = slash + 1;
    if (part.empty() || part == ".")
      continue;
    // Names on disk are UTF-8; compare them with the Latin-1 part through the same conversion.
    const auto want_u8 = FromLatin1(part).u8string();
    const std::string want = Lower(std::string(want_u8.begin(), want_u8.end()));
    std::optional<fs::path> match;
    for (auto it = fs::directory_iterator(cur, ec); !ec && it != fs::directory_iterator();
         it.increment(ec))
    {
      if (Lower(FileName(it->path())) == want)
      {
        match = it->path();
        break;
      }
    }
    if (ec || !match)
      return std::nullopt;
    cur = *match;
  }
  if (fs::is_regular_file(cur, ec))
    return cur;
  return std::nullopt;
}
}  // namespace

std::optional<RgbaImage> ReadVtf(const Bytes& data, std::string* error)
{
  // vtf.vtf_header
  if (data.size() < 64 || std::memcmp(data.data(), "VTF\0", 4) != 0)
  {
    *error = "not a VTF file";
    return std::nullopt;
  }
  const uint32_t major = U32(data, 4), minor = U32(data, 8), header_size = U32(data, 12);
  const int w = U16(data, 16), h = U16(data, 18);
  const uint32_t flags = U32(data, 20);
  const int frames = std::max<int>(1, U16(data, 24));
  const uint16_t first_frame = U16(data, 26);
  const int fmt = static_cast<int32_t>(U32(data, 52));
  const int mips = std::max<int>(1, data[56]);
  const int lo_fmt = static_cast<int32_t>(data[57] | (data[58] << 8) | (data[59] << 16) |
                                          (static_cast<uint32_t>(data[60]) << 24));
  const int lo_w = data[61], lo_h = data[62];
  const auto version_ge = [&](uint32_t a, uint32_t b) {
    return major > a || (major == a && minor >= b);
  };
  const int depth = std::max<int>(1, version_ge(7, 2) && data.size() >= 65 ? U16(data, 63) : 1);
  std::optional<int64_t> hi_off;
  if (version_ge(7, 3) && data.size() >= 72)
  {
    const uint32_t nres = U32(data, 68);
    for (uint32_t i = 0; i < std::min<uint32_t>(nres, 32); i++)
    {
      const size_t p = 80 + i * 8;
      if (p + 8 > data.size())
      {
        *error = "VTF is truncated";
        return std::nullopt;
      }
      if (data[p] == 0x30 && data[p + 1] == 0 && data[p + 2] == 0)
        hi_off = U32(data, p + 4);
    }
  }
  if (!hi_off)
  {
    int64_t lo = 0;
    if (lo_fmt >= 0 && lo_w && lo_h)
    {
      lo = LevelBytes(lo_fmt, lo_w, lo_h);
      if (lo < 0)
      {
        *error = "unsupported VTF format " + std::to_string(lo_fmt);
        return std::nullopt;
      }
    }
    hi_off = header_size + lo;
  }
  int faces = 1;
  if (flags & 0x4000)
    faces = !version_ge(7, 5) && first_frame != 0xFFFF && minor >= 1 ? 7 : 6;

  // vtf.read_vtf: mips are stored smallest first, each holding frames x faces x slices.
  if (LevelBytes(fmt, w, h) < 0)
  {
    *error = "unsupported VTF format " + std::to_string(fmt);
    return std::nullopt;
  }
  int64_t off = *hi_off;
  for (int level = mips - 1; level > 0; level--)
  {
    const int lw = std::max(1, w >> level), lh = std::max(1, h >> level);
    const int ld = std::max(1, depth >> level);
    off += LevelBytes(fmt, lw, lh) * frames * faces * ld;
  }
  const int64_t size = LevelBytes(fmt, w, h);
  if (off + size > static_cast<int64_t>(data.size()))
  {
    *error = "VTF is truncated";
    return std::nullopt;
  }
  if (w == 0 || h == 0)
  {
    *error = "VTF has no pixels";
    return std::nullopt;
  }
  return DecodePixels(fmt, data.data() + off, w, h, error);
}

std::pair<std::string, std::map<std::string, std::string>> ParseVmt(const std::string& raw)
{
  // re.sub(r"//[^\n]*", "", text)
  std::string text;
  for (size_t i = 0; i < raw.size();)
  {
    if (raw[i] == '/' && i + 1 < raw.size() && raw[i + 1] == '/')
    {
      while (i < raw.size() && raw[i] != '\n')
        i++;
      continue;
    }
    text += raw[i++];
  }
  // re.findall(r'"([^"]*)"|([{}])|([^\s{}"]+)', text)
  std::vector<std::string> flat;
  for (size_t i = 0; i < text.size();)
  {
    const char c = text[i];
    if (c == '"')
    {
      const size_t end = text.find('"', i + 1);
      if (end == std::string::npos)
      {
        i++;  // an unclosed quote matches nothing
        continue;
      }
      flat.push_back(text.substr(i + 1, end - i - 1));
      i = end + 1;
    }
    else if (c == '{' || c == '}')
    {
      flat.emplace_back(1, c);
      i++;
    }
    else if (IsSpace(static_cast<uint8_t>(c)))
    {
      i++;
    }
    else
    {
      const size_t start = i;
      while (i < text.size() && text[i] != '{' && text[i] != '}' && text[i] != '"' &&
             !IsSpace(static_cast<uint8_t>(text[i])))
      {
        i++;
      }
      flat.push_back(text.substr(start, i - start));
    }
  }
  // `tok in "{}"` is also true for an empty token.
  const auto brace = [](const std::string& t) { return t.empty() || t == "{" || t == "}"; };
  std::string shader = flat.empty() ? "" : Lower(flat[0]);
  std::map<std::string, std::string> params;
  size_t i = 1;
  while (i < flat.size())
  {
    if (brace(flat[i]))
    {
      i++;
      continue;
    }
    if (i + 1 < flat.size() && !brace(flat[i + 1]))
    {
      params.emplace(Lower(flat[i]), flat[i + 1]);
      i += 2;
    }
    else
    {
      i++;
    }
  }
  return {shader, params};
}

std::optional<fs::path> FindMaterialsRoot(const fs::path& start)
{
  fs::path d = start;
  while (true)
  {
    std::error_code ec;
    if (fs::is_directory(d / "materials", ec))
      return d / "materials";
    const fs::path parent = d.parent_path();
    if (parent.empty() || parent == d)
      return std::nullopt;
    d = parent;
  }
}

ResolvedMaterial ResolveMaterial(const std::string& raw_name, const std::vector<std::string>& dirs,
                                 const fs::path& root, std::vector<fs::path>* read)
{
  std::string name = raw_name;
  std::replace(name.begin(), name.end(), '\\', '/');
  name = Strip(name, '/');
  const std::vector<std::string> search = dirs.empty() ? std::vector<std::string>{""} : dirs;
  for (std::string d : search)
  {
    std::replace(d.begin(), d.end(), '\\', '/');
    const std::string rel = Strip(Strip(d, '/') + "/" + name, '/');
    std::map<std::string, std::string> params;
    if (const auto vmt = CiFile(root, rel + ".vmt"))
    {
      if (read)
        read->push_back(*vmt);
      const auto bytes = ReadAll(*vmt);
      std::string shader;
      if (bytes)
        std::tie(shader, params) = ParseVmt(std::string(bytes->begin(), bytes->end()));
      if (shader == "patch" && params.contains("include"))
      {
        std::string inc = params["include"];
        std::replace(inc.begin(), inc.end(), '\\', '/');
        if (Lower(inc).starts_with("materials/"))
          inc = inc.substr(10);
        if (const auto inc_vmt = CiFile(root, inc))
        {
          if (read)
            read->push_back(*inc_vmt);
          const auto inc_bytes = ReadAll(*inc_vmt);
          auto base = ParseVmt(inc_bytes ? std::string(inc_bytes->begin(), inc_bytes->end()) :
                                           std::string())
                          .second;
          for (const auto& [k, v] : params)
            base[k] = v;
          params = std::move(base);
        }
      }
      const auto it = params.find("$basetexture");
      if (it != params.end() && !it->second.empty())
      {
        std::string tex = it->second;
        std::replace(tex.begin(), tex.end(), '\\', '/');
        tex = Strip(tex, '/');
        if (tex.ends_with(".vtf"))
          tex.resize(tex.size() - 4);
        if (const auto vtf = CiFile(root, tex + ".vtf"))
          return {vtf, params};
      }
    }
    if (const auto vtf = CiFile(root, rel + ".vtf"))
      return {vtf, params};
  }
  return {};
}
}  // namespace GxBake
