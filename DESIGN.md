# bitkv design

This document explains how bitkv works and, more importantly, *why* each piece
is built the way it is. Most of the interesting decisions are about what
happens when the process dies at an inconvenient moment.

bitkv follows the design in the Bitcask paper (Justin Sheehy and David Smith,
Basho Technologies, 2010), which was the default storage engine of the Riak
database.

---

## 1. Goals and non-goals

**Goals**

- Never lose a write that was acknowledged as durable.
- Never return a value that was not written, and never resurrect a deleted key,
  no matter where a crash happens.
- One disk read per lookup.
- Sequential writes only, so write throughput is limited by the disk's
  sequential bandwidth or by fsync latency, never by seeks.
- Code small enough to read in an afternoon.

**Non-goals**

- Datasets whose *keys* do not fit in RAM. The whole index lives in memory.
- Range queries (`scan from a to m`). A hash index has no order.
- Transactions across multiple keys.
- Replication.

These are the same trade-offs Bitcask made. Section 11 discusses what you
would change to remove each one.

---

## 2. Architecture

```
            Put / Delete                          Get
                 |                                 |
                 v                                 v
       +-------------------+   key -> location   +----------------+
       |  append record to |-------------------->|  keydir        |
       |  active data file |                     |  (hash table   |
       +-------------------+                     |   in memory)   |
                 |                               +----------------+
                 v                                        |
   data dir:                                              | (file, offset, size)
     000000001.data  000000001.hint   <- immutable,       v
     000000002.data  000000002.hint      merged       one pread()
     000000007.data                   <- immutable
     000000008.data                   <- ACTIVE (appends go here)
     LOCK
```

Every write appends a record to the **active** data file and then points the
in-memory **keydir** at it. When the active file passes `max_file_size` it is
synced, closed for writing, and a new active file starts. Old files are
immutable forever; the only thing that ever removes them is a **merge**.

---

## 3. On-disk format

All integers are little-endian, encoded byte by byte, so files are portable
between machines regardless of CPU endianness.

### Data record

```
offset  size  field
0       4     crc         CRC-32 over bytes [4, end of record)
4       8     seq         sequence number, strictly increasing
12      4     key_size
16      4     value_size
20      1     flags       bit 0 set = tombstone
21      k     key
21+k    v     value
```

**Why a CRC?** After a crash, the end of the active file can hold half a
record. Its length fields might even look plausible. The CRC is how recovery
tells a real record from garbage that happens to parse. The test
`Record.EverySingleBitFlipIsDetected` flips every bit of a record one at a
time and checks that each flip is caught.

**Why a sequence number instead of a timestamp?** Recovery and merge both
answer "which of these two records is newer?" by comparing `seq`. Wall clocks
can jump backwards (NTP corrections, virtual machine migration); a counter
cannot. The sequence number is restored on open as `max seq seen + 1`.

**Why validate lengths before trusting them?** A torn header can contain any
bytes. `DecodeRecord` rejects a `value_size` above 256 MiB *before* allocating
anything, so a corrupt file can never make recovery attempt a 4 GiB allocation.

**Deletes** are ordinary records with the tombstone flag set and an empty
value. Nothing is ever erased in place.

### Hint file

Written next to each file produced by a merge. One entry per live key, listing
its location without its value, so recovery can rebuild the keydir without
reading values:

```
0   4  crc     4  8  seq    12  4  key_size    16  4  record_size
20  8  offset  28 1  flags  29  k  key
```

A hint file is purely an optimisation. If any entry fails its CRC, recovery
discards the hint file and scans the data file instead (tested in
`Recovery.CorruptHintFileFallsBackToScanningData`).

### Other files

| file | purpose |
|---|---|
| `NNNNNNNNN.data` | data files, numbered |
| `NNNNNNNNN.hint` | hint file for a merged data file |
| `MERGE` | commit marker for a merge in progress (section 8) |
| `LOCK` | held with `flock()` while the database is open |
| `*.tmp` | half-written merge output; deleted on open |

---

## 4. Write path

`Put(key, value)`:

1. Take the write lock (`mu`, exclusive).
2. If the record would push the active file past `max_file_size`, rotate.
3. Assign `seq = next_seq++`, encode the record, `write()` it to the end of
   the active file.
4. Point the keydir entry for `key` at `(active file, offset, size)`.
5. Release the lock.
6. Wait for durability, depending on the sync mode (section 5).

`Delete(key)` is the same, with a tombstone record, and removes the keydir
entry. Deleting a key that is not in the keydir writes nothing, because no
file anywhere holds a live record for it.

**If `write()` fails** (disk full, I/O error), part of a record may be at the
end of the file. Recovery stops reading at the first bad record, so anything
appended *after* the garbage would be silently lost on restart. bitkv
therefore makes the error sticky: every later write fails until the database
is reopened, at which point recovery truncates the damaged tail. RocksDB
handles background write errors the same way.

---

## 5. Durability and group commit

`write()` copies bytes into the operating system's page cache, which is RAM.
The data survives the *process* dying, because the kernel still has it. It
does not survive the *machine* losing power. `fdatasync()` forces the data
to the device and waits for it. That call is slow (tens of microseconds to
several milliseconds), so how often you make it is the central trade-off.

| mode | Put returns when | can lose on power failure |
|---|---|---|
| `kAlways` | the record is on stable storage | nothing acknowledged |
| `kEverySec` | the record is in the page cache; a thread syncs every second | about the last second |
| `kNone` | the record is in the page cache | whatever the OS had not flushed |

These deliberately mirror Redis' `appendfsync always / everysec / no`.

### Group commit

The naive way to implement `kAlways` is to call `fdatasync()` inside the write
lock after every record. That caps throughput at one write per fsync latency,
no matter how many threads are writing.

Group commit lets one fsync cover many writes:

1. Each writer appends its record under the lock, notes the log position just
   past its record (`my_lsn`), and releases the lock.
2. It then waits until `synced_lsn >= my_lsn`.
3. One waiter at a time becomes the **leader**. The leader snapshots the
   current end of the log, calls `fdatasync()` once, then publishes the new
   `synced_lsn` and wakes everyone.
4. While the leader is blocked inside `fdatasync()`, other writers keep
   appending. When it finishes, the next leader syncs all of them in a single
   call.

Measured on this project's benchmark, 8 concurrent writers: without group
commit, 5,000 writes cost 5,000 fsyncs. With it, they cost 1,144, and
throughput rose about 3.3x. See the README for the table.

**Two details that make it correct:**

- **Immutable files are always synced.** Rotation calls `fdatasync()` on the
  outgoing file in *every* sync mode. So the leader only ever needs to sync the
  active file: everything before it is already durable.
- **The leader `dup()`s the file descriptor.** It reads the active descriptor
  and the log position together under a shared lock, duplicates the
  descriptor, and syncs the duplicate *without* holding any lock. A rotation
  or merge can then close the original descriptor at any moment without
  pulling it out from under an in-progress fsync.

---

## 6. Read path

`Get(key)`:

1. Take the lock shared (many readers can hold it at once).
2. Look up the key in the keydir. Absent means not found, with zero disk I/O.
3. `pread()` exactly `record_size` bytes at `offset` from that file.
4. Verify the CRC, and verify the stored key equals the requested key.
5. Return the value.

The shared lock is held across the `pread()` so that a concurrent merge cannot
close the file descriptor mid-read.

Reads from the active file work immediately after a write, because `pread()`
reads the same page cache that `write()` filled.

---

## 7. Recovery

`DB::Open()`:

1. **Take the lock.** `flock(LOCK, LOCK_EX | LOCK_NB)`. Two processes
   appending to the same files would interleave records and corrupt both.
2. **Finish any interrupted merge** (if a `MERGE` marker exists; section 8).
3. **Delete `*.tmp` files** left by a merge that crashed before committing.
4. **Load every data file.** If it has a valid hint file, read the hint;
   otherwise scan the data file record by record.
5. **Resolve conflicts by sequence number.** For each record, keep it only if
   its `seq` is higher than what the keydir already holds for that key.
