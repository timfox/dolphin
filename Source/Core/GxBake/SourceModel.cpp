// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

// core/source_mdl.py: Source studio models (.mdl v44-49 + .vvd + .dx90.vtx) to an ImportedModel.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <tuple>

#include "GxBake/Source.h"
#include "GxBake/Vtf.h"

namespace GxBake
{
namespace fs = std::filesystem;

namespace
{
using Bytes = std::vector<uint8_t>;
using Vec3 = std::array<double, 3>;
using Quat = std::array<double, 4>;

constexpr int MAXSTUDIOBONES = 128;
constexpr int STUDIO_ANIM_RAWPOS = 0x01;
constexpr int STUDIO_ANIM_RAWROT = 0x02;
constexpr int STUDIO_ANIM_ANIMPOS = 0x04;
constexpr int STUDIO_ANIM_ANIMROT = 0x08;
constexpr int STUDIO_ANIM_DELTA = 0x10;
constexpr int STUDIO_ANIM_RAWROT2 = 0x20;
constexpr int STUDIO_LOOPING = 0x0001;
constexpr int STUDIO_DELTA = 0x0004;

constexpr size_t BONE = 216;
constexpr size_t TEXTURE = 64;
constexpr size_t BODYPART = 16;
constexpr size_t MODEL = 148;
constexpr size_t MESH = 116;
constexpr size_t ANIMDESC = 100;
constexpr size_t SEQDESC = 212;
constexpr size_t VVD_VERTEX = 48;

// Bounds-checked little-endian reads; a read past the end fails the import like the Python
// tool's struct.error.
struct Reader
{
  const Bytes& d;
  bool ok = true;

