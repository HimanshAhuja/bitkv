// crash_test: kills bitkv with SIGKILL at random moments, thousands of
// times, and proves after every kill that no acknowledged write was lost
// and no deleted key came back.
//
// HOW THE CHECK WORKS
//   A child process runs a deterministic workload generated from a seed:
//   puts, overwrites, deletes and merges over a small keyspace, in
//   SyncMode::kAlways. After each operation *returns*, it reports the
//   operation's index to the parent over a pipe.
//
//   The parent kills the child at a random moment. Because the child is
//   sequential, the acknowledged operations are exactly ops[0, A). Op A may
//   have been in flight: it might or might not have reached the disk. Every
//   later op was never started.
//
//   The parent replays the same seed into a std::map to compute the two
//   legal outcomes, state(A) and state(A+1), then opens the database and
//   requires that its ENTIRE contents equal one of the two. Anything else
//   is a durability or consistency bug.
//
//   Every Nth round also tears the tail of a data file at a random byte
//   before reopening. That destroys acknowledged data on purpose, so the
//   durability check is skipped; instead we require that Open() succeeds
//   and every value read back is one that was genuinely written to that key.
//
// usage: crash_test [iterations=300] [seed=1]

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ftw.h>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "bitkv/db.h"
#include "../src/file_util.h"

using namespace bitkv;

namespace {

constexpr int kKeyspace = 200;
constexpr int kMaxOps = 1'000'000;  // the child is always killed long before

enum class OpType { kPut, kDelete, kMerge };
struct Op {
  OpType type;
  int key;
};

// The workload is a pure function of (seed, index), so parent and child
// agree on it without sending it over the pipe.
class Workload {
 public:
  explicit Workload(uint64_t seed) : rng_(seed) {}
  Op Next() {
    const int r = int(rng_() % 1000);
    const int key = int(rng_() % kKeyspace);
    if (r < 5) return {OpType::kMerge, -1};
    if (r < 250) return {OpType::kDelete, key};
    return {OpType::kPut, key};
  }

 private:
  std::mt19937_64 rng_;
};

std::string Key(int k) { return "key" + std::to_string(k); }

// Values encode which op wrote them and for which key, so a torn-tail round
// can check that whatever survives is a genuine historical value.
std::string Value(int op_index, int key) {
  std::string v = "op" + std::to_string(op_index) + ":k" + std::to_string(key) + ":";
  v.append(size_t(op_index % 97), 'x');
  return v;
}

using Model = std::map<std::string, std::string>;

void ApplyOp(Model* m, const Op& op, int index) {
  if (op.type == OpType::kPut) (*m)[Key(op.key)] = Value(index, op.key);
  else if (op.type == OpType::kDelete) m->erase(Key(op.key));
}

void RemoveTree(const std::string& p) {
  ::nftw(p.c_str(), [](const char* f, const struct stat*, int, struct FTW*) { return ::remove(f); },
         16, FTW_DEPTH | FTW_PHYS);
}

[[noreturn]] void RunChild(const std::string& dir, uint64_t seed, int ack_fd) {
  Options o;
  o.sync_mode = SyncMode::kAlways;
  o.max_file_size = 4096;  // rotate constantly so merges have real work
  std::unique_ptr<DB> db;
  if (!DB::Open(o, dir, &db).ok()) ::_exit(3);
  Workload w(seed);
  for (int i = 0; i < kMaxOps; ++i) {
    Op op = w.Next();
    Status s;
    if (op.type == OpType::kPut) s = db->Put(Key(op.key), Value(i, op.key));
    else if (op.type == OpType::kDelete) s = db->Delete(Key(op.key));
    else s = db->Merge();
    if (!s.ok()) ::_exit(4);
    if (::write(ack_fd, &i, sizeof(i)) != sizeof(i)) ::_exit(5);
  }
  ::_exit(0);
}

// Reads every key in the keyspace into a map.
bool Snapshot(DB* db, Model* out, std::string* err) {
  out->clear();
  for (int k = 0; k < kKeyspace; ++k) {
    std::string v;
    Status s = db->Get(Key(k), &v);
    if (s.ok()) (*out)[Key(k)] = v;
    else if (!s.IsNotFound()) {
      *err = Key(k) + ": " + s.ToString();
      return false;
    }
  }
  return true;
}

std::string Diff(const Model& got, const Model& want) {
  for (int k = 0; k < kKeyspace; ++k) {
    auto a = got.find(Key(k)), b = want.find(Key(k));
    std::string av = a == got.end() ? "<absent>" : a->second;
    std::string bv = b == want.end() ? "<absent>" : b->second;
    if (av != bv) return Key(k) + " got=" + av.substr(0, 24) + " want=" + bv.substr(0, 24);
  }
  return "(sizes differ)";
}

}  // namespace

