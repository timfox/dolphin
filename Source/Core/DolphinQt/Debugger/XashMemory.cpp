// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Debugger/XashMemory.h"

#include <algorithm>
#include <cstring>
#include <map>

#include <QFile>
#include <QObject>

#include "Common/Swap.h"
#include "Common/SymbolDB.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/System.h"

namespace
{
constexpr u32 MEM1_BASE = 0x80000000;
constexpr u32 MEM2_BASE = 0x90000000;

u32 BE32(const u8* data)
{
  return Common::swap32(data);
}
u16 BE16(const u8* data)
{
  return Common::swap16(data);
}

// zone.c (ILP32). mempool_t is 96 bytes, plus 16 for the GameCube pool arena fields.
constexpr u32 POOL_CHAIN = 0x00;
constexpr u32 POOL_CHAIN_SMALL = 0x04;
constexpr u32 POOL_TOTALSIZE = 0x08;
constexpr u32 POOL_REALSIZE = 0x0C;
constexpr u32 POOL_FILENAME = 0x14;
constexpr u32 POOL_FILELINE = 0x18;
constexpr u32 POOL_NAME = 0x20;
constexpr u32 POOL_NAME_LENGTH = 64;
constexpr u32 POOL_GC_ARENA = 0x60;
constexpr u32 POOL_SIZE_GAMECUBE = 112;
constexpr u32 POOL_SIZE_PLAIN = 96;

constexpr u32 MEMHEADER_SIZE = 24;
constexpr u32 MEMHEADER_FILENAME = 0x08;
constexpr u32 MEMHEADER_SIZE_FIELD = 0x0C;
constexpr u32 MEMHEADER_FILELINE = 0x14;
constexpr u32 MEMHEADER_SENTINEL = 0x16;
constexpr u16 MEMHEADER_SENTINEL_BIG = 0xA1BA;
constexpr u32 MEMHEADER_SMALL_SIZE = 16;
constexpr u32 MEMHEADER_SMALL_SIZE_FIELD = 0x0C;
constexpr u32 MEMHEADER_SMALL_SENTINEL = 0x0E;
constexpr u16 MEMHEADER_SENTINEL_SMALL = 0xAD1E;

constexpr u32 MAX_POOLS = 4096;
constexpr int MAX_BLOCKS_PER_SAMPLE = 400000;

// aram_core.h
constexpr u32 ARAM_MAX_SEGS = 64;
constexpr u32 ARAM_SEG_SIZE = 24;
constexpr u32 ARAM_TAG_LENGTH = 12;
constexpr u32 ARAM_HEAP_SEGS = 12;
constexpr u32 ARAM_HEAP_SIZE = ARAM_HEAP_SEGS + ARAM_MAX_SEGS * ARAM_SEG_SIZE + 16;
constexpr u32 ARAM_CACHE_MAX = 48;
constexpr u32 ARAM_NAME_LENGTH = 48;
constexpr u32 ARAM_ASSET_SIZE = 64;
constexpr u32 ARAM_CACHE_ASSETS = 40;
constexpr u32 ARAM_CACHE_SIZE = ARAM_CACHE_ASSETS + ARAM_CACHE_MAX * ARAM_ASSET_SIZE;

constexpr int MAX_HEAP_CHUNKS = 1 << 20;

QString StripColourCodes(const QString& name)
{
  QString result;
  result.reserve(name.size());
  for (int i = 0; i < name.size(); ++i)
  {
    if (name[i] == QLatin1Char('^') && i + 1 < name.size() && name[i + 1].isDigit())
    {
      ++i;
      continue;
    }
    result.append(name[i]);
  }
  return result;
}

Xash::PoolCategory Categorise(const QString& name)
{
  using Xash::PoolCategory;
  const QString lower = name.toLower();
  const auto has = [&](const char* text) { return lower.contains(QLatin1String(text)); };
  if (lower.endsWith(QLatin1String(".mdl")) || lower.endsWith(QLatin1String(".bsp")) ||
      lower.endsWith(QLatin1String(".spr")) || lower.startsWith(QLatin1String("models/")) ||
      lower.startsWith(QLatin1String("sprites/")) || lower.startsWith(QLatin1String("maps/")) ||
      has("studio") || has("model"))
  {
    return PoolCategory::Model;
  }
  if (lower.startsWith(QLatin1String("wad:")) || has("image") || has("texture") || has("decal"))
    return PoolCategory::Texture;
  if (has("sound") || has("snd") || has("voice") || has("audio"))
    return PoolCategory::Sound;
  if (has("render") || has("ref_") || has("vbo") || has("hud") || has("avi"))
    return PoolCategory::Render;
  if (lower.startsWith(QLatin1String("server")))
    return PoolCategory::Server;
  if (lower.startsWith(QLatin1String("client")) || has("menu") || has("vgui") || has("touch"))
    return PoolCategory::Client;
  if (has("filesystem") || has("http") || has("network") || has("wad"))
    return PoolCategory::FileSystem;
  return PoolCategory::Engine;
}
}  // namespace

