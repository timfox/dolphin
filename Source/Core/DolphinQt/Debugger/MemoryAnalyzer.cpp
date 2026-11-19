// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Debugger/MemoryAnalyzer.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <variant>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTimer>

#include "Common/Swap.h"
#include "Common/SymbolDB.h"
#include "Core/Boot/Boot.h"
#include "Core/Core.h"
#include "Core/HW/DSP.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/System.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/VolumeDisc.h"
#include "DolphinQt/Settings.h"

namespace
{
constexpr u32 MEM1_BASE = 0x80000000;
constexpr u32 MEM2_BASE = 0x90000000;

// Dolphin OS globals (YAGCD 4.2.1.4) and Wii globals.
constexpr u32 OS_ARENA_HI = 0x80000034;
constexpr u32 OS_FST_START = 0x80000038;
constexpr u32 OS_DEFAULT_THREAD = 0x800000D8;
constexpr u32 OS_ACTIVE_THREAD_HEAD = 0x800000DC;
constexpr u32 OS_CURRENT_THREAD = 0x800000E4;
constexpr u32 WII_MEM1_ARENA_HI = 0x80003110;
constexpr u32 WII_MEM2_ARENA_LO = 0x80003124;
constexpr u32 WII_MEM2_ARENA_HI = 0x80003128;

// OSThread field offsets (see Core/Debugger/OSThread.h).
constexpr u32 THREAD_SP = 0x04;
constexpr u32 THREAD_LINK_ACTIVE_NEXT = 0x2FC;
constexpr u32 THREAD_STACK_BASE = 0x304;
constexpr u32 THREAD_STACK_END = 0x308;
constexpr u32 STACK_MAGIC = 0xDEADBABE;

constexpr int MAX_THREADS = 128;
constexpr int MAX_HEAPS = 64;
constexpr int MAX_HEAP_CELLS = 1 << 16;

u32 ReadBE32(const u8* data)
{
  return Common::swap32(data);
}
u16 ReadBE16(const u8* data)
{
  return Common::swap16(data);
}

// Maps cached/uncached mirrors onto the cached address.
u32 NormalizeAddress(u32 address)
{
  const u32 top = address & 0xF0000000;
  if (top == 0xC0000000 || top == 0xD0000000)
    return address - 0x40000000;
  return address;
}

std::optional<ExecutableLayout> ParseDolHeader(const u8* header, size_t size)
{
  constexpr int TEXT_SECTIONS = 7;
  constexpr int DATA_SECTIONS = 11;
  if (size < 0x100)
    return std::nullopt;

  ExecutableLayout layout;
  layout.entry_point = ReadBE32(header + 0xE0);
  for (int i = 0; i < TEXT_SECTIONS + DATA_SECTIONS; ++i)
  {
    const bool text = i < TEXT_SECTIONS;
    const int index = text ? i : i - TEXT_SECTIONS;
    const u32 address = ReadBE32(header + 0x48 + i * 4);
    const u32 section_size = ReadBE32(header + 0x90 + i * 4);
    if (section_size == 0 || address == 0)
      continue;
    layout.sections.push_back(
        {text ? QStringLiteral("text%1").arg(index) : QStringLiteral("data%1").arg(index),
         text ? MemoryRegionKind::Text : MemoryRegionKind::Data, address, section_size});
  }
  const u32 bss_address = ReadBE32(header + 0xD8);
  const u32 bss_size = ReadBE32(header + 0xDC);
  if (bss_address != 0 && bss_size != 0)
  {
    layout.sections.push_back(
        {QStringLiteral("bss"), MemoryRegionKind::Bss, bss_address, bss_size});
  }

  if (layout.sections.empty())
    return std::nullopt;
  return layout;
}

// 32-bit big-endian PowerPC ELF: uses the section table (for names like .text, .sdata, .bss),
// falling back to the program headers.
std::optional<ExecutableLayout> ParseElf(const QByteArray& file)
{
  const auto* data = reinterpret_cast<const u8*>(file.constData());
  const size_t size = static_cast<size_t>(file.size());
  if (size < 0x34 || std::memcmp(data, "\x7f" "ELF", 4) != 0 || data[4] != 1 || data[5] != 2)
    return std::nullopt;

  ExecutableLayout layout;
  layout.entry_point = ReadBE32(data + 0x18);

  const u32 shoff = ReadBE32(data + 0x20);
  const u16 shentsize = ReadBE16(data + 0x2E);
  const u16 shnum = ReadBE16(data + 0x30);
  const u16 shstrndx = ReadBE16(data + 0x32);
  const auto section_header = [&](u32 index) -> const u8* {
    const u64 offset = u64(shoff) + u64(index) * shentsize;
    if (shentsize < 0x28 || offset + 0x28 > size)
      return nullptr;
    return data + offset;
  };

  const u8* strtab_header = shstrndx < shnum ? section_header(shstrndx) : nullptr;
  const u32 strtab_offset = strtab_header ? ReadBE32(strtab_header + 0x10) : 0;
  const u32 strtab_size = strtab_header ? ReadBE32(strtab_header + 0x14) : 0;

  constexpr u32 SHF_ALLOC = 0x2;
  constexpr u32 SHF_EXECINSTR = 0x4;
  constexpr u32 SHT_NOBITS = 8;
  for (u32 i = 0; i < shnum; ++i)
  {
    const u8* header = section_header(i);
    if (!header)
      break;
    const u32 name = ReadBE32(header + 0x00);
    const u32 type = ReadBE32(header + 0x04);
    const u32 flags = ReadBE32(header + 0x08);
    const u32 address = ReadBE32(header + 0x0C);
    const u32 section_size = ReadBE32(header + 0x14);
    if (!(flags & SHF_ALLOC) || address == 0 || section_size == 0)
      continue;

    QString section_name = QStringLiteral("section%1").arg(i);
    if (strtab_header && name < strtab_size && u64(strtab_offset) + strtab_size <= size)
    {
      const char* const start = reinterpret_cast<const char*>(data + strtab_offset + name);
      section_name = QString::fromLatin1(start, qstrnlen(start, strtab_size - name));
    }

    MemoryRegionKind kind = MemoryRegionKind::Data;
    if (flags & SHF_EXECINSTR)
      kind = MemoryRegionKind::Text;
    else if (type == SHT_NOBITS)
      kind = MemoryRegionKind::Bss;
    layout.sections.push_back({section_name, kind, address, section_size});
  }

  if (layout.sections.empty())
  {
    const u32 phoff = ReadBE32(data + 0x1C);
    const u16 phentsize = ReadBE16(data + 0x2A);
    const u16 phnum = ReadBE16(data + 0x2C);
    for (u32 i = 0; i < phnum; ++i)
    {
      const u64 offset = u64(phoff) + u64(i) * phentsize;
      if (phentsize < 0x20 || offset + 0x20 > size)
        break;
      const u8* const header = data + offset;
      constexpr u32 PT_LOAD = 1;
      constexpr u32 PF_X = 1;
      if (ReadBE32(header) != PT_LOAD)
        continue;
      const u32 address = ReadBE32(header + 0x08);
      const u32 file_size = ReadBE32(header + 0x10);
      const u32 mem_size = ReadBE32(header + 0x14);
      const bool exec = ReadBE32(header + 0x18) & PF_X;
      if (file_size != 0)
      {
        layout.sections.push_back({QStringLiteral("segment%1").arg(i),
                                   exec ? MemoryRegionKind::Text : MemoryRegionKind::Data, address,
                                   file_size});
      }
      if (mem_size > file_size)
      {
        layout.sections.push_back({QStringLiteral("segment%1 bss").arg(i), MemoryRegionKind::Bss,
                                   address + file_size, mem_size - file_size});
      }
    }
  }

  if (layout.sections.empty())
    return std::nullopt;
  return layout;
}

QString FormatAddress(u32 address)
{
  return QStringLiteral("%1").arg(address, 8, 16, QLatin1Char('0')).toUpper();
}
}  // namespace

