// On-disk formats. Everything is little-endian, written byte by byte so the
// files are portable regardless of the host CPU's endianness.
//
// DATA RECORD (one per Put/Delete, appended to a .data file)
//
//   offset size  field
//   0      4     crc        CRC-32 over bytes [4, end of record)
//   4      8     seq        global sequence number, strictly increasing
//   12     4     key_size
//   16     4     value_size
//   20     1     flags      bit 0 set => tombstone (a Delete)
//   21     k     key
//   21+k   v     value      (empty for tombstones)
//
// Why a sequence number rather than a wall-clock timestamp? Clocks can jump
// backwards (NTP, VM migration). Recovery and merge both decide "which record
// is newer" by comparing seq, so it must be monotonic by construction.
//
// HINT ENTRY (one per live key, in the .hint file written next to a merged
// .data file). Lets Open() rebuild the keydir without reading any values.
//
//   0      4     crc        CRC-32 over bytes [4, end of entry)
//   4      8     seq
//   12     4     key_size
//   16     4     record_size  size of the full data record
//   20     8     offset       where that record starts in the .data file
//   28     1     flags
//   29     k     key
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace bitkv {

constexpr size_t kRecordHeaderSize = 21;
constexpr size_t kHintHeaderSize = 29;

// Sanity limits. Recovery checks these *before* trusting a length field, so a
// corrupt header can never make us try to allocate 4 GiB.
constexpr uint32_t kMaxKeySize = 64 * 1024;
constexpr uint32_t kMaxValueSize = 256u * 1024 * 1024;

constexpr uint8_t kFlagTombstone = 0x1;

struct Record {
  uint64_t seq = 0;
  uint8_t flags = 0;
  std::string_view key;
  std::string_view value;
  bool tombstone() const { return flags & kFlagTombstone; }
};

struct HintEntry {
  uint64_t seq = 0;
  uint32_t record_size = 0;
  uint64_t offset = 0;
  uint8_t flags = 0;
  std::string_view key;
};

inline uint64_t RecordSize(size_t key_len, size_t value_len) {
  return kRecordHeaderSize + key_len + value_len;
}

// Appends the encoded record to *dst.
void EncodeRecord(uint64_t seq, uint8_t flags, std::string_view key,
                  std::string_view value, std::string* dst);

enum class DecodeResult {
  kOk,
  kTruncated,  // not enough bytes left: a torn write at the end of the file
  kCorrupt,    // bytes are all there but the CRC or a length is wrong
};

// Decodes the record starting at buf[0]. On kOk, *rec points into buf and
// *consumed is the record's total size. `avail` is how many bytes exist.
DecodeResult DecodeRecord(const char* buf, size_t avail, Record* rec,
                          size_t* consumed);

void EncodeHint(const HintEntry& h, std::string* dst);
DecodeResult DecodeHint(const char* buf, size_t avail, HintEntry* h,
                        size_t* consumed);

}  // namespace bitkv
