// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

// Source engine maps (VBSP v19-21 and 25) rebuilt as GoldSrc BSP v30, ported from the Python
// tool (core/source_bsp.py and core/source_map.py) with byte-identical output.
//
// numpy's arithmetic is repeated step by step: float64 dot products and matrix products are the
// fused chains OpenBLAS uses on x86-64, float32 ones follow its sgemv blocks, and inv / solve /
// det are OpenBLAS's getf2 factorisation with its triangular solves.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

#include <fmt/format.h>
#include <lzma.h>
#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include "Common/MinizipUtil.h"
#include "GxBake/Bsp30.h"
#include "GxBake/Source.h"
#include "GxBake/Studio.h"
#include "GxBake/Vtf.h"

namespace GxBake
{
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace
{
// -- small helpers ------------------------------------------------------------------------------

template <typename T>
T Get(const uint8_t* p)
{
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
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
  size_t a = 0, b = s.size();
  while (a < b && s[a] == c)
    a++;
  while (b > a && s[b - 1] == c)
    b--;
  return s.substr(a, b - a);
}

std::string Slashes(std::string s)
{
  std::replace(s.begin(), s.end(), '\\', '/');
  return s;
}

std::string PathString(const fs::path& p)
{
  const auto u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
}

std::optional<Bytes> ReadAll(const fs::path& path)
{
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec)
    return std::nullopt;
  std::FILE* f = std::fopen(PathString(path).c_str(), "rb");
  if (!f)
    return std::nullopt;
  Bytes out(size);
  const size_t got = size ? std::fread(out.data(), 1, size, f) : 0;
  std::fclose(f);
  out.resize(got);
  return out;
}

Vec3 Sub(const Vec3& a, const Vec3& b)
{
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Vec3 Scale(const Vec3& a, double k)
{
  return {a[0] * k, a[1] * k, a[2] * k};
}

Vec3 Neg(const Vec3& a)
{
  return {-a[0], -a[1], -a[2]};
}

// numpy's mean over the rows of a small (k, 3) array: rows added in order, then divided.
Vec3 Mean(const Winding& w)
{
  Vec3 s = {0.0, 0.0, 0.0};
  for (const Vec3& p : w)
  {
    for (int i = 0; i < 3; i++)
      s[i] += p[i];
  }
  for (double& c : s)
    c /= static_cast<double>(w.size());
  return s;
}

// numpy's float32 (k, 3) @ (3,) (OpenBLAS sgemv on x86-64) for row |i| of |rows|.
float SRow(const std::array<float, 3>& a, const std::array<float, 3>& v, size_t i, size_t rows)
{
  if (rows == 1)
  {
    const float p0 = a[0] * v[0], p1 = a[1] * v[1], p2 = a[2] * v[2];
    return static_cast<float>(static_cast<double>(p0) + static_cast<double>(p1) +
                              static_cast<double>(p2));
  }
  const size_t full8 = rows / 8 * 8;
  const auto fused = [&] {
    float s = a[1] * v[1];
    s = std::fma(a[0], v[0], s);
    return std::fma(a[2], v[2], s);
  };
  if (i < full8)
  {
    float s = a[0] * v[0];
    s = std::fma(a[1], v[1], s);
    return std::fma(a[2], v[2], s);
  }
  size_t rem = rows - full8;
  size_t at = full8;
  if (rem >= 4)
  {
    if (i < at + 4)
      return fused();
    at += 4;
    rem -= 4;
  }
  if (rem >= 2 && i < at + 2)
  {
    const float p0 = a[0] * v[0], p1 = a[1] * v[1], p2 = a[2] * v[2];
    return (p0 + p1) + p2;
  }
  return fused();
}

// np.linalg.norm of a float32 3-vector (OpenBLAS sdot, then sqrtf).
float Norm32(const float* v)
{
  const float p0 = v[0] * v[0], p1 = v[1] * v[1], p2 = v[2] * v[2];
  const float s = static_cast<float>(static_cast<double>(p0) + static_cast<double>(p1) +
                                     static_cast<double>(p2));
  return std::sqrt(s);
}

// Python's round(x, 3) on a float.
double Round3(double x)
{
  if (!std::isfinite(x))
    return x;
  char buf[512];
  std::snprintf(buf, sizeof(buf), "%.3f", x);
  return std::strtod(buf, nullptr);
}

std::string FormatG(double v)
{
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%g", v);
  return buf;
}

// -- LAPACK as numpy's OpenBLAS runs it --------------------------------------------------------

template <int N>
using Mat = std::array<std::array<double, N>, N>;

// getf2: left-looking LU with partial pivoting. false when a pivot is exactly zero.
template <int N>
bool Getf2(Mat<N>* m, std::array<int, N>* piv)
{
  Mat<N>& a = *m;
  bool ok = true;
  for (int j = 0; j < N; j++)
  {
    for (int i = 0; i < j; i++)
    {
      if ((*piv)[i] != i)
        std::swap(a[i][j], a[(*piv)[i]][j]);
    }
    for (int i = 1; i < j; i++)
    {
      double s = a[i][0] * a[0][j];
      for (int k = 1; k < i; k++)
        s = std::fma(a[i][k], a[k][j], s);
      a[i][j] -= s;
    }
    if (j > 0)
    {
      for (int i = j; i < N; i++)
      {
        double s = a[i][0] * a[0][j];
        for (int k = 1; k < j; k++)
          s = std::fma(a[i][k], a[k][j], s);
        a[i][j] = a[i][j] - s;
      }
    }
    int jp = j;
    for (int i = j + 1; i < N; i++)
    {
      if (std::abs(a[i][j]) > std::abs(a[jp][j]))
        jp = i;
    }
    (*piv)[j] = jp;
    const double t = a[jp][j];
    if (jp != j)
    {
      for (int k = 0; k <= j; k++)
        std::swap(a[j][k], a[jp][k]);
    }
    if (t != 0)
    {
      const double r = 1.0 / t;
      for (int i = j + 1; i < N; i++)
        a[i][j] *= r;
    }
    else
    {
      ok = false;
    }
  }
  return ok;
}

// getrs for one right-hand side (trsv), or several (trsm: reciprocal diagonal).
template <int N>
std::array<double, N> Getrs(const Mat<N>& a, const std::array<int, N>& piv,
                            std::array<double, N> b, bool several)
{
  for (int i = 0; i < N; i++)
  {
    if (piv[i] != i)
      std::swap(b[i], b[piv[i]]);
  }
  for (int i = 0; i < N; i++)
  {
    for (int k = i + 1; k < N; k++)
      b[k] = std::fma(-b[i], a[k][i], b[k]);
  }
  for (int i = N - 1; i >= 0; i--)
  {
    b[i] = several ? b[i] * (1.0 / a[i][i]) : b[i] / a[i][i];
    for (int k = 0; k < i; k++)
      b[k] = std::fma(-b[i], a[k][i], b[k]);
  }
  return b;
}

// np.linalg.inv of a 3x3 (gesv against the identity: trsm kernel blocks of two and one rows).
std::optional<Mat<3>> Inv3(Mat<3> m)
{
  std::array<int, 3> piv;
  if (!Getf2<3>(&m, &piv))
    return std::nullopt;
  const Mat<3>& a = m;
  Mat<3> out;
  for (int c = 0; c < 3; c++)
  {
    std::array<double, 3> b = {0.0, 0.0, 0.0};
    b[c] = 1.0;
    for (int i = 0; i < 3; i++)
    {
      if (piv[i] != i)
        std::swap(b[i], b[piv[i]]);
    }
    b[1] = std::fma(-b[0], a[1][0], b[1]);
    double acc = a[2][0] * b[0];
    acc = std::fma(a[2][1], b[1], acc);
    b[2] = b[2] - acc;
    const double x2 = b[2] * (1.0 / a[2][2]);
    b[1] = b[1] - a[1][2] * x2;
    b[0] = b[0] - a[0][2] * x2;
    const double x1 = b[1] * (1.0 / a[1][1]);
    b[0] = std::fma(-x1, a[0][1], b[0]);
    const double x0 = b[0] * (1.0 / a[0][0]);
    out[0][c] = x0;
    out[1][c] = x1;
    out[2][c] = x2;
  }
  return out;
}

// np.linalg.det: sign * exp(sum of log |u_ii|).
double Det4(Mat<4> m)
{
  std::array<int, 4> piv;
  if (!Getf2<4>(&m, &piv))
    return 0.0;
  double sign = 1.0, logdet = 0.0;
  for (int i = 0; i < 4; i++)
  {
    if (piv[i] != i)
      sign = -sign;
  }
  for (int i = 0; i < 4; i++)
  {
    double e = m[i][i];
    if (e < 0)
    {
      sign = -sign;
      e = -e;
    }
    logdet += std::log(e);
  }
  return sign * std::exp(logdet);
}

// -- the VBSP file ------------------------------------------------------------------------------

constexpr int L_ENTITIES = 0, L_PLANES = 1, L_TEXDATA = 2, L_VERTEXES = 3, L_VISIBILITY = 4,
              L_NODES = 5, L_TEXINFO = 6, L_FACES = 7, L_LIGHTING = 8, L_LEAFS = 10,
              L_EDGES = 12, L_SURFEDGES = 13, L_MODELS = 14, L_LEAFFACES = 16,
              L_LEAFBRUSHES = 17, L_BRUSHES = 18, L_BRUSHSIDES = 19, L_DISPINFO = 26,
              L_DISP_VERTS = 33, L_GAME_LUMP = 35, L_PAKFILE = 40, L_TEXDATA_STRING_DATA = 43,
              L_TEXDATA_STRING_TABLE = 44, L_OVERLAYS = 45, L_LIGHTING_HDR = 53,
              L_FACES_HDR = 58;

constexpr int CONTENTS_SOLID = 0x1, CONTENTS_SLIME = 0x10, CONTENTS_WATER = 0x20;
constexpr int MASK_PLAYERSOLID = 0x1 | 0x2 | 0x8 | 0x4000 | 0x10000;
constexpr int SURF_SKY2D = 0x2, SURF_SKY = 0x4, SURF_TRIGGER = 0x40, SURF_NODRAW = 0x80,
              SURF_HINT = 0x100, SURF_SKIP = 0x200, SURF_NOLIGHT = 0x400,
              SURF_BUMPLIGHT = 0x800;

struct Lump
{
  int32_t offset = 0, length = 0, version = 0, fourcc = 0;
};

Bytes ValveLzma(const uint8_t* b, size_t n)
{
  if (n < 17)
    return {};
  const uint32_t actual = Get<uint32_t>(b + 4);
  Bytes alone(b + 12, b + 17);
  const uint64_t size64 = actual;
  for (int i = 0; i < 8; i++)
    alone.push_back(static_cast<uint8_t>(size64 >> (8 * i)));
  alone.insert(alone.end(), b + 17, b + n);
  lzma_stream strm = LZMA_STREAM_INIT;
  if (lzma_alone_decoder(&strm, UINT64_MAX) != LZMA_OK)
    return {};
  Bytes out(actual);
  strm.next_in = alone.data();
  strm.avail_in = alone.size();
  strm.next_out = out.data();
  strm.avail_out = out.size();
  (void)!lzma_code(&strm, LZMA_FINISH);
  out.resize(out.size() - strm.avail_out);
  lzma_end(&strm);
  return out;
}

struct Plane
{
  Vec3 normal;  // float32 values
  double dist;
};
struct TexInfo
{
  std::array<std::array<float, 4>, 2> vecs, lmvecs;
  int flags, texdata;
};
struct TexData
{
  int name, width, height;
};
struct Face
{
  int planenum, firstedge, numedges, texinfo, dispinfo;
  std::array<uint8_t, 4> styles;
  int lightofs;
  std::array<int, 2> lm_mins, lm_size;
};
struct Node
{
  int planenum;
  std::array<int, 2> children;
  Vec3 mins, maxs;
  int firstface, numfaces;
};
struct Leaf
{
  int contents, cluster;
  Vec3 mins, maxs;
  int firstleafface, numleaffaces, firstleafbrush, numleafbrushes;
};
struct Model
{
  std::array<float, 9> bounds;
  int headnode, firstface, numfaces;
};
struct Brush
{
  int firstside, numsides, contents;
};
struct DispInfo
{
  Vec3 start;
  int firstvert, power;
};
struct DispVert
{
  Vec3 vec;
  double dist;
};
struct Overlay
{
  int texinfo, facecount;
  std::array<int, 64> faces;
  std::array<float, 2> u, v;
  std::array<std::array<float, 3>, 4> points;
  std::array<float, 3> origin, normal;
};
struct StaticProp
{
  std::string model;
  std::array<float, 3> origin, angles;
};

class Vbsp
{
public:
  Vbsp(const Bytes& data) : m_data(data)
  {
    version = Get<int32_t>(data.data() + 4);
    for (int i = 0; i < 64; i++)
    {
      const uint8_t* p = data.data() + 8 + 16 * i;
      Lump l{Get<int32_t>(p), Get<int32_t>(p + 4), Get<int32_t>(p + 8), Get<int32_t>(p + 12)};
      if (l.offset < 0 || l.length < 0 ||
          static_cast<int64_t>(l.offset) + l.length > static_cast<int64_t>(data.size()))
      {
        l.offset = 0;
        l.length = 0;
      }
      lumps[i] = l;
    }
  }

  const Bytes& Raw(int i)
  {
    auto it = m_cache.find(i);
    if (it != m_cache.end())
      return it->second;
    const Lump& l = lumps[i];
    const uint8_t* p = m_data.data() + l.offset;
    Bytes b(p, p + l.length);
    if (b.size() >= 4 && std::memcmp(b.data(), "LZMA", 4) == 0)
      b = ValveLzma(b.data(), b.size());
    return m_cache.emplace(i, std::move(b)).first->second;
  }

  // The records of a lump, each read from |size| bytes.
  template <typename T, typename F>
  std::vector<T> Records(int lump, size_t size, F read)
  {
    const Bytes& b = Raw(lump);
    std::vector<T> out(b.size() / size);
    for (size_t i = 0; i < out.size(); i++)
      out[i] = read(b.data() + i * size);
    return out;
  }

  bool Wide() const { return version >= 25; }

  std::vector<Plane> Planes()
  {
    return Records<Plane>(L_PLANES, 20, [](const uint8_t* p) {
      return Plane{{Get<float>(p), Get<float>(p + 4), Get<float>(p + 8)}, Get<float>(p + 12)};
    });
  }
  std::vector<Vec3> Vertexes()
  {
    return Records<Vec3>(L_VERTEXES, 12, [](const uint8_t* p) {
      return Vec3{Get<float>(p), Get<float>(p + 4), Get<float>(p + 8)};
    });
  }
  std::vector<std::array<uint32_t, 2>> Edges()
  {
    if (Wide())
    {
      return Records<std::array<uint32_t, 2>>(L_EDGES, 8, [](const uint8_t* p) {
        return std::array<uint32_t, 2>{Get<uint32_t>(p), Get<uint32_t>(p + 4)};
      });
    }
    return Records<std::array<uint32_t, 2>>(L_EDGES, 4, [](const uint8_t* p) {
      return std::array<uint32_t, 2>{Get<uint16_t>(p), Get<uint16_t>(p + 2)};
    });
  }
  std::vector<int> Surfedges()
  {
    return Records<int>(L_SURFEDGES, 4, [](const uint8_t* p) { return Get<int32_t>(p); });
  }
  std::vector<TexInfo> TexInfos()
  {
    return Records<TexInfo>(L_TEXINFO, 72, [](const uint8_t* p) {
      TexInfo t;
      for (int r = 0; r < 2; r++)
      {
        for (int c = 0; c < 4; c++)
        {
          t.vecs[r][c] = Get<float>(p + 16 * r + 4 * c);
          t.lmvecs[r][c] = Get<float>(p + 32 + 16 * r + 4 * c);
        }
      }
      t.flags = Get<int32_t>(p + 64);
      t.texdata = Get<int32_t>(p + 68);
      return t;
    });
  }
  std::vector<TexData> TexDatas()
  {
    return Records<TexData>(L_TEXDATA, 32, [](const uint8_t* p) {
      return TexData{Get<int32_t>(p + 12), Get<int32_t>(p + 16), Get<int32_t>(p + 20)};
    });
  }
  std::vector<Face> Faces(int lump)
  {
    if (Wide())
    {
      return Records<Face>(lump, 72, [](const uint8_t* p) {
        Face f;
        f.planenum = static_cast<int>(Get<uint32_t>(p));
        f.firstedge = Get<int32_t>(p + 8);
        f.numedges = Get<int32_t>(p + 12);
        f.texinfo = Get<int32_t>(p + 16);
        f.dispinfo = Get<int32_t>(p + 20);
        std::memcpy(f.styles.data(), p + 28, 4);
        f.lightofs = Get<int32_t>(p + 32);
        f.lm_mins = {Get<int32_t>(p + 40), Get<int32_t>(p + 44)};
        f.lm_size = {Get<int32_t>(p + 48), Get<int32_t>(p + 52)};
        return f;
      });
    }
    return Records<Face>(lump, 56, [](const uint8_t* p) {
      Face f;
      f.planenum = Get<uint16_t>(p);
      f.firstedge = Get<int32_t>(p + 4);
      f.numedges = Get<int16_t>(p + 8);
      f.texinfo = Get<int16_t>(p + 10);
      f.dispinfo = Get<int16_t>(p + 12);
      std::memcpy(f.styles.data(), p + 16, 4);
      f.lightofs = Get<int32_t>(p + 20);
      f.lm_mins = {Get<int32_t>(p + 28), Get<int32_t>(p + 32)};
      f.lm_size = {Get<int32_t>(p + 36), Get<int32_t>(p + 40)};
      return f;
    });
  }
  std::vector<Node> Nodes()
  {
    if (Wide())
    {
      return Records<Node>(L_NODES, 48, [](const uint8_t* p) {
        Node n;
        n.planenum = Get<int32_t>(p);
        n.children = {Get<int32_t>(p + 4), Get<int32_t>(p + 8)};
        for (int i = 0; i < 3; i++)
        {
          n.mins[i] = Get<float>(p + 12 + 4 * i);
          n.maxs[i] = Get<float>(p + 24 + 4 * i);
        }
        n.firstface = static_cast<int>(Get<uint32_t>(p + 36));
        n.numfaces = static_cast<int>(Get<uint32_t>(p + 40));
        return n;
      });
    }
    return Records<Node>(L_NODES, 32, [](const uint8_t* p) {
      Node n;
      n.planenum = Get<int32_t>(p);
      n.children = {Get<int32_t>(p + 4), Get<int32_t>(p + 8)};
      for (int i = 0; i < 3; i++)
      {
        n.mins[i] = Get<int16_t>(p + 12 + 2 * i);
        n.maxs[i] = Get<int16_t>(p + 18 + 2 * i);
      }
      n.firstface = Get<uint16_t>(p + 24);
      n.numfaces = Get<uint16_t>(p + 26);
      return n;
    });
  }
  std::vector<Leaf> Leafs()
  {
    if (Wide())
    {
      return Records<Leaf>(L_LEAFS, 56, [](const uint8_t* p) {
        Leaf l;
        l.contents = Get<int32_t>(p);
        l.cluster = Get<int32_t>(p + 4);
        for (int i = 0; i < 3; i++)
        {
          l.mins[i] = Get<float>(p + 12 + 4 * i);
          l.maxs[i] = Get<float>(p + 24 + 4 * i);
        }
        l.firstleafface = static_cast<int>(Get<uint32_t>(p + 36));
        l.numleaffaces = static_cast<int>(Get<uint32_t>(p + 40));
        l.firstleafbrush = static_cast<int>(Get<uint32_t>(p + 44));
        l.numleafbrushes = static_cast<int>(Get<uint32_t>(p + 48));
        return l;
      });
    }
    const bool v0 = version < 20 && lumps[L_LEAFS].version == 0;
    return Records<Leaf>(L_LEAFS, v0 ? 56 : 32, [](const uint8_t* p) {
      Leaf l;
      l.contents = Get<int32_t>(p);
      l.cluster = Get<int16_t>(p + 4);
      for (int i = 0; i < 3; i++)
      {
        l.mins[i] = Get<int16_t>(p + 8 + 2 * i);
        l.maxs[i] = Get<int16_t>(p + 14 + 2 * i);
      }
      l.firstleafface = Get<uint16_t>(p + 20);
      l.numleaffaces = Get<uint16_t>(p + 22);
      l.firstleafbrush = Get<uint16_t>(p + 24);
      l.numleafbrushes = Get<uint16_t>(p + 26);
      return l;
    });
  }
  std::vector<Model> Models()
  {
    return Records<Model>(L_MODELS, 48, [](const uint8_t* p) {
      Model m;
      for (int i = 0; i < 9; i++)
        m.bounds[i] = Get<float>(p + 4 * i);
      m.headnode = Get<int32_t>(p + 36);
      m.firstface = Get<int32_t>(p + 40);
      m.numfaces = Get<int32_t>(p + 44);
      return m;
    });
  }
  std::vector<int> Indices(int lump)
  {
    if (Wide())
      return Records<int>(lump, 4, [](const uint8_t* p) { return int(Get<uint32_t>(p)); });
    return Records<int>(lump, 2, [](const uint8_t* p) { return int(Get<uint16_t>(p)); });
  }
  std::vector<Brush> Brushes()
  {
    return Records<Brush>(L_BRUSHES, 12, [](const uint8_t* p) {
      return Brush{Get<int32_t>(p), Get<int32_t>(p + 4), Get<int32_t>(p + 8)};
    });
  }
  std::vector<int> BrushSidePlanes()
  {
    if (Wide())
      return Records<int>(L_BRUSHSIDES, 16, [](const uint8_t* p) { return int(Get<uint32_t>(p)); });
    return Records<int>(L_BRUSHSIDES, 8, [](const uint8_t* p) { return int(Get<uint16_t>(p)); });
  }
  std::vector<DispInfo> DispInfos()
  {
    return Records<DispInfo>(L_DISPINFO, Wide() ? 232 : 176, [](const uint8_t* p) {
      return DispInfo{{Get<float>(p), Get<float>(p + 4), Get<float>(p + 8)},
                      Get<int32_t>(p + 12),
                      Get<int32_t>(p + 20)};
    });
  }
  std::vector<DispVert> DispVerts()
  {
    return Records<DispVert>(L_DISP_VERTS, 20, [](const uint8_t* p) {
      return DispVert{{Get<float>(p), Get<float>(p + 4), Get<float>(p + 8)}, Get<float>(p + 12)};
    });
  }
  std::vector<Overlay> Overlays()
  {
    if (Wide())
      return {};
    return Records<Overlay>(L_OVERLAYS, 352, [](const uint8_t* p) {
      Overlay o;
      o.texinfo = Get<int16_t>(p + 4);
      o.facecount = Get<uint16_t>(p + 6);
      for (int i = 0; i < 64; i++)
        o.faces[i] = Get<int32_t>(p + 8 + 4 * i);
      o.u = {Get<float>(p + 264), Get<float>(p + 268)};
      o.v = {Get<float>(p + 272), Get<float>(p + 276)};
      for (int i = 0; i < 4; i++)
      {
        for (int k = 0; k < 3; k++)
          o.points[i][k] = Get<float>(p + 280 + 12 * i + 4 * k);
      }
      for (int k = 0; k < 3; k++)
      {
        o.origin[k] = Get<float>(p + 328 + 4 * k);
        o.normal[k] = Get<float>(p + 340 + 4 * k);
      }
      return o;
    });
  }

  // (faces, lighting, hdr): LDR when present, else HDR.
  std::vector<Face> FacesAndLighting(Bytes* lighting, bool* hdr)
  {
    const Bytes& ldr = Raw(L_LIGHTING);
    if (!ldr.empty())
    {
      *lighting = ldr;
      *hdr = false;
      return Faces(L_FACES);
    }
    *lighting = Raw(L_LIGHTING_HDR);
    *hdr = true;
    return Faces(lumps[L_FACES_HDR].length ? L_FACES_HDR : L_FACES);
  }

  std::vector<std::string> TextureNames()
  {
    const Bytes strdata = Raw(L_TEXDATA_STRING_DATA);
    const auto table =
        Records<int>(L_TEXDATA_STRING_TABLE, 4, [](const uint8_t* p) { return Get<int32_t>(p); });
    std::vector<std::string> out;
    for (const TexData& td : TexDatas())
    {
      const int i = td.name;
      const int64_t off = (0 <= i && i < static_cast<int>(table.size())) ? table[i] : -1;
      if (off < 0 || off >= static_cast<int64_t>(strdata.size()))
      {
        out.emplace_back();
        continue;
      }
      const auto begin = strdata.begin() + off;
      out.emplace_back(begin, std::find(begin, strdata.end(), uint8_t(0)));
    }
    return out;
  }

  std::string EntitiesText()
  {
    const Bytes& b = Raw(L_ENTITIES);
    return std::string(b.begin(), std::find(b.begin(), b.end(), uint8_t(0)));
  }

  // (numclusters, decompressed PVS row per cluster)
  std::vector<Bytes> Visibility()
  {
    const Bytes& vis = Raw(L_VISIBILITY);
    if (vis.size() < 4)
      return {};
    const int n = Get<int32_t>(vis.data());
    const size_t row = (static_cast<size_t>(std::max(n, 0)) + 7) / 8;
    std::vector<Bytes> out;
    for (int c = 0; c < n; c++)
    {
      if (4 + 8 * static_cast<size_t>(c) + 4 > vis.size())
        break;
      const int ofs = Get<int32_t>(vis.data() + 4 + 8 * c);
      Bytes r;
      size_t p = static_cast<size_t>(std::max(ofs, 0));
      while (r.size() < row && p < vis.size())
      {
        const uint8_t ch = vis[p];
        if (ch)
        {
          r.push_back(ch);
          p++;
        }
        else
        {
          if (p + 1 >= vis.size())
            break;
          r.insert(r.end(), vis[p + 1], 0);
          p += 2;
        }
      }
      r.resize(row, 0);
      out.push_back(std::move(r));
    }
    return out;
  }

  std::vector<StaticProp> StaticProps()
  {
    const Lump gl = lumps[L_GAME_LUMP];
    const Bytes raw = Raw(L_GAME_LUMP);
    if (raw.size() < 4)
      return {};
    const int count = Get<int32_t>(raw.data());
    for (int i = 0; i < count; i++)
    {
      const size_t at = 4 + 16 * static_cast<size_t>(i);
      if (at + 16 > raw.size())
        return {};
      if (std::memcmp(raw.data() + at, "prps", 4) != 0)
        continue;
      const int ver = Get<uint16_t>(raw.data() + at + 6);
      (void)ver;
      const int fileofs = Get<int32_t>(raw.data() + at + 8);
      const int filelen = Get<int32_t>(raw.data() + at + 12);
      Bytes b;
      if (gl.fourcc == 0 || fileofs >= static_cast<int>(raw.size()))
      {
        const int64_t a = std::clamp<int64_t>(fileofs, 0, m_data.size());
        const int64_t e = std::clamp<int64_t>(int64_t(fileofs) + filelen, a, m_data.size());
        b.assign(m_data.begin() + a, m_data.begin() + e);
      }
      else
      {
        b = raw;
      }
      if (b.size() >= 4 && std::memcmp(b.data(), "LZMA", 4) == 0)
        b = ValveLzma(b.data(), b.size());
      return ReadSprp(b);
    }
    return {};
  }

  int version = 0;
  std::array<Lump, 64> lumps;

private:
  static std::vector<StaticProp> ReadSprp(const Bytes& b)
  {
    const auto need = [&](int64_t p, int64_t n) { return p >= 0 && p + n <= int64_t(b.size()); };
    int64_t p = 0;
    if (!need(p, 4))
      return {};
    int n = Get<int32_t>(b.data() + p);
    p += 4;
    std::vector<std::string> names;
    for (int i = 0; i < n; i++)
    {
      const int64_t a = std::min<int64_t>(p + 128 * int64_t(i), b.size());
      const int64_t e = std::min<int64_t>(p + 128 * int64_t(i + 1), b.size());
      const auto begin = b.begin() + a;
      names.emplace_back(begin, std::find(begin, b.begin() + e, uint8_t(0)));
    }
    p += 128 * int64_t(n);
    if (!need(p, 4))
      return {};
    n = Get<int32_t>(b.data() + p);
    p += 4 + 2 * int64_t(n);
    if (!need(p, 4))
      return {};
    n = Get<int32_t>(b.data() + p);
    p += 4;
    if (n <= 0)
      return {};
    const int64_t size = (int64_t(b.size()) - p) / n;
    std::vector<StaticProp> out;
    for (int i = 0; i < n; i++)
    {
      const int64_t q = p + i * size;
      if (!need(q, 26) || !need(q + 30, 1) || !need(q + 32, 4))
        return {};
      StaticProp sp;
      for (int k = 0; k < 3; k++)
      {
        sp.origin[k] = Get<float>(b.data() + q + 4 * k);
        sp.angles[k] = Get<float>(b.data() + q + 12 + 4 * k);
      }
      const int ptype = Get<uint16_t>(b.data() + q + 24);
      if (ptype < static_cast<int>(names.size()))
      {
        sp.model = names[ptype];
        out.push_back(std::move(sp));
      }
    }
    return out;
  }

  const Bytes& m_data;
  std::map<int, Bytes> m_cache;
};

// -- entities -----------------------------------------------------------------------------------

using Entity = std::vector<std::pair<std::string, std::string>>;

std::string* EntGet(Entity& e, const std::string& key)
{
  for (auto& [k, v] : e)
  {
    if (k == key)
      return &v;
  }
  return nullptr;
}

void EntSet(Entity& e, const std::string& key, const std::string& value)
{
  if (std::string* v = EntGet(e, key))
    *v = value;
  else
    e.emplace_back(key, value);
}

std::vector<Entity> ParseEntities(const std::string& text)
{
  std::vector<Entity> out;
  std::optional<Entity> cur;
  std::optional<std::string> key;
  size_t i = 0;
  const size_t n = text.size();
  while (i < n)
  {
    const char c = text[i];
    if (c == '{')
    {
      cur = Entity{};
      key.reset();
      i++;
    }
    else if (c == '}')
    {
      if (cur)
        out.push_back(std::move(*cur));
      cur.reset();
      i++;
    }
    else if (c == '"')
    {
      const size_t j = text.find('"', i + 1);
      if (j == std::string::npos)
        break;
      std::string s = text.substr(i + 1, j - i - 1);
      i = j + 1;
      if (!cur)
        continue;
      if (!key)
      {
        key = std::move(s);
      }
      else
      {
        EntSet(*cur, *key, s);
        key.reset();
      }
    }
    else
    {
      i++;
    }
  }
  return out;
}

std::string FormatEntities(const std::vector<Entity>& ents)
{
  std::string out;
  for (const Entity& e : ents)
  {
    out += "{\n";
    for (const auto& [k, v] : e)
      out += "\"" + k + "\" \"" + v + "\"\n";
    out += "}\n";
  }
  return out;
}

// -- content lookup -----------------------------------------------------------------------------

struct VpkMember
{
  std::shared_ptr<const Bytes> dir;
  fs::path chunk;  // archive file, empty when the data follows the tree
  Bytes preload;
  uint32_t offset = 0, length = 0;
  size_t data_base = 0;
};

void ReadVpk(const fs::path& path, std::unordered_map<std::string, VpkMember>* index)
{
  const auto data_opt = ReadAll(path);
  if (!data_opt || data_opt->size() < 12)
    return;
  auto data = std::make_shared<const Bytes>(std::move(*data_opt));
  const Bytes& d = *data;
  if (Get<uint32_t>(d.data()) != 0x55AA1234)
    return;
  const uint32_t version = Get<uint32_t>(d.data() + 4);
  const uint32_t tree = Get<uint32_t>(d.data() + 8);
  const size_t header = version == 1 ? 12 : 28;
  size_t pos = header;
  const size_t data_base = header + tree;
  std::string stem = PathString(path.stem());
  if (Lower(stem).ends_with("_dir"))
    stem.resize(stem.size() - 4);
  bool bad = false;
  const auto cstr = [&] {
    const auto end = std::find(d.begin() + std::min(pos, d.size()), d.end(), uint8_t(0));
    if (end == d.end())
    {
      bad = true;
      return std::string();
    }
    std::string s(d.begin() + pos, end);
    pos = (end - d.begin()) + 1;
    return s;
  };
  // Members are listed in file order; the first of a name wins, so collect them first.
  std::vector<std::pair<std::string, VpkMember>> members;
  while (!bad)
  {
    const std::string ext = cstr();
    if (ext.empty())
      break;
    while (!bad)
    {
      const std::string folder = cstr();
      if (folder.empty())
        break;
      while (!bad)
      {
        const std::string fname = cstr();
        if (fname.empty() || pos + 18 > d.size())
          break;
        const uint16_t preload = Get<uint16_t>(d.data() + pos + 4);
        const uint16_t arch = Get<uint16_t>(d.data() + pos + 6);
        VpkMember m;
        m.dir = data;
        m.offset = Get<uint32_t>(d.data() + pos + 8);
        m.length = Get<uint32_t>(d.data() + pos + 12);
        m.data_base = data_base;
        pos += 18;
        const size_t pend = std::min(pos + preload, d.size());
        m.preload.assign(d.begin() + pos, d.begin() + pend);
        pos += preload;
        if (arch != 0x7FFF)
          m.chunk = path.parent_path() / fs::path(reinterpret_cast<const char8_t*>(fmt::format("{}_{:03d}.vpk", stem, arch).c_str()));
        const bool has_folder = !Strip(Strip(folder, ' '), '\t').empty();
        const bool has_ext = !Strip(Strip(ext, ' '), '\t').empty();
        std::string full = (has_folder ? folder + "/" : "") + fname + (has_ext ? "." + ext : "");
        members.emplace_back(std::move(full), std::move(m));
      }
    }
  }
  if (bad)
    return;  // archives.members raised: the archive is skipped
  for (auto& [name, m] : members)
    index->try_emplace(Lower(name), std::move(m));
}

std::optional<Bytes> ReadVpkMember(const VpkMember& m)
{
  if (!m.length)
    return m.preload;
  Bytes out = m.preload;
  if (m.chunk.empty())
  {
    const Bytes& d = *m.dir;
    const size_t a = std::min(d.size(), m.data_base + m.offset);
    const size_t e = std::min(d.size(), m.data_base + m.offset + m.length);
    out.insert(out.end(), d.begin() + a, d.begin() + e);
    return out;
  }
  std::FILE* f = std::fopen(PathString(m.chunk).c_str(), "rb");
  if (!f)
    return std::nullopt;
  std::fseek(f, m.offset, SEEK_SET);
  const size_t at = out.size();
  out.resize(at + m.length);
  out.resize(at + std::fread(out.data() + at, 1, m.length, f));
  std::fclose(f);
  return out;
}

bool HasDirVpk(const fs::path& dir)
{
  std::error_code ec;
  for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
       it.increment(ec))
  {
    if (PathString(it->path().filename()).ends_with("_dir.vpk"))
      return true;
  }
  return false;
}

std::vector<fs::path> DirVpks(const fs::path& dir)
{
  std::vector<fs::path> out;
  std::error_code ec;
  for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
       it.increment(ec))
  {
    if (PathString(it->path().filename()).ends_with("_dir.vpk"))
      out.push_back(it->path());
  }
  std::ranges::sort(out, [](const fs::path& a, const fs::path& b) {
    return PathString(a) < PathString(b);
  });
  return out;
}