QString GetMemoryRegionKindName(MemoryRegionKind kind)
{
  switch (kind)
  {
  case MemoryRegionKind::System:
    return QObject::tr("System");
  case MemoryRegionKind::Text:
    return QObject::tr("Code");
  case MemoryRegionKind::Data:
    return QObject::tr("Data");
  case MemoryRegionKind::Bss:
    return QObject::tr("BSS");
  case MemoryRegionKind::Arena:
    return QObject::tr("Arena");
  case MemoryRegionKind::Heap:
    return QObject::tr("Heap (allocated)");
  case MemoryRegionKind::HeapFree:
    return QObject::tr("Heap (free)");
  case MemoryRegionKind::Stack:
    return QObject::tr("Stack");
  case MemoryRegionKind::Reserved:
    return QObject::tr("Reserved");
  case MemoryRegionKind::Unknown:
  default:
    return QObject::tr("Unclassified");
  }
}

QColor GetMemoryRegionKindColor(MemoryRegionKind kind)
{
  switch (kind)
  {
  case MemoryRegionKind::System:
    return QColor(0x8a, 0x93, 0xb8);
  case MemoryRegionKind::Text:
    return QColor(0x8b, 0x7c, 0xff);
  case MemoryRegionKind::Data:
    return QColor(0x3f, 0xc8, 0xd8);
  case MemoryRegionKind::Bss:
    return QColor(0xf2, 0xb3, 0x3d);
  case MemoryRegionKind::Arena:
    return QColor(0x4a, 0x5f, 0x9a);
  case MemoryRegionKind::Heap:
    return QColor(0x5c, 0xd6, 0x7a);
  case MemoryRegionKind::HeapFree:
    return QColor(0x2a, 0x6e, 0x45);
  case MemoryRegionKind::Stack:
    return QColor(0xff, 0x6b, 0x9a);
  case MemoryRegionKind::Reserved:
    return QColor(0x6e, 0x66, 0x8c);
  case MemoryRegionKind::Unknown:
  default:
    return QColor(0x3a, 0x35, 0x55);
  }
}

MemoryAnalyzer::MemoryAnalyzer(QObject* parent) : QObject(parent), m_timer(new QTimer(this))
{
  m_timer->setInterval(SAMPLE_INTERVAL_MS);
  connect(m_timer, &QTimer::timeout, this, &MemoryAnalyzer::Sample);
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &MemoryAnalyzer::OnEmulationStateChanged);
}

MemoryAnalyzer::~MemoryAnalyzer() = default;

std::optional<ExecutableLayout> MemoryAnalyzer::ReadExecutableLayout(const std::string& path)
{
  const QFileInfo info(QString::fromStdString(path));
  const QString suffix = info.suffix().toLower();

  const auto read_file = [](const QString& file_path, qint64 max_size) -> QByteArray {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly))
      return {};
    return file.read(max_size);
  };

  if (suffix == QStringLiteral("elf"))
  {
    auto layout = ParseElf(read_file(info.absoluteFilePath(), 256 * 1024 * 1024));
    if (layout)
      layout->source = info.fileName();
    return layout;
  }

  if (suffix != QStringLiteral("dol"))
    return std::nullopt;

  const QByteArray header = read_file(info.absoluteFilePath(), 0x100);
  auto layout = ParseDolHeader(reinterpret_cast<const u8*>(header.constData()),
                               static_cast<size_t>(header.size()));
  if (!layout)
    return std::nullopt;
  layout->source = info.fileName();

  // Homebrew toolchains write an ELF next to the DOL. Its section table has real names (.text,
  // .rodata, .sdata, .sbss...), so prefer it when it's the same program.
  const QString elf_path = info.absolutePath() + QLatin1Char('/') + info.completeBaseName() +
                           QStringLiteral(".elf");
  if (QFileInfo::exists(elf_path))
  {
    auto elf_layout = ParseElf(read_file(elf_path, 256 * 1024 * 1024));
    if (elf_layout && elf_layout->entry_point == layout->entry_point)
    {
      elf_layout->source =
          QObject::tr("%1 (sections from %2)").arg(info.fileName(), QFileInfo(elf_path).fileName());
      return elf_layout;
    }
  }
  return layout;
}

std::optional<ExecutableLayout> MemoryAnalyzer::ReadExecutableLayout(const BootParameters& params)
{
  if (const auto* executable = std::get_if<BootParameters::Executable>(&params.parameters))
    return ReadExecutableLayout(executable->path);

  if (const auto* disc = std::get_if<BootParameters::Disc>(&params.parameters))
  {
    if (!disc->volume)
      return std::nullopt;
    const DiscIO::Partition partition = disc->volume->GetGamePartition();
    const std::optional<u64> dol_offset = DiscIO::GetBootDOLOffset(*disc->volume, partition);
    if (!dol_offset)
      return std::nullopt;
    std::array<u8, 0x100> header{};
    if (!disc->volume->Read(*dol_offset, header.size(), header.data(), partition))
      return std::nullopt;
    auto layout = ParseDolHeader(header.data(), header.size());
    if (layout)
      layout->source = QObject::tr("Main DOL on disc");
    return layout;
  }

  return std::nullopt;
}

std::string MemoryAnalyzer::GetBootPath(const BootParameters& params)
{
  if (const auto* executable = std::get_if<BootParameters::Executable>(&params.parameters))
    return executable->path;
  if (const auto* disc = std::get_if<BootParameters::Disc>(&params.parameters))
    return disc->path;
  return {};
}

