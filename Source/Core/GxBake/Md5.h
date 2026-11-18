// Copyright 2026 GxBake contributors
// SPDX-License-Identifier: MIT

// MD5, used for GxBake's texture tags and cache keys (the Python tool uses hashlib.md5, and the
// engine treats the first 16 bits as a content tag).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace GxBake
{
class Md5
{
public:
  Md5();
  void Update(const void* data, size_t size);
  std::array<uint8_t, 16> Finish();
  std::string HexDigest();

private:
  void Block(const uint8_t* p);
  uint32_t m_state[4];
  uint64_t m_length = 0;
  uint8_t m_buffer[64];
  size_t m_fill = 0;
};
}  // namespace GxBake