struct MaterialResult
{
  std::optional<RgbaImage> rgba;
  std::string shader;
  std::map<std::string, std::string> params;
};

// source_map.Content: files by game path, from the map's pakfile, then game folders, then VPKs.
class Content
{
public:
  Content(const Bytes* pak, const fs::path& map_path, const std::vector<fs::path>& search)
  {
    if (pak && pak->size() >= 22)
    {
      m_pak = *pak;
      m_zip = mz_zip_reader_create();
      if (m_zip && mz_zip_reader_open_buffer(m_zip, m_pak.data(), static_cast<int32_t>(m_pak.size()),
                                             0) == MZ_OK)
      {
        for (int32_t r = mz_zip_reader_goto_first_entry(m_zip); r == MZ_OK;
             r = mz_zip_reader_goto_next_entry(m_zip))
        {
          mz_zip_file* info = nullptr;
          if (mz_zip_reader_entry_get_info(m_zip, &info) == MZ_OK && info && info->filename)
            m_pak_names[Lower(info->filename)] = info->filename;
        }
      }
      else
      {
        m_pak_names.clear();
      }
    }
    std::vector<fs::path> cands;
    if (!map_path.empty())
    {
      std::error_code ec;
      fs::path p = fs::weakly_canonical(fs::absolute(map_path, ec), ec).parent_path();
      cands.push_back(Lower(PathString(p.filename())) == "maps" ? p.parent_path() : p);
      fs::path d = p;
      for (int k = 0; k < 6; k++)
      {
        if (fs::is_directory(d / "materials", ec) || HasDirVpk(d))
          cands.push_back(d);
        const fs::path parent = d.parent_path();
        if (parent == d)
          break;
        d = parent;
      }
    }
    cands.insert(cands.end(), search.begin(), search.end());
    for (const fs::path& c : cands)
    {
      std::error_code ec;
      if (fs::is_regular_file(c, ec) && Lower(PathString(c.filename())).ends_with(".vpk"))
      {
        vpks.push_back(c);
      }
      else if (fs::is_directory(c, ec))
      {
        if (std::ranges::find(dirs, c) == dirs.end())
          dirs.push_back(c);
        std::vector<fs::path> add;
        for (const fs::path& v : DirVpks(c))
        {
          if (std::ranges::find(vpks, v) == vpks.end())
            add.push_back(v);
        }
        vpks.insert(vpks.end(), add.begin(), add.end());
      }
    }
  }