void MemoryAnalyzer::SetBootPath(const std::string& path, const std::string& game_id)
{
  m_boot_path = path;
  m_game_id = game_id;
  m_symbol_candidates.clear();
  m_candidates_searched = false;
  // Symbols belong to one program. A file picked by hand is remembered for this boot path and
  // comes back through the candidate search, which checks it against the code in RAM first.
  m_xash_reader.SetSymbols(std::nullopt);
  m_engine_symbols_status.clear();
}

static QString EngineSymbolsSettingsKey(const std::string& game_id, const std::string& boot_path)
{
  const std::string key = game_id.empty() ? boot_path : game_id;
  return QStringLiteral("debugger/enginesymbols/") +
         QString::fromUtf8(QByteArray::fromStdString(key).toHex());
}

QString MemoryAnalyzer::LoadEngineSymbols(const QString& elf_path)
{
  auto table = LoadElfSymbolTable(elf_path);
  if (!table)
    return tr("%1 has no symbol table. Pick the unstripped ELF the build writes before stripping "
              "(xash3d-gc: build/engine/xash).")
        .arg(QFileInfo(elf_path).fileName());

  Core::System& system = Core::System::GetInstance();
  const Core::State state = Core::GetState(system);
  if ((state == Core::State::Running || state == Core::State::Paused) &&
      !table->entry_code.isEmpty())
  {
    const QByteArray running = PeekBytes(table->entry_point, table->entry_code.size());
    if (running != table->entry_code)
    {
      return tr("%1 doesn't match the running program (the code at its entry point %2 is "
                "different). Rebuild the disc, or pick the ELF it was made from.")
          .arg(QFileInfo(elf_path).fileName())
          .arg(table->entry_point, 8, 16, QLatin1Char('0'));
    }
  }

  m_engine_symbols_status = tr("%1 symbols from %2")
                                .arg(table->symbols.size())
                                .arg(QDir::toNativeSeparators(elf_path));
  m_xash_reader.SetSymbols(std::move(table));
  Settings::GetQSettings().setValue(EngineSymbolsSettingsKey(m_game_id, m_boot_path), elf_path);
  if (m_has_data)
    SampleNow();
  return {};
}

void MemoryAnalyzer::UnloadEngineSymbols()
{
  m_xash_reader.SetSymbols(std::nullopt);
  m_engine_symbols_status.clear();
  Settings::GetQSettings().remove(EngineSymbolsSettingsKey(m_game_id, m_boot_path));
  if (m_has_data)
    SampleNow();
}

QString MemoryAnalyzer::GetEngineSymbolsPath() const
{
  const auto& symbols = m_xash_reader.GetSymbols();
  return symbols ? symbols->path : QString();
}

// Looks for an unstripped ELF beside what was booted: the booted ELF itself, foo.elf next to
// foo.dol, the path used last time for this game, and xash3d-gc's build tree (OUT/... next to
// build/engine/xash).
void MemoryAnalyzer::TryEngineSymbolCandidates(Core::System& system)
{
  if (m_xash_reader.GetSymbols() || m_boot_path.empty())
    return;

  if (!m_candidates_searched)
  {
    m_candidates_searched = true;
    QStringList paths;
    const QString remembered =
        Settings::GetQSettings().value(EngineSymbolsSettingsKey(m_game_id, m_boot_path)).toString();
    if (!remembered.isEmpty())
      paths << remembered;

    const QFileInfo boot(QString::fromStdString(m_boot_path));
    const QString suffix = boot.suffix().toLower();
    if (suffix == QStringLiteral("elf"))
      paths << boot.absoluteFilePath();
    paths << boot.absolutePath() + QLatin1Char('/') + boot.completeBaseName() +
                 QStringLiteral(".elf");
    QDir dir = boot.absoluteDir();
    for (int up = 0; up < 3; ++up)
    {
      paths << dir.absoluteFilePath(QStringLiteral("build/engine/xash"));
      if (!dir.cdUp())
        break;
    }

    QStringList seen;
    for (const QString& path : paths)
    {
      const QString clean = QDir::cleanPath(path);
      if (seen.contains(clean) || !QFileInfo(clean).isFile())
        continue;
      seen << clean;
      if (auto table = LoadElfSymbolTable(clean))
        m_symbol_candidates.push_back(std::move(*table));
    }
  }

  // The program has to be in RAM before its code can be compared, so keep trying.
  for (auto it = m_symbol_candidates.begin(); it != m_symbol_candidates.end(); ++it)
  {
    if (it->entry_code.isEmpty() ||
        PeekBytes(it->entry_point, it->entry_code.size()) != it->entry_code)
    {
      continue;
    }
    m_engine_symbols_status = tr("%1 symbols from %2 (found automatically)")
                                  .arg(it->symbols.size())
                                  .arg(QDir::toNativeSeparators(it->path));
    m_xash_reader.SetSymbols(std::move(*it));
    m_symbol_candidates.clear();
    return;
  }
  (void)system;
}

void MemoryAnalyzer::SampleXash(Core::System& system)
{
  TryEngineSymbolCandidates(system);
  m_xash = m_xash_reader.Read(system);
  if (!m_xash || !m_xash->pools_valid)
    return;

  std::unordered_map<QString, u64> pools;
  for (const Xash::Pool& pool : m_xash->pools)
    pools[pool.name] += pool.real;

  const QString map = m_xash->map.isEmpty() ? tr("(none)") : m_xash->map;
  if (m_xash_visits.empty() || m_xash_visits.back().map != map)
  {
    XashMapVisit visit;
    visit.map = map;
    visit.entered = m_time;
    visit.zone_at_entry = m_xash->zone_real;
    m_xash_visits.push_back(std::move(visit));
    constexpr size_t MAX_VISITS = 200;
    if (m_xash_visits.size() > MAX_VISITS)
      m_xash_visits.erase(m_xash_visits.begin());
  }
  XashMapVisit& visit = m_xash_visits.back();
  visit.last_seen = m_time;
  visit.zone_now = m_xash->zone_real;
  visit.zone_peak = std::max(visit.zone_peak, m_xash->zone_real);
  visit.heap_now = m_xash->heap ? m_xash->heap->used : 0;
  visit.aram_now = m_xash->aram ? m_xash->aram->session_bytes + m_xash->aram->stream_bytes : 0;
  visit.pools = std::move(pools);
}

void MemoryAnalyzer::SetExecutableLayout(std::optional<ExecutableLayout> layout)
{
  m_executable = std::move(layout);
  if (m_has_data)
    SampleNow();
}

void MemoryAnalyzer::AddViewer(QObject* viewer)
{
  if (std::find(m_viewers.begin(), m_viewers.end(), viewer) == m_viewers.end())
    m_viewers.push_back(viewer);
  UpdateTimer();
  SampleNow();
}

