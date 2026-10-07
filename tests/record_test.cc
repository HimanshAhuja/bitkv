#include <gtest/gtest.h>

#include "crc32.h"
#include "record.h"

namespace bitkv {
namespace {

TEST(Crc32, MatchesKnownVector) {
  // The standard CRC-32 check value for the ASCII string "123456789".
  EXPECT_EQ(Crc32("123456789", 9), 0xCBF43926u);
}

TEST(Record, RoundTrip) {
  std::string buf;
  EncodeRecord(42, 0, "hello", "world", &buf);
  ASSERT_EQ(buf.size(), RecordSize(5, 5));

  Record r;
  size_t n;
  ASSERT_EQ(DecodeRecord(buf.data(), buf.size(), &r, &n), DecodeResult::kOk);
  EXPECT_EQ(n, buf.size());
  EXPECT_EQ(r.seq, 42u);
  EXPECT_EQ(r.key, "hello");
  EXPECT_EQ(r.value, "world");
  EXPECT_FALSE(r.tombstone());
}

TEST(Record, TombstoneRoundTrip) {
  std::string buf;
  EncodeRecord(7, kFlagTombstone, "gone", "", &buf);
  Record r;
  size_t n;
  ASSERT_EQ(DecodeRecord(buf.data(), buf.size(), &r, &n), DecodeResult::kOk);
  EXPECT_TRUE(r.tombstone());
  EXPECT_TRUE(r.value.empty());
}

TEST(Record, BinarySafe) {
  const std::string key("k\0e\0y", 5), value("\0\xff\x00\n\r", 5);
  std::string buf;
  EncodeRecord(1, 0, key, value, &buf);
  Record r;
  size_t n;
  ASSERT_EQ(DecodeRecord(buf.data(), buf.size(), &r, &n), DecodeResult::kOk);
  EXPECT_EQ(r.key, key);
  EXPECT_EQ(r.value, value);
}

// Simulates a crash at every possible point while writing a record: every
// strict prefix must be reported as truncated, never as a valid record.
TEST(Record, EveryPrefixIsTruncatedNotValid) {
  std::string buf;
  EncodeRecord(9, 0, "key", "some value here", &buf);
  for (size_t len = 0; len < buf.size(); ++len) {
    Record r;
    size_t n;
    EXPECT_NE(DecodeRecord(buf.data(), len, &r, &n), DecodeResult::kOk) << "prefix " << len;
  }
}

// Flipping any single bit anywhere in the record must be caught.
TEST(Record, EverySingleBitFlipIsDetected) {
  std::string good;
  EncodeRecord(123, 0, "user:42", "{\"name\":\"himansh\"}", &good);
  for (size_t byte = 0; byte < good.size(); ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string bad = good;
      bad[byte] ^= static_cast<char>(1 << bit);
      Record r;
      size_t n;
      EXPECT_NE(DecodeRecord(bad.data(), bad.size(), &r, &n), DecodeResult::kOk)
          << "byte " << byte << " bit " << bit;
    }
  }
}

TEST(Record, ZeroFilledRegionIsCorrupt) {
  // What a crash can leave behind when the filesystem extended the file size
  // but never wrote the data blocks.
  std::string zeros(64, '\0');
  Record r;
  size_t n;
  EXPECT_EQ(DecodeRecord(zeros.data(), zeros.size(), &r, &n), DecodeResult::kCorrupt);
}

TEST(Record, AbsurdLengthRejectedBeforeAllocation) {
  std::string buf;
  EncodeRecord(1, 0, "k", "v", &buf);
  buf[16] = buf[17] = buf[18] = buf[19] = '\xff';  // value_size = 4 GiB
  Record r;
  size_t n;
  EXPECT_EQ(DecodeRecord(buf.data(), buf.size(), &r, &n), DecodeResult::kCorrupt);
}

TEST(Hint, RoundTrip) {
  std::string buf;
  EncodeHint({99, 1234, 56789, 0, "hint-key"}, &buf);
  HintEntry h;
  size_t n;
  ASSERT_EQ(DecodeHint(buf.data(), buf.size(), &h, &n), DecodeResult::kOk);
  EXPECT_EQ(h.seq, 99u);
  EXPECT_EQ(h.record_size, 1234u);
  EXPECT_EQ(h.offset, 56789u);
  EXPECT_EQ(h.key, "hint-key");
  buf[10] ^= 1;
  EXPECT_EQ(DecodeHint(buf.data(), buf.size(), &h, &n), DecodeResult::kCorrupt);
}

}  // namespace
}  // namespace bitkv
