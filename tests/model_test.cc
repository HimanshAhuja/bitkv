// Model-based (property) test.
//
// Runs a long random sequence of Put/Delete/Get/Merge/Reopen against bitkv
// and against a plain std::map in parallel. After every step they must
// agree. This finds bugs nobody thought to write a targeted test for,
// especially in the interaction between merge, tombstones, and recovery.

#include <gtest/gtest.h>

#include <map>
#include <random>

#include "test_util.h"

namespace bitkv {
namespace {

using test::OpenOrDie;
using test::TempDir;

void CheckAgrees(DB* db, const std::map<std::string, std::string>& model, int keyspace,
                 uint64_t seed, int step) {
  ASSERT_EQ(db->Size(), model.size()) << "seed " << seed << " step " << step;
  std::string v;
  for (int k = 0; k < keyspace; ++k) {
    const std::string key = "key" + std::to_string(k);
    auto it = model.find(key);
    Status s = db->Get(key, &v);
    if (it == model.end()) {
      ASSERT_TRUE(s.IsNotFound()) << key << " seed " << seed << " step " << step;
    } else {
      ASSERT_TRUE(s.ok()) << key << " " << s.ToString() << " seed " << seed << " step " << step;
      ASSERT_EQ(v, it->second) << key << " seed " << seed << " step " << step;
    }
  }
}

class ModelTest : public ::testing::TestWithParam<uint64_t> {};

TEST_P(ModelTest, AgreesWithStdMap) {
  const uint64_t seed = GetParam();
  std::mt19937_64 rng(seed);
  TempDir t;
  Options o;
  o.max_file_size = 2048;  // small, so rotations and merges happen constantly
  auto db = OpenOrDie(t.path(), o);
  std::map<std::string, std::string> model;
  constexpr int kKeyspace = 60;

  for (int step = 0; step < 3000; ++step) {
    const std::string key = "key" + std::to_string(rng() % kKeyspace);
    const int op = int(rng() % 100);
    if (op < 55) {
      std::string val = "v" + std::to_string(step) + std::string(rng() % 64, 'x');
      ASSERT_TRUE(db->Put(key, val).ok());
      model[key] = val;
    } else if (op < 80) {
      ASSERT_TRUE(db->Delete(key).ok());
      model.erase(key);
    } else if (op < 92) {
      std::string v;
      Status s = db->Get(key, &v);
      auto it = model.find(key);
      if (it == model.end()) ASSERT_TRUE(s.IsNotFound()) << "seed " << seed;
      else ASSERT_EQ(v, it->second) << "seed " << seed << " step " << step;
    } else if (op < 97) {
      ASSERT_TRUE(db->Merge().ok());
    } else {
      db.reset();  // close
      db = OpenOrDie(t.path(), o);
      CheckAgrees(db.get(), model, kKeyspace, seed, step);
    }
  }
  CheckAgrees(db.get(), model, kKeyspace, seed, -1);
  db.reset();
  db = OpenOrDie(t.path(), o);
  CheckAgrees(db.get(), model, kKeyspace, seed, -2);
}

INSTANTIATE_TEST_SUITE_P(Seeds, ModelTest, ::testing::Values(1, 2, 3, 42, 1337, 20260101));

}  // namespace
}  // namespace bitkv