void MemoryAnalyzer::RemoveViewer(QObject* viewer)
{
  std::erase(m_viewers, viewer);
  UpdateTimer();
}

void MemoryAnalyzer::UpdateTimer()
{
  if (m_viewers.empty())
    m_timer->stop();
  else if (!m_timer->isActive())
    m_timer->start();
}

void MemoryAnalyzer::SampleNow()
{
  Sample();
}

void MemoryAnalyzer::OnEmulationStateChanged()
{
  const Core::State state = Core::GetState(Core::System::GetInstance());
  if (state == Core::State::Starting || state == Core::State::Uninitialized ||
      state == Core::State::Stopping)
  {
    Reset();
    if (state == Core::State::Uninitialized)
      m_executable.reset();
    emit Updated();
    return;
  }
  if (!m_viewers.empty())
    Sample();
}

void MemoryAnalyzer::Reset()
{
  for (SpaceState& space : m_spaces)
    space = {};
  m_stacks.clear();
  m_heaps.clear();
  for (auto& mask : m_heap_page_allocated)
    mask.clear();
  m_blocks.clear();
  m_blocks_truncated = false;
  m_block_first_seen.clear();
  m_mark.reset();
  m_history.clear();
  m_xash.reset();
  m_xash_visits.clear();
  m_xash_mark_pools.clear();
  m_xash_reader.ClearCache();
  m_heap_symbols = false;
  m_has_data = false;
  m_scanned = false;
  m_time = 0;
}

bool MemoryAnalyzer::IsSpaceAvailable(MemorySpace space) const
{
  return m_has_data && m_spaces[static_cast<int>(space)].size != 0;
}

u32 MemoryAnalyzer::GetSpaceBase(MemorySpace space) const
{
  return m_spaces[static_cast<int>(space)].base;
}

u32 MemoryAnalyzer::GetSpaceSize(MemorySpace space) const
{
  return m_spaces[static_cast<int>(space)].size;
}

QString MemoryAnalyzer::GetSpaceName(MemorySpace space)
{
  switch (space)
  {
  case MemorySpace::MEM1:
    return tr("MEM1");
  case MemorySpace::MEM2:
    return tr("MEM2");
  case MemorySpace::ARAM:
  default:
    return tr("ARAM");
  }
}

const std::vector<MemoryRegion>& MemoryAnalyzer::GetRegions(MemorySpace space) const
{
  return m_spaces[static_cast<int>(space)].regions;
}

const MemoryRegion* MemoryAnalyzer::FindRegion(MemorySpace space, u32 address) const
{
  // The smallest region containing the address is the most specific one.
  const MemoryRegion* best = nullptr;
  for (const MemoryRegion& region : GetRegions(space))
  {
    if (region.Contains(address) && (!best || region.Size() < best->Size()))
      best = &region;
  }
  return best;
}

size_t MemoryAnalyzer::GetPageCount(MemorySpace space) const
{
  return m_spaces[static_cast<int>(space)].page_kind.size();
}

MemoryRegionKind MemoryAnalyzer::GetPageKind(MemorySpace space, size_t page) const
{
  const auto& kinds = m_spaces[static_cast<int>(space)].page_kind;
  return page < kinds.size() ? kinds[page] : MemoryRegionKind::Unknown;
}

bool MemoryAnalyzer::IsPageInUse(MemorySpace space, size_t page) const
{
  const auto& in_use = m_spaces[static_cast<int>(space)].page_in_use;
  return page < in_use.size() && in_use[page] != 0;
}

double MemoryAnalyzer::GetPageWriteAge(MemorySpace space, size_t page) const
{
  const auto& written = m_spaces[static_cast<int>(space)].page_written_at;
  if (page >= written.size() || written[page] < 0)
    return -1;
  return m_time - written[page];
}

std::optional<u32> MemoryAnalyzer::ReadU32(Core::System& system, u32 address) const
{
  address = NormalizeAddress(address);
  if (address & 3)
    return std::nullopt;
  auto& memory = system.GetMemory();
  const u32 offset = address & 0x0FFFFFFF;
  const u32 top = address & 0xF0000000;
  if (top == MEM1_BASE && memory.GetRAM() && offset + 4 <= memory.GetRamSizeReal())
    return Common::swap32(memory.GetRAM() + offset);
  if (top == MEM2_BASE && m_is_wii && memory.GetEXRAM() && offset + 4 <= memory.GetExRamSizeReal())
    return Common::swap32(memory.GetEXRAM() + offset);
  return std::nullopt;
}

std::optional<u32> MemoryAnalyzer::ReadSymbolU32(Core::System& system, const char* name) const
{
  const Common::Symbol* const symbol = system.GetPPCSymbolDB().GetSymbolFromName(name);
  if (!symbol)
    return std::nullopt;
  return ReadU32(system, symbol->address);
}

u64 MemoryAnalyzer::ScanPages(SpaceState& state, const u8* memory, bool first_scan)
{
  const size_t pages = state.size / PAGE_SIZE;
  if (state.page_hash.size() != pages)
  {
    state.page_hash.assign(pages, 0);
    state.page_written_at.assign(pages, -1.0);
    state.page_in_use.assign(pages, 0);
    first_scan = true;
  }

  u64 written = 0;
  for (size_t page = 0; page < pages; ++page)
  {
    const u8* const data = memory + page * PAGE_SIZE;
    u64 hash = 0xcbf29ce484222325ULL;
    u64 any = 0;
    for (u32 i = 0; i < PAGE_SIZE; i += 8)
    {
      u64 word;
      std::memcpy(&word, data + i, sizeof(word));
      any |= word;
      hash = (hash ^ word) * 0x100000001b3ULL;
    }
    state.page_in_use[page] = any != 0;
    if (!first_scan && hash != state.page_hash[page])
    {
      state.page_written_at[page] = m_time;
      written += PAGE_SIZE;
    }
    state.page_hash[page] = hash;
  }
  return written;
}

