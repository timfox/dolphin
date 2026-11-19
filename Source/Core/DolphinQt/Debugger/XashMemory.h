// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <QByteArray>
#include <QString>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

// Symbols read from an ELF's .symtab, kept separately from Dolphin's symbol database so the
// engine views work with a stripped boot executable (the usual case for a disc) as long as the
// unstripped ELF it was made from is around.
struct ElfSymbolTable
{
  struct Symbol
  {
    u32 address = 0;
    u32 size = 0;
    bool function = false;
  };

  QString path;
  u32 entry_point = 0;
  std::unordered_map<std::string, Symbol> symbols;
  // Code at the entry point, to check the ELF is the program that's actually running.
  QByteArray entry_code;

  const Symbol* Find(const std::string& name) const;
};

std::optional<ElfSymbolTable> LoadElfSymbolTable(const QString& path);

// Xash3D FWGS (xash3d-gc) memory, read straight from emulated RAM using the engine's own
// bookkeeping: the zone allocator's mempools, the C heap underneath them, the static map-load
// arena, the ARAM heap and asset cache, and the memory telemetry the GameCube port keeps.
namespace Xash
{
enum class PoolCategory
{
  Model,
  Texture,
  Sound,
  Render,
  Server,
  Client,
  FileSystem,
  Engine,
};

QString GetPoolCategoryName(PoolCategory category);

struct Pool
{
  int index = 0;      // poolhandle_t - 1
  u32 address = 0;    // mempool_t
  QString name;       // As the engine names it, colour codes removed
  PoolCategory category = PoolCategory::Engine;
  u32 payload = 0;    // totalsize: bytes requested by Mem_Alloc callers
  u32 real = 0;       // realsize: what the pool actually holds, headers included
  u32 created_file = 0;  // const char* of the Mem_AllocPool caller
  int created_line = 0;
  u32 arena = 0;          // GameCube pool arena (0 if none)
  u32 arena_size = 0;
  u32 arena_offset = 0;
  int blocks = 0;
  bool blocks_complete = true;  // false if the chain walk stopped early
};

// newlib (dlmalloc 2.6, libogc) or dlmalloc 2.8.6 (libogc2): the C heap every pool sits on.
struct CHeap
{
  QString allocator;
  u32 start = 0;
  u32 top = 0;      // Start of the top (wilderness) chunk
  u32 end = 0;      // Current break
  u32 arena_hi = 0; // Arena1Hi: how far the heap could still grow
  u64 used = 0;
  u64 free = 0;     // Free chunks inside the heap, top chunk included
  u64 largest_free = 0;
  u64 peak_footprint = 0;  // dlmalloc 2.8.6 only
  int used_chunks = 0;
  int free_chunks = 0;
  bool walked = false;     // Chunk walk finished cleanly
  std::vector<std::pair<u32, u32>> used_ranges;  // [start, end) of allocated chunks

  u64 Unclaimed() const { return arena_hi > end ? arena_hi - end : 0; }
};

struct AramSegment
{
  u32 offset = 0;
  u32 size = 0;
  bool used = false;
  bool stream = false;  // Stream blocks come from the top and can be freed
  QString tag;
};

struct AramAsset
{
  QString name;
  u32 size = 0;
  u32 aram = 0;
  u32 last_use = 0;
  int pins = 0;
  bool ready = false;
};

struct Aram
{
  u32 base = 0;
  u32 end = 0;
  u32 session_bytes = 0;
  u32 stream_bytes = 0;
  u32 peak_bytes = 0;
  u32 failures = 0;
  std::vector<AramSegment> segments;

  bool has_cache = false;
  u32 cache_budget = 0;
  u32 cache_bytes = 0;
  u32 cache_clock = 0;
  u32 hits = 0, misses = 0, stores = 0, evictions = 0, refusals = 0;
  std::vector<AramAsset> assets;

  u64 FreeBytes() const;
  u64 LargestFree() const;
};

// The static map-load arena: BSP image staging plus whatever the loader carves from its tail.
struct MapArena
{
  u32 base = 0;
  u32 size = 0;
  u32 tail_highwater = 0;
  u32 tail_peak = 0;
  bool in_use = false;
};

struct BootArena
{
  u32 base = 0;
  u32 size = 0;
  u32 live_bytes = 0;
  u32 peak_bytes = 0;
  bool closed = false;
};

struct Snapshot
{
  QString map;              // gc_mem_map
  std::optional<u64> hwm;   // gc_mem_hwm: zone high-water mark
  std::vector<Pool> pools;
  bool pools_valid = false;
  u64 zone_real = 0;
  u64 zone_payload = 0;
  std::optional<CHeap> heap;
  std::optional<Aram> aram;
  std::optional<MapArena> map_arena;
  std::optional<BootArena> boot_arena;
  std::optional<u32> prefetch_used, prefetch_capacity, prefetch_entries;
  QString problem;  // Why something couldn't be read, if anything
};

// Where Mem_Alloc calls came from: one row per source line.
struct AllocationSite
{
  QString file;
  int line = 0;
  int count = 0;
  u64 bytes = 0;     // Payload bytes
  u64 largest = 0;
  std::vector<int> pools;
};

class Reader
{
public:
  // Symbols come from |table| when set, otherwise from Dolphin's symbol database.
  void SetSymbols(std::optional<ElfSymbolTable> table);
  const std::optional<ElfSymbolTable>& GetSymbols() const { return m_symbols; }

  // True if the running program looks like Xash3D (the zone allocator's symbols resolve).
  bool Detect(Core::System& system) const;
  std::optional<Snapshot> Read(Core::System& system);
  // Walks every allocation in |pool_index| (or every pool for -1) and groups by caller.
  std::vector<AllocationSite> ReadAllocationSites(Core::System& system, int pool_index);

  QString ReadCString(Core::System& system, u32 address, u32 max_length = 256);
  void ClearCache() { m_strings.clear(); }

  // Large data objects in the executable (from the ELF symbols), largest first.
  std::vector<std::pair<std::string, ElfSymbolTable::Symbol>> LargeObjects(u32 min_size) const;

private:
  struct Resolved
  {
    u32 address = 0;
    u32 size = 0;
  };
  std::optional<Resolved> Resolve(Core::System& system, const char* name) const;
  std::optional<u32> ReadU32(Core::System& system, u32 address) const;
  std::optional<u32> ReadSymbolU32(Core::System& system, const char* name) const;
  const u8* Pointer(Core::System& system, u32 address, u32 length) const;

  void ReadPools(Core::System& system, Snapshot& snapshot);
  std::optional<CHeap> ReadCHeap(Core::System& system);
  std::optional<Aram> ReadAram(Core::System& system, QString* problem);

  std::optional<ElfSymbolTable> m_symbols;
  std::unordered_map<u32, QString> m_strings;  // Source file names are string literals
};
}  // namespace Xash
