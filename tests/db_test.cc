#include <gtest/gtest.h>

#include "test_util.h"

namespace bitkv {
namespace {

using test::OpenOrDie;
using test::TempDir;

TEST(DB, PutGet) {
  TempDir t;
  auto db = OpenOrDie(t.path());
  ASSERT_TRUE(db->Put("a", "1").ok());
  std::string v;
  ASSERT_TRUE(db->Get("a", &v).ok());
  EXPECT_EQ(v, "1");
}

TEST(DB, MissingKeyIsNotFound) {
  TempDir t;
  auto db = OpenOrDie(t.path());
  std::string v;
  EXPECT_TRUE(db->Get("nope", &v).IsNotFound());
}

TEST(DB, OverwriteReturnsNewest) {
  TempDir t;
  auto db = OpenOrDie(t.path());
  for (int i = 0; i < 100; ++i) ASSERT_TRUE(db->Put("k", std::to_string(i)).ok());
  std::string v;
  ASSERT_TRUE(db->Get("k", &v).ok());
  EXPECT_EQ(v, "99");
  EXPECT_EQ(db->Size(), 1u);
}

TEST(DB, Delete) {
  TempDir t;
  auto db = OpenOrDie(t.path());
  db->Put("k", "v");
  ASSERT_TRUE(db->Delete("k").ok());
  std::string v;
  EXPECT_TRUE(db->Get("k", &v).IsNotFound());
  EXPECT_EQ(db->Size(), 0u);
  EXPECT_TRUE(db->Delete("never-existed").ok());
}

TEST(DB, EmptyKeyRejected) {
  TempDir t;
  auto db = OpenOrDie(t.path());
  EXPECT_FALSE(db->Put("", "v").ok());
}

TEST(DB, EmptyValueAllowed) {
  TempDir t;
  auto db = OpenOrDie(t.path());
  ASSERT_TRUE(db->Put("k", "").ok());
  std::string v = "sentinel";
  ASSERT_TRUE(db->Get("k", &v).ok());
  EXPECT_EQ(v, "");
}

TEST(DB, LargeValue) {
  TempDir t;
  auto db = OpenOrDie(t.path());
  std::string big(4 * 1024 * 1024, 'x');
  big[12345] = 'y';
  ASSERT_TRUE(db->Put("big", big).ok());
  std::string v;
  ASSERT_TRUE(db->Get("big", &v).ok());
  EXPECT_EQ(v, big);
}

TEST(DB, PersistsAcrossReopen) {
  TempDir t;
  {
    auto db = OpenOrDie(t.path());
    for (int i = 0; i < 1000; ++i) db->Put("key" + std::to_string(i), "val" + std::to_string(i));
    db->Delete("key500");
  }
  auto db = OpenOrDie(t.path());
  EXPECT_EQ(db->Size(), 999u);
  std::string v;
  ASSERT_TRUE(db->Get("key999", &v).ok());
  EXPECT_EQ(v, "val999");
  EXPECT_TRUE(db->Get("key500", &v).IsNotFound());
}

TEST(DB, SequenceNumbersContinueAfterReopen) {
  // If seq restarted at 1 after reopen, a new write could lose to an older
  // record with a higher seq during the *next* recovery.
  TempDir t;
  {
    auto db = OpenOrDie(t.path());
    db->Put("k", "old");
  }
  {
    auto db = OpenOrDie(t.path());
    db->Put("k", "new");
  }
  auto db = OpenOrDie(t.path());
  std::string v;
  ASSERT_TRUE(db->Get("k", &v).ok());
  EXPECT_EQ(v, "new");
}

TEST(DB, RotatesFilesAtSizeLimit) {
  TempDir t;
  Options o;
  o.max_file_size = 4096;
  auto db = OpenOrDie(t.path(), o);
  for (int i = 0; i < 500; ++i) db->Put("key" + std::to_string(i), std::string(100, 'v'));
  EXPECT_GT(db->GetStats().data_files, 5u);
  std::string v;
  ASSERT_TRUE(db->Get("key0", &v).ok());
  ASSERT_TRUE(db->Get("key499", &v).ok());
}

TEST(DB, SecondOpenOfSameDirectoryFails) {
  TempDir t;
  auto db = OpenOrDie(t.path());
  std::unique_ptr<DB> second;
  Status s = DB::Open({}, t.path(), &second);
  EXPECT_FALSE(s.ok());
  EXPECT_NE(s.message().find("already open"), std::string::npos);
}

TEST(DB, ReopenAfterCloseSucceeds) {
  TempDir t;
  { auto db = OpenOrDie(t.path()); }
  std::unique_ptr<DB> db;
  EXPECT_TRUE(DB::Open({}, t.path(), &db).ok());
}

TEST(DB, StatsTrackSpaceAmplification) {
  TempDir t;
  auto db = OpenOrDie(t.path());
  for (int i = 0; i < 10; ++i) db->Put("k", std::string(1000, 'a'));
  Stats st = db->GetStats();
  EXPECT_EQ(st.live_keys, 1u);
  EXPECT_NEAR(st.SpaceAmplification(), 10.0, 0.1);
}

TEST(DB, EverySyncModeWorks) {
  for (SyncMode m : {SyncMode::kAlways, SyncMode::kEverySec, SyncMode::kNone}) {
    for (bool group : {true, false}) {
      TempDir t;
      Options o;
      o.sync_mode = m;
      o.group_commit = group;
      {
        auto db = OpenOrDie(t.path(), o);
        for (int i = 0; i < 50; ++i) ASSERT_TRUE(db->Put(std::to_string(i), "v").ok());
      }
      auto db = OpenOrDie(t.path(), o);
      EXPECT_EQ(db->Size(), 50u);
    }
  }
}

TEST(DB, AlwaysModeSyncsBeforeReturning) {
  TempDir t;
  Options o;
  o.sync_mode = SyncMode::kAlways;
  auto db = OpenOrDie(t.path(), o);
  for (int i = 0; i < 20; ++i) db->Put(std::to_string(i), "v");
  // Single-threaded, so every write needs its own fsync.
  EXPECT_GE(db->GetStats().fsyncs, 20u);
}

}  // namespace
}  // namespace bitkv
