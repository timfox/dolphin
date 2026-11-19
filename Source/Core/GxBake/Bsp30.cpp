// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

#include "GxBake/Bsp30.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

#include <fmt/format.h>

namespace GxBake
{
namespace
{
template <typename T>
void Put(std::vector<uint8_t>* out, T v)
{
  uint8_t b[sizeof(T)];
  std::memcpy(b, &v, sizeof(T));
  out->insert(out->end(), b, b + sizeof(T));
}

void PutF(std::vector<uint8_t>* out, double v)
{
  Put<float>(out, static_cast<float>(v));
}

// Python's round(x, 3) on a float: the exact value rounded to three decimals.
double Round3(double x)
{
  if (!std::isfinite(x))
    return x;
  char buf[512];
  std::snprintf(buf, sizeof(buf), "%.3f", x);
  return std::strtod(buf, nullptr);
}

int ArgMaxAbs(const Vec3& n)
{
  int best = 0;
  for (int i = 1; i < 3; i++)
  {
    if (std::abs(n[i]) > std::abs(n[best]))
      best = i;
  }
  return best;
}

// numpy's pairwise sum of float64.
double PairwiseSum(const double* a, size_t n)
{
  if (n < 8)
  {
    double res = 0.0;
    for (size_t i = 0; i < n; i++)
      res += a[i];
    return res;
  }
  if (n <= 128)
  {
    double r[8];
    for (int j = 0; j < 8; j++)
      r[j] = a[j];
    size_t i = 8;
    for (; i < n - (n % 8); i += 8)
    {
      for (int j = 0; j < 8; j++)
        r[j] += a[i + j];
    }
    double res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
    for (; i < n; i++)
      res += a[i];
    return res;
  }
  size_t n2 = n / 2;
  n2 -= n2 % 8;
  return PairwiseSum(a, n2) + PairwiseSum(a + n2, n - n2);
}

double NumpySum(const std::vector<double>& v)
{
  return PairwiseSum(v.data(), v.size());
}

// -- clip hulls -------------------------------------------------------------------------------

struct Side
{
  int plane;
  bool flip;
  Winding winding;
  bool boundary = false;
};

struct Frag
{
  std::vector<Side> sides;
  Winding Points() const
  {
    Winding out;
    for (const Side& s : sides)
      out.insert(out.end(), s.winding.begin(), s.winding.end());
    return out;
  }
};

std::vector<std::pair<size_t, Winding>> BrushWindings(const BrushSides& sides)
{
  std::vector<std::pair<size_t, Winding>> out;
  for (size_t i = 0; i < sides.size(); i++)
  {
    std::optional<Winding> w = BaseWinding(sides[i].first, sides[i].second);
    for (size_t j = 0; j < sides.size() && w; j++)
    {
      if (i == j)
        continue;
      w = ClipWinding(*w, sides[j].first, sides[j].second, false, 0.001);
    }
    if (w)
      out.emplace_back(i, std::move(*w));
  }
  return out;
}

std::optional<Winding> BrushPoints(const BrushSides& sides)
{
  const auto ws = BrushWindings(sides);
  if (ws.size() < 4)
    return std::nullopt;
  Winding out;
  for (const auto& [i, w] : ws)
    out.insert(out.end(), w.begin(), w.end());
  return out;
}

bool AllClose(const Vec3& a, const Vec3& b, double atol)
{
  for (int i = 0; i < 3; i++)
  {
    if (!(std::abs(a[i] - b[i]) <= atol + 1e-05 * std::abs(b[i])))
      return false;
  }
  return true;
}

BrushSides ExpandedBrush(const BrushSides& sides, const Vec3& e)
{
  const auto pts = BrushPoints(sides);
  if (!pts)
    return {};
  BrushSides out;
  for (const auto& [n, d] : sides)
  {
    const Vec3 an = {std::abs(n[0]), std::abs(n[1]), std::abs(n[2])};
    out.emplace_back(n, d + Bsp30Dot(an, e));
  }
  Vec3 lo = (*pts)[0], hi = (*pts)[0];
  for (const Vec3& p : *pts)
  {
    for (int k = 0; k < 3; k++)
    {
      lo[k] = std::min(lo[k], p[k]);
      hi[k] = std::max(hi[k], p[k]);
    }
  }
  for (int ax = 0; ax < 3; ax++)
  {
    for (const double sign : {1.0, -1.0})
    {
      Vec3 n = {0.0, 0.0, 0.0};
      n[ax] = sign;
      if (std::ranges::any_of(sides, [&](const auto& s) { return AllClose(n, s.first, 1e-5); }))
        continue;
      out.emplace_back(n, (sign > 0 ? hi[ax] : -lo[ax]) + e[ax]);
    }
  }
  return out;
}

class HullBuilder
{
public:
  explicit HullBuilder(PlaneTable* planes) : m_planes(planes) {}

