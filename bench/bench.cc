
// bitkv benchmark.
//
// Measures, on whatever disk --dir lives on:
//   1. write throughput and latency in each sync mode
//   2. what group commit buys under concurrent durable writes
//   3. random-read throughput and latency
//   4. recovery time, with and without hint files
//   5. space amplification before and after a merge
//
// IMPORTANT: run it on a real disk. On Linux /tmp is often tmpfs (RAM), where
// fsync costs nothing and every durable-write number becomes meaningless.
//
// usage: bench [--dir PATH] [--ops N] [--value-size BYTES] [--threads N]

#include <dirent.h>
#include <ftw.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "bitkv/db.h"

using namespace bitkv;
using Clock = std::chrono::steady_clock;

namespace {

struct Config {
  std::string dir = "./bench-data";
  int ops = 100000;
  int value_size = 100;
  int threads = 8;
};

void RemoveTree(const std::string& p) {
  ::nftw(p.c_str(), [](const char* f, const struct stat*, int, struct FTW*) { return ::remove(f); },
         16, FTW_DEPTH | FTW_PHYS);
}

std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%010d", i);
  return buf;
}

struct Result {
  std::string name;
  long ops = 0;
  double secs = 0;
  std::vector<double> lat_us;
  uint64_t fsyncs = 0;
};

double Pct(std::vector<double>& v, double p) {
  if (v.empty()) return 0;
  size_t i = std::min(v.size() - 1, size_t(p / 100.0 * double(v.size())));
  std::nth_element(v.begin(), v.begin() + long(i), v.end());
  return v[i];
}

void PrintHeader() {
  std::printf("| %-40s | %11s | %9s | %9s | %9s | %8s |\n", "workload", "ops/sec", "p50 us",
              "p99 us", "p99.9 us", "fsyncs");
  std::printf("|%s|%s|%s|%s|%s|%s|\n", std::string(42, '-').c_str(), std::string(13, '-').c_str(),
              std::string(11, '-').c_str(), std::string(11, '-').c_str(),
              std::string(11, '-').c_str(), std::string(10, '-').c_str());
}

void PrintRow(Result r) {
  std::printf("| %-40s | %11.0f | %9.1f | %9.1f | %9.1f | %8llu |\n", r.name.c_str(),
              double(r.ops) / r.secs, Pct(r.lat_us, 50), Pct(r.lat_us, 99), Pct(r.lat_us, 99.9),
              (unsigned long long)r.fsyncs);
  std::fflush(stdout);
}

std::unique_ptr<DB> FreshDB(const Config& c, Options o) {
  RemoveTree(c.dir);
  std::unique_ptr<DB> db;
  Status s = DB::Open(o, c.dir, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "open: %s\n", s.ToString().c_str());
    std::exit(1);
  }
  return db;
}