  ~Content()
  {
    if (m_zip)
      mz_zip_reader_delete(&m_zip);
  }
  Content(const Content&) = delete;
  Content& operator=(const Content&) = delete;

  // Where |rel| would be read from: "pak", a file on disk, or "vpk".
  enum class Where
  {
    None,
    Pak,
    Disk,
    Vpk
  };
  Where Locate(const std::string& raw, fs::path* disk = nullptr)
  {
    const std::string rel = Strip(Slashes(raw), '/');
    const std::string low = Lower(rel);
    if (m_pak_names.contains(low))
      return Where::Pak;
    for (const fs::path& d : dirs)
    {
      if (const auto f = FindFileNoCase(d, rel))
      {
        if (disk)
          *disk = *f;
        return Where::Disk;
      }
    }
    return Vpk().contains(low) ? Where::Vpk : Where::None;
  }

  std::optional<Bytes> Read(const std::string& raw)
  {
    const std::string rel = Strip(Slashes(raw), '/');
    const std::string low = Lower(rel);
    if (const auto it = m_pak_names.find(low); it != m_pak_names.end())
    {
      if (mz_zip_reader_locate_entry(m_zip, it->second.c_str(), 0) != MZ_OK)
        return std::nullopt;
      mz_zip_file* info = nullptr;
      if (mz_zip_reader_entry_get_info(m_zip, &info) != MZ_OK || !info)
        return std::nullopt;
      Bytes out(static_cast<size_t>(info->uncompressed_size));
      if (!out.empty() && !Common::ReadFileFromZip(m_zip, &out))
        return std::nullopt;
      return out;
    }
    for (const fs::path& d : dirs)
    {
      if (const auto f = FindFileNoCase(d, rel))
        return ReadAll(*f);
    }
    const auto& index = Vpk();
    if (const auto it = index.find(low); it != index.end())
      return ReadVpkMember(it->second);
    return std::nullopt;
  }