const ElfSymbolTable::Symbol* ElfSymbolTable::Find(const std::string& name) const
{
  const auto it = symbols.find(name);
  return it == symbols.end() ? nullptr : &it->second;
}

std::optional<ElfSymbolTable> LoadElfSymbolTable(const QString& path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 512 * 1024 * 1024)
    return std::nullopt;
  const QByteArray contents = file.readAll();
  const auto* data = reinterpret_cast<const u8*>(contents.constData());
  const size_t size = static_cast<size_t>(contents.size());
  if (size < 0x34 || std::memcmp(data, "\x7f" "ELF", 4) != 0 || data[4] != 1 || data[5] != 2)
    return std::nullopt;

  ElfSymbolTable table;
  table.path = path;
  table.entry_point = BE32(data + 0x18);

  const u32 shoff = BE32(data + 0x20);
  const u16 shentsize = BE16(data + 0x2E);
  const u16 shnum = BE16(data + 0x30);
  const auto section = [&](u32 index) -> const u8* {
    const u64 offset = u64(shoff) + u64(index) * shentsize;
    if (index >= shnum || shentsize < 0x28 || offset + 0x28 > size)
      return nullptr;
    return data + offset;
  };

  constexpr u32 SHT_PROGBITS = 1;
  constexpr u32 SHT_SYMTAB = 2;
  constexpr u32 STT_OBJECT = 1;
  constexpr u32 STT_FUNC = 2;
  for (u32 i = 0; i < shnum; ++i)
  {
    const u8* const header = section(i);
    if (!header)
      break;
    const u32 type = BE32(header + 0x04);
    const u32 address = BE32(header + 0x0C);
    const u32 offset = BE32(header + 0x10);
    const u32 section_size = BE32(header + 0x14);
    if (u64(offset) + section_size > size)
      continue;

    if (type == SHT_PROGBITS && table.entry_point >= address &&
        table.entry_point < address + section_size && table.entry_code.isEmpty())
    {
      const u32 start = table.entry_point - address;
      const u32 length = std::min<u32>(64, section_size - start);
      table.entry_code = QByteArray(reinterpret_cast<const char*>(data + offset + start), length);
    }

    if (type != SHT_SYMTAB)
      continue;
    const u8* const strings_header = section(BE32(header + 0x18));
    if (!strings_header)
      continue;
    const u32 strings_offset = BE32(strings_header + 0x10);
    const u32 strings_size = BE32(strings_header + 0x14);
    if (u64(strings_offset) + strings_size > size)
      continue;
    const char* const strings = reinterpret_cast<const char*>(data + strings_offset);

    for (u32 entry = 0; entry + 16 <= section_size; entry += 16)
    {
      const u8* const symbol = data + offset + entry;
      const u32 name = BE32(symbol);
      const u32 value = BE32(symbol + 4);
      const u32 symbol_size = BE32(symbol + 8);
      const u32 symbol_type = symbol[12] & 0xF;
      if (name >= strings_size || value == 0 ||
          (symbol_type != STT_OBJECT && symbol_type != STT_FUNC))
      {
        continue;
      }
      std::string symbol_name(strings + name, qstrnlen(strings + name, strings_size - name));
      // Keep the first definition: a static in one file can share its name with another's.
      table.symbols.emplace(std::move(symbol_name),
                            ElfSymbolTable::Symbol{value, symbol_size, symbol_type == STT_FUNC});
    }
  }

  if (table.symbols.empty())
    return std::nullopt;
  return table;
}

