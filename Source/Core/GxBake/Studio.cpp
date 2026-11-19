// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

#include "GxBake/Studio.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <tuple>

#include "GxBake/PilQuant.h"
#include "GxBake/Resample.h"

namespace GxBake
{
namespace
{
constexpr int MAXSTUDIOVERTS = 2048;
constexpr int MAXSTUDIOTRIANGLES = 20000;
constexpr int MAXSTUDIOSKINS = 100;
constexpr int STUDIO_VERSION = 10;
constexpr int STUDIO_NF_MASKED = 0x40;
constexpr int STUDIO_LOOPING = 0x0001;

constexpr size_t HDR = 244;
constexpr size_t SEQ = 176;
constexpr size_t BODYPART = 76;
constexpr size_t MODEL = 112;
constexpr size_t MESH = 20;
constexpr size_t TEXTURE = 80;

using Bytes = std::vector<uint8_t>;
using Vec3 = std::array<double, 3>;
using Mat4 = std::array<std::array<double, 4>, 4>;

void PutI32(Bytes* out, int32_t v)
{
  for (int i = 0; i < 4; i++)
    out->push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i)));
}

void PutI16(Bytes* out, int32_t v)
{
  out->push_back(static_cast<uint8_t>(v & 0xff));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

void PutF32(Bytes* out, double v)
{
  const float f = static_cast<float>(v);
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  PutI32(out, static_cast<int32_t>(bits));
}

void SetI32(Bytes* out, size_t p, int32_t v)
{
  for (int i = 0; i < 4; i++)
    (*out)[p + i] = static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i));
}

void SetF32(Bytes* out, size_t p, double v)
{
  const float f = static_cast<float>(v);
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  SetI32(out, p, static_cast<int32_t>(bits));
}

// struct.pack("<Ns", value): truncated or zero-padded to |size| bytes.
void SetString(Bytes* out, size_t p, const std::string& value, size_t size)
{
  for (size_t i = 0; i < size; i++)
    (*out)[p + i] = i < value.size() ? static_cast<uint8_t>(value[i]) : 0;
}

void PutString(Bytes* out, const std::string& value, size_t size)
{
  const size_t p = out->size();
  out->resize(p + size);
  SetString(out, p, value, size);
}

void Align(Bytes* out)
{
  while (out->size() % 4)
    out->push_back(0);
}

// numpy.round(x, decimals): rint(x * 10**decimals) / 10**decimals.
double RoundTo(double x, int decimals)
{
  const double f = std::pow(10.0, decimals);
  return std::nearbyint(x * f) / f;
}

uint8_t LowerLatin1(uint8_t c)
{
  if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
    return static_cast<uint8_t>(c + 32);
  return c;
}

bool IsAlnumLatin1(uint8_t c)
{
  if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
    return true;
  switch (c)
  {
  case 0xAA:
  case 0xB2:
  case 0xB3:
  case 0xB5:
  case 0xB9:
  case 0xBA:
  case 0xBC:
  case 0xBD:
  case 0xBE:
    return true;
  default:
    return c >= 0xC0 && c != 0xD7 && c != 0xF7;
  }
}

std::string Lower(std::string s)
{
  for (char& c : s)
    c = static_cast<char>(LowerLatin1(static_cast<uint8_t>(c)));
  return s;
}

// models._texture_name
std::string TextureName(const std::string& name, size_t i)
{
  std::string safe;
  for (const char c : Lower(ToLatin1(name)))
    safe += IsAlnumLatin1(static_cast<uint8_t>(c)) || c == '_' || c == '-' ? c : '_';
  if (safe.empty())
    safe = "skin" + std::to_string(i);
  return safe.substr(0, 58) + ".bmp";
}

// --- skeleton.py --------------------------------------------------------------------------------

Mat4 MatrixFromPose(const Pose& p)
{
  const double sr = std::sin(p[3]), cr = std::cos(p[3]);
  const double sp = std::sin(p[4]), cp = std::cos(p[4]);
  const double sy = std::sin(p[5]), cy = std::cos(p[5]);
  Mat4 m{};
  m[0] = {cp * cy, sr * sp * cy - cr * sy, cr * sp * cy + sr * sy, p[0]};
  m[1] = {cp * sy, sr * sp * sy + cr * cy, cr * sp * sy - sr * cy, p[1]};
  m[2] = {-sp, sr * cp, cr * cp, p[2]};
  m[3] = {0.0, 0.0, 0.0, 1.0};
  return m;
}