  // (RGBA of $basetexture, shader, params) for a material name like 'brick/wall01'. Without
  // |decode| the texture is only located (for dependencies), never read.
  MaterialResult Material(const std::string& raw_name, int depth = 0, bool decode = true,
                          std::vector<std::string>* touched = nullptr)
  {
    const std::string name = Lower(Strip(Slashes(raw_name), '/'));
    const std::string vmt_rel = "materials/" + name + ".vmt";
    if (touched)
      touched->push_back(vmt_rel);
    const auto raw = Read(vmt_rel);
    MaterialResult res;
    if (raw)
    {
      std::tie(res.shader, res.params) = ParseVmt(std::string(raw->begin(), raw->end()));
      if (res.shader == "patch" && res.params.contains("include") && depth < 4)
      {
        std::string inc = Lower(Slashes(res.params["include"]));
        if (inc.starts_with("materials/"))
          inc = inc.substr(10);
        if (inc.ends_with(".vmt"))
          inc.resize(inc.size() - 4);
        MaterialResult base = Material(inc, depth + 1, decode, touched);
        std::map<std::string, std::string> merged = base.params;
        for (const auto& [k, v] : res.params)
        {
          if (k != "include")
            merged[k] = v;
        }
        const auto mt = merged.find("$basetexture");
        const auto bt = base.params.find("$basetexture");
        const bool same = (mt == merged.end() && bt == base.params.end()) ||
                          (mt != merged.end() && bt != base.params.end() && mt->second == bt->second);
        res.shader = base.shader;
        if (same && base.rgba)
        {
          base.params = std::move(merged);
          return base;
        }
        res.params = std::move(merged);
      }
    }
    std::string tex;
    if (const auto it = res.params.find("$basetexture"); it != res.params.end())
      tex = it->second;
    if (tex.empty() && !raw)
      tex = name;
    if (tex.empty())
      return res;
    std::string t = Lower(Strip(Slashes(tex), '/'));
    if (t.ends_with(".vtf"))
      t.resize(t.size() - 4);
    const std::string vtf_rel = "materials/" + t + ".vtf";
    if (touched)
      touched->push_back(vtf_rel);
    if (!decode)
      return res;
    const auto data = Read(vtf_rel);
    if (!data)
      return res;
    std::string error;
    res.rgba = ReadVtf(*data, &error);
    return res;
  }

  std::vector<fs::path> dirs;
  std::vector<fs::path> vpks;

private:
  const std::unordered_map<std::string, VpkMember>& Vpk()
  {
    if (!m_vpk_index)
    {
      m_vpk_index.emplace();
      for (const fs::path& v : vpks)
        ReadVpk(v, &*m_vpk_index);
    }
    return *m_vpk_index;
  }

  Bytes m_pak;
  void* m_zip = nullptr;
  std::unordered_map<std::string, std::string> m_pak_names;
  std::optional<std::unordered_map<std::string, VpkMember>> m_vpk_index;
};

// -- textures ---------------------------------------------------------------------------------

int Pow2(double v, double lo, double hi)
{
  v = std::max(lo, std::min(hi, v));
  return static_cast<int>(std::ldexp(1.0, static_cast<int>(std::nearbyint(std::log2(std::max(1.0, v))))));
}

std::string ShortName(const std::string& name, const std::string& prefix, std::set<std::string>* used)
{
  std::string base = Lower(name);
  if (const size_t slash = base.rfind('/'); slash != std::string::npos)
    base = base.substr(slash + 1);
  for (char& c : base)
  {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
      c = '_';
  }
  if (base.empty())
    base = "tex";
  std::string cand = (prefix + base).substr(0, 15);
  int n = 1;
  while (used->contains(cand))
  {
    n++;
    const std::string suffix = fmt::format("~{}", n);
    cand = (prefix + base).substr(0, 15 - suffix.size()) + suffix;
  }
  used->insert(cand);
  return cand;
}

RgbaImage ResizeImage(const RgbaImage& img, int w, int h, Filter filter)
{
  if (img.width == w && img.height == h)
    return img;
  return {w, h, ResizeRgba(img.pixels, img.width, img.height, w, h, filter)};
}

std::optional<Miptex> MakeMiptex(const std::string& name, const RgbaImage& rgba, bool masked)
{
  const auto quantized = QuantizeSkin(rgba, masked);
  if (!quantized)
    return std::nullopt;
  const auto& [idx, pal] = *quantized;
  Miptex mt{name, rgba.width, rgba.height, {idx}, pal};
  const int colours = masked ? 255 : 256;
  for (int lvl = 1; lvl <= 3; lvl++)
  {
    const int sw = std::max(1, rgba.width >> lvl), sh = std::max(1, rgba.height >> lvl);
    const RgbaImage small = ResizeImage(rgba, sw, sh, Filter::Box);
    Bytes best(static_cast<size_t>(sw) * sh);
    for (size_t i = 0; i < best.size(); i++)
    {
      const uint8_t* px = &small.pixels[i * 4];
      int bi = 0, bd = -1;
      for (int c = 0; c < colours; c++)
      {
        const int dr = px[0] - pal[c * 3], dg = px[1] - pal[c * 3 + 1], db = px[2] - pal[c * 3 + 2];
        const int d = dr * dr + dg * dg + db * db;
        if (bd < 0 || d < bd)
        {
          bd = d;
          bi = c;
        }
      }
      best[i] = static_cast<uint8_t>(bi);
      if (masked && px[3] < 128)
        best[i] = 255;
    }
    mt.mips.push_back(std::move(best));
  }
  return mt;
}

// -- geometry helpers ---------------------------------------------------------------------------

using Vecs = std::array<double, 8>;  // 2 x 4 texture vectors

std::pair<double, double> TexRange(const Winding& pts, const double* vec)
{
  const std::array<float, 3> v = {static_cast<float>(vec[0]), static_cast<float>(vec[1]),
                                  static_cast<float>(vec[2])};
  const float off = static_cast<float>(vec[3]);
  float lo = 0, hi = 0;
  for (size_t i = 0; i < pts.size(); i++)
  {
    const std::array<float, 3> a = {static_cast<float>(pts[i][0]), static_cast<float>(pts[i][1]),
                                    static_cast<float>(pts[i][2])};
    const float s = SRow(a, v, i, pts.size()) + off;
    lo = i == 0 ? s : std::min(lo, s);
    hi = i == 0 ? s : std::max(hi, s);
  }
  return {lo, hi};
}

std::vector<Winding> Subdivide(const Winding& poly, const Vecs& vecs, int limit = 16)
{
  std::vector<Winding> out;
  std::vector<Winding> todo = {poly};
  while (!todo.empty())
  {
    Winding p = std::move(todo.back());
    todo.pop_back();
    bool done = true;
    for (int ax = 0; ax < 2; ax++)
    {
      const auto [lo, hi] = TexRange(p, &vecs[4 * ax]);
      if (std::ceil(hi / 16) - std::floor(lo / 16) > limit)
      {
        const double cut = (std::floor(lo / 16) + limit - 1) * 16;
        const Vec3 n0 = {vecs[4 * ax], vecs[4 * ax + 1], vecs[4 * ax + 2]};
        const double ln = Bsp30Norm(n0);
        const double d = (cut - vecs[4 * ax + 3]) / ln;
        const Vec3 n = {n0[0] / ln, n0[1] / ln, n0[2] / ln};
        auto back = ClipWinding(p, n, d);
        auto front = ClipWinding(p, n, d, true);
        if (!back || !front)
          continue;
        todo.push_back(std::move(*back));
        todo.push_back(std::move(*front));
        done = false;
        break;
      }
    }
    if (done)
      out.push_back(std::move(p));
  }
  return out;
}

struct OutFace
{
  Winding pts;
  Vec3 normal;
  double dist;
  int texinfo;
  int src;
  bool special;
  std::optional<Winding> flat;
  bool lit = true;
};

std::vector<std::pair<Vec3, double>> Wedge(const Winding& tri, const Vec3& up, double depth = 12.0)
{
  Vec3 n = Bsp30Cross(Sub(tri[2], tri[0]), Sub(tri[1], tri[0]));
  const double ln = Bsp30Norm(n);
  if (ln < 1e-6)
    return {};
  for (double& c : n)
    c /= ln;
  Winding bottom(3);
  for (int i = 0; i < 3; i++)
    bottom[i] = Sub(tri[i], Scale(up, depth));
  std::vector<std::pair<Vec3, double>> out = {{n, Bsp30Dot(n, tri[0])},
                                              {Neg(up), Bsp30Dot(Neg(up), bottom[0])}};
  const Vec3 mt = Mean(tri), mb = Mean(bottom);
  const Vec3 centre = {(mt[0] + mb[0]) / 2, (mt[1] + mb[1]) / 2, (mt[2] + mb[2]) / 2};
  for (int i = 0; i < 3; i++)
  {
    Vec3 sn = Bsp30Cross(Sub(tri[(i + 1) % 3], tri[i]), up);
    const double ls = Bsp30Norm(sn);
    if (ls < 1e-6)
      return {};
    for (double& c : sn)
      c /= ls;
    double sd = Bsp30Dot(sn, tri[i]);
    if (Bsp30Dot(sn, centre) > sd)
    {
      sn = Neg(sn);
      sd = -sd;
    }
    out.emplace_back(sn, sd);
  }
  return out;
}