namespace Xash
{
QString GetPoolCategoryName(PoolCategory category)
{
  switch (category)
  {
  case PoolCategory::Model:
    return QObject::tr("Models");
  case PoolCategory::Texture:
    return QObject::tr("Textures / WADs");
  case PoolCategory::Sound:
    return QObject::tr("Sound");
  case PoolCategory::Render:
    return QObject::tr("Renderer");
  case PoolCategory::Server:
    return QObject::tr("Server");
  case PoolCategory::Client:
    return QObject::tr("Client / UI");
  case PoolCategory::FileSystem:
    return QObject::tr("Files / network");
  case PoolCategory::Engine:
    break;
  }
  return QObject::tr("Engine");
}

u64 Aram::FreeBytes() const
{
  u64 total = 0;
  for (const AramSegment& segment : segments)
  {
    if (!segment.used)
      total += segment.size;
  }
  return total;
}

u64 Aram::LargestFree() const
{
  u64 largest = 0;
  for (const AramSegment& segment : segments)
  {
    if (!segment.used)
      largest = std::max<u64>(largest, segment.size);
  }
  return largest;
}

void Reader::SetSymbols(std::optional<ElfSymbolTable> table)
{
  m_symbols = std::move(table);
  m_strings.clear();
}

std::optional<Reader::Resolved> Reader::Resolve(Core::System& system, const char* name) const
{
  if (m_symbols)
  {
    if (const ElfSymbolTable::Symbol* symbol = m_symbols->Find(name))
      return Resolved{symbol->address, symbol->size};
    return std::nullopt;
  }
  const Common::Symbol* const symbol = system.GetPPCSymbolDB().GetSymbolFromName(name);
  if (!symbol)
    return std::nullopt;
  return Resolved{symbol->address, symbol->size};
}

const u8* Reader::Pointer(Core::System& system, u32 address, u32 length) const
{
  const u32 top = address & 0xF0000000;
  const u32 offset = address & 0x0FFFFFFF;
  auto& memory = system.GetMemory();
  if ((top == MEM1_BASE || top == 0xC0000000) && memory.GetRAM() &&
      u64(offset) + length <= memory.GetRamSizeReal())
  {
    return memory.GetRAM() + offset;
  }
  if ((top == MEM2_BASE || top == 0xD0000000) && memory.GetEXRAM() &&
      u64(offset) + length <= memory.GetExRamSizeReal())
  {
    return memory.GetEXRAM() + offset;
  }
  return nullptr;
}

std::optional<u32> Reader::ReadU32(Core::System& system, u32 address) const
{
  const u8* const data = Pointer(system, address, 4);
  if (!data)
    return std::nullopt;
  return BE32(data);
}

std::optional<u32> Reader::ReadSymbolU32(Core::System& system, const char* name) const
{
  const auto symbol = Resolve(system, name);
  if (!symbol)
    return std::nullopt;
  return ReadU32(system, symbol->address);
}

QString Reader::ReadCString(Core::System& system, u32 address, u32 max_length)
{
  if (address == 0)
    return {};
  if (const auto it = m_strings.find(address); it != m_strings.end())
    return it->second;
  QString result;
  for (u32 i = 0; i < max_length; ++i)
  {
    const u8* const c = Pointer(system, address + i, 1);
    if (!c || *c == 0)
      break;
    result.append(QLatin1Char(static_cast<char>(*c)));
  }
  // String literals in .rodata never change; cache them (but not names in writable memory).
  if (m_symbols && max_length >= 64)
    m_strings.emplace(address, result);
  return result;
}

bool Reader::Detect(Core::System& system) const
{
  return Resolve(system, "poolchain") && Resolve(system, "poolcount");
}

std::optional<Snapshot> Reader::Read(Core::System& system)
{
  if (!Detect(system))
    return std::nullopt;

  Snapshot snapshot;
  ReadPools(system, snapshot);

  if (const auto map = Resolve(system, "gc_mem_map"))
  {
    QString name;
    for (u32 i = 0; i < 64; ++i)
    {
      const u8* const c = Pointer(system, map->address + i, 1);
      if (!c || *c == 0)
        break;
      name.append(QLatin1Char(static_cast<char>(*c)));
    }
    snapshot.map = name;
  }
  if (const auto hwm = ReadSymbolU32(system, "gc_mem_hwm"))
    snapshot.hwm = *hwm;

  snapshot.heap = ReadCHeap(system);
  snapshot.aram = ReadAram(system, &snapshot.problem);

  const auto map_buf = ReadSymbolU32(system, "gc_mapload_buf");
  const auto map_size = ReadSymbolU32(system, "gc_mapload_buf_size");
  if (map_buf && map_size && *map_buf && *map_size)
  {
    MapArena arena;
    arena.base = *map_buf;
    arena.size = *map_size;
    arena.tail_highwater = ReadSymbolU32(system, "gc_mapload_tail_highwater").value_or(0);
    arena.tail_peak = ReadSymbolU32(system, "gc_mapload_tail_peak").value_or(0);
    arena.in_use = ReadSymbolU32(system, "gc_mapload_buf_in_use").value_or(0) != 0;
    snapshot.map_arena = arena;
  }

  const auto boot_base = ReadSymbolU32(system, "gc_boot_base");
  const auto boot_cap = ReadSymbolU32(system, "gc_boot_cap");
  if (boot_base && boot_cap && *boot_base && *boot_cap)
  {
    BootArena arena;
    arena.base = *boot_base;
    arena.size = *boot_cap;
    arena.live_bytes = ReadSymbolU32(system, "gc_boot_live_bytes").value_or(0);
    arena.peak_bytes = ReadSymbolU32(system, "gc_boot_peak_bytes").value_or(0);
    arena.closed = ReadSymbolU32(system, "gc_boot_closed").value_or(0) != 0;
    snapshot.boot_arena = arena;
  }

  snapshot.prefetch_used = ReadSymbolU32(system, "gc_prefetch_used");
  snapshot.prefetch_capacity = ReadSymbolU32(system, "gc_prefetch_capacity");
  snapshot.prefetch_entries = ReadSymbolU32(system, "gc_prefetch_count");
  return snapshot;
}

void Reader::ReadPools(Core::System& system, Snapshot& snapshot)
{
  const auto chain = ReadSymbolU32(system, "poolchain");
  const auto count = ReadSymbolU32(system, "poolcount");
  if (!chain || !count || *count > MAX_POOLS)
  {
    snapshot.problem = QObject::tr("The zone allocator's pool table couldn't be read.");
    return;
  }
  if (*chain == 0 || *count == 0)
  {
    snapshot.pools_valid = true;  // Not created yet
    return;
  }

  // GameCube builds add four arena fields. The static reserve table tells them apart when its
  // size is known; otherwise assume the GameCube layout.
  u32 stride = POOL_SIZE_GAMECUBE;
  if (const auto reserve = Resolve(system, "gc_poolchain_reserve");
      reserve && reserve->size && reserve->size % POOL_SIZE_GAMECUBE != 0 &&
      reserve->size % POOL_SIZE_PLAIN == 0)
  {
    stride = POOL_SIZE_PLAIN;
  }
  else if (!Resolve(system, "gc_poolchain_reserve") && !Resolve(system, "gc_mem_map"))
  {
    stride = POOL_SIZE_PLAIN;
  }

  int blocks_budget = MAX_BLOCKS_PER_SAMPLE;
  for (u32 i = 0; i < *count; ++i)
  {
    const u32 address = *chain + i * stride;
    const u8* const pool = Pointer(system, address, stride);
    if (!pool)
    {
      snapshot.problem = QObject::tr("The pool table points outside RAM.");
      return;
    }
    const u32 filename = BE32(pool + POOL_FILENAME);
    if (filename == 0)
      continue;  // Freed slot

    Pool info;
    info.index = static_cast<int>(i);
    info.address = address;
    const char* const name = reinterpret_cast<const char*>(pool + POOL_NAME);
    info.name = StripColourCodes(QString::fromLatin1(name, qstrnlen(name, POOL_NAME_LENGTH)));
    info.category = Categorise(info.name);
    info.payload = BE32(pool + POOL_TOTALSIZE);
    info.real = BE32(pool + POOL_REALSIZE);
    info.created_file = filename;
    info.created_line = static_cast<int>(BE32(pool + POOL_FILELINE));
    if (stride == POOL_SIZE_GAMECUBE)
    {
      info.arena = BE32(pool + POOL_GC_ARENA);
      info.arena_size = BE32(pool + POOL_GC_ARENA + 4);
      info.arena_offset = BE32(pool + POOL_GC_ARENA + 8);
    }

    // Count blocks. Each header carries a sentinel, so a torn read stops the walk.
    const auto walk = [&](u32 head, u32 header_size, u32 sentinel_offset, u16 sentinel) {
      u32 block = head;
      while (block != 0)
      {
        if (blocks_budget-- <= 0)
        {
          info.blocks_complete = false;
          return;
        }
        const u8* const header = Pointer(system, block, header_size);
        if (!header || BE16(header + sentinel_offset) != sentinel)
        {
          info.blocks_complete = false;
          return;
        }
        ++info.blocks;
        block = BE32(header);
      }
    };
    walk(BE32(pool + POOL_CHAIN), MEMHEADER_SIZE, MEMHEADER_SENTINEL, MEMHEADER_SENTINEL_BIG);
    walk(BE32(pool + POOL_CHAIN_SMALL), MEMHEADER_SMALL_SIZE, MEMHEADER_SMALL_SENTINEL,
         MEMHEADER_SENTINEL_SMALL);

    snapshot.zone_real += info.real;
    snapshot.zone_payload += info.payload;
    snapshot.pools.push_back(std::move(info));
  }
  snapshot.pools_valid = true;
}

std::vector<AllocationSite> Reader::ReadAllocationSites(Core::System& system, int pool_index)
{
  std::vector<AllocationSite> sites;
  const auto chain = ReadSymbolU32(system, "poolchain");
  const auto count = ReadSymbolU32(system, "poolcount");
  if (!chain || !count || *chain == 0 || *count > MAX_POOLS)
    return sites;

  u32 stride = POOL_SIZE_GAMECUBE;
  if (const auto reserve = Resolve(system, "gc_poolchain_reserve");
      reserve && reserve->size && reserve->size % POOL_SIZE_GAMECUBE != 0 &&
      reserve->size % POOL_SIZE_PLAIN == 0)
  {
    stride = POOL_SIZE_PLAIN;
  }

  std::map<std::pair<u32, int>, AllocationSite> by_site;
  int budget = MAX_BLOCKS_PER_SAMPLE;
  for (u32 i = 0; i < *count; ++i)
  {
    if (pool_index >= 0 && static_cast<int>(i) != pool_index)
      continue;
    const u8* const pool = Pointer(system, *chain + i * stride, stride);
    if (!pool || BE32(pool + POOL_FILENAME) == 0)
      continue;

    const auto add = [&](u32 filename, int line, u64 size) {
      AllocationSite& site = by_site[{filename, line}];
      site.line = line;
      ++site.count;
      site.bytes += size;
      site.largest = std::max(site.largest, size);
      if (site.pools.empty() || site.pools.back() != static_cast<int>(i))
        site.pools.push_back(static_cast<int>(i));
    };

    for (u32 block = BE32(pool + POOL_CHAIN); block != 0 && budget-- > 0;)
    {
      const u8* const header = Pointer(system, block, MEMHEADER_SIZE);
      if (!header || BE16(header + MEMHEADER_SENTINEL) != MEMHEADER_SENTINEL_BIG)
        break;
      add(BE32(header + MEMHEADER_FILENAME), BE16(header + MEMHEADER_FILELINE),
          BE32(header + MEMHEADER_SIZE_FIELD));
      block = BE32(header);
    }
    // Compact headers don't record a caller.
    for (u32 block = BE32(pool + POOL_CHAIN_SMALL); block != 0 && budget-- > 0;)
    {
      const u8* const header = Pointer(system, block, MEMHEADER_SMALL_SIZE);
      if (!header || BE16(header + MEMHEADER_SMALL_SENTINEL) != MEMHEADER_SENTINEL_SMALL)
        break;
      add(0, 0, header[MEMHEADER_SMALL_SIZE_FIELD]);
      block = BE32(header);
    }
  }

  for (auto& [key, site] : by_site)
  {
    site.file = key.first ? ReadCString(system, key.first) :
                            QObject::tr("(small allocations, caller not recorded)");
    sites.push_back(std::move(site));
  }
  std::sort(sites.begin(), sites.end(),
            [](const AllocationSite& a, const AllocationSite& b) { return a.bytes > b.bytes; });
  return sites;
}

std::optional<CHeap> Reader::ReadCHeap(Core::System& system)
{
  CHeap heap;
  const auto arena_lo = ReadSymbolU32(system, "__sysarena1lo");
  const auto arena_hi = ReadSymbolU32(system, "__sysarena1hi");

  const auto add_used = [&](u32 start, u32 end) {
    if (!heap.used_ranges.empty() && heap.used_ranges.back().second == start)
      heap.used_ranges.back().second = end;
    else
      heap.used_ranges.emplace_back(start, end);
  };

  if (const auto av = Resolve(system, "__malloc_av_"))
  {
    // newlib's dlmalloc 2.6: top lives in bin 0's fd; chunks run contiguously from the sbrk base.
    heap.allocator = QObject::tr("newlib malloc");
    const auto top = ReadU32(system, av->address + 8);
    const auto sbrk_base = ReadSymbolU32(system, "__malloc_sbrk_base");
    if (!top || !sbrk_base || *sbrk_base == 0xFFFFFFFF || *sbrk_base == 0)
      return std::nullopt;
    heap.start = (*sbrk_base + 7) & ~7u;
    heap.top = *top;
    const u32 top_size = ReadU32(system, heap.top + 4).value_or(0) & ~3u;
    heap.end = heap.top + top_size;
    heap.free = top_size;
    heap.largest_free = top_size;

    if (heap.start <= heap.top && Pointer(system, heap.start, heap.top - heap.start))
    {
      u32 p = heap.start;
      int chunks = 0;
      while (p < heap.top && chunks++ < MAX_HEAP_CHUNKS)
      {
        const u32 size = ReadU32(system, p + 4).value_or(0) & ~3u;
        if (size < 16 || (size & 7) || p + size > heap.top)
          break;
        const u32 next = p + size;
        const bool in_use = ReadU32(system, next + 4).value_or(0) & 1;
        if (in_use)
        {
          heap.used += size;
          ++heap.used_chunks;
          add_used(p, next);
        }
        else
        {
          heap.free += size;
          heap.largest_free = std::max<u64>(heap.largest_free, size);
          ++heap.free_chunks;
        }
        p = next;
      }
      heap.walked = p == heap.top;
    }
  }
  else if (const auto gm = Resolve(system, "_gm_"))
  {
    // libogc2's dlmalloc 2.8.6 (malloc_state: topsize 0x0C, least_addr 0x10, top 0x18,
    // footprint 0x1B0, max_footprint 0x1B4).
    heap.allocator = QObject::tr("dlmalloc 2.8.6");
    const auto top_size = ReadU32(system, gm->address + 0x0C);
    const auto least = ReadU32(system, gm->address + 0x10);
    const auto top = ReadU32(system, gm->address + 0x18);
    const auto max_footprint = ReadU32(system, gm->address + 0x1B4);
    if (!top_size || !least || !top || *top == 0 || *least == 0 || *least > *top)
      return std::nullopt;
    heap.top = *top;
    heap.end = heap.top + *top_size;
    heap.free = *top_size;
    heap.largest_free = *top_size;
    heap.peak_footprint = max_footprint.value_or(0);
    const u32 mem = *least + 8;
    heap.start = *least + ((mem & 31) ? (32 - (mem & 31)) : 0);

    if (Pointer(system, heap.start, heap.top - heap.start))
    {
      u32 p = heap.start;
      int chunks = 0;
      while (p < heap.top && chunks++ < MAX_HEAP_CHUNKS)
      {
        const u32 head = ReadU32(system, p + 4).value_or(0);
        const u32 size = head & ~7u;
        if (size < 16 || p + size > heap.top)
          break;
        if (head & 2)
        {
          heap.used += size;
          ++heap.used_chunks;
          add_used(p, p + size);
        }
        else
        {
          heap.free += size;
          heap.largest_free = std::max<u64>(heap.largest_free, size);
          ++heap.free_chunks;
        }
        p += size;
      }
      heap.walked = p == heap.top;
    }
  }
  else
  {
    return std::nullopt;
  }

  if (arena_lo && *arena_lo >= heap.end && (*arena_lo & 0xF0000000) == (heap.end & 0xF0000000))
    heap.end = *arena_lo;
  heap.arena_hi = arena_hi.value_or(heap.end);
  if (!heap.walked)
    heap.used = heap.end > heap.start ? (heap.end - heap.start) - heap.free : 0;
  return heap;
}

std::optional<Aram> Reader::ReadAram(Core::System& system, QString* problem)
{
  const auto heap_symbol = Resolve(system, "gc_aram_heap");
  if (!heap_symbol)
    return std::nullopt;
  if (heap_symbol->size != 0 && heap_symbol->size != ARAM_HEAP_SIZE)
  {
    *problem = QObject::tr("gc_aram_heap is %1 bytes, not the %2 this version of the panel "
                           "understands; ARAM blocks are not shown.")
                   .arg(heap_symbol->size)
                   .arg(ARAM_HEAP_SIZE);
    return std::nullopt;
  }
  const u8* const data = Pointer(system, heap_symbol->address, ARAM_HEAP_SIZE);
  if (!data)
    return std::nullopt;

  Aram aram;
  aram.base = BE32(data);
  aram.end = BE32(data + 4);
  const u32 count = BE32(data + 8);
  if (aram.end == 0)
    return std::nullopt;  // ARAM manager not started yet
  if (count > ARAM_MAX_SEGS || aram.base >= aram.end)
    return std::nullopt;
  for (u32 i = 0; i < count; ++i)
  {
    const u8* const segment = data + ARAM_HEAP_SEGS + i * ARAM_SEG_SIZE;
    AramSegment info;
    info.offset = BE32(segment);
    info.size = BE32(segment + 4);
    info.used = segment[8] != 0;
    info.stream = segment[9] != 0;
    const char* const tag = reinterpret_cast<const char*>(segment + 10);
    info.tag = QString::fromLatin1(tag, qstrnlen(tag, ARAM_TAG_LENGTH));
    aram.segments.push_back(std::move(info));
  }
  // The segments cover [base, end) in order; anything else is a torn read or a layout change.
  u32 expected = aram.base;
  for (const AramSegment& segment : aram.segments)
  {
    if (segment.offset != expected || segment.size > aram.end - segment.offset)
    {
      *problem = QObject::tr("The ARAM block list doesn't add up (block at %1); it may have been "
                             "read mid-update.")
                     .arg(segment.offset, 8, 16, QLatin1Char('0'));
      return std::nullopt;
    }
    expected = segment.offset + segment.size;
  }

  const u8* const totals = data + ARAM_HEAP_SEGS + ARAM_MAX_SEGS * ARAM_SEG_SIZE;
  aram.session_bytes = BE32(totals);
  aram.stream_bytes = BE32(totals + 4);
  aram.peak_bytes = BE32(totals + 8);
  aram.failures = BE32(totals + 12);

  const auto cache_symbol = Resolve(system, "gc_aram_cache");
  if (cache_symbol && (cache_symbol->size == 0 || cache_symbol->size == ARAM_CACHE_SIZE))
  {
    if (const u8* const cache = Pointer(system, cache_symbol->address, ARAM_CACHE_SIZE);
        cache && BE32(cache) != 0)
    {
      aram.has_cache = true;
      aram.cache_budget = BE32(cache + 8);
      aram.cache_bytes = BE32(cache + 12);
      aram.cache_clock = BE32(cache + 16);
      aram.hits = BE32(cache + 20);
      aram.misses = BE32(cache + 24);
      aram.stores = BE32(cache + 28);
      aram.evictions = BE32(cache + 32);
      aram.refusals = BE32(cache + 36);
      for (u32 i = 0; i < ARAM_CACHE_MAX; ++i)
      {
        const u8* const asset = cache + ARAM_CACHE_ASSETS + i * ARAM_ASSET_SIZE;
        if (asset[ARAM_NAME_LENGTH + 14] == 0)
          continue;  // Unused slot
        AramAsset info;
        const char* const name = reinterpret_cast<const char*>(asset);
        info.name = QString::fromLatin1(name, qstrnlen(name, ARAM_NAME_LENGTH));
        info.size = BE32(asset + ARAM_NAME_LENGTH);
        info.aram = BE32(asset + ARAM_NAME_LENGTH + 4);
        info.last_use = BE32(asset + ARAM_NAME_LENGTH + 8);
        info.pins = BE16(asset + ARAM_NAME_LENGTH + 12);
        info.ready = asset[ARAM_NAME_LENGTH + 15] != 0;
        aram.assets.push_back(std::move(info));
      }
    }
  }
  return aram;
}

std::vector<std::pair<std::string, ElfSymbolTable::Symbol>> Reader::LargeObjects(u32 min_size) const
{
  std::vector<std::pair<std::string, ElfSymbolTable::Symbol>> objects;
  if (!m_symbols)
    return objects;
  for (const auto& [name, symbol] : m_symbols->symbols)
  {
    if (!symbol.function && symbol.size >= min_size)
      objects.emplace_back(name, symbol);
  }
  std::sort(objects.begin(), objects.end(),
            [](const auto& a, const auto& b) { return a.second.size > b.second.size; });
  return objects;
}
}  // namespace Xash