Mat4 Multiply(const Mat4& a, const Mat4& b)
{
  Mat4 out{};
  for (int i = 0; i < 4; i++)
  {
    for (int j = 0; j < 4; j++)
    {
      double sum = a[i][0] * b[0][j];
      for (int k = 1; k < 4; k++)
        sum += a[i][k] * b[k][j];
      out[i][j] = sum;
    }
  }
  return out;
}

std::vector<Mat4> WorldMatrices(const std::vector<Pose>& poses, const std::vector<int>& parents)
{
  std::vector<Mat4> mats;
  for (size_t b = 0; b < parents.size(); b++)
  {
    const Mat4 m = MatrixFromPose(poses[b]);
    mats.push_back(parents[b] < 0 ? m : Multiply(mats[parents[b]], m));
  }
  return mats;
}

std::vector<Pose> BoneScales(const std::vector<Pose>& rest,
                             const std::vector<const std::vector<std::vector<Pose>>*>& sequences)
{
  std::vector<Pose> dev(rest.size(), Pose{});
  for (const auto* frames : sequences)
  {
    for (size_t b = 0; b < rest.size(); b++)
    {
      for (int j = 0; j < 6; j++)
      {
        double m = std::abs((*frames)[0][b][j] - rest[b][j]);
        for (const auto& f : *frames)
          m = std::max(m, std::abs(f[b][j] - rest[b][j]));
        dev[b][j] = std::max(dev[b][j], m);
      }
    }
  }
  for (auto& d : dev)
  {
    for (double& v : d)
      v = v > 1e-9 ? v / 32000.0 : 1.0 / 32.0;
  }
  return dev;
}

// skeleton.rle_encode: chunks chosen by dynamic programming to minimise the stream.
std::vector<int16_t> RleEncode(const std::vector<int64_t>& q)
{
  const size_t n = q.size();
  if (!n)
    return {};
  constexpr int64_t INF = int64_t(1) << 30;
  std::vector<int64_t> best(n + 1, INF);
  std::vector<size_t> cut(n + 1, 0);
  best[n] = 0;
  std::vector<bool> is_end(n + 1, false);
  for (size_t k = 1; k < n; k++)
    is_end[k] = q[k] != q[k - 1];
  is_end[n] = true;
  std::vector<size_t> run_start(n, 0);
  for (size_t k = 1; k < n; k++)
    run_start[k] = q[k] == q[k - 1] ? run_start[k - 1] : k;
  for (size_t ii = n; ii-- > 0;)
  {
    const size_t lim = std::min(n, ii + 255);
    for (size_t j = ii + 1; j <= lim; j++)
    {
      if (!is_end[j] && j != lim)
        continue;
      const size_t valid = std::max(run_start[j - 1], ii) - ii + 1;
      const int64_t c = 1 + static_cast<int64_t>(valid) + best[j];
      if (c < best[ii])
      {
        best[ii] = c;
        cut[ii] = j;
      }
    }
  }
  std::vector<int16_t> out;
  size_t i = 0;
  while (i < n)
  {
    const size_t j = cut[i];
    const size_t valid = std::max(run_start[j - 1], i) - i + 1;
    out.push_back(static_cast<int16_t>(static_cast<uint16_t>(valid | ((j - i) << 8))));
    for (size_t k = i; k < i + valid; k++)
      out.push_back(static_cast<int16_t>(q[k]));
    i = j;
  }
  return out;
}

// skeleton.encode_anim_block. Empty when the offsets don't fit 16 bits.
std::optional<Bytes> EncodeAnimBlock(const std::vector<std::vector<Pose>>& frames,
                                     const std::vector<Pose>& rest, const std::vector<Pose>& scale)
{
  const size_t nbones = rest.size();
  Bytes head(nbones * 12, 0);
  Bytes data;
  for (size_t b = 0; b < nbones; b++)
  {
    std::array<uint16_t, 6> offs{};
    for (int j = 0; j < 6; j++)
    {
      std::vector<int64_t> q;
      bool any = false;
      for (const auto& f : frames)
      {
        const double v = std::nearbyint((f[b][j] - rest[b][j]) / scale[b][j]);
        int64_t iv = static_cast<int64_t>(v);
        any |= iv != 0;
        q.push_back(iv);
      }
      if (!any)
        continue;
      for (int64_t& v : q)
        v = std::clamp<int64_t>(v, -32768, 32767);
      const size_t pos = head.size() + data.size();
      const int64_t rel = static_cast<int64_t>(pos) - static_cast<int64_t>(b * 12);
      if (rel > 0xFFFF)
        return std::nullopt;
      offs[j] = static_cast<uint16_t>(rel);
      for (const int16_t v : RleEncode(q))
        PutI16(&data, v);
    }
    for (int j = 0; j < 6; j++)
    {
      head[b * 12 + j * 2] = static_cast<uint8_t>(offs[j] & 0xff);
      head[b * 12 + j * 2 + 1] = static_cast<uint8_t>(offs[j] >> 8);
    }
  }
  Bytes out = head;
  out.insert(out.end(), data.begin(), data.end());
  Align(&out);
  return out;
}

