// bitkv: a persistent, crash-safe key-value storage engine.
//
// Design: Bitcask (Sheehy & Smith, Basho, 2010). Every write is appended to a
// log file; an in-memory hash table ("keydir") maps each key to the position
// of its newest record. A read is one hash lookup plus one disk read. A
// background merge rewrites old files to reclaim space from overwritten and
// deleted keys. See DESIGN.md for the full on-disk format and invariants.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "bitkv/options.h"
#include "bitkv/status.h"

namespace bitkv {

struct Stats {
  uint64_t live_keys = 0;        // keys currently visible
  uint64_t data_files = 0;       // number of .data files on disk
  uint64_t disk_bytes = 0;       // total bytes in .data files
  uint64_t live_bytes = 0;       // bytes of records the keydir points at
  uint64_t fsyncs = 0;           // fdatasync calls issued since open
  uint64_t puts = 0;             // successful Put()/Delete() calls since open
  uint64_t recovered_torn_bytes = 0;  // bytes truncated from a torn tail at open
  uint64_t corrupt_records = 0;  // invalid records skipped in non-tail files

  // disk_bytes / live_bytes. 1.0 means no garbage; compaction drives it down.
  double SpaceAmplification() const {
    return live_bytes == 0 ? 0.0 : double(disk_bytes) / double(live_bytes);
  }
};

class DB {
 public:
  // Opens (and recovers) the database in `dir`, creating it if needed.
  // Only one process may have a directory open at a time (enforced by flock).
  static Status Open(const Options& options, const std::string& dir,
                     std::unique_ptr<DB>* db);

  ~DB();
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  // Thread-safe. Returns once the write is visible to Get(); whether it is
  // also durable depends on Options::sync_mode.
  Status Put(std::string_view key, std::string_view value);

  // Thread-safe. Returns Status::NotFound if the key does not exist.
  Status Get(std::string_view key, std::string* value) const;

  // Thread-safe. Writes a tombstone. Deleting a missing key is not an error.
  Status Delete(std::string_view key);

  // Rewrites every immutable data file, keeping only live records, then
  // atomically swaps them in. Safe to call while reads and writes continue.
  Status Merge();

  // Forces everything written so far onto stable storage.
  Status Sync();

  uint64_t Size() const;
  Stats GetStats() const;

 private:
  struct Impl;
  explicit DB(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace bitkv