// (displaced triangle, its undisplaced corners) pairs, clockwise from the front, and wedges.
void Displacement(const Winding& quad, const DispInfo& di, const std::vector<DispVert>& dispverts,
                  int max_power, const Vec3& normal, std::vector<std::pair<Winding, Winding>>* tris,
                  std::vector<BrushSides>* coll)
{
  const Vec3 start = di.start;
  int k0 = 0;
  double kd = 0;
  for (int k = 0; k < 4; k++)
  {
    const Vec3 dd = Sub(quad[k], start);
    const double s = (dd[0] * dd[0] + dd[1] * dd[1]) + dd[2] * dd[2];
    if (k == 0 || s < kd)
    {
      kd = s;
      k0 = k;
    }
  }
  const Vec3 &a = quad[k0], &b = quad[(k0 + 1) % 4], &c = quad[(k0 + 2) % 4],
             &d = quad[(k0 + 3) % 4];
  const int power = di.power;
  if (power < 0 || power > 12)
    return;
  const int n = (1 << power) + 1;
  const int64_t first = di.firstvert;
  if (first < 0 || first + int64_t(n) * n > static_cast<int64_t>(dispverts.size()))
    return;
  Winding flat(n * n), pos(n * n);
  for (int idx = 0; idx < n * n; idx++)
  {
    const double t1 = static_cast<double>(idx % n) / (n - 1);
    const double t2 = static_cast<double>(idx / n) / (n - 1);
    const DispVert& dv = dispverts[first + idx];
    for (int i = 0; i < 3; i++)
    {
      const double ad = a[i] + (d[i] - a[i]) * t1;
      const double bc = b[i] + (c[i] - b[i]) * t1;
      flat[idx][i] = ad + (bc - ad) * t2;
      pos[idx][i] = flat[idx][i] + dv.vec[i] * dv.dist;
    }
  }
  const int step = 1 << std::max(0, power - std::max(0, std::min(max_power, power)));
  std::vector<int> keep;
  for (int i = 0; i < n; i += step)
    keep.push_back(i);
  const int m = static_cast<int>(keep.size());
  const auto at = [&](int r, int cc) { return keep[r] * n + keep[cc]; };
  std::vector<BrushSides> wedges;
  for (int r = 0; r < m - 1; r++)
  {
    for (int cc = 0; cc < m - 1; cc++)
    {
      const std::array<int, 4> q = {at(r, cc), at(r, cc + 1), at(r + 1, cc + 1), at(r + 1, cc)};
      std::array<std::array<int, 3>, 2> pair;
      if ((r + cc) % 2 == 0)
        pair = {{{q[0], q[1], q[2]}, {q[0], q[2], q[3]}}};
      else
        pair = {{{q[0], q[1], q[3]}, {q[1], q[2], q[3]}}};
      for (const auto& t : pair)
      {
        Winding tp = {pos[t[0]], pos[t[1]], pos[t[2]]};
        Winding tf = {flat[t[0]], flat[t[1]], flat[t[2]]};
        if (Bsp30Dot(Bsp30Cross(Sub(tp[1], tp[0]), Sub(tp[2], tp[0])), normal) > 0)
        {
          std::reverse(tp.begin(), tp.end());
          std::reverse(tf.begin(), tf.end());
        }
        wedges.push_back(Wedge(tp, normal));
        tris->emplace_back(std::move(tp), std::move(tf));
      }
    }
  }
  for (auto& w : wedges)
  {
    if (!w.empty())
      coll->push_back(std::move(w));
  }
}

// numpy's (3, 3) @ (3, 2) of points by texture axes plus offsets (dgemm: a fused chain).
std::array<std::array<double, 2>, 3> St(const Winding& pts, const Vecs& v)
{
  std::array<std::array<double, 2>, 3> out;
  for (int r = 0; r < 3; r++)
  {
    for (int ax = 0; ax < 2; ax++)
    {
      double s = pts[r][0] * v[4 * ax];
      s = std::fma(pts[r][1], v[4 * ax + 1], s);
      s = std::fma(pts[r][2], v[4 * ax + 2], s);
      out[r][ax] = s + v[4 * ax + 3];
    }
  }
  return out;
}

Vecs TriVecs(const Winding& pts, const Winding& flat, const Vecs& vecs, const Vec3& n)
{
  const auto st = St(flat, vecs);
  const auto plain = St(pts, vecs);
  double worst = 0;
  for (int r = 0; r < 3; r++)
  {
    for (int ax = 0; ax < 2; ax++)
      worst = std::max(worst, std::abs(plain[r][ax] - st[r][ax]));
  }
  if (worst < 0.25)
    return vecs;
  Mat<4> m{};
  for (int r = 0; r < 3; r++)
  {
    for (int k = 0; k < 3; k++)
      m[r][k] = pts[r][k];
    m[r][3] = 1.0;
  }
  for (int k = 0; k < 3; k++)
    m[3][k] = n[k];
  m[3][3] = 0.0;
  std::array<int, 4> piv;
  Mat<4> lu = m;
  if (!Getf2<4>(&lu, &piv))
    return vecs;
  Vecs out{};
  for (int ax = 0; ax < 2; ax++)
  {
    const auto x = Getrs<4>(lu, piv, {st[0][ax], st[1][ax], st[2][ax], 0.0}, false);
    for (int k = 0; k < 4; k++)
      out[4 * ax + k] = x[k];
  }
  return out;
}

// s2map._uv_vecs_many for one triangle: (vecs, solvable).
std::pair<Vecs, bool> UvVecs3(const Winding& pts, const std::array<std::array<double, 2>, 3>& st,
                              const Vec3& n)
{
  Mat<4> m{};
  for (int r = 0; r < 3; r++)
  {
    for (int k = 0; k < 3; k++)
      m[r][k] = pts[r][k];
    m[r][3] = 1.0;
  }
  for (int k = 0; k < 3; k++)
    m[3][k] = n[k];
  Vecs out{};
  if (!(std::abs(Det4(m)) > 1e-9))
    return {out, false};
  std::array<int, 4> piv;
  if (!Getf2<4>(&m, &piv))
    return {Vecs{}, false};
  for (int ax = 0; ax < 2; ax++)
  {
    const auto x = Getrs<4>(m, piv, {st[0][ax], st[1][ax], st[2][ax], 0.0}, true);
    for (int k = 0; k < 4; k++)
      out[4 * ax + k] = x[k];
  }
  for (const double v : out)
  {
    if (!std::isfinite(v) || !(std::abs(v) < 1e6))
      return {out, false};
  }
  return {out, true};
}

struct OverlayQuad
{
  Winding pts;
  std::array<std::array<double, 2>, 4> uv;
  Vec3 n;
};

std::optional<OverlayQuad> MakeOverlayQuad(const Overlay& ov)
{
  Vec3 n = {ov.normal[0], ov.normal[1], ov.normal[2]};
  const double ln = Bsp30Norm(n);
  if (ln < 1e-6)
    return std::nullopt;
  for (double& c : n)
    c /= ln;
  Vec3 bu = {ov.points[0][2], ov.points[1][2], ov.points[2][2]};
  if (Bsp30Norm(bu) < 1e-6)
    bu = std::abs(n[2]) < 0.9 ? Bsp30Cross(n, {0.0, 0.0, 1.0}) : Bsp30Cross(n, {1.0, 0.0, 0.0});
  const double lb = Bsp30Norm(bu);
  for (double& c : bu)
    c /= lb;
  const Vec3 bv = Bsp30Cross(n, bu);
  const Vec3 o = {ov.origin[0], ov.origin[1], ov.origin[2]};
  OverlayQuad q;
  q.pts.resize(4);
  for (int i = 0; i < 4; i++)
  {
    const double p0 = ov.points[i][0], p1 = ov.points[i][1];
    for (int k = 0; k < 3; k++)
      q.pts[i][k] = (o[k] + p0 * bu[k]) + p1 * bv[k];
  }
  const double u0 = ov.u[0], u1 = ov.u[1], v0 = ov.v[0], v1 = ov.v[1];
  q.uv = {{{u0, v0}, {u0, v1}, {u1, v1}, {u1, v0}}};
  if (Bsp30Dot(Bsp30Cross(Sub(q.pts[1], q.pts[0]), Sub(q.pts[2], q.pts[0])), n) > 0)
  {
    std::reverse(q.pts.begin(), q.pts.end());
    std::reverse(q.uv.begin(), q.uv.end());
  }
  if (Bsp30Norm(Bsp30Cross(Sub(q.pts[1], q.pts[0]), Sub(q.pts[2], q.pts[0]))) < 1e-3)
    return std::nullopt;
  q.n = n;
  return q;
}

std::array<int, 3> Box(const Vec3& v, int direction)
{
  std::array<int, 3> out;
  for (int i = 0; i < 3; i++)
  {
    const double a = direction < 0 ? std::floor(v[i]) : std::ceil(v[i]);
    out[i] = static_cast<int>(std::clamp(a, -32768.0, 32767.0));
  }
  return out;
}

std::set<int> LeafsUnder(const std::vector<Node>& nodes, int head)
{
  std::set<int> out;
  std::vector<int> stack = {head};
  size_t guard = 0;
  while (!stack.empty() && guard++ < 4 * nodes.size() + 16)
  {
    const int n = stack.back();
    stack.pop_back();
    if (n < 0)
    {
      out.insert(-1 - n);
      continue;
    }
    if (n >= static_cast<int>(nodes.size()))
      continue;
    stack.push_back(nodes[n].children[0]);
    stack.push_back(nodes[n].children[1]);
  }
  return out;
}

int PointLeaf(const std::vector<Node>& nodes, const std::vector<Plane>& planes, int head,
              const Vec3& p)
{
  int n = head;
  size_t guard = 0;
  while (n >= 0 && n < static_cast<int>(nodes.size()) && guard++ < nodes.size() + 1)
  {
    const Node& nd = nodes[n];
    if (nd.planenum < 0 || nd.planenum >= static_cast<int>(planes.size()))
      break;
    const Plane& pl = planes[nd.planenum];
    const double d = Bsp30Dot(pl.normal, p) - pl.dist;
    n = d >= 0 ? nd.children[0] : nd.children[1];
  }
  return -1 - n;
}

// Barycentric coordinates on the displaced triangle, applied to its undisplaced corners.
Winding ToFlat(const Winding& world, const Winding& tri, const Winding& flat)
{
  const Vec3 v0 = Sub(tri[1], tri[0]), v1 = Sub(tri[2], tri[0]);
  const double d00 = Bsp30Dot(v0, v0), d01 = Bsp30Dot(v0, v1), d11 = Bsp30Dot(v1, v1);
  const double den = d00 * d11 - d01 * d01;
  if (std::abs(den) < 1e-9)
    return Winding(world.size(), Mean(flat));
  Winding out(world.size());
  for (size_t i = 0; i < world.size(); i++)
  {
    const Vec3 p = Sub(world[i], tri[0]);
    const double d20 = Bsp30Row(p, v0, world.size()), d21 = Bsp30Row(p, v1, world.size());
    const double w1 = (d11 * d20 - d01 * d21) / den;
    const double w2 = (d00 * d21 - d01 * d20) / den;
    const double w0 = 1 - w1 - w2;
    for (int k = 0; k < 3; k++)
      out[i][k] = (w0 * flat[0][k] + w1 * flat[1][k]) + w2 * flat[2][k];
  }
  return out;
}

struct LightOptions
{
  double gamma, scale;
};