  std::optional<Frag> MakeFrag(const BrushSides& sides)
  {
    auto ws = BrushWindings(sides);
    if (ws.size() < 4)
      return std::nullopt;
    Frag f;
    for (auto& [i, w] : ws)
    {
      const auto added = m_planes->Add(sides[i].first, sides[i].second);
      if (!added)
        return std::nullopt;
      f.sides.push_back({added->first, added->second, std::move(w), false});
    }
    return f;
  }

  int Build(std::vector<Frag> frags, int depth = 0)
  {
    if (frags.empty())
      return BSP30_CONTENTS_EMPTY;
    for (const Frag& f : frags)
    {
      if (std::ranges::all_of(f.sides, [](const Side& s) { return s.boundary; }))
        return BSP30_CONTENTS_SOLID;
    }
    if (depth > 400)
      return BSP30_CONTENTS_SOLID;
    const int p = Choose(frags);
    if (p < 0)
      return BSP30_CONTENTS_SOLID;
    std::vector<Frag> fronts, backs;
    for (const Frag& f : frags)
    {
      auto [a, b] = Split(f, p);
      if (a)
        fronts.push_back(std::move(*a));
      if (b)
        backs.push_back(std::move(*b));
    }
    frags.clear();
    frags.shrink_to_fit();
    const int index = static_cast<int>(nodes.size());
    nodes.push_back({p, 0, 0});
    const int c0 = Build(std::move(fronts), depth + 1);
    nodes[index][1] = c0;
    const int c1 = Build(std::move(backs), depth + 1);
    nodes[index][2] = c1;
    return index;
  }

  std::vector<std::array<int, 3>> nodes;

private:
  static Frag Mark(const Frag& f, int p)
  {
    if (std::ranges::any_of(f.sides, [&](const Side& s) { return s.plane == p && !s.boundary; }))
    {
      Frag out = f;
      for (Side& s : out.sides)
        s.boundary = s.boundary || s.plane == p;
      return out;
    }
    return f;
  }

  std::pair<std::optional<Frag>, std::optional<Frag>> Split(const Frag& f, int p)
  {
    constexpr double eps = 0.01;
    const Vec3& n = m_planes->Normal(p);
    const double d = m_planes->Dist(p);
    const Winding pts = f.Points();
    std::vector<double> dist(pts.size());
    bool all_front = true, all_back = true;
    for (size_t i = 0; i < pts.size(); i++)
    {
      dist[i] = Bsp30Row(pts[i], n, pts.size()) - d;
      all_front = all_front && dist[i] >= -eps;
      all_back = all_back && dist[i] <= eps;
    }
    if (all_front)
      return {Mark(f, p), std::nullopt};
    if (all_back)
      return {std::nullopt, Mark(f, p)};
    std::vector<Side> front, back;
    for (const Side& s : f.sides)
    {
      if (s.plane == p)
        continue;
      auto wf = ClipWinding(s.winding, n, d, true);
      auto wb = ClipWinding(s.winding, n, d);
      if (wf)
        front.push_back({s.plane, s.flip, std::move(*wf), s.boundary});
      if (wb)
        back.push_back({s.plane, s.flip, std::move(*wb), s.boundary});
    }
    std::optional<Winding> cap = BaseWinding(n, d);
    for (const Side& s : f.sides)
    {
      const double k = s.flip ? -1.0 : 1.0;
      const Vec3& sn0 = m_planes->Normal(s.plane);
      const Vec3 sn = {sn0[0] * k, sn0[1] * k, sn0[2] * k};
      cap = ClipWinding(*cap, sn, m_planes->Dist(s.plane) * k, false, 0.001);
      if (!cap)
        break;
    }
    if (!cap || front.size() < 3 || back.size() < 3)
    {
      // numerically a sliver: keep it whole on the side holding more of it
      if (NumpySum(dist) > 0)
        return {Mark(f, p), std::nullopt};
      return {std::nullopt, Mark(f, p)};
    }
    Winding reversed(cap->rbegin(), cap->rend());
    front.push_back({p, true, std::move(reversed), true});
    back.push_back({p, false, std::move(*cap), true});
    return {Frag{std::move(front)}, Frag{std::move(back)}};
  }