// Runs `ops` puts split across `threads` writers, timing each one.
Result Writes(const Config& c, const std::string& name, Options o, int ops, int threads) {
  auto db = FreshDB(c, o);
  const std::string value(size_t(c.value_size), 'v');
  std::vector<std::vector<double>> lat(static_cast<size_t>(threads));
  const int per = ops / threads;

  auto t0 = Clock::now();
  std::vector<std::thread> ts;
  for (int t = 0; t < threads; ++t) {
    ts.emplace_back([&, t] {
      lat[size_t(t)].reserve(size_t(per));
      for (int i = 0; i < per; ++i) {
        auto a = Clock::now();
        db->Put(Key(t * per + i), value);
        lat[size_t(t)].push_back(std::chrono::duration<double, std::micro>(Clock::now() - a).count());
      }
    });
  }
  for (auto& th : ts) th.join();
  Result r;
  r.secs = std::chrono::duration<double>(Clock::now() - t0).count();
  r.name = name;
  r.ops = long(per) * threads;
  for (auto& l : lat) r.lat_us.insert(r.lat_us.end(), l.begin(), l.end());
  r.fsyncs = db->GetStats().fsyncs;
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  Config c;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string a = argv[i];
    if (a == "--dir") c.dir = argv[i + 1];
    else if (a == "--ops") c.ops = std::atoi(argv[i + 1]);
    else if (a == "--value-size") c.value_size = std::atoi(argv[i + 1]);
    else if (a == "--threads") c.threads = std::atoi(argv[i + 1]);
  }
  // Durable writes are orders of magnitude slower; scale them down so the
  // benchmark finishes on a real SSD in reasonable time.
  const int durable_ops = std::max(1000, c.ops / 20);

  utsname u{};
  ::uname(&u);
  std::printf("## bitkv benchmark\n\n");
  std::printf("- host: %s %s, %u hardware threads\n", u.sysname, u.release,
              std::thread::hardware_concurrency());
  std::printf("- data dir: `%s` (must be a real disk, not tmpfs)\n", c.dir.c_str());
  std::printf("- keys: 13 bytes, values: %d bytes, sequential keys, fresh DB per row\n",
              c.value_size);
  std::printf("- ops: %d for non-durable rows, %d for durable rows; latency per op\n\n", c.ops,
              durable_ops);

  std::printf("### Writes\n\n");
  PrintHeader();
  {
    Options o;
    o.sync_mode = SyncMode::kNone;
    PrintRow(Writes(c, "put, sync=none, 1 thread", o, c.ops, 1));
    o.sync_mode = SyncMode::kEverySec;
    PrintRow(Writes(c, "put, sync=everysec, 1 thread", o, c.ops, 1));
    o.sync_mode = SyncMode::kAlways;
    o.group_commit = false;
    PrintRow(Writes(c, "put, sync=always, 1 thread", o, durable_ops, 1));
    PrintRow(Writes(c, "put, sync=always, no group commit, " + std::to_string(c.threads) + "t", o,
                    durable_ops, c.threads));
    o.group_commit = true;
    PrintRow(Writes(c, "put, sync=always, group commit, " + std::to_string(c.threads) + "t", o,
                    durable_ops, c.threads));
  }

  std::printf("\n### Reads (random keys, data in page cache)\n\n");
  PrintHeader();
  {
    Options o;
    o.sync_mode = SyncMode::kNone;
    auto db = FreshDB(c, o);
    const std::string value(size_t(c.value_size), 'v');
    for (int i = 0; i < c.ops; ++i) db->Put(Key(i), value);
    for (int threads : {1, c.threads}) {
      std::vector<std::vector<double>> lat(static_cast<size_t>(threads));
      const int per = c.ops / threads;
      auto t0 = Clock::now();
      std::vector<std::thread> ts;
      for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&, t] {
          std::mt19937 rng(uint32_t(t + 1));
          std::string v;
          lat[size_t(t)].reserve(size_t(per));
          for (int i = 0; i < per; ++i) {
            auto a = Clock::now();
            db->Get(Key(int(rng() % uint32_t(c.ops))), &v);
            lat[size_t(t)].push_back(
                std::chrono::duration<double, std::micro>(Clock::now() - a).count());
          }
        });
      }
      for (auto& th : ts) th.join();
      Result r;
      r.secs = std::chrono::duration<double>(Clock::now() - t0).count();
      r.name = "get, " + std::to_string(threads) + " thread" + (threads > 1 ? "s" : "");
      r.ops = long(per) * threads;
      for (auto& l : lat) r.lat_us.insert(r.lat_us.end(), l.begin(), l.end());
      PrintRow(r);
    }
  }

  std::printf("\n### Compaction and recovery\n\n");
  {
    Options o;
    o.sync_mode = SyncMode::kNone;
    o.max_file_size = 4 * 1024 * 1024;
    const int keys = std::max(1000, c.ops / 10);
    const std::string value(size_t(c.value_size), 'v');
    {
      auto db = FreshDB(c, o);
      for (int round = 0; round < 10; ++round)
        for (int i = 0; i < keys; ++i) db->Put(Key(i), value);
      Stats before = db->GetStats();
      auto t0 = Clock::now();
      db->Merge();
      double merge_s = std::chrono::duration<double>(Clock::now() - t0).count();
      Stats after = db->GetStats();
      std::printf("- %d keys overwritten 10x: %.1f MiB on disk, space amplification %.2fx\n",
                  keys, double(before.disk_bytes) / 1048576.0, before.SpaceAmplification());
      std::printf("- merge took %.3fs -> %.1f MiB on disk, space amplification %.2fx\n", merge_s,
                  double(after.disk_bytes) / 1048576.0, after.SpaceAmplification());
    }
    auto reopen = [&] {
      auto t0 = Clock::now();
      std::unique_ptr<DB> db;
      DB::Open(o, c.dir, &db);
      return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    };
    std::printf("- recovery after merge (hint files): %.1f ms for %d keys\n", reopen(), keys);
    // Delete the hint files to force a full scan of the same data.
    std::vector<std::string> hints;
    if (auto* d = ::opendir(c.dir.c_str())) {
      while (auto* e = ::readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 5 && n.compare(n.size() - 5, 5, ".hint") == 0) hints.push_back(c.dir + "/" + n);
      }
      ::closedir(d);
    }
    for (auto& h : hints) ::unlink(h.c_str());
    std::printf("- recovery of the same data by full scan: %.1f ms\n", reopen());
  }
  RemoveTree(c.dir);
  return 0;
}
