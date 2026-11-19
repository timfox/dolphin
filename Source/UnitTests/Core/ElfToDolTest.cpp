// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "Core/Boot/DolReader.h"
#include "Core/Boot/ElfToDol.h"

namespace
{
void Put32(std::vector<u8>* d, size_t off, u32 v)
{
  (*d)[off] = u8(v >> 24);
  (*d)[off + 1] = u8(v >> 16);
  (*d)[off + 2] = u8(v >> 8);
  (*d)[off + 3] = u8(v);
}

// A PowerPC ELF with a code segment, a data segment with a BSS tail and a non-loadable segment.
std::vector<u8> TestElf()
{
  std::vector<u8> elf(0x200, 0);
  elf[0] = 0x7f;
  elf[1] = 'E';
  elf[2] = 'L';
  elf[3] = 'F';
  elf[4] = 1;  // 32-bit
  elf[5] = 2;  // big-endian
  elf[6] = 1;
  elf[17] = 2;   // ET_EXEC
  elf[19] = 20;  // EM_PPC
  Put32(&elf, 24, 0x80003100);
  Put32(&elf, 28, 52);  // program headers
  elf[42] = 0;
  elf[43] = 32;
  elf[45] = 3;
  const auto segment = [&](int i, u32 type, u32 offset, u32 vaddr, u32 filesz, u32 memsz,
                           u32 flags) {
    const size_t ph = 52 + i * 32;
    Put32(&elf, ph, type);
    Put32(&elf, ph + 4, offset);
    Put32(&elf, ph + 8, vaddr);
    Put32(&elf, ph + 12, vaddr);
    Put32(&elf, ph + 16, filesz);
    Put32(&elf, ph + 20, memsz);
    Put32(&elf, ph + 24, flags);
  };
  segment(0, 1, 0x100, 0x80003100, 0x40, 0x40, 5);    // R+X
  segment(1, 1, 0x140, 0x80004000, 0x24, 0x1000, 6);  // R+W, 0xfdc bytes of BSS
  segment(2, 4, 0x180, 0, 0x10, 0x10, 4);             // PT_NOTE
  for (u32 i = 0; i < 0x40; ++i)
    elf[0x100 + i] = u8(i);
  for (u32 i = 0; i < 0x24; ++i)
    elf[0x140 + i] = u8(0xa0 + i);
  return elf;
}
}  // namespace

TEST(ElfToDol, Converts)
{
  std::string error;
  Boot::DolInfo info;
  const auto dol = Boot::ElfToDol(TestElf(), &error, &info);
  ASSERT_TRUE(dol.has_value()) << error;
  EXPECT_EQ(info.text_sections, 1);
  EXPECT_EQ(info.data_sections, 1);
  EXPECT_EQ(info.bss_address, 0x80004024u);
  EXPECT_EQ(info.bss_size, 0xfdcu);

  // 0x100 header, 0x40 of code, 0x24 of data padded to 0x40.
  ASSERT_EQ(dol->size(), 0x180u);
  EXPECT_EQ((*dol)[0x100], 0x00);
  EXPECT_EQ((*dol)[0x13f], 0x3f);
  EXPECT_EQ((*dol)[0x140], 0xa0);

  DolReader reader(*dol);
  EXPECT_TRUE(reader.IsValid());
  EXPECT_EQ(reader.GetEntryPoint(), 0x80003100u);
}

TEST(ElfToDol, RejectsOtherFiles)
{
  std::string error;
  EXPECT_FALSE(Boot::ElfToDol(std::vector<u8>(64, 0), &error).has_value());
  EXPECT_FALSE(error.empty());

  std::vector<u8> little = TestElf();
  little[5] = 1;
  EXPECT_FALSE(Boot::ElfToDol(little, &error).has_value());
}