  int Choose(const std::vector<Frag>& frags) const
  {
    std::vector<std::pair<int, int>> cands;  // plane, count, in first-seen order
    std::unordered_map<int, size_t> where;
    for (const Frag& f : frags)
    {
      for (const Side& s : f.sides)
      {
        if (s.boundary)
          continue;
        const auto [it, added] = where.try_emplace(s.plane, cands.size());
        if (added)
          cands.emplace_back(s.plane, 0);
        cands[it->second].second++;
      }
    }
    if (cands.empty())
      return -1;
    std::ranges::stable_sort(cands, [&](const auto& a, const auto& b) {
      const bool na = m_planes->Type(a.first) >= 3, nb = m_planes->Type(b.first) >= 3;
      if (na != nb)
        return !na;
      return a.second > b.second;
    });
    if (cands.size() > 24)
      cands.resize(24);
    if (frags.size() == 1)
      return cands[0].first;
    std::vector<Winding> pts;
    for (const Frag& f : frags)
      pts.push_back(f.Points());
    int best = -1;
    int best_score = 0;
    for (const auto& [p, count] : cands)
    {
      const Vec3& n = m_planes->Normal(p);
      const double d = m_planes->Dist(p);
      int front = 0, back = 0, split = 0;
      for (const Winding& q : pts)
      {
        bool all_front = true, all_back = true;
        for (const Vec3& x : q)
        {
          const double dist = Bsp30Row(x, n, q.size()) - d;
          all_front = all_front && dist >= -0.01;
          all_back = all_back && dist <= 0.01;
        }
        if (all_front)
          front++;
        else if (all_back)
          back++;
        else
          split++;
      }
      const int score = split * 5 + std::abs(front - back) + (m_planes->Type(p) >= 3 ? 2 : 0);
      if (best < 0 || score < best_score)
      {
        best = p;
        best_score = score;
      }
    }
    return best;
  }

