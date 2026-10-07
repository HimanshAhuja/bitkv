// bitkv core. Read DESIGN.md alongside this file.
//
// LOCKING (the part to understand before touching anything)
//
//   mu        shared_mutex. Guards the keydir, the file-descriptor table, the
//             active file, next_seq and written_lsn. Get() takes it shared;
//             Put/Delete/rotation/merge-swap take it exclusive.
//   sync_mu   Guards group-commit state (syncing, synced_lsn).
//   merge_mu  Only one Merge() runs at a time.
//
//   Lock order is always  mu -> sync_mu.  Nothing ever acquires mu while
//   holding sync_mu. The group-commit leader therefore *releases* sync_mu
//   before taking mu. Breaking this order is how you get a deadlock.

#include "bitkv/db.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include "file_util.h"
#include "record.h"

namespace bitkv {
namespace {

// Where the newest record for a key lives on disk.
struct KeyDirEntry {
  uint32_t file_id = 0;
  uint32_t record_size = 0;
  uint64_t offset = 0;
  uint64_t seq = 0;
  bool tombstone = false;  // only ever true transiently, during recovery
};

// Lets the keydir be queried with a string_view without allocating a
// temporary std::string on every Get (C++20 heterogeneous lookup).
struct SvHash {
  using is_transparent = void;
  size_t operator()(std::string_view s) const noexcept {
    return std::hash<std::string_view>{}(s);
  }
};

using KeyDir = std::unordered_map<std::string, KeyDirEntry, SvHash, std::equal_to<>>;

}  // namespace

struct DB::Impl {
  Options opts;
  std::string dir;
  int lock_fd = -1;

  // ---- guarded by mu ----
  mutable std::shared_mutex mu;
  KeyDir keydir;
  std::map<uint32_t, int> fds;          // every open data file, by id
  std::map<uint32_t, uint64_t> sizes;   // bytes in each data file
  uint32_t active_id = 0;
  int active_fd = -1;
  uint32_t next_file_id = 1;
  uint64_t next_seq = 1;
  uint64_t written_lsn = 0;  // total bytes appended since Open, across all files
  uint64_t live_bytes = 0;   // sum of record_size over the keydir
  Status write_error;        // sticky: once a write fails, all writes fail

  // ---- guarded by sync_mu ----
  std::mutex sync_mu;
  std::condition_variable sync_cv;
  bool syncing = false;
  uint64_t synced_lsn = 0;   // every byte below this is on stable storage

  std::mutex merge_mu;

  // Background fsync thread for SyncMode::kEverySec.
  std::thread bg;
  std::mutex bg_mu;
  std::condition_variable bg_cv;
  bool stopping = false;

  std::atomic<uint64_t> fsyncs{0};
  std::atomic<uint64_t> puts{0};
  uint64_t recovered_torn_bytes = 0;
  uint64_t corrupt_records = 0;

  ~Impl();
  Status Recover();
  Status FinishInterruptedMerge();
  Status LoadFile(uint32_t id, uint64_t* max_seq);
  void Apply(std::string_view key, const KeyDirEntry& e);
  Status OpenActiveFile(uint32_t id);
  Status RotateLocked();
  Status Write(std::string_view key, std::string_view value, uint8_t flags);
  Status WaitDurable(uint64_t lsn);
  Status SyncAll();
  void BackgroundSync();
};

// ---------------------------------------------------------------------------
// Open and recovery
// ---------------------------------------------------------------------------

Status DB::Open(const Options& options, const std::string& dir,
                std::unique_ptr<DB>* db) {
  if (options.max_file_size < kRecordHeaderSize + 1)
    return Status::InvalidArgument("max_file_size too small");

  struct stat st;
  if (::stat(dir.c_str(), &st) != 0) {
    if (options.error_if_missing) return Status::InvalidArgument(dir + " does not exist");
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST)
      return Status::IOError(ErrnoMessage("mkdir " + dir));
  }

