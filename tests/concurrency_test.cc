#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "test_util.h"

namespace bitkv {
namespace {

using test::OpenOrDie;
using test::TempDir;

TEST(Concurrency, ParallelWritersAndReaders) {
  TempDir t;
  Options o;
  o.max_file_size = 32 * 1024;  // lots of rotations mid-test
  auto db = OpenOrDie(t.path(), o);
  constexpr int kWriters = 4, kPerWriter = 2000;

  std::atomic<bool> stop{false};
  std::atomic<long> bad_reads{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      for (int i = 0; i < kPerWriter; ++i) {
        std::string k = "w" + std::to_string(w) + ":" + std::to_string(i);
        ASSERT_TRUE(db->Put(k, k).ok());
      }
    });
  }
  // Readers check that any value they see is exactly the one written for
  // that key; a torn or misdirected read would show up here.
  for (int r = 0; r < 3; ++r) {
    threads.emplace_back([&, r] {
      std::string v;
      unsigned i = r;
      while (!stop) {
        std::string k = "w" + std::to_string(i % kWriters) + ":" + std::to_string(i % kPerWriter);
        Status s = db->Get(k, &v);
        if (s.ok() && v != k) ++bad_reads;
        if (!s.ok() && !s.IsNotFound()) ++bad_reads;
        i += 7;
      }
    });
  }
  for (int w = 0; w < kWriters; ++w) threads[w].join();
  stop = true;
  for (size_t i = kWriters; i < threads.size(); ++i) threads[i].join();

  EXPECT_EQ(bad_reads.load(), 0);
  EXPECT_EQ(db->Size(), uint64_t(kWriters * kPerWriter));
}

TEST(Concurrency, ReadsAndWritesDuringMerge) {
  TempDir t;
  Options o;
  o.max_file_size = 16 * 1024;
  auto db = OpenOrDie(t.path(), o);
  for (int i = 0; i < 1000; ++i) db->Put("k" + std::to_string(i), "v" + std::to_string(i));

  std::atomic<bool> stop{false};
  std::atomic<long> bad{0};
  std::thread reader([&] {
    std::string v;
    for (unsigned i = 0; !stop; ++i) {
      std::string k = "k" + std::to_string(i % 1000);
      if (!db->Get(k, &v).ok() || v.empty() || v[0] != 'v') ++bad;
    }
  });
  std::thread writer([&] {
    for (int round = 0; round < 5; ++round)
      for (int i = 0; i < 1000; ++i) db->Put("k" + std::to_string(i), "v" + std::to_string(i));
  });
  for (int m = 0; m < 10; ++m) ASSERT_TRUE(db->Merge().ok());
  writer.join();
  stop = true;
  reader.join();
  EXPECT_EQ(bad.load(), 0);
}

// The point of group commit: with several concurrent writers in kAlways
// mode, one fsync should cover many writes.
TEST(Concurrency, GroupCommitBatchesFsyncs) {
  TempDir t;
  Options o;
  o.sync_mode = SyncMode::kAlways;
  o.group_commit = true;
  auto db = OpenOrDie(t.path(), o);
  constexpr int kThreads = 8, kPer = 200;
  std::vector<std::thread> threads;
  for (int w = 0; w < kThreads; ++w)
    threads.emplace_back([&, w] {
      for (int i = 0; i < kPer; ++i)
        ASSERT_TRUE(db->Put(std::to_string(w) + "-" + std::to_string(i), "v").ok());
    });
  for (auto& th : threads) th.join();

  Stats st = db->GetStats();
  EXPECT_EQ(st.puts, uint64_t(kThreads * kPer));
  EXPECT_LT(st.fsyncs, st.puts) << "group commit never shared an fsync";
  std::printf("  group commit: %llu writes, %llu fsyncs (%.1f writes per fsync)\n",
              (unsigned long long)st.puts, (unsigned long long)st.fsyncs,
              double(st.puts) / double(st.fsyncs));
}

}  // namespace
}  // namespace bitkv
