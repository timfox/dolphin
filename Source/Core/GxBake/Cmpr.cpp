// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

#include "GxBake/Cmpr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

// The float code below mirrors numpy's float32 evaluation order. Fused multiply-adds would change
// rounding, so this file is compiled with contraction off (see CMakeLists.txt); the pragma covers
// compilers that honour it.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

namespace GxBake
{
namespace
{
int To565(int r, int g, int b)
{
  return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

std::array<int, 3> From565(int v)
{
  const int r = (v >> 11) & 31;
  const int g = (v >> 5) & 63;
  const int b = v & 31;
  return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
}

// Writes blocks in GX order: 8x8 tiles of 2x2 4x4 blocks, each c0, c1 (big-endian) and 4 rows.
template <typename BlockFn>
std::vector<uint8_t> EncodeTiled(int width, int height, BlockFn&& block)
{
  const int bh = height / 4;
  const int bw = width / 4;
  std::vector<uint8_t> out;
  out.reserve(static_cast<size_t>(bh) * bw * 8);
  for (int ty = 0; ty < bh; ty += 2)
  {
    for (int tx = 0; tx < bw; tx += 2)
    {
      const int order[4][2] = {{ty, tx}, {ty, tx + 1}, {ty + 1, tx}, {ty + 1, tx + 1}};
      for (const auto& o : order)
      {
        uint16_t c0, c1;
        std::array<uint8_t, 4> rows;
        block(o[0], o[1], &c0, &c1, &rows);
        out.push_back(static_cast<uint8_t>(c0 >> 8));
        out.push_back(static_cast<uint8_t>(c0));
        out.push_back(static_cast<uint8_t>(c1 >> 8));
        out.push_back(static_cast<uint8_t>(c1));
        out.insert(out.end(), rows.begin(), rows.end());
      }
    }
  }
  return out;
}

std::array<uint8_t, 4> PackRows(const std::array<int, 16>& idx)
{
  std::array<uint8_t, 4> rows;
  for (int r = 0; r < 4; r++)
  {
    rows[r] = static_cast<uint8_t>((idx[r * 4 + 0] << 6) | (idx[r * 4 + 1] << 4) |
                                   (idx[r * 4 + 2] << 2) | idx[r * 4 + 3]);
  }
  return rows;
}
}  // namespace

size_t CmprSize(int width, int height)
{
  return static_cast<size_t>((width + 7) / 8) * ((height + 7) / 8) * 32;
}

std::vector<uint8_t> CmprEncodeSimple(const uint8_t* rgb, int width, int height)
{
  return EncodeTiled(width, height, [&](int by, int bx, uint16_t* out_c0, uint16_t* out_c1,
                                        std::array<uint8_t, 4>* rows) {
    std::array<std::array<int, 3>, 16> px;
    for (int k = 0; k < 16; k++)
    {
      const uint8_t* p = &rgb[((static_cast<size_t>(by) * 4 + k / 4) * width + bx * 4 + k % 4) * 3];
      px[k] = {p[0], p[1], p[2]};
    }
    int hi = 0, lo = 0;
    int hi_l = std::numeric_limits<int>::min(), lo_l = std::numeric_limits<int>::max();
    for (int k = 0; k < 16; k++)
    {
      const int l = px[k][0] * 299 + px[k][1] * 587 + px[k][2] * 114;
      if (l > hi_l)
      {
        hi_l = l;
        hi = k;
      }
      if (l < lo_l)
      {
        lo_l = l;
        lo = k;
      }
    }
    int c0 = To565(px[hi][0], px[hi][1], px[hi][2]);
    int c1 = To565(px[lo][0], px[lo][1], px[lo][2]);
    if (c0 < c1)
      std::swap(c0, c1);
    const auto e0 = From565(c0), e1 = From565(c1);
    std::array<std::array<int, 3>, 4> pal;
    for (int c = 0; c < 3; c++)
    {
      pal[0][c] = e0[c];
      pal[1][c] = e1[c];
      pal[2][c] = (2 * e0[c] + e1[c]) / 3;
      pal[3][c] = (e0[c] + 2 * e1[c]) / 3;
    }
    std::array<int, 16> idx{};
    for (int k = 0; k < 16; k++)
    {
      if (c0 == c1)
        continue;
      int best = 0;
      int64_t best_d = std::numeric_limits<int64_t>::max();
      for (int p = 0; p < 4; p++)
      {
        int64_t d = 0;
        for (int c = 0; c < 3; c++)
          d += static_cast<int64_t>(px[k][c] - pal[p][c]) * (px[k][c] - pal[p][c]);
        if (d < best_d)
        {
          best_d = d;
          best = p;
        }
      }
      idx[k] = best;
    }
    *out_c0 = static_cast<uint16_t>(c0);
    *out_c1 = static_cast<uint16_t>(c1);
    *rows = PackRows(idx);
  });
}

std::vector<uint8_t> CmprEncodeBlocks(const uint8_t* rgb, const uint8_t* opaque, int width,
                                      int height)
{
  return EncodeTiled(width, height, [&](int by, int bx, uint16_t* out_c0, uint16_t* out_c1,
                                        std::array<uint8_t, 4>* rows) {
    std::array<std::array<float, 3>, 16> flat;
    std::array<bool, 16> mask;
    for (int k = 0; k < 16; k++)
    {
      const size_t i = (static_cast<size_t>(by) * 4 + k / 4) * width + bx * 4 + k % 4;
      for (int c = 0; c < 3; c++)
        flat[k][c] = static_cast<float>(rgb[i * 3 + c]);
      mask[k] = opaque[i] != 0;
    }

    float count = 0.0f;
    for (int k = 0; k < 16; k++)
      count += mask[k] ? 1.0f : 0.0f;
    count = std::max(count, 1.0f);
    std::array<float, 3> mean;
    for (int c = 0; c < 3; c++)
    {
      float s = 0.0f;
      for (int k = 0; k < 16; k++)
        s += flat[k][c] * (mask[k] ? 1.0f : 0.0f);
      mean[c] = s / count;
    }
    std::array<std::array<float, 3>, 16> diff;
    for (int k = 0; k < 16; k++)
      for (int c = 0; c < 3; c++)
        diff[k][c] = (flat[k][c] - mean[c]) * (mask[k] ? 1.0f : 0.0f);
    float cov[3][3];
    for (int i = 0; i < 3; i++)
    {
      for (int j = 0; j < 3; j++)
      {
        float s = 0.0f;
        for (int k = 0; k < 16; k++)
        {
          const float p = diff[k][i] * diff[k][j];
          s = s + p;
        }
        cov[i][j] = s;
      }
    }
    std::array<float, 3> axis = {1.0f, 1.0f, 1.0f};
    for (int it = 0; it < 8; it++)
    {
      std::array<float, 3> next;
      for (int i = 0; i < 3; i++)
      {
        float s = 0.0f;
        for (int j = 0; j < 3; j++)
        {
          const float p = cov[i][j] * axis[j];
          s = s + p;
        }
        next[i] = s;
      }
      const float sq0 = next[0] * next[0];
      const float sq1 = next[1] * next[1];
      const float sq2 = next[2] * next[2];
      const float norm = std::sqrt((sq0 + sq1) + sq2);
      const float denom = std::max(norm, 1e-6f);
      for (int i = 0; i < 3; i++)
        axis[i] = next[i] / denom;
    }
    float lo_t = std::numeric_limits<float>::infinity();
    float hi_t = -std::numeric_limits<float>::infinity();
    for (int k = 0; k < 16; k++)
    {
      float s = 0.0f;
      for (int i = 0; i < 3; i++)
      {
        const float d = flat[k][i] - mean[i];
        const float p = d * axis[i];
        s = s + p;
      }
      if (mask[k])
      {
        lo_t = std::min(lo_t, s);
        hi_t = std::max(hi_t, s);
      }
    }
    if (!std::isfinite(lo_t))
      lo_t = 0.0f;
    if (!std::isfinite(hi_t))
      hi_t = 0.0f;
    std::array<int, 3> lo, hi;
    for (int c = 0; c < 3; c++)
    {
      const float pl = axis[c] * lo_t;
      const float ph = axis[c] * hi_t;
      lo[c] = static_cast<int>(std::clamp(mean[c] + pl, 0.0f, 255.0f));
      hi[c] = static_cast<int>(std::clamp(mean[c] + ph, 0.0f, 255.0f));
    }

    const int a = To565(hi[0], hi[1], hi[2]);
    const int b = To565(lo[0], lo[1], lo[2]);
    bool has_alpha = false;
    for (int k = 0; k < 16; k++)
      has_alpha |= !mask[k];
    const int four0 = a > b ? a : b;
    const int four1 = a > b ? b : a;
    const int c0 = has_alpha ? four1 : four0;
    const int c1 = has_alpha ? four0 : four1;
    const auto e0 = From565(c0), e1 = From565(c1);
    std::array<std::array<int, 3>, 4> pal;
    for (int c = 0; c < 3; c++)
    {
      pal[0][c] = e0[c];
      pal[1][c] = e1[c];
      if (has_alpha)
      {
        pal[2][c] = (e0[c] + e1[c]) / 2;
        pal[3][c] = 100000;
      }
      else
      {
        pal[2][c] = (2 * e0[c] + e1[c]) / 3;
        pal[3][c] = (e0[c] + 2 * e1[c]) / 3;
      }
    }
    std::array<int, 16> idx{};
    for (int k = 0; k < 16; k++)
    {
      if (!mask[k])
      {
        idx[k] = 3;
        continue;
      }
      if (c0 == c1 && !has_alpha)
        continue;
      int best = 0;
      int64_t best_d = std::numeric_limits<int64_t>::max();
      for (int p = 0; p < 4; p++)
      {
        int64_t d = 0;
        for (int c = 0; c < 3; c++)
        {
          const int64_t v = static_cast<int64_t>(flat[k][c]) - pal[p][c];
          d += v * v;
        }
        if (d < best_d)
        {
          best_d = d;
          best = p;
        }
      }
      idx[k] = best;
    }
    *out_c0 = static_cast<uint16_t>(c0);
    *out_c1 = static_cast<uint16_t>(c1);
    *rows = PackRows(idx);
  });
}

int EncodeMipChain(const std::vector<uint8_t>& rgb, const std::vector<uint8_t>& opaque, int width,
                   int height, bool mipmaps, int min_edge, std::vector<uint8_t>* out)
{
  int w = width, h = height;
  std::vector<float> fcol(rgb.begin(), rgb.end());
  std::vector<float> fcov(opaque.size());
  for (size_t i = 0; i < opaque.size(); i++)
    fcov[i] = opaque[i] ? 1.0f : 0.0f;

  int count = 0;
  std::vector<uint8_t> level_rgb, level_mask;
  while (true)
  {
    level_rgb.resize(fcol.size());
    for (size_t i = 0; i < fcol.size(); i++)
      level_rgb[i] = static_cast<uint8_t>(std::clamp(fcol[i] + 0.5f, 0.0f, 255.0f));
    level_mask.resize(fcov.size());
    for (size_t i = 0; i < fcov.size(); i++)
      level_mask[i] = fcov[i] >= 0.5f;
    const auto level = CmprEncodeBlocks(level_rgb.data(), level_mask.data(), w, h);
    out->insert(out->end(), level.begin(), level.end());
    count++;
    if (!mipmaps || std::min(w, h) <= std::max(8, min_edge))
      break;

    const int nw = w / 2, nh = h / 2;
    std::vector<float> ncol(static_cast<size_t>(nw) * nh * 3);
    std::vector<float> ncov(static_cast<size_t>(nw) * nh);
    for (int y = 0; y < nh; y++)
    {
      for (int x = 0; x < nw; x++)
      {
        const size_t s[4] = {static_cast<size_t>(2 * y) * w + 2 * x,
                             static_cast<size_t>(2 * y) * w + 2 * x + 1,
                             static_cast<size_t>(2 * y + 1) * w + 2 * x,
                             static_cast<size_t>(2 * y + 1) * w + 2 * x + 1};
        float cover = fcov[s[0]];
        for (int i = 1; i < 4; i++)
          cover = cover + fcov[s[i]];
        const size_t d = static_cast<size_t>(y) * nw + x;
        for (int c = 0; c < 3; c++)
        {
          float sum = fcol[s[0] * 3 + c] * fcov[s[0]];
          for (int i = 1; i < 4; i++)
          {
            const float p = fcol[s[i] * 3 + c] * fcov[s[i]];
            sum = sum + p;
          }
          ncol[d * 3 + c] = cover > 0 ? sum / std::max(cover, 1e-6f) : 0.0f;
        }
        ncov[d] = cover / 4;
      }
    }
    fcol = std::move(ncol);
    fcov = std::move(ncov);
    w = nw;
    h = nh;
  }
  return count;
}

std::vector<uint8_t> CmprDecode(const uint8_t* data, size_t size, int width, int height)
{
  const int tw = (width + 7) / 8, th = (height + 7) / 8;
  std::vector<uint8_t> img(static_cast<size_t>(tw) * 8 * th * 8 * 4, 0);
  const int full_w = tw * 8;
  if (size < static_cast<size_t>(tw) * th * 32)
    return {};
  size_t pos = 0;
  for (int ty = 0; ty < th; ty++)
  {
    for (int tx = 0; tx < tw; tx++)
    {
      for (int sub = 0; sub < 4; sub++, pos += 8)
      {
        const uint8_t* blk = data + pos;
        const int c0 = (blk[0] << 8) | blk[1];
        const int c1 = (blk[2] << 8) | blk[3];
        const auto e0 = From565(c0), e1 = From565(c1);
        std::array<std::array<int, 4>, 4> pal;
        for (int c = 0; c < 3; c++)
        {
          pal[0][c] = e0[c];
          pal[1][c] = e1[c];
          pal[2][c] = c0 > c1 ? (2 * e0[c] + e1[c]) / 3 : (e0[c] + e1[c]) / 2;
          pal[3][c] = c0 > c1 ? (e0[c] + 2 * e1[c]) / 3 : 0;
        }
        pal[0][3] = pal[1][3] = pal[2][3] = 255;
        pal[3][3] = c0 > c1 ? 255 : 0;
        const int ox = tx * 8 + (sub & 1) * 4;
        const int oy = ty * 8 + (sub >> 1) * 4;
        for (int r = 0; r < 4; r++)
        {
          for (int col = 0; col < 4; col++)
          {
            const int i = (blk[4 + r] >> (6 - 2 * col)) & 3;
            uint8_t* px = &img[(static_cast<size_t>(oy + r) * full_w + ox + col) * 4];
            for (int c = 0; c < 4; c++)
              px[c] = static_cast<uint8_t>(pal[i][c]);
          }
        }
      }
    }
  }
  if (full_w == width && th * 8 == height)
    return img;
  std::vector<uint8_t> cropped(static_cast<size_t>(width) * height * 4);
  for (int y = 0; y < height; y++)
    std::copy_n(&img[static_cast<size_t>(y) * full_w * 4], width * 4,
                &cropped[static_cast<size_t>(y) * width * 4]);
  return cropped;
}
}  // namespace GxBake