  bool Has(int64_t p, int64_t n)
  {
    if (p < 0 || n < 0 || p + n > static_cast<int64_t>(d.size()))
      ok = false;
    return ok;
  }
  int32_t I32(int64_t p)
  {
    if (!Has(p, 4))
      return 0;
    uint32_t v;
    std::memcpy(&v, d.data() + p, 4);
    return static_cast<int32_t>(v);
  }
  int16_t I16(int64_t p)
  {
    if (!Has(p, 2))
      return 0;
    return static_cast<int16_t>(d[p] | (d[p + 1] << 8));
  }
  uint16_t U16(int64_t p) { return static_cast<uint16_t>(I16(p)); }
  uint8_t U8(int64_t p) { return Has(p, 1) ? d[p] : 0; }
  float F32(int64_t p)
  {
    const uint32_t bits = static_cast<uint32_t>(I32(p));
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  }
  std::string CStr(int64_t p)
  {
    if (p < 0 || p > static_cast<int64_t>(d.size()))
    {
      ok = false;
      return {};
    }
    const auto begin = d.begin() + p;
    const auto end = std::find(begin, d.end(), 0);
    return std::string(begin, end);
  }
};

std::optional<Bytes> ReadAll(const fs::path& path)
{
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return std::nullopt;
  return Bytes(std::istreambuf_iterator<char>(f), {});
}

std::string Utf8(const fs::path& p)
{
  const auto u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
}

std::string LowerAsciiLatin1(std::string s)
{
  for (char& ch : s)
  {
    const uint8_t c = static_cast<uint8_t>(ch);
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
      ch = static_cast<char>(c + 32);
  }
  return s;
}

std::string Latin1ToUtf8(const std::string& s)
{
  std::string out;
  for (const char ch : s)
  {
    const uint8_t c = static_cast<uint8_t>(ch);
    if (c < 0x80)
    {
      out += ch;
    }
    else
    {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return out;
}

// --- maths (skeleton.py and source_mdl.py) -------------------------------------------------------

// np.dot / np.linalg.norm of a 1-D vector go through BLAS ddot, an FMA chain.
double Dot4(const Quat& a, const Quat& b)
{
  double f = a[0] * b[0];
  for (int k = 1; k < 4; k++)
    f = std::fma(a[k], b[k], f);
  return f;
}

double Norm4(const Quat& q)
{
  return std::sqrt(Dot4(q, q));
}

// np.einsum("nij,nj->ni") over three terms sums the first and last products first.
double EinsumDot3(double a0, double a1, double a2, double b0, double b1, double b2)
{
  return (a0 * b0 + a2 * b2) + a1 * b1;
}

// skeleton.quat_matrix: (x, y, z, w), normalised.
std::array<std::array<double, 3>, 3> QuatMatrix(const Quat& in)
{
  double n = Norm4(in);
  if (n == 0.0)
    n = 1.0;
  const double x = in[0] / n, y = in[1] / n, z = in[2] / n, w = in[3] / n;
  return {{{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
           {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
           {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}}};
}

Mat4 Identity()
{
  Mat4 m{};
  for (int i = 0; i < 4; i++)
    m[i][i] = 1.0;
  return m;
}

// source_mdl._local_matrix
Mat4 LocalMatrix(const Vec3& pos, const Quat& quat, double scale)
{
  Mat4 m = Identity();
  const auto r = QuatMatrix(quat);
  for (int i = 0; i < 3; i++)
  {
    for (int j = 0; j < 3; j++)
      m[i][j] = r[i][j];
    m[i][3] = pos[i] * scale;
  }
  return m;
}

// skeleton.rigid: the nearest rotation (polar factor) plus the translation.
Mat4 Rigid(const Mat4& m)
{
  std::array<std::array<double, 3>, 3> x;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      x[i][j] = m[i][j];
  for (int iter = 0; iter < 32; iter++)
  {
    // x <- (x + x^-T) / 2
    const double det = x[0][0] * (x[1][1] * x[2][2] - x[1][2] * x[2][1]) -
                       x[0][1] * (x[1][0] * x[2][2] - x[1][2] * x[2][0]) +
                       x[0][2] * (x[1][0] * x[2][1] - x[1][1] * x[2][0]);
    if (det == 0.0)
      break;
    std::array<std::array<double, 3>, 3> cof;
    cof[0][0] = x[1][1] * x[2][2] - x[1][2] * x[2][1];
    cof[0][1] = x[1][2] * x[2][0] - x[1][0] * x[2][2];
    cof[0][2] = x[1][0] * x[2][1] - x[1][1] * x[2][0];
    cof[1][0] = x[0][2] * x[2][1] - x[0][1] * x[2][2];
    cof[1][1] = x[0][0] * x[2][2] - x[0][2] * x[2][0];
    cof[1][2] = x[0][1] * x[2][0] - x[0][0] * x[2][1];
    cof[2][0] = x[0][1] * x[1][2] - x[0][2] * x[1][1];
    cof[2][1] = x[0][2] * x[1][0] - x[0][0] * x[1][2];
    cof[2][2] = x[0][0] * x[1][1] - x[0][1] * x[1][0];
    double change = 0.0;
    for (int i = 0; i < 3; i++)
    {
      for (int j = 0; j < 3; j++)
      {
        const double next = 0.5 * (x[i][j] + cof[i][j] / det);  // inverse transpose
        change = std::max(change, std::abs(next - x[i][j]));
        x[i][j] = next;
      }
    }
    if (change < 1e-15)
      break;
  }
  Mat4 out = Identity();
  for (int i = 0; i < 3; i++)
  {
    for (int j = 0; j < 3; j++)
      out[i][j] = x[i][j];
    out[i][3] = m[i][3];
  }
  return out;
}

Mat4 RigidInverse(const Mat4& m)
{
  Mat4 out = Identity();
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      out[i][j] = m[j][i];
  for (int i = 0; i < 3; i++)
    out[i][3] = std::fma(-out[i][2], m[2][3],
                         std::fma(-out[i][1], m[1][3], -out[i][0] * m[0][3]));  // BLAS gemv
  return out;
}

// source_mdl._mat_quat: (w, x, y, z)
Quat MatQuat(const Mat4& m)
{
  const auto& r = m;
  const double tr = r[0][0] + r[1][1] + r[2][2];
  Quat q;
  if (tr > 0)
  {
    const double s = std::sqrt(tr + 1.0) * 2;
    q = {0.25 * s, (r[2][1] - r[1][2]) / s, (r[0][2] - r[2][0]) / s, (r[1][0] - r[0][1]) / s};
  }
  else if (r[0][0] > r[1][1] && r[0][0] > r[2][2])
  {
    const double s = std::sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2;
    q = {(r[2][1] - r[1][2]) / s, 0.25 * s, (r[0][1] + r[1][0]) / s, (r[0][2] + r[2][0]) / s};
  }
  else if (r[1][1] > r[2][2])
  {
    const double s = std::sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2;
    q = {(r[0][2] - r[2][0]) / s, (r[0][1] + r[1][0]) / s, 0.25 * s, (r[1][2] + r[2][1]) / s};
  }
  else
  {
    const double s = std::sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2;
    q = {(r[1][0] - r[0][1]) / s, (r[0][2] + r[2][0]) / s, (r[1][2] + r[2][1]) / s, 0.25 * s};
  }
  const double n = Norm4(q);
  for (double& v : q)
    v /= n;
  return q;
}

Quat QMul(const Quat& a, const Quat& b)
{
  const double aw = a[0], ax = a[1], ay = a[2], az = a[3];
  const double bw = b[0], bx = b[1], by = b[2], bz = b[3];
  return {aw * bw - ax * bx - ay * by - az * bz, aw * bx + ax * bw + ay * bz - az * by,
          aw * by - ax * bz + ay * bw + az * bx, aw * bz + ax * by - ay * bx + az * bw};
}

// source_mdl.dq_blend: dual-quaternion blend of two rigid transforms (weight wa on ma).
Mat4 DqBlend(const Mat4& ma, const Mat4& mb, double wa)
{
  const Quat ra = MatQuat(Rigid(ma));
  Quat rb = MatQuat(Rigid(mb));
  if (Dot4(ra, rb) < 0)
    for (double& v : rb)
      v = -v;
  Quat da = QMul({0.0, ma[0][3], ma[1][3], ma[2][3]}, ra);
  Quat db = QMul({0.0, mb[0][3], mb[1][3], mb[2][3]}, rb);
  for (double& v : da)
    v = 0.5 * v;
  for (double& v : db)
    v = 0.5 * v;
  Quat r, dual;
  for (int k = 0; k < 4; k++)
  {
    r[k] = wa * ra[k] + (1 - wa) * rb[k];
    dual[k] = wa * da[k] + (1 - wa) * db[k];
  }
  const double n = Norm4(r);
  for (int k = 0; k < 4; k++)
  {
    r[k] = r[k] / n;
    dual[k] = dual[k] / n;
  }
  const Quat conj = {r[0] * 1, r[1] * -1, r[2] * -1, r[3] * -1};
  const Quat t = QMul(dual, conj);
  Mat4 out = Identity();
  const auto rot = QuatMatrix({r[1], r[2], r[3], r[0]});
  for (int i = 0; i < 3; i++)
  {
    for (int j = 0; j < 3; j++)
      out[i][j] = rot[i][j];
    out[i][3] = 2 * t[i + 1];
  }
  return out;
}

// source_mdl.euler_quat: Source AngleQuaternion, (x, y, z, w).
Quat EulerQuat(const Vec3& a)
{
  const double sr = std::sin(a[0] * 0.5), cr = std::cos(a[0] * 0.5);
  const double sp = std::sin(a[1] * 0.5), cp = std::cos(a[1] * 0.5);
  const double sy = std::sin(a[2] * 0.5), cy = std::cos(a[2] * 0.5);
  return {sr * cp * cy - cr * sp * sy, cr * sp * cy + sr * cp * sy, cr * cp * sy - sr * sp * cy,
          cr * cp * cy + sr * sp * sy};
}

double HalfToDouble(uint16_t h)
{
  const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, mant = h & 1023;
  double v;
  if (exp == 0)
    v = std::ldexp(static_cast<double>(mant), -24);
  else if (exp == 31)
    v = mant ? std::nan("") : HUGE_VAL;
  else
    v = std::ldexp(static_cast<double>(mant | 1024), static_cast<int>(exp) - 25);
  return sign ? -v : v;
}

// --- .mdl ------------------------------------------------------------------------------------

struct Header
{
  int version = 0;
  int32_t numbones, boneindex, numanim, animindex, numseq, seqindex, numtextures, textureindex,
      numcdtextures, cdtextureindex, numskinref, numskinfamilies, skinindex, numbodyparts,
      bodypartindex;
};

Header ReadHeader(Reader& r)
{
  Header h;
  h.version = r.I32(4);
  const auto at = [&](int i) { return r.I32(156 + i * 4); };
  h.numbones = at(0);
  h.boneindex = at(1);
  h.numanim = at(6);
  h.animindex = at(7);
  h.numseq = at(8);
  h.seqindex = at(9);
  h.numtextures = at(12);
  h.textureindex = at(13);
  h.numcdtextures = at(14);
  h.cdtextureindex = at(15);
  h.numskinref = at(16);
  h.numskinfamilies = at(17);
  h.skinindex = at(18);
  h.numbodyparts = at(19);
  h.bodypartindex = at(20);
  return h;
}

struct SourceBone
{
  std::string name;
  int parent;
  Vec3 pos;
  Quat quat;  // x, y, z, w
  Vec3 rot, posscale, rotscale;
  Mat4 pose_to_bone;
};

std::vector<SourceBone> ReadBones(Reader& r, const Header& h)
{
  std::vector<SourceBone> bones;
  for (int32_t i = 0; i < h.numbones && r.ok; i++)
  {
    const int64_t off = h.boneindex + static_cast<int64_t>(i) * BONE;
    SourceBone b;
    b.name = r.CStr(off + r.I32(off));
    b.parent = r.I32(off + 4);
    for (int k = 0; k < 3; k++)
    {
      b.pos[k] = r.F32(off + 32 + k * 4);
      b.rot[k] = r.F32(off + 60 + k * 4);
      b.posscale[k] = r.F32(off + 72 + k * 4);
      b.rotscale[k] = r.F32(off + 84 + k * 4);
    }
    for (int k = 0; k < 4; k++)
      b.quat[k] = r.F32(off + 44 + k * 4);
    b.pose_to_bone = Identity();
    for (int i2 = 0; i2 < 3; i2++)
      for (int j = 0; j < 4; j++)
        b.pose_to_bone[i2][j] = r.F32(off + 96 + (i2 * 4 + j) * 4);
    bones.push_back(std::move(b));
  }
  return bones;
}

// --- .vvd ------------------------------------------------------------------------------------

struct Vvd
{
  std::vector<std::array<float, 3>> weights;
  std::vector<std::array<int, 3>> bones;
  std::vector<int> nbones;
  std::vector<Vec3> pos, normal;
  std::vector<std::array<float, 2>> uv;
};

std::optional<Vvd> ReadVvd(const Bytes& data, std::string* error)
{
  Reader r{data};
  if (data.size() < 64 || std::memcmp(data.data(), "IDSV", 4) != 0)
  {
    *error = "not a VVD file";
    return std::nullopt;
  }
  const int32_t n_all = r.I32(16);
  const int32_t numfixups = r.I32(48), fixup_start = r.I32(52), vert_start = r.I32(56);
  if (n_all < 0 || !r.Has(vert_start, static_cast<int64_t>(n_all) * VVD_VERTEX))
  {
    *error = "VVD is truncated";
    return std::nullopt;
  }
  std::vector<int64_t> order;
  if (numfixups)
  {
    for (int32_t i = 0; i < numfixups; i++)
    {
      const int64_t p = fixup_start + static_cast<int64_t>(i) * 12;
      const int32_t lod = r.I32(p), src = r.I32(p + 4), num = r.I32(p + 8);
      if (lod >= 0)
        for (int32_t k = src; k < src + num; k++)
          order.push_back(k);
    }
  }
  else
  {
    for (int32_t k = 0; k < n_all; k++)
      order.push_back(k);
  }
  Vvd v;
  for (const int64_t k : order)
  {
    if (k < 0 || k >= n_all || !r.ok)
    {
      *error = "VVD fixups point outside the vertices";
      return std::nullopt;
    }
    const int64_t p = vert_start + k * static_cast<int64_t>(VVD_VERTEX);
    v.weights.push_back({r.F32(p), r.F32(p + 4), r.F32(p + 8)});
    v.bones.push_back({data[p + 12], data[p + 13], data[p + 14]});
    v.nbones.push_back(data[p + 15]);
    v.pos.push_back({r.F32(p + 16), r.F32(p + 20), r.F32(p + 24)});
    v.normal.push_back({r.F32(p + 28), r.F32(p + 32), r.F32(p + 36)});
    v.uv.push_back({r.F32(p + 40), r.F32(p + 44)});
  }
  return v;
}

// --- .vtx ------------------------------------------------------------------------------------

// {(bodypart, model): [per mesh: list of triangle lists (vertex ids within the mesh)]}
using VtxLod = std::map<std::pair<int, int>, std::vector<std::vector<std::vector<std::array<int64_t, 3>>>>>;

std::optional<VtxLod> ReadVtxLod0(const Bytes& data, int mdl_version, std::string* error)
{
  Reader r{data};
  const int32_t version = r.I32(0);
  if (!r.ok || version != 7)
  {
    *error = "unsupported VTX version " + std::to_string(version);
    return std::nullopt;
  }
  const int32_t nbp = r.I32(28), bp_off = r.I32(32);
  const int64_t sgs = mdl_version >= 49 ? 33 : 25;
  VtxLod out;
  for (int32_t bp = 0; bp < nbp && r.ok; bp++)
  {
    const int64_t bpo = bp_off + static_cast<int64_t>(bp) * 8;
    const int32_t nmodels = r.I32(bpo), mo = r.I32(bpo + 4);
    for (int32_t m = 0; m < nmodels && r.ok; m++)
    {
      const int64_t mod = bpo + mo + static_cast<int64_t>(m) * 8;
      const int32_t nlods = r.I32(mod), lo = r.I32(mod + 4);
      if (nlods < 1)
        continue;
      const int64_t lod = mod + lo;
      const int32_t nmeshes = r.I32(lod), meo = r.I32(lod + 4);
      std::vector<std::vector<std::vector<std::array<int64_t, 3>>>> meshes;
      for (int32_t me = 0; me < nmeshes && r.ok; me++)
      {
        const int64_t mesh = lod + meo + static_cast<int64_t>(me) * 9;
        const int32_t nsg = r.I32(mesh), sgo = r.I32(mesh + 4);
        std::vector<std::vector<std::array<int64_t, 3>>> tris;
        for (int32_t g = 0; g < nsg && r.ok; g++)
        {
          const int64_t sg = mesh + sgo + g * sgs;
          const int32_t nverts = r.I32(sg), vo = r.I32(sg + 4), nidx = r.I32(sg + 8);
          const int32_t io = r.I32(sg + 12), nstrips = r.I32(sg + 16), so = r.I32(sg + 20);
          std::vector<int64_t> orig;
          for (int32_t v = 0; v < nverts; v++)
            orig.push_back(r.U16(sg + vo + static_cast<int64_t>(v) * 9 + 4));
          std::vector<int64_t> idx;
          for (int32_t k = 0; k < nidx; k++)
            idx.push_back(r.U16(sg + io + static_cast<int64_t>(k) * 2));
          for (int32_t s = 0; s < nstrips && r.ok; s++)
          {
            const int64_t st = sg + so + static_cast<int64_t>(s) * 27;
            const int32_t s_nidx = r.I32(st), s_io = r.I32(st + 4);
            const uint8_t s_flags = r.U8(st + 18);
            // Python slicing clamps to the index list.
            const int64_t a = std::clamp<int64_t>(s_io, 0, idx.size());
            const int64_t b = std::clamp<int64_t>(static_cast<int64_t>(s_io) + s_nidx, a, idx.size());
            std::vector<int64_t> sel(idx.begin() + a, idx.begin() + b);
            if (s_flags & 2)  // triangle strip
            {
              std::vector<int64_t> t;
              for (size_t k = 0; k + 2 < sel.size(); k++)
              {
                if (k % 2 == 0)
                  t.insert(t.end(), {sel[k], sel[k + 1], sel[k + 2]});
                else
                  t.insert(t.end(), {sel[k + 1], sel[k], sel[k + 2]});
              }
              sel = std::move(t);
            }
            if (sel.size() >= 3)
            {
              std::vector<std::array<int64_t, 3>> group;
              for (size_t k = 0; k + 3 <= sel.size() / 3 * 3; k += 3)
              {
                std::array<int64_t, 3> tri;
                for (int c = 0; c < 3; c++)
                {
                  const int64_t i = sel[k + c];
                  if (i < 0 || i >= static_cast<int64_t>(orig.size()))
                  {
                    *error = "VTX index out of range";
                    return std::nullopt;
                  }
                  tri[c] = orig[i];
                }
                group.push_back(tri);
              }
              tris.push_back(std::move(group));
            }
          }
        }
        meshes.push_back(std::move(tris));
      }
      out[{bp, m}] = std::move(meshes);
    }
  }
  if (!r.ok)
  {
    *error = "VTX is truncated";
    return std::nullopt;
  }
  return out;
}

// --- animation -------------------------------------------------------------------------------

Quat Quat48(Reader& r, int64_t off)
{
  const uint16_t x = r.U16(off), y = r.U16(off + 2), zw = r.U16(off + 4);
  Quat q = {(x - 32768) / 32768.0, (y - 32768) / 32768.0, ((zw & 0x7FFF) - 16384) / 16384.0, 0.0};
  q[3] = std::sqrt(std::max(0.0, 1.0 - q[0] * q[0] - q[1] * q[1] - q[2] * q[2])) *
         ((zw >> 15) ? -1 : 1);
  return q;
}

Quat Quat64(Reader& r, int64_t off)
{
  uint64_t v = 0;
  if (r.Has(off, 8))
    std::memcpy(&v, r.d.data() + off, 8);
  const int64_t x = v & 0x1FFFFF, y = (v >> 21) & 0x1FFFFF, z = (v >> 42) & 0x1FFFFF;
  Quat q = {(x - 1048576) / 1048576.5, (y - 1048576) / 1048576.5, (z - 1048576) / 1048576.5, 0.0};
  q[3] = std::sqrt(std::max(0.0, 1.0 - q[0] * q[0] - q[1] * q[1] - q[2] * q[2])) *
         ((v >> 63) ? -1 : 1);
  return q;
}

// source_mdl._anim_value: ExtractAnimValue over the RLE stream.
double AnimValue(Reader& r, int64_t off, int frame)
{
  int k = frame;
  for (int n = 0; n < 4096 && r.ok; n++)
  {
    const int valid = r.U8(off), total = r.U8(off + 1);
    if (total == 0)
      return 0.0;
    if (total > k)
    {
      const int i = valid > k ? k : valid - 1;
      return r.I16(off + 2 + 2 * i);
    }
    k -= total;
    off += 2 * (valid + 1);
  }
  return 0.0;
}

std::vector<std::pair<Vec3, Quat>> DecodeFrame(Reader& r, int64_t anim_off,
                                               const std::vector<SourceBone>& bones, int frame)
{
  std::vector<std::pair<Vec3, Quat>> pose;
  for (const SourceBone& b : bones)
    pose.emplace_back(b.pos, b.quat);
  int64_t off = anim_off;
  for (size_t n = 0; n < bones.size() + 1; n++)
  {
    if (off + 4 > static_cast<int64_t>(r.d.size()))
      break;
    const int bone = r.U8(off), flags = r.U8(off + 1);
    const int16_t next = r.I16(off + 2);
    if (bone >= static_cast<int>(bones.size()))
      break;
    const SourceBone& b = bones[bone];
    const int64_t data = off + 4;
    Vec3 pos = b.pos;
    Quat quat = b.quat;
    if (flags & STUDIO_ANIM_RAWROT)
    {
      quat = Quat48(r, data);
    }
    else if (flags & STUDIO_ANIM_RAWROT2)
    {
      quat = Quat64(r, data);
    }
    else if (flags & STUDIO_ANIM_ANIMROT)
    {
      Vec3 ang{};
      for (int j = 0; j < 3; j++)
      {
        const int16_t o = r.I16(data + 2 * j);
        ang[j] = o > 0 ? AnimValue(r, data + o, frame) * b.rotscale[j] : 0.0;
      }
      if (!(flags & STUDIO_ANIM_DELTA))
        for (int j = 0; j < 3; j++)
          ang[j] += b.rot[j];
      quat = EulerQuat(ang);
    }
    const int64_t pdata = data + ((flags & STUDIO_ANIM_RAWROT) ? 6 : 0) +
                          ((flags & STUDIO_ANIM_RAWROT2) ? 8 : 0);
    if (flags & STUDIO_ANIM_RAWPOS)
    {
      for (int j = 0; j < 3; j++)
        pos[j] = HalfToDouble(r.U16(pdata + 2 * j));
    }
    else if (flags & STUDIO_ANIM_ANIMPOS)
    {
      const int64_t pv = data + ((flags & STUDIO_ANIM_ANIMROT) ? 6 : 0);
      for (int j = 0; j < 3; j++)
      {
        const int16_t o = r.I16(pv + 2 * j);
        pos[j] = o > 0 ? AnimValue(r, pv + o, frame) * b.posscale[j] : 0.0;
      }
      if (!(flags & STUDIO_ANIM_DELTA))
        for (int j = 0; j < 3; j++)
          pos[j] += b.pos[j];
    }
    pose[bone] = {pos, quat};
    if (next == 0)
      break;
    off += next;
  }
  return pose;
}

struct AnimDesc
{
  int64_t off;
  double fps;
  int32_t flags, numframes, animblock, animindex, sectionindex, sectionframes;

  // (chain offset, frame within it), or nothing when the frame lives in an external block.
  std::optional<std::pair<int64_t, int>> FrameChain(Reader& r, int frame) const
  {
    if (sectionframes)
    {
      const int sec = frame / sectionframes;
      const int64_t p = off + sectionindex + static_cast<int64_t>(sec) * 8;
      const int32_t block = r.I32(p), index = r.I32(p + 4);
      if (block)
        return std::nullopt;
      return std::pair{off + index, frame - sec * sectionframes};
    }
    if (animblock)
      return std::nullopt;
    return std::pair{off + animindex, frame};
  }
};

// Python's round() on a float: half to even.
int64_t RoundHalfEven(double x)
{
  return static_cast<int64_t>(std::nearbyint(x));
}

std::vector<StudioSequence> ReadSequences(Reader& r, const Header& h,
                                          const std::vector<SourceBone>& bones, double scale,
                                          int anim_fps, std::vector<std::string>* warnings)
{
  std::vector<AnimDesc> descs;
  for (int32_t i = 0; i < h.numanim; i++)
  {
    const int64_t off = h.animindex + static_cast<int64_t>(i) * ANIMDESC;
    descs.push_back({off, r.F32(off + 8), r.I32(off + 12), r.I32(off + 16), r.I32(off + 52),
                     r.I32(off + 56), r.I32(off + 80), r.I32(off + 84)});
  }
  std::vector<std::string> used;
  std::vector<StudioSequence> out;
  int external = 0, delta = 0;
  for (int32_t i = 0; i < h.numseq && r.ok; i++)
  {
    const int64_t off = h.seqindex + static_cast<int64_t>(i) * SEQDESC;
    const std::string label = r.CStr(off + r.I32(off + 4));
    const int32_t flags = r.I32(off + 12);
    const int anim = r.I16(off + r.I32(off + 60));
    if (anim < 0 || anim >= static_cast<int>(descs.size()))
      continue;
    const AnimDesc& desc = descs[anim];
    if ((flags & STUDIO_DELTA) || (desc.flags & STUDIO_DELTA))
    {
      delta++;
      continue;
    }
    const int n = std::max(1, desc.numframes);
    const double src_fps = desc.fps > 0 ? desc.fps : 30.0;
    const double fps = anim_fps ? std::min(src_fps, static_cast<double>(anim_fps)) : src_fps;
    const int64_t count =
        n > 1 ? std::max<int64_t>(1, RoundHalfEven((n - 1) / src_fps * fps) + 1) : 1;
    std::vector<std::vector<Pose>> frames;
    bool ok = true;
    for (int64_t k = 0; k < count; k++)
    {
      const int f = static_cast<int>(
          std::min<int64_t>(n - 1, RoundHalfEven(static_cast<double>(k) * src_fps / fps)));
      const auto chain = desc.FrameChain(r, f);
      if (!chain)
      {
        ok = false;
        break;
      }
      std::vector<Pose> frame;
      for (const auto& [p, q] : DecodeFrame(r, chain->first, bones, chain->second))
        frame.push_back(PoseFromMatrix(LocalMatrix(p, q, scale)));
      frames.push_back(std::move(frame));
    }
    if (!ok)
    {
      external++;
      continue;
    }
    std::string name = label;
    name.erase(0, name.find_first_not_of('@') == std::string::npos ? name.size() :
                                                                      name.find_first_not_of('@'));
    out.push_back({Latin1ToUtf8(UniqueName(SequenceName(name), &used)), fps, std::move(frames),
                   (flags & STUDIO_LOOPING) != 0});
  }
  if (external)
  {
    warnings->push_back(std::to_string(external) +
                        " sequence(s) keep their animation in a .ani file; skipped");
  }
  if (delta)
    warnings->push_back(std::to_string(delta) + " delta (gesture/layer) sequence(s) skipped");
  return out;
}

// --- skins -----------------------------------------------------------------------------------

std::string PosixName(const std::string& name)
{
  const size_t slash = name.rfind('/');
  std::string out = slash == std::string::npos ? name : name.substr(slash + 1);
  return out.empty() ? name : out;
}

StudioMaterial SourceSkin(const std::string& name, const std::vector<std::string>& dirs,
                          const std::optional<fs::path>& root, const SourceModelOptions& opts,
                          std::vector<std::string>* warnings)
{
  ResolvedMaterial res;
  if (root)
    res = ResolveMaterial(name, dirs, *root);
  std::optional<RgbaImage> rgba;
  if (res.vtf)
  {
    std::string error;
    const auto data = ReadAll(*res.vtf);
    if (!data)
      error = "could not read the file";
    else
      rgba = ReadVtf(*data, &error);
    if (!rgba)
      warnings->push_back("material " + name + ": " + Utf8(res.vtf->filename()) + ": " + error);
  }
  if (!rgba)
  {
    if (!root)
      warnings->push_back("material " + name + ": no materials/ folder above the model; using grey");
    else
      warnings->push_back("material " + name + ": .vmt/.vtf not found under " + Utf8(*root) +
                          "; using grey");
    rgba = RgbaImage{8, 8, {}};
    for (int i = 0; i < 64; i++)
      rgba->pixels.insert(rgba->pixels.end(), {180, 180, 180, 255});
  }
  const auto param = [&](const char* key) {
    const auto it = res.params.find(key);
    return it == res.params.end() ? std::string("0") : it->second;
  };
  const std::string alphatest = param("$alphatest"), translucent = param("$translucent");
  const bool cutout = (alphatest != "0" && !alphatest.empty()) ||
                      (translucent != "0" && !translucent.empty());
  if (!cutout)
  {
    // Source packs spec/phong masks in alpha; only cut-outs keep it.
    for (size_t i = 3; i < rgba->pixels.size(); i += 4)
      rgba->pixels[i] = 255;
  }
  RgbaImage fitted = FitSkin(*rgba, opts.skin_max_edge);
  bool masked = false;
  for (size_t i = 3; i < fitted.pixels.size() && !masked; i += 4)
    masked = fitted.pixels[i] < opts.alpha_threshold;
  return {PosixName(name), std::move(fitted), masked, res.vtf ? Utf8(*res.vtf) : ""};
}

struct Pending
{
  int material;
  std::vector<Vec3> pos, nrm;
  std::vector<std::array<float, 2>> uv;
  std::vector<std::array<int64_t, 3>> tris;
  std::vector<std::array<int64_t, 2>> top;
  std::vector<float> share;
};

// source_mdl._blend_bones: helper bones stand in for two-bone weights.
void BlendBones(const std::vector<Pending>& pending, std::vector<std::vector<int64_t>>* vbones,
                const std::vector<SourceBone>& sbones, ImportedModel* model,
                const std::vector<Mat4>& world_rest, const std::vector<Mat4>& bind_s,
                std::vector<std::string>* warnings)
{
  const int nb = static_cast<int>(sbones.size());
  const int room = MAXSTUDIOBONES - nb;
  using Key = std::tuple<int64_t, int64_t, double>;
  std::optional<std::pair<std::vector<std::pair<std::vector<double>, std::vector<bool>>>,
                          std::vector<Key>>>
      found;
  for (const int steps : {4, 2})
  {
    std::vector<std::pair<std::vector<double>, std::vector<bool>>> trial;
    std::set<Key> uniq;
    for (const Pending& p : pending)
    {
      std::vector<double> q;
      std::vector<bool> blended;
      for (size_t v = 0; v < p.share.size(); v++)
      {
        const float qv =
            static_cast<float>(std::nearbyint(p.share[v] * static_cast<float>(steps))) /
            static_cast<float>(steps);
        q.push_back(qv);
        const bool b = qv < 1.0f && p.top[v][0] != p.top[v][1];
        blended.push_back(b);
        if (b)
          uniq.insert({p.top[v][0], p.top[v][1], static_cast<double>(qv)});
      }
      trial.emplace_back(std::move(q), std::move(blended));
    }
    if (static_cast<int>(uniq.size()) <= room)
    {
      found.emplace(std::move(trial), std::vector<Key>(uniq.begin(), uniq.end()));
      break;
    }
  }
  if (!found)
  {
    warnings->push_back("too many weighted joints for the 128-bone limit; blended vertices follow "
                        "their heaviest bone");
    return;
  }
  const auto& [trial, keys] = *found;
  if (keys.empty())
    return;
  std::map<Key, int> index;
  for (size_t i = 0; i < keys.size(); i++)
    index[keys[i]] = nb + static_cast<int>(i);
  std::vector<int> parents;
  for (const SourceBone& b : sbones)
    parents.push_back(b.parent);
  for (size_t i = 0; i < keys.size(); i++)
  {
    const int a = static_cast<int>(std::get<0>(keys[i]));
    const std::string name = (sbones[a].name.substr(0, 22) + "~" + std::to_string(i)).substr(0, 31);
    model->bones.push_back({name, a, Pose{}});
  }
  for (StudioSequence& sq : model->sequences)
  {
    for (auto& frame : sq.frames)
    {
      const std::vector<Pose> own(frame.begin(), frame.begin() + nb);
      const std::vector<Mat4> wm = WorldMatrices(own, parents);
      std::vector<Mat4> skin;
      for (int b = 0; b < nb; b++)
        skin.push_back(Multiply(wm[b], bind_s[b]));
      for (const Key& k : keys)
      {
        const auto [a, b, q] = k;
        const Mat4 blend = DqBlend(skin[a], skin[b], q);
        frame.push_back(
            PoseFromMatrix(Multiply(Multiply(RigidInverse(wm[a]), blend), world_rest[a])));
      }
    }
  }
  for (size_t m = 0; m < pending.size(); m++)
  {
    const auto& [q, blended] = trial[m];
    for (size_t v = 0; v < blended.size(); v++)
    {
      if (blended[v])
        (*vbones)[m][v] = index.at({pending[m].top[v][0], pending[m].top[v][1], q[v]});
    }
  }
  warnings->push_back(std::to_string(keys.size()) +
                      " helper bones emulate blended skin weights (studio skinning is rigid)");
}
}  // namespace

bool IsSourceModel(const Bytes& d)
{
  if (d.size() < 8 || std::memcmp(d.data(), "IDST", 4) != 0)
    return false;
  int32_t version;
  std::memcpy(&version, d.data() + 4, 4);
  return version >= 44 && version <= 49;
}

SourceModelFiles SourceModelCompanions(const fs::path& mdl)
{
  std::map<std::string, fs::path> siblings;
  std::error_code ec;
  for (auto it = fs::directory_iterator(mdl.parent_path(), ec); !ec && it != fs::directory_iterator();
       it.increment(ec))
  {
    siblings[LowerAsciiLatin1(Utf8(it->path().filename()))] = it->path();
  }
  const std::string stem = LowerAsciiLatin1(Utf8(mdl.stem()));
  SourceModelFiles files;
  if (const auto it = siblings.find(stem + ".vvd"); it != siblings.end())
    files.vvd = it->second;
  for (const char* suffix : {".dx90.vtx", ".vtx", ".dx80.vtx", ".sw.vtx", ".xbox.vtx"})
  {
    if (const auto it = siblings.find(stem + suffix); it != siblings.end())
    {
      files.vtx = it->second;
      break;
    }
  }
  return files;
}

std::vector<fs::path> SourceModelDependencies(const fs::path& mdl, const Bytes& data)
{
  std::vector<fs::path> deps;
  const SourceModelFiles files = SourceModelCompanions(mdl);
  if (files.vvd)
    deps.push_back(*files.vvd);
  if (files.vtx)
    deps.push_back(*files.vtx);
  if (!IsSourceModel(data))
    return deps;
  Reader r{data};
  const Header h = ReadHeader(r);
  const auto root = FindMaterialsRoot(mdl.parent_path());
  if (!root)
    return deps;
  std::vector<std::string> dirs;
  for (int32_t i = 0; i < h.numcdtextures && r.ok; i++)
    dirs.push_back(r.CStr(r.I32(h.cdtextureindex + static_cast<int64_t>(i) * 4)));
  for (int32_t i = 0; i < h.numtextures && r.ok; i++)
  {
    const int64_t off = h.textureindex + static_cast<int64_t>(i) * TEXTURE;
    const std::string name = r.CStr(off + r.I32(off));
    std::vector<fs::path> read;
    const ResolvedMaterial res = ResolveMaterial(name, dirs, *root, &read);
    deps.insert(deps.end(), read.begin(), read.end());
    if (res.vtf)
      deps.push_back(*res.vtf);
  }
  return deps;
}

std::optional<ImportedModel> ImportSourceModel(const fs::path& path, const Bytes& d,
                                               const SourceModelOptions& opts, std::string* error)
{
  if (!IsSourceModel(d))
  {
    *error = "not a Source studio model (IDST v44-49)";
    return std::nullopt;
  }
  Reader r{d};
  const Header h = ReadHeader(r);
  const std::string file_name = Utf8(path.filename()), stem = Utf8(path.stem());
  const SourceModelFiles files = SourceModelCompanions(path);
  if (!files.vvd || !files.vtx)
  {
    *error = file_name + ": needs " + stem + ".vvd and " + stem + ".dx90.vtx next to it";
    return std::nullopt;
  }
  const auto vvd_data = ReadAll(*files.vvd), vtx_data = ReadAll(*files.vtx);
  if (!vvd_data || !vtx_data)
  {
    *error = "could not read the .vvd or .vtx file";
    return std::nullopt;
  }
  const auto vvd = ReadVvd(*vvd_data, error);
  if (!vvd)
    return std::nullopt;
  const auto vtx = ReadVtxLod0(*vtx_data, h.version, error);
  if (!vtx)
    return std::nullopt;
  std::vector<std::string> warnings;
  const double scale = opts.scale;
  const std::vector<SourceBone> sbones = ReadBones(r, h);
  if (sbones.empty())
  {
    *error = file_name + ": no bones";
    return std::nullopt;
  }

  // Bones: Source rest pose (pos + quat relative to the parent) to studio poses.
  bool static_model = opts.static_model;
  if (sbones.size() > MAXSTUDIOBONES)
  {
    warnings.push_back(std::to_string(sbones.size()) + " bones exceeds the studio limit of " +
                       std::to_string(MAXSTUDIOBONES) + "; baked static instead");
    static_model = true;
  }
  std::vector<Mat4> locals, world;
  for (size_t i = 0; i < sbones.size(); i++)
  {
    locals.push_back(LocalMatrix(sbones[i].pos, sbones[i].quat, scale));
    const int parent = sbones[i].parent;
    if (parent >= static_cast<int>(i))
    {
      *error = file_name + ": a bone's parent comes after it";
      return std::nullopt;
    }
    world.push_back(parent < 0 ? locals[i] : Multiply(world[parent], locals[i]));
  }

  ImportedModel model;
  model.name = stem;
  if (static_model)
  {
    model.bones = {StudioBone{"root", -1, Pose{}}};
  }
  else
  {
    model.bones.clear();
    for (size_t i = 0; i < sbones.size(); i++)
    {
      const std::string name = sbones[i].name.substr(0, 31);
      model.bones.push_back({Latin1ToUtf8(name.empty() ? "bone" + std::to_string(i) : name),
                             sbones[i].parent, PoseFromMatrix(locals[i])});
    }
  }

  // Skins: family 0, resolved through .vmt files under the nearest materials/ folder.
  std::vector<std::string> tex_names, cd_dirs;
  for (int32_t i = 0; i < h.numtextures && r.ok; i++)
  {
    const int64_t off = h.textureindex + static_cast<int64_t>(i) * TEXTURE;
    tex_names.push_back(r.CStr(off + r.I32(off)));
  }
  for (int32_t i = 0; i < h.numcdtextures && r.ok; i++)
    cd_dirs.push_back(r.CStr(r.I32(h.cdtextureindex + static_cast<int64_t>(i) * 4)));
  std::vector<int> skin_ref;
  if (h.numskinref && h.numskinfamilies)
  {
    for (int32_t i = 0; i < h.numskinref; i++)
      skin_ref.push_back(r.I16(h.skinindex + static_cast<int64_t>(i) * 2));
  }
  else
  {
    for (int32_t i = 0; i < h.numtextures; i++)
      skin_ref.push_back(i);
  }
  if (!r.ok)
  {
    *error = file_name + ": truncated model";
    return std::nullopt;
  }
  const auto root = FindMaterialsRoot(path.parent_path());
  std::map<int, int> mat_of_tex;
  const auto material = [&](int tex) {
    if (const auto it = mat_of_tex.find(tex); it != mat_of_tex.end())
      return it->second;
    const std::string name = tex >= 0 && tex < static_cast<int>(tex_names.size()) ?
                                 tex_names[tex] :
                                 "skin" + std::to_string(tex);
    StudioMaterial mat = SourceSkin(name, cd_dirs, root, opts, &warnings);
    mat.name = Latin1ToUtf8(mat.name);
    mat_of_tex[tex] = static_cast<int>(model.materials.size());
    model.materials.push_back(std::move(mat));
    return mat_of_tex[tex];
  };

  std::vector<Mat4> bind;
  for (const SourceBone& b : sbones)
    bind.push_back(b.pose_to_bone);

  int64_t flips = 0, total = 0;
  std::vector<Pending> pending;
  const int64_t nvvd = static_cast<int64_t>(vvd->pos.size());
  for (int32_t bp = 0; bp < h.numbodyparts && r.ok; bp++)
  {
    const int64_t bpoff = h.bodypartindex + static_cast<int64_t>(bp) * BODYPART;
    const int32_t nummodels = r.I32(bpoff + 4), modelindex = r.I32(bpoff + 12);
    for (int32_t mi = 0; mi < std::min(nummodels, 1); mi++)
    {
      const int64_t model_off = bpoff + modelindex + static_cast<int64_t>(mi) * MODEL;
      const int32_t nmeshes = r.I32(model_off + 72), meshindex = r.I32(model_off + 76);
      const int32_t vertexindex = r.I32(model_off + 84);
      const int64_t first = vertexindex >= 0 ? vertexindex / static_cast<int64_t>(VVD_VERTEX) :
                                               -((-static_cast<int64_t>(vertexindex) + 47) / 48);
      const auto vit = vtx->find({bp, mi});
      for (int32_t me = 0; me < nmeshes && r.ok; me++)
      {
        const int64_t mesh_off = model_off + meshindex + static_cast<int64_t>(me) * MESH;
        const int32_t mat = r.I32(mesh_off), voff = r.I32(mesh_off + 12);
        if (vit == vtx->end() || me >= static_cast<int32_t>(vit->second.size()) ||
            vit->second[me].empty())
        {
          continue;
        }
        std::vector<std::array<int64_t, 3>> tris;
        for (const auto& group : vit->second[me])
          for (auto t : group)
          {
            for (int64_t& v : t)
              v += first + voff;
            tris.push_back(t);
          }
        int64_t max_id = -1;
        for (const auto& t : tris)
          for (const int64_t v : t)
            max_id = std::max(max_id, v);
        if (tris.empty() || max_id >= nvvd)
        {
          warnings.push_back("bodypart " + std::to_string(bp) + " mesh " + std::to_string(me) +
                             ": vertex ids out of range; skipped");
          continue;
        }
        std::vector<int64_t> used;
        for (const auto& t : tris)
          used.insert(used.end(), t.begin(), t.end());
        std::sort(used.begin(), used.end());
        used.erase(std::unique(used.begin(), used.end()), used.end());
        if (used.front() < 0)
        {
          warnings.push_back("bodypart " + std::to_string(bp) + " mesh " + std::to_string(me) +
                             ": vertex ids out of range; skipped");
          continue;
        }
        std::map<int64_t, int64_t> remap;
        for (size_t i = 0; i < used.size(); i++)
          remap[used[i]] = static_cast<int64_t>(i);
        Pending p;
        for (const int64_t u : used)
        {
          p.pos.push_back({vvd->pos[u][0] * scale, vvd->pos[u][1] * scale, vvd->pos[u][2] * scale});
          p.nrm.push_back(vvd->normal[u]);
          p.uv.push_back(vvd->uv[u]);
          // The two heaviest influences and the first one's share of them.
          const int nbones = std::clamp(vvd->nbones[u], 1, 3);
          std::array<float, 3> w{};
          for (int k = 0; k < 3; k++)
            w[k] = k < nbones ? vvd->weights[u][k] : 0.0f;
          std::array<int, 3> order = {0, 1, 2};
          std::stable_sort(order.begin(), order.end(),
                           [&](int a, int b) { return -w[a] < -w[b]; });
          std::array<int64_t, 2> top;
          for (int k = 0; k < 2; k++)
            top[k] = std::clamp<int64_t>(vvd->bones[u][order[k]], 0,
                                         static_cast<int64_t>(sbones.size()) - 1);
          const float tw0 = w[order[0]], tw1 = w[order[1]];
          const float sum = tw0 + tw1;
          p.top.push_back(top);
          p.share.push_back(sum > 0 ? tw0 / std::max(sum, 1e-9f) : 1.0f);
        }
        for (const auto& t : tris)
          p.tris.push_back({remap[t[0]], remap[t[1]], remap[t[2]]});
        // Source fronts are clockwise; the studio writer wants counter-clockwise.
        for (const auto& t : p.tris)
        {
          const Vec3 &a = p.pos[t[0]], &b = p.pos[t[1]], &c = p.pos[t[2]];
          const Vec3 e1 = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
          const Vec3 e2 = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
          const Vec3 fn = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                           e1[0] * e2[1] - e1[1] * e2[0]};
          Vec3 vn;
          for (int k = 0; k < 3; k++)
            vn[k] = p.nrm[t[0]][k] + p.nrm[t[1]][k] + p.nrm[t[2]][k];
          const double agree = fn[0] * vn[0] + fn[1] * vn[1] + fn[2] * vn[2];
          flips += agree < 0;
          total++;
        }
        p.material = material(mat >= 0 && mat < static_cast<int32_t>(skin_ref.size()) ?
                                  skin_ref[mat] :
                                  mat);
        pending.push_back(std::move(p));
      }
    }
  }

  if (!static_model)
    model.sequences = ReadSequences(r, h, sbones, scale, opts.anim_fps, &warnings);
  std::vector<Mat4> bind_s = bind;
  for (Mat4& m : bind_s)
    for (int i = 0; i < 3; i++)
      m[i][3] *= scale;
  std::vector<std::vector<int64_t>> vbones;
  for (const Pending& p : pending)
  {
    std::vector<int64_t> vb;
    for (const auto& t : p.top)
      vb.push_back(t[0]);
    vbones.push_back(std::move(vb));
  }
  if (!static_model && opts.blend_bones)
    BlendBones(pending, &vbones, sbones, &model, world, bind_s, &warnings);

  const bool flip = flips * 2 > total;
  for (size_t m = 0; m < pending.size(); m++)
  {
    const Pending& p = pending[m];
    StudioSubmesh sm;
    sm.material = p.material;
    for (size_t v = 0; v < p.pos.size(); v++)
    {
      Vec3 local = p.pos[v], ln = p.nrm[v];
      if (!static_model)
      {
        // Bone space through the bind of the heaviest Source bone (its poseToBone).
        const Mat4& pb = bind_s[p.top[v][0]];
        for (int i = 0; i < 3; i++)
        {
          local[i] = EinsumDot3(pb[i][0], pb[i][1], pb[i][2], p.pos[v][0], p.pos[v][1],
                                p.pos[v][2]) +
                     pb[i][3];
          ln[i] = EinsumDot3(pb[i][0], pb[i][1], pb[i][2], p.nrm[v][0], p.nrm[v][1], p.nrm[v][2]);
        }
      }
      const double len = std::sqrt(ln[0] * ln[0] + ln[1] * ln[1] + ln[2] * ln[2]);
      if (len > 1e-8)
      {
        const double l = std::max(len, 1e-8);
        ln = {ln[0] / l, ln[1] / l, ln[2] / l};
      }
      else
      {
        ln = {0.0, 0.0, 1.0};
      }
      sm.positions.push_back(
          {static_cast<float>(local[0]), static_cast<float>(local[1]), static_cast<float>(local[2])});
      sm.normals.push_back(
          {static_cast<float>(ln[0]), static_cast<float>(ln[1]), static_cast<float>(ln[2])});
      sm.bones.push_back(static_model ? 0 : static_cast<int32_t>(vbones[m][v]));
    }
    sm.uvs = p.uv;
    for (const auto& t : p.tris)
    {
      if (flip)
        sm.tris.push_back({static_cast<uint32_t>(t[0]), static_cast<uint32_t>(t[2]),
                           static_cast<uint32_t>(t[1])});
      else
        sm.tris.push_back({static_cast<uint32_t>(t[0]), static_cast<uint32_t>(t[1]),
                           static_cast<uint32_t>(t[2])});
    }
    model.meshes.push_back(std::move(sm));
  }
  if (model.meshes.empty())
  {
    *error = file_name + ": no triangles in LOD 0";
    return std::nullopt;
  }
  for (int32_t i = 0; i < h.numbodyparts; i++)
  {
    if (r.I32(h.bodypartindex + static_cast<int64_t>(i) * BODYPART + 4) > 1)
    {
      warnings.push_back("bodygroups: only the first model of each bodypart is converted");
      break;
    }
  }
  if (model.sequences.empty())
    model.sequences.push_back({"idle", 30.0, model.RestFrames(), true});
  model.warnings = std::move(warnings);
  return model;
}
}  // namespace GxBake