// Per-style RGB blocks for one output face, sampled from its Source face's lightmap.
std::optional<std::vector<Bytes>> Relight(const OutFace& of, const Face& fc, const TexInfo& ti,
                                          const Vecs& vecs, const Bytes& lighting,
                                          const LightOptions& opts)
{
  const auto [s_lo, s_hi] = TexRange(of.pts, &vecs[0]);
  const auto [t_lo, t_hi] = TexRange(of.pts, &vecs[4]);
  const double bs = std::floor(s_lo / 16), bt = std::floor(t_lo / 16);
  const double smax_d = std::ceil(s_hi / 16) - bs + 1;
  const double tmax_d = std::ceil(t_hi / 16) - bt + 1;
  if (smax_d < 1 || tmax_d < 1 || smax_d > 64 || tmax_d > 64)
    return std::nullopt;
  const int smax = static_cast<int>(smax_d), tmax = static_cast<int>(tmax_d);
  const Mat<3> m = {{{vecs[0], vecs[1], vecs[2]}, {vecs[4], vecs[5], vecs[6]}, of.normal}};
  const auto inv = Inv3(m);
  if (!inv)
    return std::nullopt;
  const size_t count = static_cast<size_t>(smax) * tmax;
  Winding world(count);
  for (int j = 0; j < tmax; j++)
  {
    for (int i = 0; i < smax; i++)
    {
      const double ss = (bs + i) * 16.0, tt = (bt + j) * 16.0;
      const double r0 = ss - vecs[3], r1 = tt - vecs[7], r2 = of.dist;
      Vec3& w = world[static_cast<size_t>(j) * smax + i];
      for (int c = 0; c < 3; c++)
      {
        double s = r0 * (*inv)[c][0];
        s = std::fma(r1, (*inv)[c][1], s);
        w[c] = std::fma(r2, (*inv)[c][2], s);
      }
    }
  }
  if (of.flat)
    world = ToFlat(world, of.pts, *of.flat);
  const Vec3 lm0 = {ti.lmvecs[0][0], ti.lmvecs[0][1], ti.lmvecs[0][2]};
  const Vec3 lm1 = {ti.lmvecs[1][0], ti.lmvecs[1][1], ti.lmvecs[1][2]};
  const double lm03 = ti.lmvecs[0][3], lm13 = ti.lmvecs[1][3];
  const int w = fc.lm_size[0] + 1, h = fc.lm_size[1] + 1;
  if (w < 1 || h < 1)
    return std::nullopt;
  std::vector<int> u0(count), v0(count), u1(count), v1(count);
  std::vector<double> fu(count), fv(count);
  for (size_t k = 0; k < count; k++)
  {
    double u = Bsp30Row(world[k], lm0, count) + lm03 - static_cast<double>(fc.lm_mins[0]);
    double v = Bsp30Row(world[k], lm1, count) + lm13 - static_cast<double>(fc.lm_mins[1]);
    u = std::min(std::max(u, 0.0), static_cast<double>(w - 1));
    v = std::min(std::max(v, 0.0), static_cast<double>(h - 1));
    u0[k] = static_cast<int>(std::floor(u));
    v0[k] = static_cast<int>(std::floor(v));
    u1[k] = std::min(u0[k] + 1, w - 1);
    v1[k] = std::min(v0[k] + 1, h - 1);
    fu[k] = u - u0[k];
    fv[k] = v - v0[k];
  }
  int nstyles = 0;
  for (const uint8_t s : fc.styles)
    nstyles += s != 255;
  const bool bump = (ti.flags & SURF_BUMPLIGHT) != 0;
  const int64_t per = int64_t(w) * h * (bump ? 4 : 1);
  std::vector<Bytes> blocks;
  const int64_t base = fc.lightofs;
  const double exponent = 1.0 / opts.gamma;
  for (int st = 0; st < std::max(1, nstyles); st++)
  {
    const int64_t ofs = base + st * per * 4;
    if (ofs + int64_t(w) * h * 4 > static_cast<int64_t>(lighting.size()))
      break;
    const auto grid = [&](int y, int x, int ch) {
      const uint8_t* px = &lighting[ofs + (int64_t(y) * w + x) * 4];
      return std::ldexp(static_cast<float>(px[ch]), static_cast<int8_t>(px[3]));
    };
    Bytes block(count * 3);
    for (size_t k = 0; k < count; k++)
    {
      for (int ch = 0; ch < 3; ch++)
      {
        double val = static_cast<double>(grid(v0[k], u0[k], ch)) * (1 - fu[k]) * (1 - fv[k]) +
                     static_cast<double>(grid(v0[k], u1[k], ch)) * fu[k] * (1 - fv[k]) +
                     static_cast<double>(grid(v1[k], u0[k], ch)) * (1 - fu[k]) * fv[k] +
                     static_cast<double>(grid(v1[k], u1[k], ch)) * fu[k] * fv[k];
        const double x = std::max(val * opts.scale / 510.0, 0.0);
        val = 2.0 * std::pow(x, exponent) * 255.0;
        const double c = std::min(std::max(val + 0.5, 0.0), 255.0);
        block[k * 3 + ch] = static_cast<uint8_t>(c);
      }
    }
    blocks.push_back(std::move(block));
  }
  if (blocks.empty())
    return std::nullopt;
  return blocks;
}

const char* const SKIP_TOOLS[] = {
    "tools/toolsnodraw",     "tools/toolsclip",          "tools/toolsplayerclip",
    "tools/toolsnpcclip",    "tools/toolstrigger",       "tools/toolsinvisible",
    "tools/toolsareaportal", "tools/toolsoccluder",      "tools/toolsblocklight",
    "tools/toolsskip",       "tools/toolshint",          "tools/toolsfog",
    "tools/toolsblockbullets", "tools/toolsblock_los",   "tools/toolsinvisibleladder",
    "tools/toolsdotted",     "tools/toolsorigin"};
const char* const SKY_TOOLS[] = {"tools/toolsskybox", "tools/toolsskybox2d", "tools/toolsskyfog"};
const char* const SPAWN_CLASSES[] = {"info_player_terrorist", "info_player_counterterrorist",
                                     "info_player_teamspawn", "info_player_combine",
                                     "info_player_rebel",     "info_player_axis",
                                     "info_player_allies"};
const char* const SKY_SUFFIXES[] = {"rt", "bk", "lf", "ft", "up", "dn"};

template <size_t N>
bool In(const char* const (&list)[N], const std::string& s)
{
  return std::ranges::any_of(list, [&](const char* x) { return s == x; });
}

bool Truthy(const std::map<std::string, std::string>& params, const char* key)
{
  const auto it = params.find(key);
  const std::string v = it == params.end() ? "0" : it->second;
  return v != "0" && !v.empty();
}

double Median(std::vector<double> v)
{
  std::ranges::sort(v);
  const size_t n = v.size();
  if (n % 2)
    return v[n / 2];
  return (v[n / 2 - 1] + v[n / 2]) / 2;
}
}  // namespace

bool IsSourceMap(const Bytes& data)
{
  if (data.size() < 8 || std::memcmp(data.data(), "VBSP", 4) != 0)
    return false;
  const int v = Get<int32_t>(data.data() + 4);
  return 17 <= v && v <= 29;
}

