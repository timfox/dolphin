// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

// GoldSrc BSP v30 writing and clip hull building, ported from the Python tool (core/bsp30.py)
// with byte-identical output. The map converters (SourceMap.cpp) build a Bsp30 and write it.
//
// The geometry repeats numpy's rounding: dot products and matrix-vector products are fused
// multiply-add chains in the order OpenBLAS sums them on x86-64 (see Bsp30Dot and Bsp30Rows).

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace GxBake
{
using Vec3 = std::array<double, 3>;
using Winding = std::vector<Vec3>;

constexpr int BSP30_CONTENTS_EMPTY = -1;
constexpr int BSP30_CONTENTS_SOLID = -2;
constexpr int BSP30_CONTENTS_WATER = -3;
constexpr int BSP30_CONTENTS_SLIME = -4;
constexpr int BSP30_TEX_SPECIAL = 1;

// numpy's 1-D float64 dot of two 3-vectors (OpenBLAS ddot): a fused chain in order 0, 1, 2.
double Bsp30Dot(const Vec3& a, const Vec3& b);
// numpy's (k, 3) @ (3,) for float64 (OpenBLAS dgemv_t): order 0, 1, 2 for one row, else 1, 0, 2.
double Bsp30Row(const Vec3& row, const Vec3& v, size_t rows);
double Bsp30Norm(const Vec3& v);
Vec3 Bsp30Cross(const Vec3& a, const Vec3& b);

// bsp30.PlaneTable: deduplicated planes, axial ones facing +axis.
class PlaneTable
{
public:
  // (index, flipped); nullopt for a zero normal.
  std::optional<std::pair<int, bool>> Add(const Vec3& normal, double dist);
  size_t size() const { return m_dists.size(); }
  const Vec3& Normal(int i) const { return m_normals[i]; }
  double Dist(int i) const { return m_dists[i]; }
  int Type(int i) const { return m_types[i]; }
  std::vector<uint8_t> Pack() const;

private:
  struct Key
  {
    double n0, n1, n2, d;
    bool operator==(const Key&) const = default;
  };
  struct KeyHash
  {
    size_t operator()(const Key& k) const;
  };
  std::vector<Vec3> m_normals;
  std::vector<double> m_dists;
  std::vector<int> m_types;
  std::unordered_map<Key, int, KeyHash> m_index;
};

Winding BaseWinding(const Vec3& normal, double dist, double size = 131072.0);
// The part of |w| behind the plane (n·x <= d), or in front with |keep_front|; nullopt when
// nothing (or fewer than three points) is left.
std::optional<Winding> ClipWinding(const Winding& w, const Vec3& normal, double dist,
                                   bool keep_front = false, double eps = 0.01);

// A convex volume as (outward normal, dist) sides, n·x <= d inside.
using BrushSides = std::vector<std::pair<Vec3, double>>;

// bsp30.build_clip_hull: appends one hull's clipnodes and returns its head node.
int BuildClipHull(const std::vector<BrushSides>& brushes, int hull, PlaneTable* planes,
                  std::vector<std::array<int, 3>>* clipnodes);

std::vector<uint8_t> CompressVis(const std::vector<uint8_t>& row);
// bsp30.merge_vis_rows; nullopt rows have no visibility.
std::vector<std::optional<std::vector<uint8_t>>>
MergeVisRows(const std::vector<std::optional<std::vector<uint8_t>>>& rows, int max_bits);

struct Miptex
{
  std::string name;  // Latin-1
  int width = 0;
  int height = 0;
  std::vector<std::vector<uint8_t>> mips;  // 4 levels of indices
  std::vector<uint8_t> palette;
  std::vector<uint8_t> Pack() const;
};

struct Bsp30Map
{
  struct Node
  {
    int plane, c0, c1;
    std::array<int, 3> mins, maxs;
    int firstface, numfaces;
  };
  struct TexInfo
  {
    std::array<double, 8> vecs;
    int miptex, flags;
  };
  struct Face
  {
    int plane, side, firstedge, numedges, texinfo;
    std::array<uint8_t, 4> styles;
    int lightofs;
  };
  struct Leaf
  {
    int contents, visofs;
    std::array<int, 3> mins, maxs;
    int firstmark, nummark;
  };
  struct Model
  {
    std::array<float, 9> bounds;  // mins, maxs, origin
    std::array<int, 4> heads;
    int visleafs, firstface, numfaces;
  };

  std::string entities;  // Latin-1
  PlaneTable planes;
  std::vector<Miptex> textures;
  std::vector<Vec3> vertexes;
  std::vector<uint8_t> visibility;
  std::vector<Node> nodes;
  std::vector<TexInfo> texinfo;
  std::vector<Face> faces;
  std::vector<uint8_t> lighting;
  std::vector<std::array<int, 3>> clipnodes;
  std::vector<Leaf> leafs;
  std::vector<int> marksurfaces;
  std::vector<std::pair<int, int>> edges = {{0, 0}};
  std::vector<int> surfedges;
  std::vector<Model> models;

  std::vector<std::string> Check() const;
  std::vector<uint8_t> Write() const;
};
}  // namespace GxBake
