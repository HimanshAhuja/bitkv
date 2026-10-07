// CRC-32 (IEEE 802.3 polynomial, the one zlib uses).
//
// Why a checksum at all? After a crash the tail of the active file can hold a
// half-written record. Its length fields might even look plausible. The CRC
// is how recovery tells "valid record" from "garbage that happens to parse":
// if the checksum over the record body does not match, we stop there.
//
// A CRC detects accidental corruption, not tampering. That is the right tool
// here: we are defending against torn writes and bit rot, not attackers.
#pragma once

#include <cstddef>
#include <cstdint>

namespace bitkv {

// Extends a running CRC. Start with crc = 0 for a fresh checksum.
uint32_t Crc32(uint32_t crc, const void* data, size_t n);

inline uint32_t Crc32(const void* data, size_t n) { return Crc32(0, data, n); }

}  // namespace bitkv
