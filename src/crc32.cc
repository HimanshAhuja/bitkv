#include "crc32.h"

#include <array>

namespace bitkv {
namespace {

// Byte-at-a-time lookup table for the reflected polynomial 0xEDB88320.
// Built once at compile time; 256 entries * 4 bytes = 1 KiB.
constexpr std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    t[i] = c;
  }
  return t;
}

constexpr std::array<uint32_t, 256> kTable = MakeTable();

}  // namespace

uint32_t Crc32(uint32_t crc, const void* data, size_t n) {
  const auto* p = static_cast<const uint8_t*>(data);
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = kTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

}  // namespace bitkv
