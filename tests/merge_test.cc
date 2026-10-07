#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "test_util.h"

namespace bitkv {
namespace {

using test::OpenOrDie;
using test::TempDir;

TEST(Merge, ReclaimsSpaceFromOverwrites) {
  TempDir t;
  Options o;
  o.max_file_size = 16 * 1024;
  auto db = OpenOrDie(t.path(), o);
  for (int round = 0; round < 20; ++round)
    for (int k = 0; k < 100; ++k) db->Put("key" + std::to_string(k), std::string(100, char('a' + round % 26)));

  const double before = db->GetStats().SpaceAmplification();
  ASSERT_TRUE(db->Merge().ok());
  const Stats after = db->GetStats();
  EXPECT_GT(before, 15.0);
  EXPECT_LT(after.SpaceAmplification(), 1.05);
  EXPECT_EQ(after.live_keys, 100u);

  std::string v;
  ASSERT_TRUE(db->Get("key7", &v).ok());
  EXPECT_EQ(v, std::string(100, 't'));  // round 19 -> 't'
}

TEST(Merge, DeletedKeysStayDeletedAfterMergeAndReopen) {
  TempDir t;
  Options o;
  o.max_file_size = 4096;
  {
    auto db = OpenOrDie(t.path(), o);
    for (int i = 0; i < 200; ++i) db->Put("k" + std::to_string(i), "v");
    for (int i = 0; i < 200; i += 2) db->Delete("k" + std::to_string(i));
    ASSERT_TRUE(db->Merge().ok());
    EXPECT_EQ(db->Size(), 100u);
  }
  auto db = OpenOrDie(t.path(), o);
  EXPECT_EQ(db->Size(), 100u);
  std::string v;
  EXPECT_TRUE(db->Get("k0", &v).IsNotFound());
  EXPECT_TRUE(db->Get("k1", &v).ok());
}

TEST(Merge, WritesHintFilesThatSurviveReopen) {
  TempDir t;
  {
    auto db = OpenOrDie(t.path());
    for (int i = 0; i < 50; ++i) db->Put("k" + std::to_string(i), "v" + std::to_string(i));
    ASSERT_TRUE(db->Merge().ok());
  }
  int hints = 0;
  for (uint32_t id : test::DataFiles(t.path()))
    if (FileExists(HintFileName(t.path(), id))) ++hints;
  EXPECT_GE(hints, 1);
  auto db = OpenOrDie(t.path());
  std::string v;
  ASSERT_TRUE(db->Get("k49", &v).ok());
  EXPECT_EQ(v, "v49");
}

TEST(Merge, RepeatedMergesAreIdempotent) {
  TempDir t;
  auto db = OpenOrDie(t.path());
  for (int i = 0; i < 100; ++i) db->Put("k" + std::to_string(i), "v");
  for (int m = 0; m < 5; ++m) ASSERT_TRUE(db->Merge().ok());
  EXPECT_EQ(db->Size(), 100u);
  EXPECT_FALSE(FileExists(MergeMarkerName(t.path())));
}

TEST(Merge, MergeOnEmptyDatabase) {
  TempDir t;
  auto db = OpenOrDie(t.path());
  EXPECT_TRUE(db->Merge().ok());
}

// Writes that land while a merge is copying must win over the merge's copy
// of the older value, both in memory and after a restart.
TEST(Merge, ConcurrentWritesDuringMergeAreNotLost) {
  TempDir t;
  Options o;
  o.max_file_size = 8 * 1024;
  {
    auto db = OpenOrDie(t.path(), o);
    for (int i = 0; i < 2000; ++i) db->Put("k" + std::to_string(i), "old");

    std::atomic<bool> done{false};
    std::thread writer([&] {
      for (int i = 0; i < 2000; ++i) db->Put("k" + std::to_string(i), "new");
      done = true;
    });
    while (!done) ASSERT_TRUE(db->Merge().ok());
    writer.join();
    ASSERT_TRUE(db->Merge().ok());

    std::string v;
    for (int i = 0; i < 2000; ++i) {
      ASSERT_TRUE(db->Get("k" + std::to_string(i), &v).ok());
      ASSERT_EQ(v, "new") << "k" << i;
    }
  }
  auto db = OpenOrDie(t.path(), o);
  std::string v;
  for (int i = 0; i < 2000; ++i) {
    ASSERT_TRUE(db->Get("k" + std::to_string(i), &v).ok());
    ASSERT_EQ(v, "new") << "after reopen, k" << i;
  }
}

}  // namespace
}  // namespace bitkv