6. **Drop tombstones** from the keydir once every file is loaded.
7. **Open a brand-new active file.** Never append to a file from a previous
   run.

### Why conflicts are resolved by sequence number, not file order

A merge writes its output to files with *new, higher* ids. So file order no
longer matches write order: a merged file can hold an older version of a key
than a lower-numbered file does. Comparing `seq` makes recovery correct
regardless of the order files are read in.

This is not hypothetical. During development, a mutation that made recovery
ignore `seq` (keeping the last record seen) passed every targeted recovery
test and was caught only by the randomized model test.

### Why tombstones are kept until the end

Suppose file 3 holds a tombstone for `k` (seq 9) and file 7, a merge output,
holds an older value for `k` (seq 4). If recovery deleted `k` from the keydir
as soon as it saw the tombstone and forgot about it, reading file 7 afterwards
would bring `k` back. So during recovery a tombstone stays in the keydir as an
entry carrying its `seq`, which blocks any older value. Only after every file
is loaded are tombstones removed.

### Torn writes

If a record fails to decode while scanning a file, recovery truncates the file
at that offset and stops. Two shapes of damage are expected after a crash:

- **Truncated:** the record's length fields point past the end of the file.
  The process died partway through `write()`.
- **Zero-filled:** some filesystems persist a larger file size before the data
  blocks, so the tail reads back as zeros. A zero header has `key_size = 0`,
  which is rejected.

`Recovery.TornTailAtEveryByteOffset` cuts the final record at every possible
byte and checks that recovery always keeps exactly the earlier records.

### Why never append to an old file

Its tail might be the remains of a torn write. Recovery truncates what it
recognises as damage, but starting a fresh active file means no new record can
ever land after a region we failed to recognise.

---

## 8. Merge (compaction)

Overwrites and deletes leave dead records behind. With 10 overwrites per key,
90% of the disk is garbage. A merge copies only the live records into new
files and deletes the old ones.

The hard part is not copying. It is ordering the steps so that a crash between
any two of them leaves a correct database.

### Commit protocol

1. **Rotate**, so every file except a fresh active file is immutable. Those
   files are the merge's *victims*.
2. **Copy live records** from the victims into `NNN.data.tmp`, writing matching
   `NNN.hint.tmp` entries. A record is live if the keydir still points at
   exactly its file and offset. Records are copied byte for byte, keeping their
   original `seq`. Tombstones are dropped. fsync each output.
3. **Rename** the `.tmp` files to their real names. fsync the directory.
4. **Write the `MERGE` marker** listing the victim ids, atomically (write a
   temp file, fsync, rename, fsync the directory). *This is the commit point.*
5. **Swap**, under the exclusive lock: for every copied key whose keydir entry
   *still* points at the old location, repoint it to the new one. Close the
   victims' descriptors.
6. **Delete** the victim files, fsync the directory, delete the marker.

### What happens if it crashes at each step

| crash during | state on disk | recovery |
|---|---|---|
| 1-2 | victims intact, some `.tmp` files | `.tmp` deleted; merge never happened |
| 3 | victims intact plus some new files | duplicates have equal `seq`, so either copy is fine; victims still hold every tombstone. Correct. |
| after 4 | marker present, victims partly deleted | `Open()` reads the marker and deletes the remaining victims before loading |

### Why the marker is necessary

The merge drops tombstones. That is safe only if *all* the victims disappear
together. Consider:

- file 1 holds `k = "value"`
- file 2 holds `k = tombstone`

The merge copies neither (the key is deleted). Now suppose the process
crashes after deleting file 2 but before deleting file 1. On restart, recovery
finds the value and no tombstone. `k` comes back to life.

The marker turns "delete these files" into an all-or-nothing operation: once
it is durable, recovery is obligated to finish deleting the whole set before
it reads anything. `Recovery.InterruptedMergeCleanupDoesNotResurrectDeletedKey`
builds exactly this on-disk state and checks the key stays deleted. Removing
the marker logic makes that test fail.

### Writes during a merge

