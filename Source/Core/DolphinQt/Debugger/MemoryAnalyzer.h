// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include <QColor>
#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include "Common/CommonTypes.h"

class QTimer;
struct BootParameters;

namespace Core
{
class System;
}

// Memory spaces the memory tools can show.
enum class MemorySpace
{
  MEM1,
  MEM2,
  ARAM,
};
constexpr int MEMORY_SPACE_COUNT = 3;

enum class MemoryRegionKind
{
  System,    // OS globals and exception vectors
  Text,      // Executable code sections
  Data,      // Initialised data sections
  Bss,       // Zero-initialised data
  Arena,     // OS arena: memory the game may allocate from
  Heap,      // An OSAlloc heap (allocated blocks)
  HeapFree,  // Free blocks of an OSAlloc heap (only used for pages)
  Stack,     // A thread's stack
  Reserved,  // OS / FST / IOS reserved memory
  Unknown,
};

QString GetMemoryRegionKindName(MemoryRegionKind kind);
QColor GetMemoryRegionKindColor(MemoryRegionKind kind);

struct MemoryRegion
{
  QString name;
  MemoryRegionKind kind = MemoryRegionKind::Unknown;
  u32 start = 0;  // Address (virtual for MEM1/MEM2, ARAM offset for ARAM)
  u32 end = 0;    // Exclusive
  std::optional<u64> used;  // Bytes in use, where that's meaningful (heaps, stacks)
  QString note;

  u32 Size() const { return end - start; }
  bool Contains(u32 address) const { return address >= start && address < end; }
};

// A section of the running executable, read from its DOL header or ELF section table.
struct ExecutableSection
{
  QString name;
  MemoryRegionKind kind = MemoryRegionKind::Data;
  u32 address = 0;
  u32 size = 0;
};

struct ExecutableLayout
{
  QString source;  // Where the layout came from, e.g. "main.elf" or "Disc DOL"
  u32 entry_point = 0;
  std::vector<ExecutableSection> sections;
};

// Thread stack, from the OS active thread list.
struct ThreadStackInfo
{
  u32 thread = 0;  // OSThread address
  u32 low = 0;     // Lowest stack address (stack end)
  u32 high = 0;    // Highest stack address (stack base)
  u32 sp = 0;      // Saved stack pointer (r1)
  u32 peak = 0;    // Deepest use seen, estimated from untouched (zero) words
  bool current = false;
  bool magic_ok = true;  // STACK_MAGIC still intact at the stack end
};

struct HeapInfo
{
  int index = 0;
  u32 start = 0;
  u32 end = 0;
  u64 allocated = 0;
  u64 free = 0;
  int allocated_blocks = 0;
  int free_blocks = 0;
  bool current = false;
};

struct MemoryUsageSample
{
  double time = 0;  // Seconds since the analyzer started sampling this session
  std::array<u64, MEMORY_SPACE_COUNT> in_use{};   // Non-zero bytes (page granularity)
  std::array<u64, MEMORY_SPACE_COUNT> written{};  // Bytes in pages written since last sample
  std::optional<u64> heap_allocated;
  std::optional<u64> heap_size;
  u64 stack_peak = 0;
};

// Samples emulated memory a couple of times a second and works out what lives where: OS low
// memory, the executable's sections, the arena, OSAlloc heaps (when the game's symbols are
// loaded) and thread stacks. Also tracks per-page activity (written recently, zero / in use) and
// a short usage history for graphs.
//
// Everything is read straight from emulated RAM on the UI thread without pausing the CPU, so the
// results are a best-effort snapshot that can be momentarily inconsistent while the game runs.
class MemoryAnalyzer final : public QObject
{
  Q_OBJECT
public:
  static constexpr u32 PAGE_SIZE = 0x1000;
  static constexpr int SAMPLE_INTERVAL_MS = 500;
  static constexpr size_t HISTORY_LENGTH = 240;  // Two minutes

  explicit MemoryAnalyzer(QObject* parent = nullptr);
  ~MemoryAnalyzer() override;

  // Reads the section layout of what's being booted. Call before BootCore() takes |params|.
  static std::optional<ExecutableLayout> ReadExecutableLayout(const BootParameters& params);
  static std::optional<ExecutableLayout> ReadExecutableLayout(const std::string& path);
  void SetExecutableLayout(std::optional<ExecutableLayout> layout);
  const std::optional<ExecutableLayout>& GetExecutableLayout() const { return m_executable; }

  // Sampling only runs while at least one view wants it.
  void AddViewer(QObject* viewer);
  void RemoveViewer(QObject* viewer);
  void SampleNow();

  bool HasData() const { return m_has_data; }
  bool IsSpaceAvailable(MemorySpace space) const;
  u32 GetSpaceBase(MemorySpace space) const;
  u32 GetSpaceSize(MemorySpace space) const;
  static QString GetSpaceName(MemorySpace space);

  const std::vector<MemoryRegion>& GetRegions(MemorySpace space) const;
  const MemoryRegion* FindRegion(MemorySpace space, u32 address) const;

  // Per-page state, indexed by page number within the space.
  MemoryRegionKind GetPageKind(MemorySpace space, size_t page) const;
  bool IsPageInUse(MemorySpace space, size_t page) const;
  // Seconds since the page last changed, or a negative value if it hasn't changed since sampling
  // began.
  double GetPageWriteAge(MemorySpace space, size_t page) const;
  size_t GetPageCount(MemorySpace space) const;

  const std::vector<ThreadStackInfo>& GetStacks() const { return m_stacks; }
  const std::vector<HeapInfo>& GetHeaps() const { return m_heaps; }
  bool HasHeapSymbols() const { return m_heap_symbols; }
  const std::deque<MemoryUsageSample>& GetHistory() const { return m_history; }

signals:
  void Updated();

private:
  struct SpaceState
  {
    u32 base = 0;
    u32 size = 0;
    std::vector<MemoryRegion> regions;
    std::vector<u64> page_hash;
    std::vector<double> page_written_at;  // Sample time of the last change, -1 if never
    std::vector<u8> page_in_use;
    std::vector<MemoryRegionKind> page_kind;
  };

  void OnEmulationStateChanged();
  void UpdateTimer();
  void Reset();
  void Sample();
  // Returns the bytes written since the previous sample.
  u64 ScanPages(SpaceState& state, const u8* memory, bool first_scan);
  void BuildMem1Regions(Core::System& system);
  void BuildMem2Regions(Core::System& system);
  void BuildAramRegions(Core::System& system);
  void ReadStacks(Core::System& system);
  void ReadHeaps(Core::System& system);
  void ColourPages(SpaceState& state);

  std::optional<u32> ReadU32(Core::System& system, u32 address) const;
  std::optional<u32> ReadSymbolU32(Core::System& system, const char* name) const;

  QTimer* m_timer;
  std::vector<QObject*> m_viewers;
  std::optional<ExecutableLayout> m_executable;

  std::array<SpaceState, MEMORY_SPACE_COUNT> m_spaces;
  std::vector<ThreadStackInfo> m_stacks;
  std::vector<HeapInfo> m_heaps;
  // Per space, pages covered by allocated heap blocks.
  std::array<std::vector<u8>, MEMORY_SPACE_COUNT> m_heap_page_allocated;
  bool m_heap_symbols = false;
  bool m_has_data = false;
  bool m_scanned = false;
  bool m_is_wii = false;
  double m_time = 0;
  QElapsedTimer m_clock;
  qint64 m_last_ms = 0;
  std::deque<MemoryUsageSample> m_history;
};