void MemoryAnalyzer::Sample()
{
  Core::System& system = Core::System::GetInstance();
  const Core::State state = Core::GetState(system);
  if (state != Core::State::Running && state != Core::State::Paused)
  {
    if (m_has_data)
    {
      Reset();
      emit Updated();
    }
    return;
  }

  auto& memory = system.GetMemory();
  if (!memory.GetRAM())
    return;

  if (!m_clock.isValid())
    m_clock.start();
  const qint64 now_ms = m_clock.elapsed();
  const bool running = state == Core::State::Running;
  const bool first_scan = !m_scanned;
  if (running && !first_scan)
    m_time += std::min<qint64>(now_ms - m_last_ms, 5000) / 1000.0;
  m_last_ms = now_ms;

  m_is_wii = system.IsWii();

  SpaceState& mem1 = m_spaces[static_cast<int>(MemorySpace::MEM1)];
  mem1.base = MEM1_BASE;
  mem1.size = memory.GetRamSizeReal();

  SpaceState& mem2 = m_spaces[static_cast<int>(MemorySpace::MEM2)];
  mem2.base = MEM2_BASE;
  mem2.size = m_is_wii && memory.GetEXRAM() ? memory.GetExRamSizeReal() : 0;

  SpaceState& aram = m_spaces[static_cast<int>(MemorySpace::ARAM)];
  auto& dsp = system.GetDSP();
  aram.base = 0;
  aram.size = !m_is_wii && dsp.GetARAMPtr() ? dsp.GetARAMSize() : 0;

  MemoryUsageSample sample;
  sample.time = m_time;
  sample.written[static_cast<int>(MemorySpace::MEM1)] =
      ScanPages(mem1, memory.GetRAM(), first_scan);
  if (mem2.size)
  {
    sample.written[static_cast<int>(MemorySpace::MEM2)] =
        ScanPages(mem2, memory.GetEXRAM(), first_scan);
  }
  if (aram.size)
  {
    sample.written[static_cast<int>(MemorySpace::ARAM)] =
        ScanPages(aram, dsp.GetARAMPtr(), first_scan);
  }
  m_scanned = true;

  for (SpaceState& space : m_spaces)
    space.regions.clear();
  ReadHeaps(system);
  ReadStacks(system);
  SampleXash(system);
  BuildMem1Regions(system);
  if (mem2.size)
    BuildMem2Regions(system);
  if (aram.size)
    BuildAramRegions(system);
  if (m_xash)
  {
    BuildXashMem1Regions();
    if (aram.size)
      BuildXashAramRegions();
  }

  for (size_t i = 0; i < m_spaces.size(); ++i)
  {
    SpaceState& space = m_spaces[i];
    std::sort(space.regions.begin(), space.regions.end(),
              [](const MemoryRegion& a, const MemoryRegion& b) {
                return a.start != b.start ? a.start < b.start : a.Size() > b.Size();
              });
    ColourPages(space);
    sample.in_use[i] = u64(std::count(space.page_in_use.begin(), space.page_in_use.end(), 1)) *
                       PAGE_SIZE;
  }

  if (m_heap_symbols)
  {
    u64 allocated = 0, size = 0;
    for (const HeapInfo& heap : m_heaps)
    {
      allocated += heap.allocated;
      size += heap.end - heap.start;
    }
    sample.heap_allocated = allocated;
    sample.heap_size = size;
  }
  else if (m_xash && m_xash->heap)
  {
    // libogc games have no OSAlloc heaps; the C heap is the equivalent.
    sample.heap_allocated = m_xash->heap->used;
    sample.heap_size = m_xash->heap->end - m_xash->heap->start;
  }
  if (m_xash && m_xash->pools_valid)
    sample.zone_allocated = m_xash->zone_real;
  for (const ThreadStackInfo& stack : m_stacks)
    sample.stack_peak = std::max<u64>(sample.stack_peak, stack.peak);

  // Only running time goes on the graph: while paused, nothing changes.
  if (running || m_history.empty() || first_scan)
  {
    if (!m_history.empty() && m_history.back().time == sample.time)
      m_history.back() = sample;
    else
      m_history.push_back(sample);
    while (m_history.size() > HISTORY_LENGTH)
      m_history.pop_front();
  }

  m_has_data = true;
  emit Updated();
}

void MemoryAnalyzer::ReadHeaps(Core::System& system)
{
  m_heaps.clear();
  for (auto& mask : m_heap_page_allocated)
    mask.clear();
  m_blocks.clear();
  m_blocks_truncated = false;
  std::unordered_map<u64, double> first_seen;

  // OSAlloc keeps its heaps in a static array. Its symbols are only known when the game's symbol
  // map is loaded (always the case for homebrew built with symbols).
  const std::optional<u32> heap_array = ReadSymbolU32(system, "HeapArray");
  const std::optional<u32> num_heaps = ReadSymbolU32(system, "NumHeaps");
  m_heap_symbols = heap_array && num_heaps && *heap_array != 0;
  if (!m_heap_symbols)
    return;

  const std::optional<u32> current = ReadSymbolU32(system, "__OSCurrHeap");
  const int count = std::min<int>(static_cast<int>(*num_heaps), MAX_HEAPS);
  for (int i = 0; i < count; ++i)
  {
    const u32 desc = *heap_array + i * 12;
    const std::optional<u32> size = ReadU32(system, desc);
    if (!size || static_cast<s32>(*size) < 0)
      continue;

    HeapInfo heap;
    heap.index = i;
    heap.current = current && static_cast<s32>(*current) == i;
    u32 low = std::numeric_limits<u32>::max(), high = 0;

    const auto walk = [&](u32 cell, bool allocated) {
      for (int n = 0; cell != 0 && n < MAX_HEAP_CELLS; ++n)
      {
        const std::optional<u32> next = ReadU32(system, cell + 4);
        const std::optional<u32> cell_size = ReadU32(system, cell + 8);
        if (!next || !cell_size || *cell_size == 0 || *cell_size > 0x04000000)
          break;
        low = std::min(low, NormalizeAddress(cell));
        high = std::max(high, NormalizeAddress(cell) + *cell_size);
        if (allocated)
        {
          heap.allocated += *cell_size;
          ++heap.allocated_blocks;
          if (m_blocks.size() < MAX_TRACKED_BLOCKS)
          {
            // A block is identified by its address and size; a block freed and reallocated at
            // the same place with the same size counts as the same block.
            const u64 key = (u64(NormalizeAddress(cell)) << 32) | *cell_size;
            const auto seen = m_block_first_seen.find(key);
            const double since = seen != m_block_first_seen.end() ? seen->second : m_time;
            first_seen[key] = since;
            m_blocks.push_back({NormalizeAddress(cell), *cell_size, i, since});
          }
          else
          {
            m_blocks_truncated = true;
          }
          const u32 address = NormalizeAddress(cell);
          for (int s = 0; s < MEMORY_SPACE_COUNT; ++s)
          {
            const SpaceState& space = m_spaces[s];
            if (address < space.base || address >= space.base + space.size || s == 2)
              continue;
            auto& mask = m_heap_page_allocated[s];
            mask.resize(space.size / PAGE_SIZE, 0);
            const u32 first = (address - space.base) / PAGE_SIZE;
            const u32 last =
                std::min<u32>((address - space.base + *cell_size - 1) / PAGE_SIZE,
                              static_cast<u32>(mask.size()) - 1);
            for (u32 p = first; p <= last; ++p)
              mask[p] = 1;
          }
        }
        else
        {
          heap.free += *cell_size;
          ++heap.free_blocks;
        }
        cell = *next;
      }
    };

    walk(ReadU32(system, desc + 4).value_or(0), false);
    walk(ReadU32(system, desc + 8).value_or(0), true);

    if (high <= low)
      continue;
    heap.start = low;
    heap.end = high;
    m_heaps.push_back(heap);
  }
  m_block_first_seen = std::move(first_seen);
}

