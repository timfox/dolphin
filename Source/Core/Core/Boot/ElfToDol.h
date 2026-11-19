// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// ELF to DOL conversion for the devkit editor's Make DOL, like devkitPro's elf2dol: every loadable
// segment with file data becomes a text section (executable) or a data section, and the
// zero-filled tails of the segments become the BSS range.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace Boot
{
struct DolInfo
{
  u32 entry = 0;
  int text_sections = 0;
  int data_sections = 0;
  u32 bss_address = 0;
  u32 bss_size = 0;
};

// |elf| is a 32-bit big-endian PowerPC executable. On failure the reason goes to |error|.
std::optional<std::vector<u8>> ElfToDol(const std::vector<u8>& elf, std::string* error,
                                        DolInfo* info = nullptr);
}  // namespace Boot