// --- triangle commands --------------------------------------------------------------------------

using Tri = std::array<int, 3>;

// models._tri_commands: strips and fans over corner ids, keeping each triangle's winding.
std::vector<std::pair<bool, std::vector<int>>> TriCommands(const std::vector<Tri>& tris,
                                                           size_t lookahead = 16)
{
  // Python dicts keep insertion order; only lookups and per-key lists matter here.
  std::map<std::pair<int, int>, std::vector<int>> edge;
  for (size_t t = 0; t < tris.size(); t++)
  {
    const auto [a, b, c] = tris[t];
    for (const auto& e : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}})
      edge[e].push_back(static_cast<int>(t));
  }
  std::vector<std::vector<int>> nbrs(tris.size());
  for (size_t t = 0; t < tris.size(); t++)
  {
    const auto [a, b, c] = tris[t];
    for (const auto& [x, y] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}})
    {
      const auto it = edge.find({y, x});
      if (it == edge.end())
        continue;
      for (const int u : it->second)
      {
        if (u != static_cast<int>(t))
          nbrs[t].push_back(u);
      }
    }
  }
  std::vector<bool> used(tris.size(), false);
  std::vector<int> deg(tris.size());
  std::map<int, std::set<int>> buckets;
  for (size_t t = 0; t < tris.size(); t++)
  {
    deg[t] = static_cast<int>(std::set<int>(nbrs[t].begin(), nbrs[t].end()).size());
    buckets[deg[t]].insert(static_cast<int>(t));
  }

  const auto third = [&](int t, int x, int y) {
    const auto [a, b, c] = tris[t];
    if (a == x && b == y)
      return c;
    if (b == x && c == y)
      return a;
    return b;
  };
  const auto take = [&](int x, int y, const std::set<int>& claimed) -> int {
    const auto it = edge.find({x, y});
    if (it == edge.end())
      return -1;
    for (const int t : it->second)
    {
      if (!used[t] && !claimed.contains(t))
        return t;
    }
    return -1;
  };
  const auto run = [&](const std::array<int, 3>& seq, int t0, bool fan) {
    std::vector<int> s(seq.begin(), seq.end());
    std::vector<int> claimed = {t0};
    std::set<int> got = {t0};
    while (true)
    {
      int x, y;
      if (fan)
      {
        x = s.front();
        y = s.back();
      }
      else if (s.size() % 2 == 0)
      {
        x = s[s.size() - 2];
        y = s.back();
      }
      else
      {
        x = s.back();
        y = s[s.size() - 2];
      }
      const int t = take(x, y, got);
      if (t < 0)
        return std::pair{s, claimed};
      claimed.push_back(t);
      got.insert(t);
      s.push_back(third(t, x, y));
    }
  };
  const auto candidates = [&] {
    std::vector<int> out;
    for (const auto& [d, b] : buckets)
    {
      if (b.empty())
        continue;
      for (const int t : b)
      {
        if (out.size() >= lookahead)
          break;
        out.push_back(t);
      }
      if (out.size() >= lookahead)
        break;
    }
    return out;
  };
  const auto mark = [&](int t) {
    used[t] = true;
    buckets[deg[t]].erase(t);
    for (const int u : std::set<int>(nbrs[t].begin(), nbrs[t].end()))
    {
      if (used[u])
        continue;
      buckets[deg[u]].erase(u);
      deg[u]--;
      buckets[deg[u]].insert(u);
    }
  };

  std::vector<std::pair<bool, std::vector<int>>> out;
  size_t left = tris.size();
  while (left)
  {
    bool have = false;
    std::pair<int, int> best_key;
    bool best_fan = false;
    std::vector<int> best_s, best_claimed;
    for (const int t0 : candidates())
    {
      const auto [a, b, c] = tris[t0];
      for (const auto& rot : {std::array{a, b, c}, std::array{b, c, a}, std::array{c, a, b}})
      {
        for (const bool fan : {false, true})
        {
          auto [s, claimed] = run(rot, t0, fan);
          const std::pair<int, int> key{static_cast<int>(claimed.size()), -deg[t0]};
          if (!have || key > best_key)
          {
            have = true;
            best_key = key;
            best_fan = fan;
            best_s = std::move(s);
            best_claimed = std::move(claimed);
          }
        }
      }
    }
    for (const int t : best_claimed)
      mark(t);
    left -= best_claimed.size();
    out.emplace_back(best_fan && best_s.size() > 3, best_s);
  }
  return out;
}

