# Runbook: high latency

**Symptoms:** `BitkvHighLatency` alert; `bitkv-doctor` reports p99 above 10 ms;
clients time out.

**Impact:** degraded service for all clients. The server is single-threaded,
so one slow operation delays every queued command behind it.

## Diagnose

1. Open the Grafana dashboard: compare the "p99 latency" and "fsyncs/sec"
   panels. Did they rise together?
2. Check the sync mode in the pod args or `/etc/default/bitkv`. In
   `--sync always` mode every SET waits for an fsync on the event-loop thread.
3. Disk health on the node: `iostat -x 1` (high `w_await`).
4. Did a large `COMPACT` run at the same time? The merge itself runs in
   the same thread when triggered via the `COMPACT` command.
5. Client behaviour: `bitkv_connected_clients` spiking means a connection
   storm; very large values inflate per-command time.

## Fix

- fsync-bound: if the durability contract allows, switch to `--sync everysec`
  (can lose about 1 s of writes on power failure; get sign-off first).
- Compaction-bound: schedule compaction off-peak.
- Disk-bound: move the PVC / EBS volume to faster storage.

## Verify

p99 back under SLO on the dashboard for 15 minutes; `bitkv-doctor` clean.

## Escalate

Latency high with low fsync rate, no compaction and a healthy disk: capture
`perf top -p $(pidof bitkv-server)` for 30 s and escalate.
