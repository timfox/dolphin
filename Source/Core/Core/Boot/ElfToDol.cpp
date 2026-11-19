// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Boot/ElfToDol.h"

#include <algorithm>
#include <limits>

#include <fmt/format.h>

namespace Boot
{
namespace
{
constexpr size_t DOL_HEADER_SIZE = 0x100;
constexpr int MAX_TEXT = 7;
constexpr int MAX_DATA = 11;
constexpr u32 PT_LOAD = 1;
constexpr u32 PF_X = 1;

u32 Be32(const std::vector<u8>& d, size_t off)
{
  return (u32(d[off]) << 24) | (u32(d[off + 1]) << 16) | (u32(d[off + 2]) << 8) | d[off + 3];
}

u16 Be16(const std::vector<u8>& d, size_t off)
{
  return u16((d[off] << 8) | d[off + 1]);
}

void Put32(std::vector<u8>* d, size_t off, u32 v)
{
  (*d)[off] = u8(v >> 24);
  (*d)[off + 1] = u8(v >> 16);
  (*d)[off + 2] = u8(v >> 8);
  (*d)[off + 3] = u8(v);
}

struct Section
{
  u32 address;
  u32 offset;  // in the ELF
  u32 size;
};
}  // namespace

std::optional<std::vector<u8>> ElfToDol(const std::vector<u8>& elf, std::string* error,
                                        DolInfo* info)
{
  const auto fail = [error](std::string reason) -> std::optional<std::vector<u8>> {
    if (error)
      *error = std::move(reason);
    return std::nullopt;
  };

  if (elf.size() < 52 || elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F')
    return fail("not an ELF file");
  if (elf[4] != 1 || elf[5] != 2)
    return fail("not a 32-bit big-endian ELF (GameCube and Wii executables are)");
  if (Be16(elf, 18) != 20)
    return fail("not a PowerPC executable");

  const u32 entry = Be32(elf, 24);
  const u32 phoff = Be32(elf, 28);
  const u16 phentsize = Be16(elf, 42);
  const u16 phnum = Be16(elf, 44);
  if (phnum == 0 || phentsize < 32 || u64(phoff) + u64(phnum) * phentsize > elf.size())
    return fail("the program headers are missing or truncated");

  std::vector<Section> text, data;
  u32 bss_start = std::numeric_limits<u32>::max(), bss_end = 0;
  for (u16 i = 0; i < phnum; ++i)
  {
    const size_t ph = phoff + size_t(i) * phentsize;
    if (Be32(elf, ph) != PT_LOAD)
      continue;
    const u32 offset = Be32(elf, ph + 4);
    const u32 vaddr = Be32(elf, ph + 8);
    const u32 filesz = Be32(elf, ph + 16);
    const u32 memsz = Be32(elf, ph + 20);
    const u32 flags = Be32(elf, ph + 24);
    if (u64(offset) + filesz > elf.size())
      return fail(fmt::format("segment {} runs past the end of the file", i));

    if (filesz > 0)
      ((flags & PF_X) ? text : data).push_back({vaddr, offset, filesz});
    if (memsz > filesz)
    {
      bss_start = std::min(bss_start, vaddr + filesz);
      bss_end = std::max(bss_end, vaddr + memsz);
    }
  }
  if (text.empty())
    return fail("the ELF has no loadable code");
  if (text.size() > MAX_TEXT)
    return fail(fmt::format("{} code segments; a DOL holds at most {}", text.size(), MAX_TEXT));
  if (data.size() > MAX_DATA)
    return fail(fmt::format("{} data segments; a DOL holds at most {}", data.size(), MAX_DATA));

  std::vector<u8> dol(DOL_HEADER_SIZE, 0);
  const auto add = [&](const Section& s, size_t slot) {
    // Sections start 32-byte aligned in the file, like elf2dol writes them.
    const u32 file_offset = u32(dol.size());
    Put32(&dol, slot, file_offset);
    Put32(&dol, slot + 0x48, s.address);
    Put32(&dol, slot + 0x90, s.size);
    dol.insert(dol.end(), elf.begin() + s.offset, elf.begin() + s.offset + s.size);
    dol.resize((dol.size() + 31) & ~size_t(31), 0);
  };
  for (size_t i = 0; i < text.size(); ++i)
    add(text[i], i * 4);
  for (size_t i = 0; i < data.size(); ++i)
    add(data[i], 0x1c + i * 4);

  const u32 bss_size = bss_end > bss_start ? bss_end - bss_start : 0;
  Put32(&dol, 0xd8, bss_size ? bss_start : 0);
  Put32(&dol, 0xdc, bss_size);
  Put32(&dol, 0xe0, entry);

  if (info)
  {
    info->entry = entry;
    info->text_sections = int(text.size());
    info->data_sections = int(data.size());
    info->bss_address = bss_size ? bss_start : 0;
    info->bss_size = bss_size;
  }
  return dol;
}
}  // namespace Boot