  PlaneTable* m_planes;
};
}  // namespace

double Bsp30Dot(const Vec3& a, const Vec3& b)
{
  double s = a[0] * b[0];
  s = std::fma(a[1], b[1], s);
  return std::fma(a[2], b[2], s);
}

double Bsp30Row(const Vec3& row, const Vec3& v, size_t rows)
{
  if (rows == 1)
    return Bsp30Dot(row, v);
  double s = row[1] * v[1];
  s = std::fma(row[0], v[0], s);
  return std::fma(row[2], v[2], s);
}

double Bsp30Norm(const Vec3& v)
{
  return std::sqrt(Bsp30Dot(v, v));
}

Vec3 Bsp30Cross(const Vec3& a, const Vec3& b)
{
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

size_t PlaneTable::KeyHash::operator()(const Key& k) const
{
  size_t h = 0;
  for (const double v : {k.n0, k.n1, k.n2, k.d})
    h = h * 1000003u ^ std::hash<double>()(v + 0.0);
  return h;
}

std::optional<std::pair<int, bool>> PlaneTable::Add(const Vec3& normal, double dist)
{
  const double ln = Bsp30Norm(normal);
  if (ln == 0)
    return std::nullopt;
  Vec3 n = {normal[0] / ln, normal[1] / ln, normal[2] / ln};
  double d = dist / ln;
  int t = -1;
  for (int i = 0; i < 3 && t < 0; i++)
  {
    if (std::abs(n[i]) > 0.99999)
      t = i;
  }
  if (t < 0)
    t = 3 + ArgMaxAbs(n);
  bool flipped = false;
  if (t < 3)
  {
    n = {0.0, 0.0, 0.0};
    n[t] = normal[t] > 0 ? 1.0 : -1.0;
    if (n[t] < 0)
    {
      n = {-n[0], -n[1], -n[2]};
      d = -d;
      flipped = true;
    }
  }
  else if (n[t - 3] < 0)
  {
    n = {-n[0], -n[1], -n[2]};
    d = -d;
    flipped = true;
  }
  // round(n[i], 5) on numpy floats is rint(x * 1e5) / 1e5; round(d, 3) is Python's.
  const Key key{std::nearbyint(n[0] * 1e5) + 0.0, std::nearbyint(n[1] * 1e5) + 0.0,
                std::nearbyint(n[2] * 1e5) + 0.0, Round3(d) + 0.0};
  const auto [it, added] = m_index.try_emplace(key, static_cast<int>(m_dists.size()));
  if (added)
  {
    m_normals.push_back(n);
    m_dists.push_back(d);
    m_types.push_back(t);
  }
  return std::pair{it->second, flipped};
}

std::vector<uint8_t> PlaneTable::Pack() const
{
  std::vector<uint8_t> out;
  out.reserve(m_dists.size() * 20);
  for (size_t i = 0; i < m_dists.size(); i++)
  {
    for (const double c : m_normals[i])
      PutF(&out, c);
    PutF(&out, m_dists[i]);
    Put<int32_t>(&out, m_types[i]);
  }
  return out;
}

Winding BaseWinding(const Vec3& n, double dist, double size)
{
  const int ax = ArgMaxAbs(n);
  Vec3 up = ax != 2 ? Vec3{0.0, 0.0, 1.0} : Vec3{1.0, 0.0, 0.0};
  const double dt = Bsp30Dot(up, n);
  for (int i = 0; i < 3; i++)
    up[i] = up[i] - n[i] * dt;
  const double ln = Bsp30Norm(up);
  for (double& c : up)
    c /= ln;
  Vec3 right = Bsp30Cross(up, n);
  const Vec3 org = {n[0] * dist, n[1] * dist, n[2] * dist};
  for (int i = 0; i < 3; i++)
  {
    up[i] *= size;
    right[i] *= size;
  }
  Winding w(4);
  for (int i = 0; i < 3; i++)
  {
    w[0][i] = org[i] - right[i] + up[i];
    w[1][i] = org[i] + right[i] + up[i];
    w[2][i] = org[i] + right[i] - up[i];
    w[3][i] = org[i] - right[i] - up[i];
  }
  return w;
}

std::optional<Winding> ClipWinding(const Winding& w, const Vec3& normal, double dist,
                                   bool keep_front, double eps)
{
  if (w.empty())
    return std::nullopt;
  const size_t n = w.size();
  std::vector<double> d(n);
  bool all_back = true, all_front = true;
  for (size_t i = 0; i < n; i++)
  {
    d[i] = Bsp30Row(w[i], normal, n) - dist;
    if (keep_front)
      d[i] = -d[i];
    all_back = all_back && d[i] <= eps;
    all_front = all_front && d[i] >= -eps;
  }
  if (all_back)
    return w;
  if (all_front)
    return std::nullopt;
  Winding out;
  for (size_t i = 0; i < n; i++)
  {
    const Vec3& a = w[i];
    const Vec3& b = w[(i + 1) % n];
    const double da = d[i], db = d[(i + 1) % n];
    if (da <= eps)
      out.push_back(a);
    if ((da < -eps && db > eps) || (da > eps && db < -eps))
    {
      const double t = da / (da - db);
      out.push_back({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t});
    }
  }
  if (out.size() < 3)
    return std::nullopt;
  return out;
}

int BuildClipHull(const std::vector<BrushSides>& brushes, int hull, PlaneTable* planes,
                  std::vector<std::array<int, 3>>* clipnodes)
{
  Vec3 size = {0.0, 0.0, 0.0};
  if (hull == 1)
    size = {16.0, 16.0, 36.0};
  else if (hull == 2)
    size = {32.0, 32.0, 32.0};
  else if (hull == 3)
    size = {16.0, 16.0, 18.0};
  HullBuilder b(planes);
  std::vector<Frag> frags;
  for (const BrushSides& sides : brushes)
  {
    const BrushSides grown = ExpandedBrush(sides, size);
    if (grown.empty())
      continue;
    if (auto f = b.MakeFrag(grown))
      frags.push_back(std::move(*f));
  }
  const int head = b.Build(std::move(frags));
  const int base = static_cast<int>(clipnodes->size());
  if (head < 0)
  {
    const auto p = planes->Add({0.0, 0.0, 1.0}, 0.0);
    clipnodes->push_back({p->first, head, head});
    return base;
  }
  for (const auto& [plane, c0, c1] : b.nodes)
    clipnodes->push_back({plane, c0 >= 0 ? c0 + base : c0, c1 >= 0 ? c1 + base : c1});
  return base + head;
}

std::vector<uint8_t> CompressVis(const std::vector<uint8_t>& row)
{
  std::vector<uint8_t> out;
  size_t i = 0;
  const size_t n = row.size();
  while (i < n)
  {
    const uint8_t c = row[i];
    if (c)
    {
      out.push_back(c);
      i++;
      continue;
    }
    int run = 0;
    while (i < n && row[i] == 0 && run < 255)
    {
      run++;
      i++;
    }
    out.push_back(0);
    out.push_back(static_cast<uint8_t>(run));
  }
  return out;
}

std::vector<std::optional<std::vector<uint8_t>>>
MergeVisRows(const std::vector<std::optional<std::vector<uint8_t>>>& rows, int max_bits)
{
  if (max_bits <= 0)
    return rows;
  std::vector<std::vector<uint8_t>> reps;
  std::vector<size_t> owner(rows.size());
  for (size_t i = 0; i < rows.size(); i++)
  {
    if (!rows[i])
      continue;
    const std::vector<uint8_t>& v = *rows[i];
    bool found = false;
    for (size_t j = 0; j < reps.size() && !found; j++)
    {
      int bits = 0;
      for (size_t k = 0; k < v.size(); k++)
        bits += std::popcount(static_cast<unsigned>(reps[j][k] ^ v[k]));
      if (bits <= max_bits)
      {
        for (size_t k = 0; k < v.size(); k++)
          reps[j][k] |= v[k];
        owner[i] = j;
        found = true;
      }
    }
    if (!found)
    {
      owner[i] = reps.size();
      reps.push_back(v);
    }
  }
  std::vector<std::optional<std::vector<uint8_t>>> out(rows.size());
  for (size_t i = 0; i < rows.size(); i++)
  {
    if (rows[i])
      out[i] = reps[owner[i]];
  }
  return out;
}

std::vector<uint8_t> Miptex::Pack() const
{
  std::vector<uint8_t> out(16, 0);
  std::memcpy(out.data(), name.data(), std::min<size_t>(15, name.size()));
  Put<uint32_t>(&out, width);
  Put<uint32_t>(&out, height);
  if (mips.empty())
  {
    out.resize(out.size() + 16, 0);
    return out;
  }
  uint32_t pos = 40;
  for (const auto& m : mips)
  {
    Put<uint32_t>(&out, pos);
    pos += static_cast<uint32_t>(m.size());
  }
  for (const auto& m : mips)
    out.insert(out.end(), m.begin(), m.end());
  Put<uint16_t>(&out, 256);
  std::vector<uint8_t> pal(palette.begin(), palette.begin() + std::min<size_t>(768, palette.size()));
  pal.resize(768, 0);
  out.insert(out.end(), pal.begin(), pal.end());
  out.push_back(0);
  out.push_back(0);
  return out;
}

std::vector<std::string> Bsp30Map::Check() const
{
  std::vector<std::string> out;
  const std::pair<const char*, std::pair<size_t, size_t>> limits[] = {
      {"nodes", {nodes.size(), 32767}},         {"clipnodes", {clipnodes.size(), 32767}},
      {"leafs", {leafs.size(), 32767}},         {"faces", {faces.size(), 65535}},
      {"vertexes", {vertexes.size(), 65535}},   {"edges", {edges.size(), 65535}},
      {"texinfo", {texinfo.size(), 32767}},     {"marksurfaces", {marksurfaces.size(), 65535}},
  };
  for (const auto& [name, nc] : limits)
  {
    if (nc.first > nc.second)
      out.push_back(fmt::format("{}: {} exceeds the BSP30 limit of {}", name, nc.first, nc.second));
  }
  return out;
}

std::vector<uint8_t> Bsp30Map::Write() const
{
  std::vector<std::vector<uint8_t>> lumps(15);
  lumps[0].assign(entities.begin(), entities.end());
  lumps[0].push_back(0);
  lumps[1] = planes.Pack();
  {
    std::vector<std::vector<uint8_t>> mips;
    for (const Miptex& t : textures)
      mips.push_back(t.Pack());
    std::vector<uint8_t>& tex = lumps[2];
    Put<int32_t>(&tex, static_cast<int32_t>(mips.size()));
    size_t pos = 4 + 4 * mips.size();
    for (const auto& m : mips)
    {
      Put<int32_t>(&tex, static_cast<int32_t>(pos));
      pos += m.size();
      pos += (4 - pos % 4) % 4;
    }
    for (const auto& m : mips)
    {
      tex.insert(tex.end(), m.begin(), m.end());
      tex.resize(tex.size() + (4 - tex.size() % 4) % 4, 0);
    }
  }
  for (const Vec3& v : vertexes)
  {
    for (const double c : v)
      PutF(&lumps[3], c);
  }
  lumps[4] = visibility;
  for (const Node& n : nodes)
  {
    std::vector<uint8_t>& o = lumps[5];
    Put<int32_t>(&o, n.plane);
    Put<int16_t>(&o, static_cast<int16_t>(n.c0));
    Put<int16_t>(&o, static_cast<int16_t>(n.c1));
    for (const int c : n.mins)
      Put<int16_t>(&o, static_cast<int16_t>(c));
    for (const int c : n.maxs)
      Put<int16_t>(&o, static_cast<int16_t>(c));
    Put<uint16_t>(&o, static_cast<uint16_t>(n.firstface));
    Put<uint16_t>(&o, static_cast<uint16_t>(n.numfaces));
  }
  for (const TexInfo& t : texinfo)
  {
    for (const double v : t.vecs)
      PutF(&lumps[6], v);
    Put<int32_t>(&lumps[6], t.miptex);
    Put<int32_t>(&lumps[6], t.flags);
  }
  for (const Face& f : faces)
  {
    std::vector<uint8_t>& o = lumps[7];
    Put<uint16_t>(&o, static_cast<uint16_t>(f.plane));
    Put<uint16_t>(&o, static_cast<uint16_t>(f.side));
    Put<int32_t>(&o, f.firstedge);
    Put<uint16_t>(&o, static_cast<uint16_t>(f.numedges));
    Put<uint16_t>(&o, static_cast<uint16_t>(f.texinfo));
    o.insert(o.end(), f.styles.begin(), f.styles.end());
    Put<int32_t>(&o, f.lightofs);
  }
  lumps[8] = lighting;
  for (const auto& [p, c0, c1] : clipnodes)
  {
    Put<int32_t>(&lumps[9], p);
    Put<int16_t>(&lumps[9], static_cast<int16_t>(c0));
    Put<int16_t>(&lumps[9], static_cast<int16_t>(c1));
  }
  for (const Leaf& l : leafs)
  {
    std::vector<uint8_t>& o = lumps[10];
    Put<int32_t>(&o, l.contents);
    Put<int32_t>(&o, l.visofs);
    for (const int c : l.mins)
      Put<int16_t>(&o, static_cast<int16_t>(c));
    for (const int c : l.maxs)
      Put<int16_t>(&o, static_cast<int16_t>(c));
    Put<uint16_t>(&o, static_cast<uint16_t>(l.firstmark));
    Put<uint16_t>(&o, static_cast<uint16_t>(l.nummark));
    o.insert(o.end(), 4, 0);
  }
  for (const int m : marksurfaces)
    Put<uint16_t>(&lumps[11], static_cast<uint16_t>(m));
  for (const auto& [a, b] : edges)
  {
    Put<uint16_t>(&lumps[12], static_cast<uint16_t>(a));
    Put<uint16_t>(&lumps[12], static_cast<uint16_t>(b));
  }
  for (const int s : surfedges)
    Put<int32_t>(&lumps[13], s);
  for (const Model& m : models)
  {
    for (const float f : m.bounds)
      Put<float>(&lumps[14], f);
    for (const int h : m.heads)
      Put<int32_t>(&lumps[14], h);
    Put<int32_t>(&lumps[14], m.visleafs);
    Put<int32_t>(&lumps[14], m.firstface);
    Put<int32_t>(&lumps[14], m.numfaces);
  }
  std::vector<uint8_t> out;
  Put<int32_t>(&out, 30);
  out.resize(4 + 15 * 8, 0);
  for (size_t i = 0; i < lumps.size(); i++)
  {
    out.resize(out.size() + (4 - out.size() % 4) % 4, 0);
    const int32_t ofs = static_cast<int32_t>(out.size());
    const int32_t len = static_cast<int32_t>(lumps[i].size());
    std::memcpy(out.data() + 4 + 8 * i, &ofs, 4);
    std::memcpy(out.data() + 8 + 8 * i, &len, 4);
    out.insert(out.end(), lumps[i].begin(), lumps[i].end());
  }
  return out;
}
}  // namespace GxBake
