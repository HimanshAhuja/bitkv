// Recovery tests. Each one manufactures the on-disk state a specific kind of
// crash leaves behind, then checks that Open() recovers to a correct state.

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>
#include <map>

#include "record.h"
#include "test_util.h"

namespace bitkv {
namespace {

using test::OpenOrDie;
using test::TempDir;

std::string Slurp(const std::string& path) {
  std::string s;
  ReadWholeFile(path, &s);
  return s;
}

void Spit(const std::string& path, const std::string& data) {
  std::ofstream(path, std::ios::binary | std::ios::trunc).write(data.data(), data.size());
}

// Writes keys k0..k9 and returns the bytes of the single resulting data file.
std::string TenRecordFile(size_t* last_record_start) {
  TempDir t;
  {
    auto db = OpenOrDie(t.path());
    for (int i = 0; i < 10; ++i) db->Put("k" + std::to_string(i), "value-" + std::to_string(i));
  }
  std::string bytes = Slurp(DataFileName(t.path(), 1));
  *last_record_start = bytes.size() - RecordSize(2, 7);
  return bytes;
}

// The core crash-safety property: a crash at ANY byte while appending the
// last record must lose only that record, never earlier ones, and never
// return garbage.
TEST(Recovery, TornTailAtEveryByteOffset) {
  size_t last_start;
  const std::string full = TenRecordFile(&last_start);

  for (size_t cut = last_start; cut < full.size(); ++cut) {
    TempDir t;
    Spit(DataFileName(t.path(), 1), full.substr(0, cut));
    {
      auto db = OpenOrDie(t.path());
      ASSERT_EQ(db->Size(), 9u) << "cut at " << cut;
      EXPECT_EQ(db->GetStats().recovered_torn_bytes, cut - last_start);
      std::string v;
      for (int i = 0; i < 9; ++i) {
        ASSERT_TRUE(db->Get("k" + std::to_string(i), &v).ok());
        EXPECT_EQ(v, "value-" + std::to_string(i));
      }
      EXPECT_TRUE(db->Get("k9", &v).IsNotFound());
    }
    // The tail was truncated on disk, so a second open finds nothing to fix.
    auto db = OpenOrDie(t.path());
    EXPECT_EQ(db->GetStats().recovered_torn_bytes, 0u);
    EXPECT_EQ(db->Size(), 9u);
  }
}

TEST(Recovery, ZeroFilledTail) {
  // Some filesystems can persist a larger file size without the data blocks,
  // leaving zeros after a crash.
  size_t last_start;
  std::string bytes = TenRecordFile(&last_start);
  TempDir t;
  Spit(DataFileName(t.path(), 1), bytes + std::string(100, '\0'));
  auto db = OpenOrDie(t.path());
  EXPECT_EQ(db->Size(), 10u);
  EXPECT_EQ(db->GetStats().recovered_torn_bytes, 100u);
}

TEST(Recovery, CorruptionMidFileDiscardsTheRestOfThatFile) {
  // Documented limitation (see DESIGN.md): without block-level framing we
  // cannot find the next record boundary after a corrupt one.
  size_t last_start;
  std::string bytes = TenRecordFile(&last_start);
  const size_t rec = RecordSize(2, 7);
  bytes[5 * rec + kRecordHeaderSize] ^= 0x40;  // damage record #5's key
  TempDir t;
  Spit(DataFileName(t.path(), 1), bytes);
  auto db = OpenOrDie(t.path());
  EXPECT_EQ(db->Size(), 5u);
  EXPECT_EQ(db->GetStats().corrupt_records, 1u);
  std::string v;
  EXPECT_TRUE(db->Get("k4", &v).ok());
  EXPECT_TRUE(db->Get("k5", &v).IsNotFound());
}

TEST(Recovery, TombstoneInLaterFileBeatsValueInEarlierFile) {
  TempDir t;
  Options o;
  o.max_file_size = 64;  // force a new file almost every write
  {
    auto db = OpenOrDie(t.path(), o);
    db->Put("victim", "alive");
    for (int i = 0; i < 20; ++i) db->Put("filler" + std::to_string(i), "x");
    db->Delete("victim");
  }
  ASSERT_GT(test::DataFiles(t.path()).size(), 5u);
  auto db = OpenOrDie(t.path(), o);
  std::string v;
  EXPECT_TRUE(db->Get("victim", &v).IsNotFound());
  EXPECT_EQ(db->Size(), 20u);
}

TEST(Recovery, ReinsertAfterDeleteSurvivesReopen) {
  TempDir t;
  Options o;
  o.max_file_size = 64;
  {
    auto db = OpenOrDie(t.path(), o);
    db->Put("k", "v1");
    db->Delete("k");
    db->Put("k", "v2");
  }
  auto db = OpenOrDie(t.path(), o);
  std::string v;
  ASSERT_TRUE(db->Get("k", &v).ok());
  EXPECT_EQ(v, "v2");
}

TEST(Recovery, LeftoverTempFilesAreRemoved) {
  TempDir t;
  { auto db = OpenOrDie(t.path()); db->Put("k", "v"); }
  Spit(t.path() + "/000000099.data.tmp", "half a merge");
  Spit(t.path() + "/000000099.hint.tmp", "half a hint");
  auto db = OpenOrDie(t.path());
  EXPECT_FALSE(FileExists(t.path() + "/000000099.data.tmp"));
  EXPECT_FALSE(FileExists(t.path() + "/000000099.hint.tmp"));
  EXPECT_EQ(db->Size(), 1u);
}

// Reproduces the exact failure the MERGE marker exists to prevent.
//
// State: file 1 holds  k=value,  file 2 holds  k=tombstone.  A merge drops
// both (k is deleted). Suppose the merge committed, then crashed after
// unlinking file 2 but before unlinking file 1. Without the marker, recovery
// would find the value with no tombstone, and k would come back to life.
TEST(Recovery, InterruptedMergeCleanupDoesNotResurrectDeletedKey) {
  TempDir t;
  Options o;
  o.max_file_size = 30;  // one record per file
  {
    auto db = OpenOrDie(t.path(), o);
    db->Put("k", "value");
    db->Delete("k");
  }
  auto ids = test::DataFiles(t.path());
  ASSERT_GE(ids.size(), 2u);
  // Simulate: marker durable, tombstone file already deleted, value file not.
  Spit(MergeMarkerName(t.path()), std::to_string(ids[0]) + "\n" + std::to_string(ids[1]) + "\n");
  ::unlink(DataFileName(t.path(), ids[1]).c_str());

  auto db = OpenOrDie(t.path(), o);
  std::string v;
  EXPECT_TRUE(db->Get("k", &v).IsNotFound()) << "deleted key was resurrected";
  EXPECT_FALSE(FileExists(MergeMarkerName(t.path())));
}

TEST(Recovery, CorruptHintFileFallsBackToScanningData) {
  TempDir t;
  {
    auto db = OpenOrDie(t.path());
    for (int i = 0; i < 100; ++i) db->Put("k" + std::to_string(i), "v" + std::to_string(i));
    ASSERT_TRUE(db->Merge().ok());
  }
  bool damaged = false;
  for (uint32_t id : test::DataFiles(t.path())) {
    const std::string hint = HintFileName(t.path(), id);
    if (FileExists(hint)) {
      std::string h = Slurp(hint);
      h[h.size() / 2] ^= 0xFF;
      Spit(hint, h);
      damaged = true;
    }
  }
  ASSERT_TRUE(damaged);
  auto db = OpenOrDie(t.path());
  ASSERT_EQ(db->Size(), 100u);
  std::string v;
  ASSERT_TRUE(db->Get("k42", &v).ok());
  EXPECT_EQ(v, "v42");
}

// A real crash, not a simulated one: a child process writes in kAlways mode
// and reports each acknowledged key over a pipe; the parent kill -9s it at a
// random moment and then checks every acknowledged write survived.
TEST(Recovery, AcknowledgedWritesSurviveKill9) {
  for (int round = 0; round < 8; ++round) {
    TempDir t;
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
      ::close(fds[0]);
      Options o;
      o.sync_mode = SyncMode::kAlways;
      o.max_file_size = 8192;
      std::unique_ptr<DB> db;
      if (!DB::Open(o, t.path(), &db).ok()) ::_exit(1);
      for (uint32_t i = 0;; ++i) {
        if (!db->Put("key" + std::to_string(i), "val" + std::to_string(i)).ok()) ::_exit(1);
        if (::write(fds[1], &i, sizeof(i)) != sizeof(i)) ::_exit(1);
      }
    }
    ::close(fds[1]);
    uint32_t acked = 0, got;
    const uint32_t stop_after = 50 + uint32_t(round) * 37;
    while (acked < stop_after && ::read(fds[0], &got, sizeof(got)) == sizeof(got)) acked = got + 1;
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    while (::read(fds[0], &got, sizeof(got)) == sizeof(got)) acked = got + 1;  // drain
    ::close(fds[0]);

    auto db = OpenOrDie(t.path());
    std::string v;
    for (uint32_t i = 0; i < acked; ++i) {
      ASSERT_TRUE(db->Get("key" + std::to_string(i), &v).ok())
          << "round " << round << ": lost acknowledged key" << i;
      ASSERT_EQ(v, "val" + std::to_string(i));
    }
  }
}

}  // namespace
}  // namespace bitkv
