// Options: the knobs that decide bitkv's durability/performance trade-off.
#pragma once

#include <cstdint>

namespace bitkv {

// When does a write become durable (survive power loss)?
//
// write() only copies bytes into the OS page cache, which lives in RAM.
// fdatasync() asks the kernel to push them to the disk. That call is slow
// (it waits for the device), so *how often* we make it is the central
// durability/throughput trade-off of the whole engine.
enum class SyncMode {
  // Put() returns only after the record is on stable storage. Concurrent
  // writers share fsyncs via group commit (see db.cc), so throughput scales
  // with the number of writers instead of being capped at one fsync per write.
  kAlways,

  // A background thread fsyncs once per second. A crash can lose up to ~1s of
  // *acknowledged* writes. Same contract as Redis' `appendfsync everysec`.
  kEverySec,

  // Never fsync explicitly; the OS flushes whenever it likes. Fastest, and a
  // power failure can lose an arbitrary amount of recent data.
  kNone,
};

struct Options {
  SyncMode sync_mode = SyncMode::kEverySec;

  // Only meaningful with kAlways. When false, every Put() pays for its own
  // fsync while holding the write lock. Exists mainly so the benchmark can
  // show what group commit buys you.
  bool group_commit = true;

  // Once the active data file passes this size it is closed (made immutable)
  // and a new one is started. Smaller files = finer-grained compaction.
  uint64_t max_file_size = 64ull * 1024 * 1024;

  // Fail Open() if the directory does not exist (instead of creating it).
  bool error_if_missing = false;
};

}  // namespace bitkv