  auto impl = std::make_unique<Impl>();
  impl->opts = options;
  impl->dir = dir;

  // An advisory lock stops two processes (or two DB objects) from appending
  // to the same files at once, which would interleave records and corrupt
  // both writers' view of the log.
  impl->lock_fd = ::open(LockFileName(dir).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (impl->lock_fd < 0) return Status::IOError(ErrnoMessage("open LOCK"));
  if (::flock(impl->lock_fd, LOCK_EX | LOCK_NB) != 0)
    return Status::IOError("database " + dir + " is already open by another process");

  Status s = impl->Recover();
  if (!s.ok()) return s;

  if (options.sync_mode == SyncMode::kEverySec)
    impl->bg = std::thread([p = impl.get()] { p->BackgroundSync(); });

  db->reset(new DB(std::move(impl)));
  return Status::OK();
}

DB::DB(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DB::~DB() = default;

DB::Impl::~Impl() {
  if (bg.joinable()) {
    {
      std::lock_guard<std::mutex> lk(bg_mu);
      stopping = true;
    }
    bg_cv.notify_all();
    bg.join();
  }
  // A clean shutdown makes everything durable, whatever the sync mode.
  if (active_fd >= 0 && write_error.ok()) SyncAll();
  for (auto& [id, fd] : fds) ::close(fd);
  if (lock_fd >= 0) ::close(lock_fd);  // closing releases the flock
}

Status DB::Impl::Recover() {
  Status s = FinishInterruptedMerge();
  if (!s.ok()) return s;
  s = RemoveTempFiles(dir);
  if (!s.ok()) return s;

  std::vector<uint32_t> ids;
  s = ListDataFiles(dir, &ids);
  if (!s.ok()) return s;

  // Files can be loaded in any order: Apply() keeps whichever record has the
  // highest sequence number, so correctness never depends on file ids.
  uint64_t max_seq = 0;
  bool removed_any = false;
  for (uint32_t id : ids) {
    s = LoadFile(id, &max_seq);
    if (!s.ok()) return s;
    if (sizes[id] == 0) {  // an empty file left by an idle run; drop it
      ::close(fds[id]);
      fds.erase(id);
      sizes.erase(id);
      ::unlink(DataFileName(dir, id).c_str());
      removed_any = true;
    }
  }
  if (removed_any) {
    s = SyncDir(dir);
    if (!s.ok()) return s;
  }

  // Tombstones were kept during loading so an older value seen later could
  // not resurrect a deleted key. Now that every file is loaded, drop them.
  for (auto it = keydir.begin(); it != keydir.end();) {
    if (it->second.tombstone) {
      it = keydir.erase(it);
    } else {
      live_bytes += it->second.record_size;
      ++it;
    }
  }

  next_seq = max_seq + 1;
  // Never append to a file that existed before this run: its tail may be the
  // remains of a torn write. Always start a fresh active file.
  const uint32_t new_id = ids.empty() ? 1 : ids.back() + 1;
  return OpenActiveFile(new_id);
}

// A merge writes a MERGE marker listing the files it replaced, *after* the
// replacement files are durable and *before* deleting anything. If we find a
// marker, the merge committed but was interrupted mid-cleanup: finish it.
//
// Why this matters: merge drops tombstones. If we crashed after deleting the
// file holding a tombstone but before deleting the file holding the older
// value, recovery would resurrect a deleted key. Deleting the whole set via
// a durable marker makes the cleanup all-or-nothing.
Status DB::Impl::FinishInterruptedMerge() {
  const std::string marker = MergeMarkerName(dir);
  if (!FileExists(marker)) return Status::OK();
  std::string contents;
  Status s = ReadWholeFile(marker, &contents);
  if (!s.ok()) return s;
  std::istringstream in(contents);
  uint32_t id;
  while (in >> id) {
    ::unlink(DataFileName(dir, id).c_str());
    ::unlink(HintFileName(dir, id).c_str());
  }
  s = SyncDir(dir);
  if (!s.ok()) return s;
  ::unlink(marker.c_str());
  return SyncDir(dir);
}

void DB::Impl::Apply(std::string_view key, const KeyDirEntry& e) {
  auto it = keydir.find(key);
  if (it == keydir.end()) {
    keydir.emplace(std::string(key), e);
  } else if (e.seq > it->second.seq) {
    it->second = e;
  }
}

Status DB::Impl::LoadFile(uint32_t id, uint64_t* max_seq) {
  const std::string path = DataFileName(dir, id);
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return Status::IOError(ErrnoMessage("open " + path));
  fds[id] = fd;
  struct stat st;
  if (::fstat(fd, &st) != 0) return Status::IOError(ErrnoMessage("fstat " + path));
  const uint64_t file_size = static_cast<uint64_t>(st.st_size);
  sizes[id] = file_size;

  // Fast path: a hint file lists every key with its location, so we can
  // rebuild the keydir without reading a single value.
  const std::string hint_path = HintFileName(dir, id);
  if (FileExists(hint_path)) {
    std::string hint;
    if (ReadWholeFile(hint_path, &hint).ok()) {
      std::vector<std::pair<std::string_view, KeyDirEntry>> entries;
      size_t pos = 0;
      bool good = true;
      while (pos < hint.size()) {
        HintEntry h;
        size_t n;
        if (DecodeHint(hint.data() + pos, hint.size() - pos, &h, &n) != DecodeResult::kOk ||
            h.offset + h.record_size > file_size) {
          good = false;
          break;
        }
        entries.push_back({h.key, {id, h.record_size, h.offset, h.seq,
                                   bool(h.flags & kFlagTombstone)}});
        pos += n;
      }
      if (good) {
        for (auto& [k, e] : entries) {
          Apply(k, e);
          *max_seq = std::max(*max_seq, e.seq);
        }
        return Status::OK();
      }
      // A damaged hint file is only an optimisation lost; fall back to a scan.
    }
  }

  std::string data;
  Status s = ReadWholeFile(path, &data);
  if (!s.ok()) return s;

  size_t pos = 0;
  while (pos < data.size()) {
    Record rec;
    size_t n;
    DecodeResult r = DecodeRecord(data.data() + pos, data.size() - pos, &rec, &n);
    if (r != DecodeResult::kOk) {
      // We cannot find the next record boundary after a bad one, so the rest
      // of this file is unreadable. At the tail of the most recent file this
      // is the expected signature of a crash mid-write (kTruncated), or of a
      // file whose size was extended but whose blocks are still zero
      // (kCorrupt). Truncate so the garbage never confuses a later scan.
      if (r == DecodeResult::kCorrupt) ++corrupt_records;
      recovered_torn_bytes += data.size() - pos;
      if (::truncate(path.c_str(), static_cast<off_t>(pos)) != 0)
        return Status::IOError(ErrnoMessage("truncate " + path));
      int wfd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
      if (wfd >= 0) {
        SyncFd(wfd);
        ::close(wfd);
      }
      sizes[id] = pos;
      break;
    }
    Apply(rec.key, {id, static_cast<uint32_t>(n), pos, rec.seq, rec.tombstone()});
    *max_seq = std::max(*max_seq, rec.seq);
    pos += n;
  }
  return Status::OK();
}

Status DB::Impl::OpenActiveFile(uint32_t id) {
  const std::string path = DataFileName(dir, id);
  // O_APPEND: every write lands at the end, even if something else moved the
  // offset. pread() still works on this fd, which is how Get reads it.
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) return Status::IOError(ErrnoMessage("open " + path));
  // The new file's *name* must be durable too, or a crash could leave synced
  // records in a file that no directory entry points to.
  Status s = SyncDir(dir);
  if (!s.ok()) {
    ::close(fd);
    return s;
  }
  fds[id] = fd;
  sizes[id] = 0;
  active_id = id;
  active_fd = fd;
  next_file_id = std::max(next_file_id, id + 1);
  return Status::OK();
}

// Caller holds mu exclusively.
Status DB::Impl::RotateLocked() {
  // Sync the outgoing file unconditionally, in every sync mode. That gives
  // the invariant "every immutable file is fully durable", which keeps
  // group commit simple: it only ever has to sync the active file.
  Status s = SyncFd(active_fd);
  if (!s.ok()) return s;
  ++fsyncs;
  {
    std::lock_guard<std::mutex> lk(sync_mu);  // order: mu -> sync_mu
    synced_lsn = std::max(synced_lsn, written_lsn);
  }
  sync_cv.notify_all();
  return OpenActiveFile(next_file_id++);
}

// ---------------------------------------------------------------------------
// Write path
// ---------------------------------------------------------------------------

Status DB::Put(std::string_view key, std::string_view value) {
  return impl_->Write(key, value, 0);
}

Status DB::Delete(std::string_view key) {
  return impl_->Write(key, {}, kFlagTombstone);
}

Status DB::Impl::Write(std::string_view key, std::string_view value, uint8_t flags) {
  if (key.empty() || key.size() > kMaxKeySize)
    return Status::InvalidArgument("key size must be in [1, 65536]");
  if (value.size() > kMaxValueSize) return Status::InvalidArgument("value too large");
  const bool tombstone = flags & kFlagTombstone;

  uint64_t my_lsn = 0;
  {
    std::unique_lock<std::shared_mutex> lk(mu);
    if (!write_error.ok()) return write_error;

    // Deleting a key that does not exist needs no tombstone: nothing on disk
    // could resurrect it, because the keydir already reflects every file.
    if (tombstone && keydir.find(key) == keydir.end()) return Status::OK();

    const uint64_t rec_size = RecordSize(key.size(), value.size());
    if (sizes[active_id] > 0 && sizes[active_id] + rec_size > opts.max_file_size) {
      Status s = RotateLocked();
      if (!s.ok()) {
        write_error = s;
        return s;
      }
    }

    std::string buf;
    const uint64_t seq = next_seq++;
    EncodeRecord(seq, flags, key, value, &buf);
    const uint64_t offset = sizes[active_id];

    Status s = WriteAll(active_fd, buf.data(), buf.size());
    if (!s.ok()) {
      // A partial write leaves garbage at the tail of the active file, and
      // recovery stops reading at the first bad record. Any record appended
      // after it would be silently lost on restart, so refuse all further
      // writes until the DB is reopened (and recovery truncates the tail).
      write_error = s;
      return s;
    }
    sizes[active_id] += buf.size();
    written_lsn += buf.size();
    my_lsn = written_lsn;

    auto it = keydir.find(key);
    if (tombstone) {
      live_bytes -= it->second.record_size;
      keydir.erase(it);
    } else {
      KeyDirEntry e{active_id, static_cast<uint32_t>(buf.size()), offset, seq, false};
      if (it == keydir.end()) {
        keydir.emplace(std::string(key), e);
      } else {
        live_bytes -= it->second.record_size;
        it->second = e;
      }
      live_bytes += buf.size();
    }
    ++puts;

    if (opts.sync_mode == SyncMode::kAlways && !opts.group_commit) {
      // Naive durability: every writer pays a full fsync while holding the
      // write lock, so throughput is capped at roughly 1 / fsync_latency.
      s = SyncFd(active_fd);
      ++fsyncs;
      if (!s.ok()) {
        write_error = s;
        return s;
      }
      std::lock_guard<std::mutex> sl(sync_mu);
      synced_lsn = std::max(synced_lsn, written_lsn);
      return Status::OK();
    }
  }

  if (opts.sync_mode == SyncMode::kAlways) return WaitDurable(my_lsn);
  return Status::OK();
}

// Group commit.
//
// Every writer appends under mu, notes how far the log now extends
// (my_lsn), drops the lock, and then waits until synced_lsn >= my_lsn.
// Exactly one waiter at a time becomes the "leader": it snapshots the
// current end of the log, issues ONE fdatasync, and publishes the new
// synced_lsn, which releases every writer whose record it covered.
//
// While the leader is blocked in fdatasync, other writers keep appending.
// When it finishes, the next leader syncs all of *those* in one call. So
// under concurrency the number of fsyncs grows far more slowly than the
// number of writes; that is the entire throughput win.
Status DB::Impl::WaitDurable(uint64_t lsn) {
  std::unique_lock<std::mutex> lk(sync_mu);
  while (synced_lsn < lsn) {
    if (syncing) {
      sync_cv.wait(lk);
      continue;
    }
    syncing = true;
    lk.unlock();  // never take mu while holding sync_mu

    int fd;
    uint64_t target;
    {
      std::shared_lock<std::shared_mutex> rl(mu);
      // dup() so a concurrent rotation or merge can close the original
      // descriptor without pulling it out from under this fsync.
      fd = ::dup(active_fd);
      target = written_lsn;
    }
    Status s = fd < 0 ? Status::IOError(ErrnoMessage("dup")) : SyncFd(fd);
    if (fd >= 0) ::close(fd);
    ++fsyncs;

    lk.lock();
    syncing = false;
    if (s.ok()) synced_lsn = std::max(synced_lsn, target);
    sync_cv.notify_all();
    if (!s.ok()) return s;
  }
  return Status::OK();
}

Status DB::Impl::SyncAll() {
  uint64_t lsn;
  {
    std::shared_lock<std::shared_mutex> rl(mu);
    lsn = written_lsn;
  }
  return WaitDurable(lsn);
}

void DB::Impl::BackgroundSync() {
  std::unique_lock<std::mutex> lk(bg_mu);
  while (!stopping) {
    bg_cv.wait_for(lk, std::chrono::seconds(1), [this] { return stopping; });
    if (stopping) break;
    lk.unlock();
    SyncAll();
    lk.lock();
  }
}

Status DB::Sync() { return impl_->SyncAll(); }

// ---------------------------------------------------------------------------
// Read path
// ---------------------------------------------------------------------------

Status DB::Get(std::string_view key, std::string* value) const {
  // Held shared for the whole read, including the pread, so a concurrent
  // merge cannot close this file's descriptor mid-read.
  std::shared_lock<std::shared_mutex> lk(impl_->mu);
  auto it = impl_->keydir.find(key);
  if (it == impl_->keydir.end()) return Status::NotFound();
  const KeyDirEntry& e = it->second;

  auto fd_it = impl_->fds.find(e.file_id);
  if (fd_it == impl_->fds.end()) return Status::Corruption("keydir points at a missing file");

  std::string buf(e.record_size, '\0');
  Status s = PreadAll(fd_it->second, e.offset, e.record_size, buf.data());
  if (!s.ok()) return s;

  Record rec;
  size_t n;
  if (DecodeRecord(buf.data(), buf.size(), &rec, &n) != DecodeResult::kOk)
    return Status::Corruption("checksum mismatch reading " + std::string(key));
  if (rec.key != key) return Status::Corruption("keydir points at the wrong record");
  if (rec.tombstone()) return Status::NotFound();
  value->assign(rec.value.data(), rec.value.size());
  return Status::OK();
}

uint64_t DB::Size() const {
  std::shared_lock<std::shared_mutex> lk(impl_->mu);
  return impl_->keydir.size();
}

Stats DB::GetStats() const {
  std::shared_lock<std::shared_mutex> lk(impl_->mu);
  Stats st;
  st.live_keys = impl_->keydir.size();
  st.data_files = impl_->fds.size();
  for (auto& [id, sz] : impl_->sizes) st.disk_bytes += sz;
  st.live_bytes = impl_->live_bytes;
  st.fsyncs = impl_->fsyncs.load();
  st.puts = impl_->puts.load();
  st.recovered_torn_bytes = impl_->recovered_torn_bytes;
  st.corrupt_records = impl_->corrupt_records;
  return st;
}

// ---------------------------------------------------------------------------
// Merge (compaction)
// ---------------------------------------------------------------------------
//
// Commit protocol, in this exact order:
//   1. Rotate, so every file except the new active one is immutable.
//   2. Copy live records out of those files into NNN.data.tmp + NNN.hint.tmp,
//      preserving each record's original sequence number. fsync them.
//   3. Rename the .tmp files to their real names; fsync the directory.
//        crash here: old and new files both exist. Recovery sees duplicate
//        records with equal seq numbers (harmless), and the old files still
//        hold every tombstone. Correct.
//   4. Durably write a MERGE marker listing the old files.  <- commit point
//   5. Under the write lock, repoint keydir entries that still reference the
//      old location, and close the old descriptors.
//   6. Delete the old files, fsync the directory, delete the marker.
//        crash in 5 or 6: Open() sees the marker and finishes the deletes.

namespace {

struct Moved {
  std::string key;
  uint32_t old_file;
  uint64_t old_offset;
  uint32_t new_file;
  uint64_t new_offset;
};

struct MergeOutput {
  uint32_t id = 0;
  int fd = -1;
  uint64_t size = 0;
  std::string hint;
};

}  // namespace

Status DB::Merge() {
  Impl& d = *impl_;
  std::lock_guard<std::mutex> merge_lock(d.merge_mu);

  std::vector<uint32_t> victims;
  {
    std::unique_lock<std::shared_mutex> lk(d.mu);
    if (!d.write_error.ok()) return d.write_error;
    if (d.sizes[d.active_id] > 0) {
      Status s = d.RotateLocked();
      if (!s.ok()) return s;
    }
    for (auto& [id, fd] : d.fds)
      if (id != d.active_id) victims.push_back(id);
  }
  if (victims.empty()) return Status::OK();

  std::vector<MergeOutput> outputs;
  std::vector<Moved> moved;
  Status s;

  auto finish_output = [&](MergeOutput& o) -> Status {
    Status st = SyncFd(o.fd);
    ::close(o.fd);
    o.fd = -1;
    if (!st.ok()) return st;
    const std::string hint_tmp = HintFileName(d.dir, o.id) + ".tmp";
    int hfd = ::open(hint_tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (hfd < 0) return Status::IOError(ErrnoMessage("open " + hint_tmp));
    st = WriteAll(hfd, o.hint.data(), o.hint.size());
    if (st.ok()) st = SyncFd(hfd);
    ::close(hfd);
    return st;
  };

  auto new_output = [&]() -> Status {
    MergeOutput o;
    {
      std::unique_lock<std::shared_mutex> lk(d.mu);
      o.id = d.next_file_id++;
    }
    const std::string tmp = DataFileName(d.dir, o.id) + ".tmp";
    o.fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (o.fd < 0) return Status::IOError(ErrnoMessage("open " + tmp));
    outputs.push_back(std::move(o));
    return Status::OK();
  };

  auto cleanup_tmp = [&] {
    for (auto& o : outputs) {
      if (o.fd >= 0) ::close(o.fd);
      ::unlink((DataFileName(d.dir, o.id) + ".tmp").c_str());
      ::unlink((HintFileName(d.dir, o.id) + ".tmp").c_str());
    }
  };

  // Step 2: copy live records.
  for (uint32_t vid : victims) {
    std::string data;
    s = ReadWholeFile(DataFileName(d.dir, vid), &data);
    if (!s.ok()) break;
    size_t pos = 0;
    while (pos < data.size()) {
      Record rec;
      size_t n;
      if (DecodeRecord(data.data() + pos, data.size() - pos, &rec, &n) != DecodeResult::kOk)
        break;  // recovery already truncated torn tails; stop defensively
      const uint64_t rec_pos = pos;
      pos += n;
      if (rec.tombstone()) continue;  // see the commit protocol: safe to drop

      bool live;
      {
        std::shared_lock<std::shared_mutex> lk(d.mu);
        auto it = d.keydir.find(rec.key);
        live = it != d.keydir.end() && it->second.file_id == vid &&
               it->second.offset == rec_pos;
      }
      if (!live) continue;  // overwritten or deleted since: garbage

      if (outputs.empty() || outputs.back().size + n > d.opts.max_file_size) {
        if (!outputs.empty()) {
          s = finish_output(outputs.back());
          if (!s.ok()) break;
        }
        s = new_output();
        if (!s.ok()) break;
      }
      MergeOutput& o = outputs.back();
      // Copy the record bytes verbatim, sequence number and CRC included.
      s = WriteAll(o.fd, data.data() + rec_pos, n);
      if (!s.ok()) break;
      EncodeHint({rec.seq, static_cast<uint32_t>(n), o.size, 0, rec.key}, &o.hint);
      moved.push_back({std::string(rec.key), vid, rec_pos, o.id, o.size});
      o.size += n;
    }
    if (!s.ok()) break;
  }
  if (s.ok() && !outputs.empty() && outputs.back().fd >= 0) s = finish_output(outputs.back());
  if (!s.ok()) {
    cleanup_tmp();
    return s;
  }

  // Step 3: publish the new files.
  for (auto& o : outputs) {
    const std::string data_path = DataFileName(d.dir, o.id);
    const std::string hint_path = HintFileName(d.dir, o.id);
    if (::rename((data_path + ".tmp").c_str(), data_path.c_str()) != 0 ||
        ::rename((hint_path + ".tmp").c_str(), hint_path.c_str()) != 0) {
      cleanup_tmp();
      return Status::IOError(ErrnoMessage("rename merge output"));
    }
  }
  s = SyncDir(d.dir);
  if (!s.ok()) return s;

  // Step 4: commit point.
  std::string marker;
  for (uint32_t vid : victims) marker += std::to_string(vid) + "\n";
  s = AtomicWriteFile(d.dir, MergeMarkerName(d.dir), marker);
  if (!s.ok()) return s;

  // Step 5: swap under the write lock.
  {
    std::unique_lock<std::shared_mutex> lk(d.mu);
    for (auto& o : outputs) {
      int fd = ::open(DataFileName(d.dir, o.id).c_str(), O_RDONLY | O_CLOEXEC);
      if (fd < 0) return Status::IOError(ErrnoMessage("reopen merge output"));
      d.fds[o.id] = fd;
      d.sizes[o.id] = o.size;
    }
    for (const Moved& m : moved) {
      auto it = d.keydir.find(m.key);
      // Only repoint if nobody wrote this key during the merge. If they did,
      // the keydir already points at something newer; our copy is garbage
      // that the next merge will drop.
      if (it != d.keydir.end() && it->second.file_id == m.old_file &&
          it->second.offset == m.old_offset) {
        it->second.file_id = m.new_file;
        it->second.offset = m.new_offset;
      }
    }
    for (uint32_t vid : victims) {
      ::close(d.fds[vid]);
      d.fds.erase(vid);
      d.sizes.erase(vid);
    }
  }

  // Step 6: delete the replaced files, then the marker.
  for (uint32_t vid : victims) {
    ::unlink(DataFileName(d.dir, vid).c_str());
    ::unlink(HintFileName(d.dir, vid).c_str());
  }
  s = SyncDir(d.dir);
  if (!s.ok()) return s;
  ::unlink(MergeMarkerName(d.dir).c_str());
  return SyncDir(d.dir);
}

}  // namespace bitkv
