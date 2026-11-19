// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

// A port of Pillow's libImaging/Resample.c (two-pass convolution with 22-bit fixed-point
// coefficients) and of ImagingScaleAffine's nearest-neighbour path in Geometry.c.

#include "GxBake/Resample.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace GxBake
{
namespace
{
constexpr int PRECISION_BITS = 32 - 8 - 2;

double Sinc(double x)
{
  if (x == 0.0)
    return 1.0;
  x = x * std::numbers::pi;
  return std::sin(x) / x;
}

double LanczosFilter(double x)
{
  if (-3.0 <= x && x < 3.0)
    return Sinc(x) * Sinc(x / 3);
  return 0.0;
}

double BicubicFilter(double x)
{
  constexpr double a = -0.5;
  if (x < 0.0)
    x = -x;
  if (x < 1.0)
    return ((a + 2.0) * x - (a + 3.0)) * x * x + 1;
  if (x < 2.0)
    return (((x - 5) * x + 8) * x - 4) * a;
  return 0.0;
}

double BoxFilter(double x)
{
  if (x > -0.5 && x <= 0.5)
    return 1.0;
  return 0.0;
}

struct Coeffs
{
  int ksize = 0;
  std::vector<int> bounds;   // xmin, xcount per output pixel
  std::vector<int32_t> kk;  // ksize fixed-point weights per output pixel
};

Coeffs Precompute(int in_size, float in0, float in1, int out_size, Filter filter)
{
  const double filter_support =
      filter == Filter::Lanczos ? 3.0 : filter == Filter::Box ? 0.5 : 2.0;
  double (*fn)(double) = filter == Filter::Lanczos ? LanczosFilter :
                         filter == Filter::Box     ? BoxFilter :
                                                     BicubicFilter;

  double scale = static_cast<double>(in1 - in0) / out_size;
  double filterscale = scale;
  if (filterscale < 1.0)
    filterscale = 1.0;
  const double support = filter_support * filterscale;
  const int ksize = static_cast<int>(std::ceil(support)) * 2 + 1;

  Coeffs c;
  c.ksize = ksize;
  c.bounds.resize(out_size * 2);
  std::vector<double> pre(static_cast<size_t>(out_size) * ksize, 0.0);
  for (int xx = 0; xx < out_size; xx++)
  {
    const double center = in0 + (xx + 0.5) * scale;
    double ww = 0.0;
    const double ss = 1.0 / filterscale;
    int xmin = static_cast<int>(center - support + 0.5);
    if (xmin < 0)
      xmin = 0;
    int xmax = static_cast<int>(center + support + 0.5);
    if (xmax > in_size)
      xmax = in_size;
    xmax -= xmin;
    double* k = &pre[static_cast<size_t>(xx) * ksize];
    for (int x = 0; x < xmax; x++)
    {
      const double w = fn((x + xmin - center + 0.5) * ss);
      k[x] = w;
      ww += w;
    }
    for (int x = 0; x < xmax; x++)
    {
      if (ww != 0.0)
        k[x] /= ww;
    }
    c.bounds[xx * 2 + 0] = xmin;
    c.bounds[xx * 2 + 1] = xmax;
  }
  c.kk.resize(pre.size());
  for (size_t i = 0; i < pre.size(); i++)
  {
    if (pre[i] < 0)
      c.kk[i] = static_cast<int32_t>(-0.5 + pre[i] * (1 << PRECISION_BITS));
    else
      c.kk[i] = static_cast<int32_t>(0.5 + pre[i] * (1 << PRECISION_BITS));
  }
  return c;
}

uint8_t Clip8(int32_t in)
{
  if (in >= (1 << PRECISION_BITS << 8))
    return 255;
  if (in <= 0)
    return 0;
  return static_cast<uint8_t>(in >> PRECISION_BITS);
}

int Coord(double v)
{
  return v < 0.0 ? -1 : static_cast<int>(v);
}
}  // namespace

std::vector<uint8_t> Resize(const std::vector<uint8_t>& src, int width, int height, int channels,
                            int out_width, int out_height, Filter filter)
{
  if (filter == Filter::Nearest)
  {
    // ImagingScaleAffine: positions are accumulated in doubles, then truncated.
    const double a0 = static_cast<double>(width) / out_width;
    const double a4 = static_cast<double>(height) / out_height;
    std::vector<int> xin(out_width, -1);
    double xo = a0 * 0.5;
    for (int x = 0; x < out_width; x++)
    {
      const int xi = Coord(xo);
      if (xi >= 0 && xi < width)
        xin[x] = xi;
      xo += a0;
    }
    std::vector<uint8_t> out(static_cast<size_t>(out_width) * out_height * channels, 0);
    double yo = a4 * 0.5;
    for (int y = 0; y < out_height; y++)
    {
      const int yi = Coord(yo);
      if (yi >= 0 && yi < height)
      {
        for (int x = 0; x < out_width; x++)
        {
          if (xin[x] < 0)
            continue;
          for (int c = 0; c < channels; c++)
          {
            out[(static_cast<size_t>(y) * out_width + x) * channels + c] =
                src[(static_cast<size_t>(yi) * width + xin[x]) * channels + c];
          }
        }
      }
      yo += a4;
    }
    return out;
  }

  const bool need_horizontal = out_width != width;
  const bool need_vertical = out_height != height;
  const Coeffs horiz = Precompute(width, 0.0f, static_cast<float>(width), out_width, filter);
  const Coeffs vert = Precompute(height, 0.0f, static_cast<float>(height), out_height, filter);

  std::vector<uint8_t> cur = src;
  int cur_w = width;
  if (need_horizontal)
  {
    std::vector<uint8_t> tmp(static_cast<size_t>(out_width) * height * channels);
    for (int y = 0; y < height; y++)
    {
      for (int xx = 0; xx < out_width; xx++)
      {
        const int xmin = horiz.bounds[xx * 2];
        const int xcount = horiz.bounds[xx * 2 + 1];
        const int32_t* k = &horiz.kk[static_cast<size_t>(xx) * horiz.ksize];
        for (int c = 0; c < channels; c++)
        {
          int32_t ss = 1 << (PRECISION_BITS - 1);
          for (int x = 0; x < xcount; x++)
            ss += src[(static_cast<size_t>(y) * width + x + xmin) * channels + c] * k[x];
          tmp[(static_cast<size_t>(y) * out_width + xx) * channels + c] = Clip8(ss);
        }
      }
    }
    cur = std::move(tmp);
    cur_w = out_width;
  }
  if (need_vertical)
  {
    std::vector<uint8_t> tmp(static_cast<size_t>(cur_w) * out_height * channels);
    for (int yy = 0; yy < out_height; yy++)
    {
      const int ymin = vert.bounds[yy * 2];
      const int ycount = vert.bounds[yy * 2 + 1];
      const int32_t* k = &vert.kk[static_cast<size_t>(yy) * vert.ksize];
      for (int xx = 0; xx < cur_w; xx++)
      {
        for (int c = 0; c < channels; c++)
        {
          int32_t ss = 1 << (PRECISION_BITS - 1);
          for (int y = 0; y < ycount; y++)
            ss += cur[(static_cast<size_t>(y + ymin) * cur_w + xx) * channels + c] * k[y];
          tmp[(static_cast<size_t>(yy) * cur_w + xx) * channels + c] = Clip8(ss);
        }
      }
    }
    cur = std::move(tmp);
  }
  return cur;
}

std::vector<uint8_t> ResizeRgba(const std::vector<uint8_t>& src, int width, int height,
                                int out_width, int out_height, Filter filter)
{
  if (filter == Filter::Nearest)
    return Resize(src, width, height, 4, out_width, out_height, filter);
  // Convert.c rgbA2rgba / rgba2rgbA
  std::vector<uint8_t> premultiplied(src.size());
  for (size_t i = 0; i + 3 < src.size(); i += 4)
  {
    const unsigned alpha = src[i + 3];
    for (int c = 0; c < 3; c++)
    {
      const unsigned tmp = src[i + c] * alpha + 128;
      premultiplied[i + c] = static_cast<uint8_t>(((tmp >> 8) + tmp) >> 8);
    }
    premultiplied[i + 3] = src[i + 3];
  }
  std::vector<uint8_t> out = Resize(premultiplied, width, height, 4, out_width, out_height, filter);
  for (size_t i = 0; i + 3 < out.size(); i += 4)
  {
    const unsigned alpha = out[i + 3];
    if (alpha == 255 || alpha == 0)
      continue;
    for (int c = 0; c < 3; c++)
      out[i + c] = static_cast<uint8_t>(std::min(255u, (255 * out[i + c]) / alpha));
  }
  return out;
}

std::vector<uint8_t> ResizeMaskNearest(const std::vector<uint8_t>& src, int width, int height,
                                       int out_width, int out_height)
{
  return Resize(src, width, height, 1, out_width, out_height, Filter::Nearest);
}
}  // namespace GxBake