QByteArray MemoryAnalyzer::PeekBytes(u32 address, u32 length) const
{
  if (!m_has_data)
    return {};
  auto& memory = Core::System::GetInstance().GetMemory();
  address = NormalizeAddress(address);
  const u32 offset = address & 0x0FFFFFFF;
  const u8* base = nullptr;
  u32 size = 0;
  if ((address & 0xF0000000) == MEM1_BASE)
  {
    base = memory.GetRAM();
    size = memory.GetRamSizeReal();
  }
  else if ((address & 0xF0000000) == MEM2_BASE && m_is_wii)
  {
    base = memory.GetEXRAM();
    size = memory.GetExRamSizeReal();
  }
  if (!base || offset >= size)
    return {};
  length = std::min(length, size - offset);
  return QByteArray(reinterpret_cast<const char*>(base + offset), static_cast<int>(length));
}

void MemoryAnalyzer::SetMark()
{
  if (!m_has_data)
    return;
  MemoryUsageSample mark = m_history.empty() ? MemoryUsageSample{} : m_history.back();
  mark.time = m_time;
  m_mark = mark;
  m_xash_mark_pools.clear();
  if (m_xash)
  {
    for (const Xash::Pool& pool : m_xash->pools)
      m_xash_mark_pools[pool.name] += pool.real;
  }
  emit Updated();
}

void MemoryAnalyzer::ClearMark()
{
  m_mark.reset();
  m_xash_mark_pools.clear();
  emit Updated();
}

void MemoryAnalyzer::ReadStacks(Core::System& system)
{
  m_stacks.clear();
  const u32 current = ReadU32(system, OS_CURRENT_THREAD).value_or(0);
  u32 thread = ReadU32(system, OS_ACTIVE_THREAD_HEAD).value_or(0);
  for (int n = 0; thread != 0 && n < MAX_THREADS; ++n)
  {
    const std::optional<u32> base = ReadU32(system, thread + THREAD_STACK_BASE);
    const std::optional<u32> end = ReadU32(system, thread + THREAD_STACK_END);
    const std::optional<u32> next = ReadU32(system, thread + THREAD_LINK_ACTIVE_NEXT);
    if (!base || !end || !next)
      break;

    const u32 high = NormalizeAddress(*base);
    const u32 low = NormalizeAddress(*end);
    if (high > low && high - low <= 0x01000000 && ReadU32(system, low))
    {
      ThreadStackInfo stack;
      stack.thread = thread;
      stack.high = high;
      stack.low = low;
      stack.sp = ReadU32(system, thread + THREAD_SP).value_or(0);
      stack.current = thread == current;
      stack.magic_ok = ReadU32(system, low) == STACK_MAGIC;

      // Stacks usually start out zeroed, so the lowest non-zero word below the magic marks the
      // deepest the stack has grown.
      u32 address = (low + 4 + 3) & ~3u;
      while (address < high && ReadU32(system, address).value_or(1) == 0)
        address += 4;
      stack.peak = high - std::min(address, high);
      m_stacks.push_back(stack);
    }
    if (*next == thread)
      break;
    thread = *next;
  }
}

void MemoryAnalyzer::BuildMem1Regions(Core::System& system)
{
  SpaceState& space = m_spaces[static_cast<int>(MemorySpace::MEM1)];
  const u32 mem1_end = space.base + space.size;
  auto& regions = space.regions;

  const u32 low_end = m_is_wii ? 0x80003400 : 0x80003100;
  regions.push_back({tr("OS globals"), MemoryRegionKind::System, 0x80000000, 0x80000100, {},
                     tr("Boot info, console type, arena and thread pointers")});
  regions.push_back({tr("Exception vectors"), MemoryRegionKind::System, 0x80000100, 0x80003000,
                     {}, tr("PowerPC interrupt handlers")});
  regions.push_back({m_is_wii ? tr("OS / IOS globals") : tr("OS globals (2)"),
                     MemoryRegionKind::System, 0x80003000, low_end, {}, {}});

  u32 exe_start = mem1_end, exe_end = low_end;
  if (m_executable)
  {
    for (const ExecutableSection& section : m_executable->sections)
    {
      const u32 start = NormalizeAddress(section.address);
      if (start < space.base || start >= mem1_end)
        continue;
      const u32 end = std::min<u32>(start + section.size, mem1_end);
      regions.push_back({section.name, section.kind, start, end, {},
                         tr("From %1").arg(m_executable->source)});
      exe_start = std::min(exe_start, start);
      exe_end = std::max(exe_end, end);
    }
  }
  if (exe_start > low_end && exe_start < mem1_end)
    regions.push_back({tr("Reserved"), MemoryRegionKind::Reserved, low_end, exe_start, {}, {}});

  // The arena runs from the end of the program to ArenaHi; the OS keeps the top of MEM1 (FST,
  // debugger and boot data) for itself.
  const auto valid_hi = [&](std::optional<u32> value) -> std::optional<u32> {
    if (!value)
      return std::nullopt;
    const u32 address = NormalizeAddress(*value);
    if (address <= exe_end || address > mem1_end)
      return std::nullopt;
    return address;
  };
  const std::optional<u32> symbol_lo = ReadSymbolU32(system, "__OSArenaLo");
  std::optional<u32> arena_hi = valid_hi(ReadSymbolU32(system, "__OSArenaHi"));
  QString hi_source = tr("from __OSArenaHi");
  if (!arena_hi && m_is_wii)
  {
    arena_hi = valid_hi(ReadU32(system, WII_MEM1_ARENA_HI));
    hi_source = tr("from OS globals");
  }
  if (!arena_hi)
  {
    arena_hi = valid_hi(ReadU32(system, OS_ARENA_HI));
    hi_source = tr("from OS globals");
  }
  if (!arena_hi)
  {
    arena_hi = valid_hi(ReadU32(system, OS_FST_START));
    hi_source = tr("estimated from the FST address");
  }
  if (!arena_hi)
  {
    arena_hi = mem1_end;
    hi_source = tr("estimated");
  }

  const u32 arena_lo = (exe_end + 31) & ~31u;
  if (*arena_hi > arena_lo)
  {
    QString note = tr("Free for the game to allocate; ArenaHi %1").arg(hi_source);
    if (symbol_lo)
      note += tr(", current ArenaLo %1").arg(FormatAddress(*symbol_lo));
    regions.push_back({tr("Arena"), MemoryRegionKind::Arena, arena_lo, *arena_hi, {}, note});
  }
  if (*arena_hi < mem1_end)
  {
    regions.push_back({tr("OS reserved / FST"), MemoryRegionKind::Reserved, *arena_hi, mem1_end,
                       {}, tr("File system table, boot and debugger data")});
  }

  const auto in_space = [&](u32 address) { return address >= space.base && address < mem1_end; };
  for (const HeapInfo& heap : m_heaps)
  {
    if (!in_space(heap.start))
      continue;
    regions.push_back({tr("Heap %1").arg(heap.index), MemoryRegionKind::Heap, heap.start,
                       std::min(heap.end, mem1_end), heap.allocated,
                       tr("%1 blocks allocated, %2 free%3")
                           .arg(heap.allocated_blocks)
                           .arg(heap.free_blocks)
                           .arg(heap.current ? tr(", current heap") : QString())});
  }

  const u32 default_thread = ReadU32(system, OS_DEFAULT_THREAD).value_or(0);
  for (const ThreadStackInfo& stack : m_stacks)
  {
    if (!in_space(stack.low))
      continue;
    const QString name = stack.thread == default_thread ?
                             tr("Main thread stack") :
                             tr("Thread %1 stack").arg(FormatAddress(stack.thread));
    regions.push_back({name, MemoryRegionKind::Stack, stack.low, std::min(stack.high, mem1_end),
                       stack.peak,
                       stack.magic_ok ? tr("Peak use estimated from untouched stack") :
                                        tr("Stack overflow: the end marker was overwritten")});
  }
}