struct Corner
{
  int vi, ni, s, t;
  auto operator<=>(const Corner&) const = default;
};

struct Mesh
{
  int skin;
  int numnorms;
  int normstart;
  std::vector<std::pair<bool, std::vector<Corner>>> strips;
};
}  // namespace

std::string ToLatin1(const std::string& utf8)
{
  std::string out;
  for (size_t i = 0; i < utf8.size();)
  {
    const uint8_t c = static_cast<uint8_t>(utf8[i]);
    uint32_t cp;
    size_t len;
    if (c < 0x80)
    {
      cp = c;
      len = 1;
    }
    else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size())
    {
      cp = ((c & 0x1F) << 6) | (utf8[i + 1] & 0x3F);
      len = 2;
    }
    else if ((c & 0xF0) == 0xE0)
    {
      cp = 0x10000;
      len = 3;
    }
    else if ((c & 0xF8) == 0xF0)
    {
      cp = 0x10000;
      len = 4;
    }
    else
    {
      cp = c;  // not UTF-8: keep the byte (it was probably Latin-1 already)
      len = 1;
    }
    out += cp <= 0xFF ? static_cast<char>(cp) : '?';
    i += len;
  }
  return out;
}

std::vector<std::vector<Pose>> ImportedModel::RestFrames() const
{
  std::vector<Pose> rest;
  for (const StudioBone& b : bones)
    rest.push_back(b.pose);
  return {rest};
}

RgbaImage FitSkin(const RgbaImage& rgba, int max_edge)
{
  const auto bit_length = [](int v) {
    int n = 0;
    while (v > 0)
    {
      n++;
      v >>= 1;
    }
    return n;
  };
  int tw = 1 << std::max(3, bit_length(rgba.width - 1));
  int th = 1 << std::max(3, bit_length(rgba.height - 1));
  while (std::max(tw, th) > std::max(8, max_edge))
  {
    tw = std::max(8, tw / 2);
    th = std::max(8, th / 2);
  }
  if (tw == rgba.width && th == rgba.height)
    return rgba;
  RgbaImage out;
  out.width = tw;
  out.height = th;
  out.pixels = ResizeRgba(rgba.pixels, rgba.width, rgba.height, tw, th, Filter::Lanczos);
  return out;
}

std::optional<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>
QuantizeSkin(const RgbaImage& rgba, bool masked, int alpha_threshold)
{
  const size_t n = static_cast<size_t>(rgba.width) * rgba.height;
  const int colours = masked ? 255 : 256;
  std::vector<uint8_t> rgb(n * 3);
  for (size_t i = 0; i < n; i++)
    std::memcpy(&rgb[i * 3], &rgba.pixels[i * 4], 3);
  std::vector<uint8_t> idx, pal;
  if (!MedianCutQuantize(rgb.data(), n, colours, &idx, &pal))
    return std::nullopt;
  pal.resize(std::min<size_t>(pal.size(), static_cast<size_t>(colours) * 3));
  pal.resize(768, 0);
  if (masked)
  {
    for (size_t i = 0; i < n; i++)
    {
      if (rgba.pixels[i * 4 + 3] < alpha_threshold)
        idx[i] = 255;
    }
    pal[765] = 0;
    pal[766] = 0;
    pal[767] = 0xff;
  }
  return std::pair{idx, pal};
}