std::optional<SourceMapResult> ConvertSourceMap(const fs::path& path, const Bytes& data,
                                                const SourceMapOptions& opts, std::string* error)
{
  if (!IsSourceMap(data) || data.size() < 8 + 16 * 64)
  {
    *error = "not a Source BSP";
    return std::nullopt;
  }
  Vbsp src(data);
  if (!((19 <= src.version && src.version <= 21) || src.version == 25))
  {
    *error = fmt::format("Source BSP version {} is not supported (19-21 and 25 are)", src.version);
    return std::nullopt;
  }
  SourceMapResult result;
  std::vector<std::string>& warnings = result.warnings;
  const Bytes pak = src.Raw(L_PAKFILE);
  Content content(&pak, path, opts.search);
  const std::vector<Plane> planes_in = src.Planes();
  const std::vector<Vec3> V = src.Vertexes();
  const auto E = src.Edges();
  const std::vector<int> S = src.Surfedges();
  Bytes lighting;
  bool hdr = false;
  const std::vector<Face> faces = src.FacesAndLighting(&lighting, &hdr);
  if (hdr)
    warnings.push_back("map has HDR lighting only; lightmaps converted from HDR");
  const std::vector<TexInfo> texinfo = src.TexInfos();
  const std::vector<TexData> texdata = src.TexDatas();
  std::vector<std::string> tnames = src.TextureNames();
  for (std::string& n : tnames)
    n = Lower(n);
  Bsp30Map out;
  PlaneTable& P = out.planes;

  const auto bad = [&](const std::string& what) {
    *error = "malformed map: " + what;
    return std::nullopt;
  };

  // -- textures -------------------------------------------------------------------------------
  const auto tex_name = [&](int ti) -> std::string {
    const int td = texinfo[ti].texdata;
    return (0 <= td && td < static_cast<int>(tnames.size())) ? tnames[td] : std::string();
  };
  const auto is_skip = [&](int ti) {
    const int fl = texinfo[ti].flags;
    return (fl & (SURF_NODRAW | SURF_SKIP | SURF_HINT | SURF_TRIGGER)) != 0 ||
           In(SKIP_TOOLS, tex_name(ti));
  };
  const auto is_sky = [&](int ti) {
    return (texinfo[ti].flags & (SURF_SKY | SURF_SKY2D)) != 0 || In(SKY_TOOLS, tex_name(ti));
  };
  const auto ti_ok = [&](int ti) { return ti < static_cast<int>(texinfo.size()); };

  std::map<int, std::vector<double>> used_tex;  // texdata -> densities
  for (const Face& fc : faces)
  {
    const int ti = fc.texinfo;
    if (ti < 0)
      continue;
    if (!ti_ok(ti))
      return bad("face texinfo out of range");
    if (is_skip(ti) || is_sky(ti))
      continue;
    const auto& v = texinfo[ti].vecs;
    const double dens = (static_cast<double>(Norm32(v[0].data())) +
                         static_cast<double>(Norm32(v[1].data()))) /
                        2;
    used_tex[texinfo[ti].texdata].push_back(dens);
  }
  // overlays: decal quads laid on faces; their textures join the set
  struct PendingOverlay
  {
    OverlayQuad quad;
    int td;
  };
  std::map<int, std::vector<PendingOverlay>> overlays;
  if (opts.overlays)
  {
    for (const Overlay& ov : src.Overlays())
    {
      const int ti = ov.texinfo;
      const int nface = ov.facecount & 0x3FFF;
      if (!(0 <= ti && ti < static_cast<int>(texinfo.size())) || nface < 1)
        continue;
      auto got = MakeOverlayQuad(ov);
      if (!got)
        continue;
      const int td = texinfo[ti].texdata;
      const int f0 = ov.faces[0];
      if (!(0 <= f0 && f0 < static_cast<int>(faces.size())) || faces[f0].dispinfo >= 0)
        continue;
      if (td < 0 || td >= static_cast<int>(texdata.size()))
        return bad("overlay texdata out of range");
      const double w0 = texdata[td].width ? texdata[td].width : 64;
      double extent = Bsp30Norm(Sub(got->pts[2], got->pts[0])) / std::sqrt(2.0);
      if (extent == 0)
        extent = 64.0;
      const float du = ov.u[1] - ov.u[0];
      used_tex[td].push_back(w0 / extent * std::max(1e-3, std::abs(static_cast<double>(du))));
      overlays[f0].push_back({std::move(*got), td});
    }
  }
  std::set<std::string> names_used = {"sky"};
  struct MipOf
  {
    int mip;
    double ks, kt;
  };
  std::map<int, MipOf> miptex_of;
  std::vector<std::string> missing;
  for (const auto& [td, dens] : used_tex)
  {
    if (td < 0 || td >= static_cast<int>(texdata.size()))
      return bad("texdata out of range");
    const std::string name =
        td < static_cast<int>(tnames.size()) ? tnames[td] : fmt::format("tex{}", td);
    MaterialResult mat = content.Material(name);
    const int w0 = texdata[td].width ? texdata[td].width : 64;
    const int h0 = texdata[td].height ? texdata[td].height : 64;
    if (!mat.rgba)
    {
      missing.push_back(name);
      RgbaImage grey{64, 64, Bytes(64 * 64 * 4)};
      for (int y = 0; y < 64; y++)
      {
        for (int x = 0; x < 64; x++)
        {
          uint8_t* px = &grey.pixels[(y * 64 + x) * 4];
          const uint8_t c = (y % 8 == 0 || x % 8 == 0) ? 96 : 128;
          px[0] = px[1] = px[2] = c;
          px[3] = 255;
        }
      }
      mat.rgba = std::move(grey);
    }
    const RgbaImage& rgba = *mat.rgba;
    bool any_hole = false;
    for (size_t i = 3; i < rgba.pixels.size(); i += 4)
      any_hole = any_hole || rgba.pixels[i] < 128;
    const bool masked = Truthy(mat.params, "$alphatest") ||
                        (Truthy(mat.params, "$translucent") && any_hole);
    const std::string last = name.substr(name.rfind('/') == std::string::npos ? 0 : name.rfind('/') + 1);
    const bool water = mat.shader.find("water") != std::string::npos ||
                       name.starts_with("liquids/") || last.find("water") != std::string::npos;
    const double k = opts.texel_density / std::max(1e-6, Median(dens));
    int tw = Pow2(w0 * k, std::min(opts.min_texture, w0), opts.max_texture);
    int th = Pow2(h0 * k, std::min(opts.min_texture, h0), opts.max_texture);
    tw = std::max(16, tw);
    th = std::max(16, th);
    const RgbaImage img = ResizeImage(rgba, tw, th, Filter::Lanczos);
    const std::string prefix = masked ? "{" : water ? "!" : "";
    auto mt = MakeMiptex(ShortName(name, prefix, &names_used), img, masked);
    if (!mt)
      return bad("could not quantize " + name);
    miptex_of[td] = {static_cast<int>(out.textures.size()), static_cast<double>(tw) / w0,
                     static_cast<double>(th) / h0};
    out.textures.push_back(std::move(*mt));
  }
  if (!missing.empty())
  {
    std::string shown;
    for (size_t i = 0; i < std::min<size_t>(5, missing.size()); i++)
      shown += (i ? ", " : "") + missing[i];
    if (missing.size() > 5)
      shown += fmt::format(" and {} more", missing.size() - 5);
    warnings.push_back(fmt::format("{} material(s) not found, grey used: {}", missing.size(), shown));
  }
  int sky_index = -1;
  const auto sky_miptex = [&] {
    if (sky_index < 0)
    {
      sky_index = static_cast<int>(out.textures.size());
      RgbaImage grey{16, 16, Bytes(16 * 16 * 4)};
      for (size_t i = 0; i < grey.pixels.size(); i += 4)
      {
        grey.pixels[i] = 90;
        grey.pixels[i + 1] = 120;
        grey.pixels[i + 2] = 160;
        grey.pixels[i + 3] = 255;
      }
      out.textures.push_back(*MakeMiptex("sky", grey, false));
    }
    return sky_index;
  };

  struct TexKey
  {
    int mip, flags;
    std::array<double, 8> r;
    auto operator<=>(const TexKey&) const = default;
  };
  std::map<TexKey, int> texinfo_index;
  const auto out_texinfo = [&](const Vecs& vecs, int mip, int flags) {
    TexKey key{mip, flags, {}};
    for (int i = 0; i < 8; i++)
      key.r[i] = std::nearbyint(vecs[i] * 1e4) + 0.0;
    const auto [it, added] = texinfo_index.try_emplace(key, static_cast<int>(out.texinfo.size()));
    if (added)
      out.texinfo.push_back({vecs, mip, flags});
    return it->second;
  };
  const auto raw_vecs = [&](int ti) {
    Vecs v;
    for (int r = 0; r < 2; r++)
    {
      for (int c = 0; c < 4; c++)
        v[4 * r + c] = texinfo[ti].vecs[r][c];
    }
    return v;
  };
  const auto scaled_vecs = [&](int ti) {
    const MipOf& mo = miptex_of.at(texinfo[ti].texdata);
    Vecs v = raw_vecs(ti);
    for (int c = 0; c < 4; c++)
    {
      v[c] *= mo.ks;
      v[4 + c] *= mo.kt;
    }
    return std::pair{v, mo.mip};
  };

  // -- faces ----------------------------------------------------------------------------------
  const std::vector<DispInfo> dispinfo = src.DispInfos();
  const std::vector<DispVert> dispverts = src.DispVerts();
  std::vector<OutFace> out_faces;
  std::vector<int64_t> counts(faces.size() + 1, 0);
  struct DispFace
  {
    int src, first, count;
  };
  std::vector<DispFace> disp_faces;
  std::vector<BrushSides> disp_collision;
  std::vector<std::pair<int, Vec3>> disp_bases;
  int overlay_count = 0;
  (void)overlay_count;
  for (size_t i = 0; i < faces.size(); i++)
  {
    const Face& fc = faces[i];
    const int ti = fc.texinfo;
    const size_t start = out_faces.size();
    if (ti < 0 || is_skip(ti))
    {
      counts[i] = 0;
      continue;
    }
    Winding pts;
    for (int k = 0; k < fc.numedges; k++)
    {
      const int64_t si = int64_t(fc.firstedge) + k;
      if (si < 0 || si >= static_cast<int64_t>(S.size()))
        return bad("surfedge out of range");
      const int e = S[si];
      const int64_t ei = e >= 0 ? e : -int64_t(e);
      if (ei >= static_cast<int64_t>(E.size()))
        return bad("edge out of range");
      const uint32_t vi = e >= 0 ? E[ei][0] : E[ei][1];
      if (vi >= V.size())
        return bad("vertex out of range");
      pts.push_back(V[vi]);
    }
    if (fc.planenum < 0 || fc.planenum >= static_cast<int>(planes_in.size()))
      return bad("face plane out of range");
    const Plane& pl = planes_in[fc.planenum];
    const Vec3 n = pl.normal;
    const double d = pl.dist;
    if (is_sky(ti))
    {
      const int oti = out_texinfo(raw_vecs(ti), sky_miptex(), BSP30_TEX_SPECIAL);
      out_faces.push_back({pts, n, d, oti, static_cast<int>(i), true, std::nullopt, false});
    }
    else if (fc.dispinfo >= 0 && fc.dispinfo < static_cast<int>(dispinfo.size()) &&
             pts.size() == 4)
    {
      const DispInfo& di = dispinfo[fc.dispinfo];
      disp_bases.emplace_back(fc.planenum, Mean(pts));
      const auto [vecs, mip] = scaled_vecs(ti);
      std::vector<std::pair<Winding, Winding>> tris;
      Displacement(pts, di, dispverts, opts.disp_power, n, &tris, &disp_collision);
      for (auto& [tri_pts, tri_flat] : tris)
      {
        Vec3 tn = Bsp30Cross(Sub(tri_pts[2], tri_pts[0]), Sub(tri_pts[1], tri_pts[0]));
        const double ln = Bsp30Norm(tn);
        if (ln < 1e-6)
          continue;
        for (double& c : tn)
          c /= ln;
        const Vecs tv = TriVecs(tri_pts, tri_flat, vecs, tn);
        const int oti = out_texinfo(tv, mip, 0);
        const double td = Bsp30Dot(tn, tri_pts[0]);
        out_faces.push_back({tri_pts, tn, td, oti, static_cast<int>(i), false, tri_flat, true});
      }
      disp_faces.push_back({static_cast<int>(i), static_cast<int>(start),
                            static_cast<int>(out_faces.size() - start)});
    }
    else
    {
      const auto [vecs, mip] = scaled_vecs(ti);
      const int oti = out_texinfo(vecs, mip, 0);
      const bool lit = fc.lightofs >= 0 && !(texinfo[ti].flags & SURF_NOLIGHT);
      for (Winding& piece : Subdivide(pts, vecs))
        out_faces.push_back({std::move(piece), n, d, oti, static_cast<int>(i), false, std::nullopt, lit});
    }
    if (const auto ov_it = overlays.find(static_cast<int>(i)); ov_it != overlays.end())
    {
      for (const PendingOverlay& po : ov_it->second)
      {
        const MipOf& mo = miptex_of.at(po.td);
        const Miptex& tex = out.textures[mo.mip];
        const Vec3& on = po.quad.n;
        Winding lifted(4);
        for (int k = 0; k < 4; k++)
        {
          for (int c = 0; c < 3; c++)
            lifted[k][c] = po.quad.pts[k][c] + on[c] * 0.5;
        }
        std::array<std::array<double, 2>, 3> st;
        for (int k = 0; k < 3; k++)
          st[k] = {po.quad.uv[k][0] * tex.width, po.quad.uv[k][1] * tex.height};
        const auto [vv, good] = UvVecs3({lifted[0], lifted[1], lifted[2]}, st, on);
        if (!good)
          continue;
        const int oti = out_texinfo(vv, mo.mip, 0);
        const bool lit = fc.lightofs >= 0;
        for (Winding& piece : Subdivide(lifted, vv))
        {
          const double pd = Bsp30Dot(on, piece[0]);
          out_faces.push_back({std::move(piece), on, pd, oti, static_cast<int>(i), false, std::nullopt, lit});
        }
        overlay_count++;
      }
    }
    counts[i] = static_cast<int64_t>(out_faces.size() - start);
  }
  std::vector<int64_t> rstart(faces.size() + 1, 0);
  for (size_t i = 0; i < faces.size(); i++)
    rstart[i + 1] = rstart[i] + counts[i];

  // -- vertexes, edges, faces, lighting -------------------------------------------------------
  struct VKey
  {
    double x, y, z;
    bool operator==(const VKey&) const = default;
  };
  struct VKeyHash
  {
    size_t operator()(const VKey& k) const
    {
      return std::hash<double>()(k.x) * 31 * 31 + std::hash<double>()(k.y) * 31 +
             std::hash<double>()(k.z);
    }
  };
  std::unordered_map<VKey, int, VKeyHash> vindex;
  std::map<std::pair<int, int>, int> eindex;
  const auto vid = [&](const Vec3& p) {
    const Vec3 r = {Round3(p[0]), Round3(p[1]), Round3(p[2])};
    const VKey key{r[0] + 0.0, r[1] + 0.0, r[2] + 0.0};
    const auto [it, added] = vindex.try_emplace(key, static_cast<int>(out.vertexes.size()));
    if (added)
      out.vertexes.push_back(r);
    return it->second;
  };
  const auto eid = [&](int a, int b) {
    if (const auto it = eindex.find({b, a}); it != eindex.end())
      return -it->second;
    const auto [it, added] = eindex.try_emplace({a, b}, static_cast<int>(out.edges.size()));
    if (added)
      out.edges.emplace_back(a, b);
    return it->second;
  };
  int light_missing = 0;
  const LightOptions lopts{opts.light_gamma, opts.light_scale};
  for (const OutFace& of : out_faces)
  {
    std::vector<int> ids;
    for (const Vec3& p : of.pts)
      ids.push_back(vid(p));
    if (ids.size() > 3)
    {
      std::vector<int> kept;
      for (size_t k = 0; k < ids.size(); k++)
      {
        if (ids[k] != ids[(k + ids.size() - 1) % ids.size()])
          kept.push_back(ids[k]);
      }
      ids = std::move(kept);
    }
    if (ids.size() < 3)
    {
      const std::vector<int> head(ids.begin(), ids.begin() + std::min<size_t>(1, ids.size()));
      const size_t pad = 3 - ids.size();
      for (size_t k = 0; k < pad; k++)
        ids.insert(ids.end(), head.begin(), head.end());
    }
    const int first = static_cast<int>(out.surfedges.size());
    for (size_t k = 0; k < ids.size(); k++)
      out.surfedges.push_back(eid(ids[k], ids[(k + 1) % ids.size()]));
    const auto added = P.Add(of.normal, of.dist);
    if (!added)
    {
      *error = "degenerate plane";
      return std::nullopt;
    }
    const Face& fc = faces[of.src];
    std::array<uint8_t, 4> styles = {255, 255, 255, 255};
    int lightofs = -1;
    if (!of.special && of.lit && fc.lightofs >= 0 && !lighting.empty())
    {
      const Vecs& vecs = out.texinfo[of.texinfo].vecs;
      const auto samples = Relight(of, fc, texinfo[fc.texinfo], vecs, lighting, lopts);
      if (samples)
      {
        lightofs = static_cast<int>(out.lighting.size());
        for (size_t k = 0; k < samples->size() && k < 4; k++)
          styles[k] = fc.styles[k];
        for (const Bytes& block : *samples)
          out.lighting.insert(out.lighting.end(), block.begin(), block.end());
      }
      else
      {
        light_missing++;
      }
    }
    if (added->first > 65535)
    {
      *error = "ushort format requires 0 <= number <= 65535 (too many planes)";
      return std::nullopt;
    }
    out.faces.push_back({added->first, added->second ? 1 : 0, first, static_cast<int>(ids.size()),
                         of.texinfo, styles, lightofs});
  }

  // -- nodes, leafs, marksurfaces, visibility ---------------------------------------------------
  const std::vector<Node> nodes = src.Nodes();
  const std::vector<Leaf> leafs = src.Leafs();
  const std::vector<Model> models = src.Models();
  const std::vector<int> leaffaces = src.Indices(L_LEAFFACES);
  const auto face_range = [&](int ff, int nf) {
    const int64_t a = rstart[std::clamp<int64_t>(ff, 0, faces.size())];
    const int64_t b = rstart[std::clamp<int64_t>(int64_t(ff) + nf, 0, faces.size())];
    return std::pair<int64_t, int64_t>{a, b};
  };
  for (const Node& nd : nodes)
  {
    if (nd.planenum < 0 || nd.planenum >= static_cast<int>(planes_in.size()))
      return bad("node plane out of range");
    const Plane& pl = planes_in[nd.planenum];
    const auto added = P.Add(pl.normal, pl.dist);
    if (!added)
    {
      *error = "degenerate plane";
      return std::nullopt;
    }
    int c0 = nd.children[0], c1 = nd.children[1];
    if (added->second)
      std::swap(c0, c1);
    const auto [a, b] = face_range(nd.firstface, nd.numfaces);
    out.nodes.push_back({added->first, c0, c1, Box(nd.mins, -1), Box(nd.maxs, 1),
                         static_cast<int>(a), static_cast<int>(b - a)});
  }

  const std::set<int> world_leafs =
      models.empty() ? std::set<int>{} : LeafsUnder(nodes, models[0].headnode);
  const int visleafs = world_leafs.empty() ? 0 : *world_leafs.rbegin();
  if (visleafs >= static_cast<int>(leafs.size()) && visleafs > 0)
    return bad("leaf out of range");
  std::map<int, std::vector<int>> disp_leafs;
  for (const DispFace& df : disp_faces)
  {
    for (int k = df.first; k < df.first + df.count; k++)
    {
      const Winding& pts = out_faces[k].pts;
      std::set<int> lfs;
      for (const Vec3& p : pts)
        lfs.insert(PointLeaf(nodes, planes_in, models[0].headnode, p));
      lfs.insert(PointLeaf(nodes, planes_in, models[0].headnode, Mean(pts)));
      for (const int lf : lfs)
      {
        if (lf > 0 && lf < static_cast<int>(leafs.size()) && (leafs[lf].contents & CONTENTS_SOLID) == 0)
          disp_leafs[lf].push_back(k);
      }
    }
  }
  const std::vector<Bytes> pvs = src.Visibility();
  const int nclusters = static_cast<int>(pvs.size());
  std::map<int, std::vector<int>> cluster_leafs;
  for (int li = 1; li <= visleafs; li++)
  {
    const int c = leafs[li].cluster;
    if (c >= 0)
      cluster_leafs[c].push_back(li);
  }
  Bytes vis;
  std::map<Bytes, int> vis_rows;
  const size_t row_bytes = (static_cast<size_t>(visleafs) + 7) / 8;
  std::map<int, int> cluster_row;
  std::vector<std::optional<Bytes>> rows;
  for (int c = 0; c < nclusters; c++)
  {
    Bytes row(row_bytes, 0);
    const Bytes& bits = pvs[c];
    for (int oc = 0; oc < nclusters; oc++)
    {
      if (bits[oc >> 3] & (1 << (oc & 7)))
      {
        const auto it = cluster_leafs.find(oc);
        if (it == cluster_leafs.end())
          continue;
        for (const int ol : it->second)
          row[(ol - 1) >> 3] |= static_cast<uint8_t>(1 << ((ol - 1) & 7));
      }
    }
    rows.push_back(std::move(row));
  }
  rows = MergeVisRows(rows, opts.vis_merge);
  for (size_t li = 0; li < leafs.size(); li++)
  {
    const Leaf& lf = leafs[li];
    int gc;
    if (lf.contents & CONTENTS_SOLID)
      gc = BSP30_CONTENTS_SOLID;
    else if (lf.contents & CONTENTS_SLIME)
      gc = BSP30_CONTENTS_SLIME;
    else if (lf.contents & CONTENTS_WATER)
      gc = BSP30_CONTENTS_WATER;
    else
      gc = BSP30_CONTENTS_EMPTY;
    int visofs = -1;
    const int c = lf.cluster;
    if (nclusters && world_leafs.contains(static_cast<int>(li)) && c >= 0 && c < nclusters)
    {
      if (!cluster_row.contains(c))
      {
        const Bytes comp = CompressVis(*rows[c]);
        const auto [it, added] = vis_rows.try_emplace(comp, static_cast<int>(vis.size()));
        if (added)
          vis.insert(vis.end(), comp.begin(), comp.end());
        cluster_row[c] = it->second;
      }
      visofs = cluster_row[c];
    }
    const int first = static_cast<int>(out.marksurfaces.size());
    if (li > 0)
    {
      for (int k = 0; k < lf.numleaffaces; k++)
      {
        const int64_t at = int64_t(lf.firstleafface) + k;
        if (at >= static_cast<int64_t>(leaffaces.size()))
          return bad("leaf face out of range");
        const int f = leaffaces[at];
        if (f < 0 || f >= static_cast<int>(faces.size()))
          return bad("leaf face out of range");
        for (int64_t m = rstart[f]; m < rstart[f + 1]; m++)
          out.marksurfaces.push_back(static_cast<int>(m));
      }
      if (const auto it = disp_leafs.find(static_cast<int>(li)); it != disp_leafs.end())
        out.marksurfaces.insert(out.marksurfaces.end(), it->second.begin(), it->second.end());
    }
    out.leafs.push_back({gc, visofs, Box(lf.mins, -1), Box(lf.maxs, 1), first,
                         static_cast<int>(out.marksurfaces.size()) - first});
  }
  out.visibility = std::move(vis);

  // -- clip hulls and models ------------------------------------------------------------------
  const std::vector<Brush> brushes = src.Brushes();
  const std::vector<int> side_planes = src.BrushSidePlanes();
  const std::vector<int> leafbrushes = src.Indices(L_LEAFBRUSHES);
  const auto brush_side_range = [&](int bi) {
    const Brush& br = brushes[bi];
    const int64_t a = std::clamp<int64_t>(br.firstside, 0, side_planes.size());
    const int64_t b = std::clamp<int64_t>(int64_t(br.firstside) + br.numsides, a, side_planes.size());
    return std::pair<int64_t, int64_t>{a, b};
  };
  bool bad_side = false;
  const auto plane_ok = [&](int p) {
    const bool ok = p >= 0 && p < static_cast<int>(planes_in.size());
    bad_side = bad_side || !ok;
    return ok;
  };
  const auto brush_sides = [&](int bi) {
    BrushSides out_s;
    const auto [a, b] = brush_side_range(bi);
    for (int64_t s = a; s < b; s++)
    {
      if (!plane_ok(side_planes[s]))
        continue;
      const Plane& pl = planes_in[side_planes[s]];
      out_s.emplace_back(pl.normal, pl.dist);
    }
    return out_s;
  };
  const auto disp_brush = [&](int bi) {
    if (disp_bases.empty())
      return false;
    const auto [a, b] = brush_side_range(bi);
    std::set<int> pnums;
    for (int64_t s = a; s < b; s++)
      pnums.insert(side_planes[s]);
    for (const auto& [plane, centre] : disp_bases)
    {
      if (!pnums.contains(plane))
        continue;
      bool inside = true;
      for (int64_t s = a; s < b && inside; s++)
      {
        if (!plane_ok(side_planes[s]))
          return false;
        const Plane& pl = planes_in[side_planes[s]];
        inside = Bsp30Dot(pl.normal, centre) <= pl.dist + 0.5;
      }
      if (inside)
        return true;
    }
    return false;
  };

  for (size_t mi = 0; mi < models.size(); mi++)
  {
    const Model& md = models[mi];
    std::array<int, 4> heads = {md.headnode, 0, 0, 0};
    const auto [a, b] = face_range(md.firstface, md.numfaces);
    if (opts.hulls)
    {
      std::set<int> bset;
      for (const int lf : LeafsUnder(nodes, md.headnode))
      {
        if (lf >= static_cast<int>(leafs.size()))
          return bad("leaf out of range");
        const Leaf& L = leafs[lf];
        for (int k = 0; k < L.numleafbrushes; k++)
        {
          const int64_t at = int64_t(L.firstleafbrush) + k;
          if (at >= static_cast<int64_t>(leafbrushes.size()))
            return bad("leaf brush out of range");
          bset.insert(leafbrushes[at]);
        }
      }
      std::vector<BrushSides> hb;
      for (const int bi : bset)
      {
        if (bi < 0 || bi >= static_cast<int>(brushes.size()))
          return bad("brush out of range");
        if ((brushes[bi].contents & MASK_PLAYERSOLID) && !(mi == 0 && disp_brush(bi)))
          hb.push_back(brush_sides(bi));
      }
      if (bad_side)
        return bad("brush side plane out of range");
      if (mi == 0)
        hb.insert(hb.end(), disp_collision.begin(), disp_collision.end());
      for (int h = 1; h <= 3; h++)
        heads[h] = BuildClipHull(hb, h, &P, &out.clipnodes);
    }
    else
    {
      for (int h = 1; h <= 3; h++)
        heads[h] = BuildClipHull({}, h, &P, &out.clipnodes);
    }
    out.models.push_back({md.bounds, heads, mi == 0 ? visleafs : 0, static_cast<int>(a),
                          static_cast<int>(b - a)});
  }

  // -- entities -------------------------------------------------------------------------------
  std::vector<Entity> ents = ParseEntities(src.EntitiesText());
  std::string sky;
  bool has_start = false;
  for (Entity& e : ents)
  {
    const std::string* cls = EntGet(e, "classname");
    has_start = has_start || (cls && *cls == "info_player_start");
  }
  std::optional<size_t> first_spawn;
  for (size_t i = 0; i < ents.size(); i++)
  {
    Entity& e = ents[i];
    const std::string* clsp = EntGet(e, "classname");
    const std::string cls = clsp ? *clsp : "";
    if (cls == "worldspawn")
    {
      const std::string* s = EntGet(e, "skyname");
      sky = s ? *s : "";
      EntSet(e, "wad", "");
      for (const char* k : {"detailmaterial", "detailvbsp", "maxpropscreenwidth", "minpropscreenwidth"})
      {
        std::erase_if(e, [&](const auto& kv) { return kv.first == k; });
      }
    }
    else if (In(SPAWN_CLASSES, cls))
    {
      EntSet(e, "classname", "info_player_deathmatch");
      if (!first_spawn)
        first_spawn = i;
    }
  }
  if (!has_start && first_spawn)
  {
    Entity& fs_ent = ents[*first_spawn];
    const std::string* o = EntGet(fs_ent, "origin");
    const std::string* an = EntGet(fs_ent, "angles");
    ents.push_back({{"classname", "info_player_start"},
                    {"origin", o ? *o : "0 0 0"},
                    {"angles", an ? *an : "0 0 0"}});
  }
  if (opts.props)
  {
    std::set<std::string> props;
    for (const StaticProp& sp : src.StaticProps())
    {
      std::string stem = sp.model.substr(sp.model.rfind('/') == std::string::npos ? 0 : sp.model.rfind('/') + 1);
      if (const size_t dot = stem.rfind('.'); dot != std::string::npos && dot > 0 && dot + 1 < stem.size())
        stem.resize(dot);
      stem = Lower(stem);
      props.insert(sp.model);
      ents.push_back({{"classname", "cycler_sprite"},
                      {"model", "models/" + stem + ".mdl"},
                      {"origin", FormatG(sp.origin[0]) + " " + FormatG(sp.origin[1]) + " " +
                                     FormatG(sp.origin[2])},
                      {"angles", FormatG(sp.angles[0]) + " " + FormatG(sp.angles[1]) + " " +
                                     FormatG(sp.angles[2])},
                      {"framerate", "0"}});
    }
    result.props.assign(props.begin(), props.end());
  }
  out.entities = FormatEntities(ents);

  if (!sky.empty())
  {
    for (const char* sfx : SKY_SUFFIXES)
    {
      MaterialResult m = content.Material("skybox/" + sky + sfx);
      if (m.rgba)
        result.sky_sides[sfx] = std::move(*m.rgba);
    }
    if (result.sky_sides.size() < 6)
      warnings.push_back(fmt::format("sky {}: {} of 6 sides not found", sky, 6 - result.sky_sides.size()));
  }
  result.sky = sky;
  const auto problems = out.Check();
  if (!problems.empty())
  {
    std::string all;
    for (const std::string& p : problems)
      all += (all.empty() ? "" : "; ") + p;
    *error = all;
    return std::nullopt;
  }
  if (light_missing)
    warnings.push_back(fmt::format("{} face(s) had no usable Source lightmap", light_missing));
  result.bsp = out.Write();
  return result;
}

std::vector<fs::path> SourceMapDependencies(const fs::path& path, const Bytes& data)
{
  if (!IsSourceMap(data) || data.size() < 8 + 16 * 64)
    return {};
  Vbsp src(data);
  const Bytes pak = src.Raw(L_PAKFILE);
  Content content(&pak, path, {});
  std::vector<fs::path> out = content.vpks;
  std::vector<std::string> touched;
  for (const std::string& name : src.TextureNames())
    content.Material(Lower(name), 0, false, &touched);
  for (const Entity& e : ParseEntities(src.EntitiesText()))
  {
    Entity copy = e;
    const std::string* cls = EntGet(copy, "classname");
    const std::string* sky = EntGet(copy, "skyname");
    if (cls && *cls == "worldspawn" && sky && !sky->empty())
    {
      for (const char* sfx : SKY_SUFFIXES)
        content.Material("skybox/" + *sky + sfx, 0, false, &touched);
    }
  }
  for (const std::string& rel : touched)
  {
    fs::path disk;
    if (content.Locate(rel, &disk) == Content::Where::Disk)
      out.push_back(disk);
  }
  std::ranges::sort(out);
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}
}  // namespace GxBake