int main(int argc, char** argv) {
  const int iterations = argc > 1 ? std::atoi(argv[1]) : 300;
  const uint64_t base_seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
  std::mt19937_64 rng(base_seed * 7919);

  long total_acked = 0, torn_rounds = 0, mid_merge_kills = 0;
  const auto t0 = std::chrono::steady_clock::now();

  for (int it = 0; it < iterations; ++it) {
    char tmpl[] = "/tmp/bitkv-crash-XXXXXX";
    const std::string dir = ::mkdtemp(tmpl);
    const uint64_t seed = base_seed * 1'000'003 + uint64_t(it);

    int fds[2];
    if (::pipe(fds) != 0) return 1;
    pid_t pid = ::fork();
    if (pid == 0) {
      ::close(fds[0]);
      RunChild(dir, seed, fds[1]);
    }
    ::close(fds[1]);

    // Let it run for a random interval, then kill it without warning.
    std::this_thread::sleep_for(std::chrono::microseconds(rng() % 40'000));
    ::kill(pid, SIGKILL);
    int status;
    ::waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
      std::printf("FAIL iter %d: child exited with error %d before kill\n", it, WEXITSTATUS(status));
      return 1;
    }
    int last = -1, got;
    while (::read(fds[0], &got, sizeof(got)) == sizeof(got)) last = got;
    ::close(fds[0]);
    const int acked = last + 1;  // ops [0, acked) definitely completed
    total_acked += acked;

    // Rebuild the two legal outcomes.
    Workload w(seed);
    Model before, after;
    Op inflight{};
    for (int i = 0; i <= acked; ++i) {
      Op op = w.Next();
      if (i < acked) ApplyOp(&before, op, i);
      else inflight = op;
    }
    after = before;
    ApplyOp(&after, inflight, acked);
    if (inflight.type == OpType::kMerge) ++mid_merge_kills;

    const bool tear = (it % 10 == 9);
    if (tear) {
      // Chop a random number of bytes off the end of the newest non-empty
      // data file, as a crash on a filesystem without ordered writes might.
      std::vector<uint32_t> ids;
      ListDataFiles(dir, &ids);
      for (auto r = ids.rbegin(); r != ids.rend(); ++r) {
        const std::string p = DataFileName(dir, *r);
        std::string data;
        ReadWholeFile(p, &data);
        if (data.empty()) continue;
        const size_t chop = 1 + rng() % std::min<size_t>(data.size(), 300);
        if (::truncate(p.c_str(), off_t(data.size() - chop)) != 0) return 1;
        break;
      }
      ++torn_rounds;
    }

    std::unique_ptr<DB> db;
    Status s = DB::Open(Options{}, dir, &db);
    if (!s.ok()) {
      std::printf("FAIL iter %d seed %llu: reopen failed: %s\n", it,
                  (unsigned long long)seed, s.ToString().c_str());
      return 1;
    }
    Model actual;
    std::string err;
    if (!Snapshot(db.get(), &actual, &err)) {
      std::printf("FAIL iter %d seed %llu: read error %s\n", it, (unsigned long long)seed,
                  err.c_str());
      return 1;
    }

    if (!tear) {
      if (actual != before && actual != after) {
        std::printf("FAIL iter %d seed %llu (acked=%d): state matches neither legal outcome\n"
                    "  vs acked state:     %s\n  vs in-flight state: %s\n",
                    it, (unsigned long long)seed, acked, Diff(actual, before).c_str(),
                    Diff(actual, after).c_str());
        return 1;
      }
    } else {
      // Torn round: every surviving value must be genuine for its key.
      for (auto& [k, v] : actual) {
        const std::string want_tag = ":k" + k.substr(3) + ":";
        if (v.rfind("op", 0) != 0 || v.find(want_tag) == std::string::npos) {
          std::printf("FAIL iter %d seed %llu: %s holds a value never written to it: %s\n", it,
                      (unsigned long long)seed, k.c_str(), v.substr(0, 32).c_str());
          return 1;
        }
      }
    }
    db.reset();
    RemoveTree(dir);

    if ((it + 1) % 50 == 0) std::printf("  %d/%d rounds ok\n", it + 1, iterations), std::fflush(stdout);
  }

  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf(
      "PASS: %d kill -9 rounds, %ld acknowledged operations verified, "
      "%ld kills landed mid-merge, %ld torn-tail rounds, %.1fs\n",
      iterations, total_acked, mid_merge_kills, torn_rounds, secs);
  return 0;
}