Reads and writes continue while the merge copies. If a key is overwritten
after its record was copied, the merge's copy is stale. Step 5 only repoints a
key if its keydir entry still names the *old* location, so the newer write
wins. On disk, the newer write has the higher `seq`, so recovery agrees. The
stale copy is garbage that the next merge removes.
`Merge.ConcurrentWritesDuringMergeAreNotLost` runs merges in a loop while a
second thread overwrites every key.

---

## 9. Concurrency

| lock | protects | held by |
|---|---|---|
| `mu` (shared_mutex) | keydir, file table, active file, `next_seq`, `written_lsn` | Get (shared), Put/Delete/rotate/merge-swap (exclusive) |
| `sync_mu` | `syncing`, `synced_lsn` | group-commit waiters and leader |
| `merge_mu` | one merge at a time | Merge |

**Lock order is always `mu` then `sync_mu`.** Nothing acquires `mu` while
holding `sync_mu`. The group-commit leader therefore releases `sync_mu`
before it takes `mu` to snapshot the log position. Violating this order
deadlocks: a writer holding `mu` would wait for `sync_mu` while the leader
holding `sync_mu` waits for `mu`.

The test suite passes under ThreadSanitizer.

**Known limitation:** a write takes `mu` exclusively, so it briefly blocks
readers. A production engine would let readers proceed against an immutable
snapshot of the index.

---

## 10. The network server

`bitkv-server` speaks RESP (the Redis protocol), so the unmodified `redis-cli`
and `redis-benchmark` work against it.

- **One thread, one `epoll` instance, non-blocking sockets.** This is Redis'
  own architecture. Each command runs to completion before the next starts,
  which makes every command atomic without locks. `INCR` is a read followed by
  a write, and it is still atomic, for exactly this reason.
- **The cost:** one slow command stalls every client. With `--sync always`, an
  fsync inside the event loop does exactly that.
- **The parser is incremental.** TCP delivers bytes in arbitrary pieces. The
  parser either consumes one complete command or reports that it needs more,
  never consuming partial input. `Resp.EveryPrefixIsIncomplete` feeds it every
  prefix of a command. Several commands in one read (pipelining) are handled
  in a loop.
- **Backpressure:** a client that sends faster than it reads replies is
  disconnected once its reply buffer passes 64 MiB, instead of letting it
  consume unbounded memory.
- `epoll` is Linux-only. On macOS or Windows, use the Dockerfile.

---

## 11. Known limitations and what I would do next

| limitation | why | the fix |
|---|---|---|
| All keys must fit in RAM | the keydir is an in-memory hash table | an LSM-tree (LevelDB, RocksDB): sorted files plus a sparse on-disk index |
| No range queries | hash tables are unordered | same: sorted SSTables, or a B-tree |
| A corrupt record mid-file discards the rest of that file | there is no way to find the next record boundary | LevelDB-style block framing: split the log into 32 KiB blocks so a reader can resynchronise at the next block |
| Merge reads each victim file whole into memory | simplicity | stream it in chunks |
| Writes block readers briefly | single `shared_mutex` | read against an immutable index snapshot |
| fsync in the event loop under `--sync always` stalls clients | single-threaded server | hand durable writes to a background thread and reply when they complete |
| Single node | out of scope | replicate the log (Raft) |

---

## 12. What the crash tests do and do not prove

`crash_test` and `Recovery.AcknowledgedWritesSurviveKill9` kill the process
with `SIGKILL` at random moments and check the reopened database against an
exact model of the acknowledged writes.

That proves bitkv is correct against **process crashes**: the ordering of
writes, renames and deletes, the merge protocol, recovery logic, and torn-tail
handling (the harness also truncates files at random offsets).

It does **not** prove correctness against **power loss**. When a process is
killed, everything it passed to `write()` is still safe in the kernel's page
cache. So a `kill -9` test passes even if `fdatasync()` were never called.
Testing power loss requires simulating a disk that drops writes which were not
yet synced, using tools such as LazyFS, Linux's `dm-log-writes`, or the ALICE
tool from the University of Wisconsin. That is the natural next step for this
project.
