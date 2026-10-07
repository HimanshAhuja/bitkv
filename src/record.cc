#include "record.h"

#include <cstring>

#include "crc32.h"

namespace bitkv {
namespace {

void PutU32(std::string* d, uint32_t v) {
  for (int i = 0; i < 4; ++i) d->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void PutU64(std::string* d, uint64_t v) {
  for (int i = 0; i < 8; ++i) d->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
uint32_t GetU32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= uint32_t(uint8_t(p[i])) << (8 * i);
  return v;
}
uint64_t GetU64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= uint64_t(uint8_t(p[i])) << (8 * i);
  return v;
}

// Writes the CRC of everything after the 4-byte CRC slot into that slot.
void SealCrc(std::string* dst, size_t start) {
  const char* body = dst->data() + start + 4;
  uint32_t crc = Crc32(body, dst->size() - start - 4);
  for (int i = 0; i < 4; ++i) (*dst)[start + i] = static_cast<char>((crc >> (8 * i)) & 0xFF);
}

}  // namespace

void EncodeRecord(uint64_t seq, uint8_t flags, std::string_view key,
                  std::string_view value, std::string* dst) {
  const size_t start = dst->size();
  dst->reserve(start + RecordSize(key.size(), value.size()));
  PutU32(dst, 0);  // CRC placeholder, filled in by SealCrc
  PutU64(dst, seq);
  PutU32(dst, static_cast<uint32_t>(key.size()));
  PutU32(dst, static_cast<uint32_t>(value.size()));
  dst->push_back(static_cast<char>(flags));
  dst->append(key.data(), key.size());
  dst->append(value.data(), value.size());
  SealCrc(dst, start);
}

DecodeResult DecodeRecord(const char* buf, size_t avail, Record* rec,
                          size_t* consumed) {
  if (avail < kRecordHeaderSize) return DecodeResult::kTruncated;

  const uint32_t stored_crc = GetU32(buf);
  const uint64_t seq = GetU64(buf + 4);
  const uint32_t ksz = GetU32(buf + 12);
  const uint32_t vsz = GetU32(buf + 16);
  const uint8_t flags = static_cast<uint8_t>(buf[20]);

  // Validate lengths before using them. A torn header can contain anything.
  if (ksz == 0 || ksz > kMaxKeySize || vsz > kMaxValueSize) return DecodeResult::kCorrupt;
  if (flags & ~kFlagTombstone) return DecodeResult::kCorrupt;

  const uint64_t total = RecordSize(ksz, vsz);
  if (total > avail) return DecodeResult::kTruncated;

  if (Crc32(buf + 4, total - 4) != stored_crc) return DecodeResult::kCorrupt;

  rec->seq = seq;
  rec->flags = flags;
  rec->key = std::string_view(buf + kRecordHeaderSize, ksz);
  rec->value = std::string_view(buf + kRecordHeaderSize + ksz, vsz);
  *consumed = static_cast<size_t>(total);
  return DecodeResult::kOk;
}

void EncodeHint(const HintEntry& h, std::string* dst) {
  const size_t start = dst->size();
  PutU32(dst, 0);
  PutU64(dst, h.seq);
  PutU32(dst, static_cast<uint32_t>(h.key.size()));
  PutU32(dst, h.record_size);
  PutU64(dst, h.offset);
  dst->push_back(static_cast<char>(h.flags));
  dst->append(h.key.data(), h.key.size());
  SealCrc(dst, start);
}

DecodeResult DecodeHint(const char* buf, size_t avail, HintEntry* h,
                        size_t* consumed) {
  if (avail < kHintHeaderSize) return DecodeResult::kTruncated;
  const uint32_t stored_crc = GetU32(buf);
  const uint32_t ksz = GetU32(buf + 12);
  if (ksz == 0 || ksz > kMaxKeySize) return DecodeResult::kCorrupt;
  const size_t total = kHintHeaderSize + ksz;
  if (total > avail) return DecodeResult::kTruncated;
  if (Crc32(buf + 4, total - 4) != stored_crc) return DecodeResult::kCorrupt;

  h->seq = GetU64(buf + 4);
  h->record_size = GetU32(buf + 16);
  h->offset = GetU64(buf + 20);
  h->flags = static_cast<uint8_t>(buf[28]);
  h->key = std::string_view(buf + kHintHeaderSize, ksz);
  *consumed = total;
  return DecodeResult::kOk;
}

}  // namespace bitkv