std::optional<std::vector<uint8_t>> BuildStudioMdl(ImportedModel& model, std::string* error,
                                                   int alpha_threshold)
{
  if (model.materials.size() > MAXSTUDIOSKINS)
  {
    *error = std::to_string(model.materials.size()) + " materials; studio models allow " +
             std::to_string(MAXSTUDIOSKINS);
    return std::nullopt;
  }

  // Positions are shared model-wide; normals are laid out per mesh.
  std::map<std::tuple<int, double, double, double>, int> pos_index;
  std::vector<Vec3> positions, normals;
  std::vector<int> vert_bone, norm_bone;
  std::vector<Mesh> meshes;
  size_t ntris = 0;
  for (size_t skin = 0; skin < model.materials.size(); skin++)
  {
    std::vector<const StudioSubmesh*> subs;
    for (const StudioSubmesh& m : model.meshes)
    {
      if (m.material == static_cast<int>(skin))
        subs.push_back(&m);
    }
    if (subs.empty())
      continue;
    const StudioMaterial& mat = model.materials[skin];
    const int th = mat.rgba.height, tw = mat.rgba.width;
    std::map<std::tuple<int, double, double, double>, int> norm_index;
    const int norm_start = static_cast<int>(normals.size());
    std::map<Corner, int> corners;
    std::vector<Corner> corner_list;
    std::vector<Tri> tris;
    for (const StudioSubmesh* sm : subs)
    {
      std::vector<int> cid(sm->positions.size());
      for (size_t v = 0; v < sm->positions.size(); v++)
      {
        const int bone = sm->bones.empty() ? 0 : sm->bones[v];
        const Vec3 p = {RoundTo(sm->positions[v][0], 4), RoundTo(sm->positions[v][1], 4),
                        RoundTo(sm->positions[v][2], 4)};
        const auto pkey = std::tuple{bone, p[0], p[1], p[2]};
        auto pit = pos_index.find(pkey);
        if (pit == pos_index.end())
        {
          pit = pos_index.emplace(pkey, static_cast<int>(positions.size())).first;
          positions.push_back(p);
          vert_bone.push_back(bone);
        }
        const Vec3 nv = {RoundTo(sm->normals[v][0], 3), RoundTo(sm->normals[v][1], 3),
                         RoundTo(sm->normals[v][2], 3)};
        const auto nkey = std::tuple{bone, nv[0], nv[1], nv[2]};
        auto nit = norm_index.find(nkey);
        if (nit == norm_index.end())
        {
          nit = norm_index.emplace(nkey, static_cast<int>(normals.size())).first;
          normals.push_back(nv);
          norm_bone.push_back(bone);
        }
        const auto clip = [](double x) {
          return static_cast<int>(std::clamp(std::nearbyint(x), -32768.0, 32767.0));
        };
        const Corner key{pit->second, nit->second,
                         clip(static_cast<double>(sm->uvs[v][0]) * tw),
                         clip(static_cast<double>(sm->uvs[v][1]) * th)};
        auto cit = corners.find(key);
        if (cit == corners.end())
        {
          cit = corners.emplace(key, static_cast<int>(corner_list.size())).first;
          corner_list.push_back(key);
        }
        cid[v] = cit->second;
      }
      for (const auto& t : sm->tris)
      {
        const int ta = cid[t[0]], tb = cid[t[1]], tc = cid[t[2]];
        if (ta == tb || tb == tc || ta == tc)
          continue;
        // studiomdl stores triangles reversed ("quake wants them in the reverse order").
        tris.push_back({ta, tc, tb});
      }
    }
    ntris += tris.size();
    Mesh mesh{static_cast<int>(skin), static_cast<int>(normals.size()) - norm_start, norm_start,
              {}};
    for (const auto& [fan, s] : TriCommands(tris))
    {
      std::vector<Corner> cs;
      for (const int c : s)
        cs.push_back(corner_list[c]);
      mesh.strips.emplace_back(fan, std::move(cs));
    }
    meshes.push_back(std::move(mesh));
  }

  // models._first_use_order: renumber vertices and normals in the order the commands use them.
  {
    std::map<int, int> pmap, nmap;
    std::vector<int> porder;
    for (const Mesh& m : meshes)
    {
      std::vector<int> local;
      std::set<int> used;
      for (const auto& [fan, s] : m.strips)
      {
        for (const Corner& c : s)
        {
          if (!pmap.contains(c.vi))
          {
            pmap[c.vi] = static_cast<int>(porder.size());
            porder.push_back(c.vi);
          }
          if (used.insert(c.ni).second)
            local.push_back(c.ni);
        }
      }
      for (int k = m.normstart; k < m.normstart + m.numnorms; k++)
      {
        if (!used.contains(k))
          local.push_back(k);
      }
      for (size_t k = 0; k < local.size(); k++)
        nmap[local[k]] = m.normstart + static_cast<int>(k);
    }
    for (int vi = 0; vi < static_cast<int>(positions.size()); vi++)
    {
      if (!pmap.contains(vi))
      {
        pmap[vi] = static_cast<int>(porder.size());
        porder.push_back(vi);
      }
    }
    std::vector<int> inv_n(normals.size());
    {
      std::vector<std::pair<int, int>> keyed;
      for (int k = 0; k < static_cast<int>(normals.size()); k++)
        keyed.emplace_back(nmap.contains(k) ? nmap[k] : k, k);
      std::stable_sort(keyed.begin(), keyed.end(),
                       [](const auto& a, const auto& b) { return a.first < b.first; });
      for (size_t i = 0; i < keyed.size(); i++)
        inv_n[i] = keyed[i].second;
    }
    std::vector<Vec3> new_positions, new_normals;
    std::vector<int> new_vert_bone, new_norm_bone;
    for (const int k : porder)
    {
      new_positions.push_back(positions[k]);
      new_vert_bone.push_back(vert_bone[k]);
    }
    for (const int k : inv_n)
    {
      new_normals.push_back(normals[k]);
      new_norm_bone.push_back(norm_bone[k]);
    }
    for (Mesh& m : meshes)
    {
      for (auto& [fan, s] : m.strips)
      {
        for (Corner& c : s)
        {
          c.vi = pmap[c.vi];
          if (nmap.contains(c.ni))
            c.ni = nmap[c.ni];
        }
      }
    }
    positions = std::move(new_positions);
    vert_bone = std::move(new_vert_bone);
    normals = std::move(new_normals);
    norm_bone = std::move(new_norm_bone);
  }

  if (positions.size() > MAXSTUDIOVERTS)
  {
    model.warnings.push_back(std::to_string(positions.size()) +
                             " vertices exceeds the studio limit of " +
                             std::to_string(MAXSTUDIOVERTS) + "; decimate the source");
  }
  if (normals.size() > MAXSTUDIOVERTS)
  {
    model.warnings.push_back(std::to_string(normals.size()) +
                             " normals exceeds the studio limit of " +
                             std::to_string(MAXSTUDIOVERTS));
  }
  if (ntris > MAXSTUDIOTRIANGLES)
  {
    model.warnings.push_back(std::to_string(ntris) + " triangles exceeds the studio limit of " +
                             std::to_string(MAXSTUDIOTRIANGLES));
  }

  const size_t nb = model.bones.size();
  std::vector<int> parents;
  std::vector<Pose> rest;
  for (const StudioBone& b : model.bones)
  {
    parents.push_back(b.parent);
    rest.push_back(b.pose);
  }
  std::vector<StudioSequence> fallback;
  if (model.sequences.empty())
    fallback.push_back({"idle", 30.0, {rest}, true});
  std::vector<StudioSequence>& seqs = model.sequences.empty() ? fallback : model.sequences;

  const auto posed = [&](const std::vector<Pose>& frame) {
    const std::vector<Mat4> mats = WorldMatrices(frame, parents);
    std::vector<Vec3> out(positions.size());
    for (size_t i = 0; i < positions.size(); i++)
    {
      const Mat4& m = mats[vert_bone[i]];
      for (int r = 0; r < 3; r++)
      {
        out[i][r] = m[r][0] * positions[i][0] + m[r][1] * positions[i][1] +
                    m[r][2] * positions[i][2] + m[r][3];
      }
    }
    return out;
  };
  const auto box = [&](const std::vector<std::vector<Pose>>& frames) {
    Vec3 lo{}, hi{};
    if (positions.empty())
      return std::pair{lo, hi};
    const size_t step = std::max<size_t>(1, frames.size() / 16);
    bool first = true;
    const auto add = [&](const std::vector<Vec3>& pts) {
      for (const Vec3& p : pts)
      {
        for (int k = 0; k < 3; k++)
        {
          lo[k] = first ? p[k] : std::min(lo[k], p[k]);
          hi[k] = first ? p[k] : std::max(hi[k], p[k]);
        }
        first = false;
      }
    };
    for (size_t f = 0; f < frames.size(); f += step)
      add(posed(frames[f]));
    add(posed(frames.back()));
    return std::pair{lo, hi};
  };

  auto [bmin, bmax] = box({rest});
  const std::vector<Vec3> rest_world = posed(rest);
  double radius = 0.0;
  for (const Vec3& p : rest_world)
    radius = std::max(radius, std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]));
  std::vector<const std::vector<std::vector<Pose>>*> seq_frames;
  for (const StudioSequence& sq : seqs)
    seq_frames.push_back(&sq.frames);
  const std::vector<Pose> scales = BoneScales(rest, seq_frames);

  Bytes out(HDR, 0);
  const std::string name = (Lower(ToLatin1(model.name)) + ".mdl").substr(0, 63);

  const size_t bone_off = out.size();
  for (size_t b = 0; b < nb; b++)
  {
    PutString(&out, ToLatin1(model.bones[b].name).substr(0, 31), 32);
    PutI32(&out, model.bones[b].parent);
    PutI32(&out, 0);
    for (int k = 0; k < 6; k++)
      PutI32(&out, -1);
    for (int k = 0; k < 6; k++)
      PutF32(&out, rest[b][k]);
    for (int k = 0; k < 6; k++)
      PutF32(&out, scales[b][k]);
  }

  // One hitbox per bone that owns vertices.
  const size_t hit_off = out.size();
  int nhit = 0;
  for (size_t b = 0; b < nb; b++)
  {
    Vec3 lo{}, hi{};
    bool any = false;
    for (size_t i = 0; i < positions.size(); i++)
    {
      if (vert_bone[i] != static_cast<int>(b))
        continue;
      for (int k = 0; k < 3; k++)
      {
        lo[k] = any ? std::min(lo[k], positions[i][k]) : positions[i][k];
        hi[k] = any ? std::max(hi[k], positions[i][k]) : positions[i][k];
      }
      any = true;
    }
    if (!any)
      continue;
    PutI32(&out, static_cast<int32_t>(b));
    PutI32(&out, 0);
    for (int k = 0; k < 3; k++)
      PutF32(&out, lo[k]);
    for (int k = 0; k < 3; k++)
      PutF32(&out, hi[k]);
    nhit++;
  }

  std::vector<size_t> anim_offs;
  for (StudioSequence& sq : seqs)
  {
    Align(&out);
    anim_offs.push_back(out.size());
    while (true)
    {
      if (const auto block = EncodeAnimBlock(sq.frames, rest, scales))
      {
        out.insert(out.end(), block->begin(), block->end());
        break;
      }
      if (sq.frames.size() <= 2)
      {
        *error = "animation data too large for 16-bit offsets; lower the animation frame rate";
        return std::nullopt;
      }
      // Too long for the 16-bit stream offsets: halve this sequence's frame rate.
      std::vector<std::vector<Pose>> halved;
      for (size_t f = 0; f < sq.frames.size(); f += 2)
        halved.push_back(sq.frames[f]);
      sq.frames = std::move(halved);
      sq.fps = sq.fps / 2.0;
      char fps[32];
      std::snprintf(fps, sizeof(fps), "%g", sq.fps);
      model.warnings.push_back("sequence " + sq.name + ": halved to " + fps +
                               " fps to fit studio offsets");
    }
  }
  Align(&out);
  const size_t seq_off = out.size();
  for (size_t i = 0; i < seqs.size(); i++)
  {
    const StudioSequence& sq = seqs[i];
    const auto [smin, smax] = box(sq.frames);
    for (int k = 0; k < 3; k++)
    {
      bmin[k] = std::min(bmin[k], smin[k]);
      bmax[k] = std::max(bmax[k], smax[k]);
    }
    const size_t p = out.size();
    out.resize(p + SEQ, 0);
    SetString(&out, p, ToLatin1(sq.name).substr(0, 31), 32);
    SetF32(&out, p + 32, sq.fps);
    SetI32(&out, p + 36, sq.loop ? STUDIO_LOOPING : 0);
    SetI32(&out, p + 56, static_cast<int32_t>(sq.frames.size()));
    for (int k = 0; k < 3; k++)
    {
      SetF32(&out, p + 96 + k * 4, smin[k]);
      SetF32(&out, p + 108 + k * 4, smax[k]);
    }
    SetI32(&out, p + 120, 1);
    SetI32(&out, p + 124, static_cast<int32_t>(anim_offs[i]));
    SetF32(&out, p + 144, 0.0);
    SetF32(&out, p + 148, 1.0);
  }
  const size_t group_off = out.size();
  PutString(&out, "default", 32);
  PutString(&out, "", 64);
  PutI32(&out, 0);
  PutI32(&out, 0);

  // body: one bodypart, one model
  const size_t bodypart_off = out.size();
  out.resize(out.size() + BODYPART, 0);
  const size_t model_off = out.size();
  out.resize(out.size() + MODEL, 0);
  const size_t vertinfo_off = out.size();
  for (const int b : vert_bone)
    out.push_back(static_cast<uint8_t>(b));
  Align(&out);
  const size_t norminfo_off = out.size();
  for (const int b : norm_bone)
    out.push_back(static_cast<uint8_t>(b));
  Align(&out);
  const size_t verts_off = out.size();
  for (const Vec3& p : positions)
  {
    for (const double v : p)
      PutF32(&out, v);
  }
  const size_t norms_off = out.size();
  for (const Vec3& n : normals)
  {
    for (const double v : n)
      PutF32(&out, v);
  }
  const size_t mesh_off = out.size();
  out.resize(out.size() + MESH * meshes.size(), 0);
  for (size_t i = 0; i < meshes.size(); i++)
  {
    const Mesh& m = meshes[i];
    const size_t tri_off = out.size();
    int count = 0;
    for (const auto& [fan, s] : m.strips)
    {
      PutI16(&out, fan ? -static_cast<int>(s.size()) : static_cast<int>(s.size()));
      for (const Corner& c : s)
      {
        PutI16(&out, c.vi);
        PutI16(&out, c.ni);
        PutI16(&out, c.s);
        PutI16(&out, c.t);
      }
      count += static_cast<int>(s.size()) - 2;
    }
    PutI16(&out, 0);
    Align(&out);
    const size_t p = mesh_off + i * MESH;
    SetI32(&out, p, count);
    SetI32(&out, p + 4, static_cast<int32_t>(tri_off));
    SetI32(&out, p + 8, m.skin);
    SetI32(&out, p + 12, m.numnorms);
    SetI32(&out, p + 16, m.normstart);
  }
  SetString(&out, bodypart_off, "body", 64);
  SetI32(&out, bodypart_off + 64, 1);
  SetI32(&out, bodypart_off + 68, 0);
  SetI32(&out, bodypart_off + 72, static_cast<int32_t>(model_off));
  SetString(&out, model_off, ToLatin1(model.name).substr(0, 63), 64);
  SetI32(&out, model_off + 64, 0);
  SetF32(&out, model_off + 68, radius);
  const int32_t model_fields[] = {static_cast<int32_t>(meshes.size()),
                                  static_cast<int32_t>(mesh_off),
                                  static_cast<int32_t>(positions.size()),
                                  static_cast<int32_t>(vertinfo_off),
                                  static_cast<int32_t>(verts_off),
                                  static_cast<int32_t>(normals.size()),
                                  static_cast<int32_t>(norminfo_off),
                                  static_cast<int32_t>(norms_off),
                                  0,
                                  0};
  for (size_t k = 0; k < 10; k++)
    SetI32(&out, model_off + 72 + k * 4, model_fields[k]);

  // Textures last, so the lean mesh can cut them off.
  const size_t tex_off = out.size();
  out.resize(out.size() + TEXTURE * model.materials.size(), 0);
  const size_t skin_off = out.size();
  for (size_t i = 0; i < model.materials.size(); i++)
    PutI16(&out, static_cast<int32_t>(i));
  Align(&out);
  const size_t texdata_off = out.size();
  for (size_t i = 0; i < model.materials.size(); i++)
  {
    const StudioMaterial& mat = model.materials[i];
    const auto quantized = QuantizeSkin(mat.rgba, mat.masked, alpha_threshold);
    if (!quantized)
    {
      *error = "could not quantize skin " + mat.name;
      return std::nullopt;
    }
    const auto& [pixels, pal] = *quantized;
    const size_t p = tex_off + i * TEXTURE;
    SetString(&out, p, TextureName(mat.name, i).substr(0, 63), 64);
    SetI32(&out, p + 64, mat.masked ? STUDIO_NF_MASKED : 0);
    SetI32(&out, p + 68, mat.rgba.width);
    SetI32(&out, p + 72, mat.rgba.height);
    SetI32(&out, p + 76, static_cast<int32_t>(out.size()));
    out.insert(out.end(), pixels.begin(), pixels.end());
    out.insert(out.end(), pal.begin(), pal.end());
  }
  Align(&out);

  std::memcpy(out.data(), "IDST", 4);
  SetI32(&out, 4, STUDIO_VERSION);
  SetString(&out, 8, name, 64);
  SetI32(&out, 72, static_cast<int32_t>(out.size()));
  for (int k = 0; k < 3; k++)
  {
    SetF32(&out, 76 + k * 4, 0.0);
    SetF32(&out, 88 + k * 4, bmin[k]);
    SetF32(&out, 100 + k * 4, bmax[k]);
    SetF32(&out, 112 + k * 4, bmin[k]);
    SetF32(&out, 124 + k * 4, bmax[k]);
  }
  SetI32(&out, 136, 0);
  const int32_t counts[] = {static_cast<int32_t>(nb),
                            static_cast<int32_t>(bone_off),
                            0,
                            0,
                            nhit,
                            static_cast<int32_t>(hit_off),
                            static_cast<int32_t>(seqs.size()),
                            static_cast<int32_t>(seq_off),
                            1,
                            static_cast<int32_t>(group_off)};
  for (size_t k = 0; k < 10; k++)
    SetI32(&out, 140 + k * 4, counts[k]);
  const int32_t textures[] = {static_cast<int32_t>(model.materials.size()),
                              static_cast<int32_t>(tex_off),
                              static_cast<int32_t>(texdata_off),
                              static_cast<int32_t>(model.materials.size()),
                              1,
                              static_cast<int32_t>(skin_off)};
  for (size_t k = 0; k < 6; k++)
    SetI32(&out, 180 + k * 4, textures[k]);
  SetI32(&out, 204, 1);
  SetI32(&out, 208, static_cast<int32_t>(bodypart_off));
  return out;
}
}  // namespace GxBake