void MemoryAnalyzer::BuildMem2Regions(Core::System& system)
{
  SpaceState& space = m_spaces[static_cast<int>(MemorySpace::MEM2)];
  const u32 mem2_end = space.base + space.size;
  auto& regions = space.regions;

  const auto in_mem2 = [&](std::optional<u32> value) -> std::optional<u32> {
    if (!value)
      return std::nullopt;
    const u32 address = NormalizeAddress(*value);
    if (address < space.base || address > mem2_end)
      return std::nullopt;
    return address;
  };
  const std::optional<u32> lo = in_mem2(ReadU32(system, WII_MEM2_ARENA_LO));
  const std::optional<u32> hi = in_mem2(ReadU32(system, WII_MEM2_ARENA_HI));
  if (lo && hi && *hi > *lo)
  {
    if (*lo > space.base)
      regions.push_back({tr("Reserved"), MemoryRegionKind::Reserved, space.base, *lo, {}, {}});
    regions.push_back({tr("MEM2 arena"), MemoryRegionKind::Arena, *lo, *hi, {},
                       tr("Free for the game to allocate")});
    if (*hi < mem2_end)
    {
      regions.push_back({tr("IOS reserved"), MemoryRegionKind::Reserved, *hi, mem2_end, {},
                         tr("Used by IOS")});
    }
  }
  else
  {
    regions.push_back({tr("MEM2"), MemoryRegionKind::Arena, space.base, mem2_end, {}, {}});
  }

  if (m_executable)
  {
    for (const ExecutableSection& section : m_executable->sections)
    {
      const u32 start = NormalizeAddress(section.address);
      if (start < space.base || start >= mem2_end)
        continue;
      regions.push_back({section.name, section.kind, start,
                         std::min<u32>(start + section.size, mem2_end), {},
                         tr("From %1").arg(m_executable->source)});
    }
  }
  for (const HeapInfo& heap : m_heaps)
  {
    if (heap.start < space.base || heap.start >= mem2_end)
      continue;
    regions.push_back({tr("Heap %1").arg(heap.index), MemoryRegionKind::Heap, heap.start,
                       std::min(heap.end, mem2_end), heap.allocated,
                       tr("%1 blocks allocated, %2 free")
                           .arg(heap.allocated_blocks)
                           .arg(heap.free_blocks)});
  }
  for (const ThreadStackInfo& stack : m_stacks)
  {
    if (stack.low < space.base || stack.low >= mem2_end)
      continue;
    regions.push_back({tr("Thread %1 stack").arg(FormatAddress(stack.thread)),
                       MemoryRegionKind::Stack, stack.low, std::min(stack.high, mem2_end),
                       stack.peak, {}});
  }
}

void MemoryAnalyzer::BuildAramRegions(Core::System&)
{
  SpaceState& space = m_spaces[static_cast<int>(MemorySpace::ARAM)];
  constexpr u32 ARAM_RESERVED = 0x4000;
  space.regions.push_back({tr("OS / DSP reserved"), MemoryRegionKind::Reserved, 0, ARAM_RESERVED,
                           {}, tr("Reserved by ARInit")});
  space.regions.push_back({tr("ARAM"), MemoryRegionKind::Arena, ARAM_RESERVED, space.size, {},
                           tr("Allocated with ARAlloc / AXR; ARAM has no allocator metadata, so "
                              "only page activity is shown")});
}

