# bitkv

A crash-safe, persistent key-value storage engine in C++20, based on the
[Bitcask](https://riak.com/assets/bitcask-intro.pdf) design, with a
Redis-compatible network server, and the full operations stack to run it in
production: Kubernetes (Helm, Argo CD), AWS (Terraform), Ansible, Prometheus
and Grafana, a Go operations CLI, and runbooks.

```
$ redis-cli -p 6380 SET user:42 himansh
OK
$ kill -9 $(pidof bitkv-server)        # no shutdown, no final sync
$ ./bitkv-server &
bitkv: recovered 1 keys from 1 files in ./bitkv-data
$ redis-cli -p 6380 GET user:42
"himansh"
```

## What it does

- **Append-only log storage.** Every write is a sequential append. An in-memory
  hash index maps each key to its newest record, so every read is one `pread`.
- **Crash recovery.** Every record carries a CRC-32. On startup, torn writes
  are detected and truncated, and the index is rebuilt.
- **Three durability modes,** mirroring Redis: `always`, `everysec`, `none`.
- **Group commit.** Concurrent durable writers share fsyncs instead of paying
  for one each. About **3.3x** the throughput with 8 writers.
- **Online compaction.** A merge rewrites old files keeping only live data,
  while reads and writes continue, with a crash-safe commit protocol.
- **Hint files** for fast restarts after a merge.
- **Redis protocol server.** A single-threaded `epoll` event loop. The stock
  `redis-cli` and `redis-benchmark` work unmodified.

## Operations

Full guide: [docs/OPERATIONS.md](docs/OPERATIONS.md).

- **Kubernetes:** Helm chart with a StatefulSet and persistent volume,
  startup, liveness and readiness probes, a default-deny NetworkPolicy with zero
  egress, and a hardened pod (non-root, read-only root filesystem, no
  capabilities). The chart refuses configurations that would corrupt data.
- **GitOps:** an Argo CD Application keeps the cluster in sync with `main`.
- **CI on a real cluster:** every push deploys into kind, deletes the pod
  (gracefully and forcibly) and checks every key survives, and checks the
  NetworkPolicy blocks an unauthorised namespace.
- **AWS with Terraform:** EC2 with IMDSv2 and encrypted storage, a
  least-privilege IAM role, SSM access, and an encrypted, versioned S3 backup
  bucket.
- **Ansible:** host hardening (sshd, sysctl), a default-deny `ufw` firewall, a
  sandboxed systemd unit, and a one-host-at-a-time rolling upgrade that backs up
  first and halts on the first failure.
- **Observability:** Prometheus `/metrics`, alert rules with unit tests, and a
  Grafana dashboard. `docker compose up` runs the whole stack locally.
- **`bitkvctl` (Go):** operations CLI, load generator, and the Kubernetes
  readiness probe.
- **Python tooling:** online backup and checksum-verified restore, a smoke test
  that gates deploys, and `bitkv-doctor`, which diagnoses problems with
  rule-based checks and can ask an LLM for a root-cause summary grounded in
  the runbooks.
- **Runbooks** for every alert.

## How it is tested

| layer | what it checks |
|---|---|
| 57 unit tests (GoogleTest) | format, recovery, merge, concurrency, protocol |
| Model test | thousands of random operations, merges and restarts, checked against `std::map` at every step |
| `crash_test` | `kill -9` at random moments, hundreds of rounds; after each, the **entire** database must equal the acknowledged state exactly. Also tears files at random byte offsets |
| Sanitizers | the full suite under AddressSanitizer + UBSan and ThreadSanitizer, in CI |
| Mutation testing | bugs deliberately planted to confirm the tests catch them (see below) |
| Go and Python | `go test -race`; Python unit tests plus integration tests against a live server |
| Infrastructure | `helm lint` and Kubernetes schema validation, `terraform validate`, `ansible-lint` (production profile), `promtool test rules`, `actionlint` |
| Kubernetes end-to-end | real kind cluster in CI: deploy, `helm test`, pod deletion, NetworkPolicy enforcement |

A test suite that has never failed proves little, so each of these bugs was
injected on purpose and confirmed caught:

| planted bug | caught by |
|---|---|
| skip the merge commit-marker cleanup | `Recovery.InterruptedMergeCleanupDoesNotResurrectDeletedKey` |
| recovery ignores sequence numbers | only the randomized model test |
| merge overwrites keys written during the merge | `Merge.ConcurrentWritesDuringMergeAreNotLost` |
| CRC check removed | `Record.EverySingleBitFlipIsDetected` |
| writes buffered in user space | `crash_test`, on its first round |

**Scope of the crash tests:** `kill -9` proves correctness against *process*
crashes. It cannot prove safety against *power loss*, because killed
processes leave their writes in the kernel page cache. See
[DESIGN.md section 12](DESIGN.md#12-what-the-crash-tests-do-and-do-not-prove).

## Results

Measured with `bench` on a 1-vCPU Linux VM, ext4 disk, 13-byte keys,
100-byte values. **Numbers depend heavily on your disk; run `bench` yourself.**

| workload | ops/sec | p50 | p99 | fsyncs |
|---|---:|---:|---:|---:|
| put, `sync=none` | 713,372 | 1.1 µs | 3.7 µs | 0 |
| put, `sync=always`, 1 thread | 7,701 | 119 µs | 284 µs | 5,000 |
| put, `sync=always`, 8 threads, no group commit | 7,639 | 1,020 µs | 2,259 µs | 5,000 |
| put, `sync=always`, 8 threads, **group commit** | **25,378** | **289 µs** | 637 µs | **1,144** |
| get, random keys | 671,438 | 1.3 µs | 2.2 µs | 0 |

Compaction: 10,000 keys overwritten 10x went from 12.8 MiB (10x space
amplification) to 1.3 MiB (1.00x) in 63 ms. Recovery took 2.9 ms using hint
files, against 5.9 ms by full scan.

Against Redis 7.0.15 with `redis-benchmark` (50 clients, 100-byte values), the
two are roughly equal without pipelining, where both are network-bound. With
16-command pipelining Redis is about 1.4x faster on GET, which is expected: it
serves from RAM, while every bitkv GET is a `pread` system call.

## Quick start

Requires Linux (for the server), CMake 3.20+, and a C++20 compiler. On macOS
or Windows, use Docker (below). GoogleTest is downloaded automatically.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j

./build/bitkv_tests                       # unit + model tests
./build/crash_test 500                    # 500 rounds of kill -9
./build/bench --dir ./bench-data          # use a real disk, not tmpfs
./build/bitkv-server --sync everysec      # then: redis-cli -p 6380
```

Sanitizer builds:

```bash
cmake -B build-asan -DBITKV_SANITIZE=address && cmake --build build-asan -j && ./build-asan/bitkv_tests
cmake -B build-tsan -DBITKV_SANITIZE=thread  && cmake --build build-tsan -j && ./build-tsan/bitkv_tests
```

Docker:

```bash
docker build -t bitkv .
docker run --rm -p 6380:6380 bitkv
```

Server flags: `--metrics-port 9121` (0 disables) serves `/metrics`, `/healthz`
and `/readyz`.

### Server commands

`PING`, `ECHO`, `SET`, `GET`, `DEL`, `EXISTS`, `INCR`, `DECR`, `DBSIZE`,
`INFO`, `QUIT`, and `COMPACT` (bitkv-specific: runs a merge).

### Library use

```cpp
#include "bitkv/db.h"

bitkv::Options opts;
opts.sync_mode = bitkv::SyncMode::kAlways;
std::unique_ptr<bitkv::DB> db;
bitkv::Status s = bitkv::DB::Open(opts, "./data", &db);

db->Put("user:42", "himansh");
std::string value;
if (db->Get("user:42", &value).ok()) { /* ... */ }
db->Delete("user:42");
db->Merge();
```

## Layout

```
include/bitkv/      public API: db.h, options.h, status.h
src/                engine: record format, CRC, file I/O, db.cc (recovery, writes, merge)
server/             RESP parser, command dispatch, epoll event loop, metrics
tests/              GoogleTest suites
tools/              crash_test: the kill -9 harness
bench/              benchmark
ctl/                bitkvctl: Go operations CLI and readiness probe
ops/                Python: backup/restore, smoke test, bitkv-doctor
deploy/helm/        Kubernetes Helm chart
deploy/argocd/      GitOps Application
deploy/terraform/   AWS infrastructure
deploy/ansible/     host provisioning and rolling upgrades
deploy/monitoring/  Prometheus config, alerts, alert tests, Grafana dashboard
docs/               OPERATIONS.md and runbooks
```

## Documentation

- [DESIGN.md](DESIGN.md): on-disk format, recovery, the merge commit protocol,
  locking, and known limitations.
- [INTERVIEW.md](INTERVIEW.md): the design questions this project raises, and
  exercises for extending it.
- [docs/OPERATIONS.md](docs/OPERATIONS.md): deploying and running bitkv, the
  security model, and operations interview questions.
- [docs/runbooks](docs/runbooks): on-call procedures.

## Limitations

All keys must fit in memory; there are no range queries; a corrupt record in
the middle of a file discards the rest of that file. These are inherent to the
Bitcask design or deliberate simplifications; DESIGN.md section 11 explains each
and how a production engine would fix it.

## License

MIT