void MemoryAnalyzer::BuildXashMem1Regions()
{
  SpaceState& space = m_spaces[static_cast<int>(MemorySpace::MEM1)];
  const u32 mem1_end = space.base + space.size;
  auto& regions = space.regions;
  const auto in_space = [&](u32 address) { return address >= space.base && address < mem1_end; };
  const Xash::Snapshot& xash = *m_xash;
  const size_t first_engine_region = regions.size();

  if (xash.heap && in_space(xash.heap->start) && xash.heap->end > xash.heap->start)
  {
    const Xash::CHeap& heap = *xash.heap;
    QString note = tr("%1: %2 chunks in use, %3 free; largest free chunk %4 bytes")
                       .arg(heap.allocator)
                       .arg(heap.used_chunks)
                       .arg(heap.free_chunks)
                       .arg(heap.largest_free);
    if (!heap.walked)
      note += tr(" (chunk walk incomplete: the heap changed mid-read)");
    regions.push_back({tr("C heap"), MemoryRegionKind::Heap, heap.start,
                       std::min(heap.end, mem1_end), heap.used, note});
    if (heap.arena_hi > heap.end && in_space(heap.end))
    {
      regions.push_back({tr("Unclaimed (heap can grow here)"), MemoryRegionKind::Arena, heap.end,
                         std::min(heap.arena_hi, mem1_end), {},
                         tr("Between the heap's break and Arena1Hi")});
    }
    auto& allocated = m_heap_page_allocated[static_cast<int>(MemorySpace::MEM1)];
    allocated.resize(space.size / PAGE_SIZE, 0);
    for (const auto& [start, end] : heap.used_ranges)
    {
      if (!in_space(start) || end <= start)
        continue;
      const size_t first = (start - space.base) / PAGE_SIZE;
      const size_t last = std::min<size_t>((end - 1 - space.base) / PAGE_SIZE, allocated.size() - 1);
      for (size_t page = first; page <= last; ++page)
        allocated[page] = 1;
    }
  }

  if (xash.map_arena && in_space(xash.map_arena->base))
  {
    const Xash::MapArena& arena = *xash.map_arena;
    regions.push_back({tr("Map-load arena"), MemoryRegionKind::Arena, arena.base,
                       std::min(arena.base + arena.size, mem1_end), arena.tail_highwater,
                       tr("BSP staging and tail carves; %1, tail peak %2 bytes")
                           .arg(arena.in_use ? tr("borrowed") : tr("idle"))
                           .arg(arena.tail_peak)});
  }
  if (xash.boot_arena && in_space(xash.boot_arena->base))
  {
    const Xash::BootArena& arena = *xash.boot_arena;
    regions.push_back({tr("Boot arena"), MemoryRegionKind::Arena, arena.base,
                       std::min(arena.base + arena.size, mem1_end), arena.live_bytes,
                       arena.closed ? tr("Closed after the first map load") :
                                      tr("Boot-time allocations over the idle map-load pad")});
  }
  for (const Xash::Pool& pool : xash.pools)
  {
    if (!pool.arena || !pool.arena_size || !in_space(pool.arena))
      continue;
    regions.push_back({tr("Pool arena: %1").arg(pool.name), MemoryRegionKind::Heap, pool.arena,
                       std::min(pool.arena + pool.arena_size, mem1_end), pool.arena_offset,
                       tr("Zone pool with its own arena")});
  }
  // The biggest static objects (vid, cl, sv, edict arrays...) within the program's sections.
  if (m_executable)
  {
    constexpr u32 MIN_OBJECT = 64 * 1024;
    constexpr size_t MAX_OBJECTS = 32;
    size_t added = 0;
    for (const auto& [name, symbol] : m_xash_reader.LargeObjects(MIN_OBJECT))
    {
      if (added >= MAX_OBJECTS)
        break;
      const u32 start = NormalizeAddress(symbol.address);
      if (!in_space(start))
        continue;
      MemoryRegionKind kind = MemoryRegionKind::Data;
      for (const ExecutableSection& section : m_executable->sections)
      {
        if (start >= section.address && start < section.address + section.size)
          kind = section.kind;
      }
      const u32 end = std::min<u32>(start + symbol.size, mem1_end);
      // The map-load and boot arenas are static buffers too; name them rather than repeat them.
      const auto same = std::find_if(regions.begin() + first_engine_region, regions.end(),
                                     [&](const MemoryRegion& region) {
                                       return region.start == start && region.end == end;
                                     });
      if (same != regions.end())
      {
        same->note += tr(" (%1)").arg(QString::fromStdString(name));
        continue;
      }
      regions.push_back({QString::fromStdString(name), kind, start, end, {},
                         tr("Static object (from the ELF symbols)")});
      ++added;
    }
  }

  for (size_t i = first_engine_region; i < regions.size(); ++i)
    regions[i].nested = true;
}

void MemoryAnalyzer::BuildXashAramRegions()
{
  if (!m_xash->aram)
    return;
  SpaceState& space = m_spaces[static_cast<int>(MemorySpace::ARAM)];
  const Xash::Aram& aram = *m_xash->aram;
  auto& allocated = m_heap_page_allocated[static_cast<int>(MemorySpace::ARAM)];
  allocated.assign(space.size / PAGE_SIZE, 0);

  // Replace the generic "ARAM" region with the engine's own block list.
  std::erase_if(space.regions, [](const MemoryRegion& region) {
    return region.kind == MemoryRegionKind::Arena;
  });
  for (const Xash::AramSegment& segment : aram.segments)
  {
    if (segment.offset >= space.size || segment.size == 0)
      continue;
    const u32 end = std::min(segment.offset + segment.size, space.size);
    if (!segment.used)
    {
      space.regions.push_back({tr("Free"), MemoryRegionKind::HeapFree, segment.offset, end, {},
                               tr("Free ARAM (gc_aram_heap)")});
      continue;
    }
    space.regions.push_back(
        {segment.tag.isEmpty() ? tr("(untagged)") : segment.tag, MemoryRegionKind::Heap,
         segment.offset, end, segment.size,
         segment.stream ? tr("Stream block: carved from the top, freed when done") :
                          tr("Session block: carved from the bottom, held for the whole boot")});
    for (size_t page = segment.offset / PAGE_SIZE; page < allocated.size() && page * PAGE_SIZE < end;
         ++page)
    {
      allocated[page] = 1;
    }
  }
  for (const Xash::AramAsset& asset : aram.assets)
  {
    if (asset.aram == 0 || asset.aram >= space.size)
      continue;
    space.regions.push_back({asset.name, MemoryRegionKind::Heap, asset.aram,
                             std::min(asset.aram + std::max(asset.size, 1u), space.size),
                             asset.size,
                             tr("Cached asset%1%2")
                                 .arg(asset.pins ? tr(", pinned x%1").arg(asset.pins) : QString())
                                 .arg(asset.ready ? QString() : tr(", still streaming"))});
    space.regions.back().nested = true;
  }
}

void MemoryAnalyzer::ColourPages(SpaceState& space)
{
  const size_t pages = space.size / PAGE_SIZE;
  space.page_kind.assign(pages, MemoryRegionKind::Unknown);

  // Paint the largest regions first so the most specific (smallest) region wins.
  std::vector<const MemoryRegion*> by_size;
  for (const MemoryRegion& region : space.regions)
    by_size.push_back(&region);
  std::stable_sort(by_size.begin(), by_size.end(),
                   [](const MemoryRegion* a, const MemoryRegion* b) {
                     return a->Size() > b->Size();
                   });
  for (const MemoryRegion* region : by_size)
  {
    if (region->end <= region->start || region->start < space.base)
      continue;
    const size_t first = (region->start - space.base) / PAGE_SIZE;
    const size_t last = std::min<size_t>((region->end - 1 - space.base) / PAGE_SIZE, pages - 1);
    for (size_t page = first; page <= last && page < pages; ++page)
      space.page_kind[page] = region->kind;
  }

  const int index = static_cast<int>(&space - m_spaces.data());
  const auto& allocated = m_heap_page_allocated[index];
  for (size_t page = 0; page < pages; ++page)
  {
    if (space.page_kind[page] == MemoryRegionKind::Heap &&
        (page >= allocated.size() || !allocated[page]))
    {
      space.page_kind[page] = MemoryRegionKind::HeapFree;
    }
  }
}
