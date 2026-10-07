# Runbook: high space amplification

**Symptoms:** `BitkvSpaceAmplificationHigh` alert; `bitkv_space_amplification`
above 3; disk usage growing while the key count is flat.

**Impact:** none yet. Left alone, the volume fills and writes start failing.

## Why it happens

Every overwrite and delete appends a new record; the old one becomes dead
bytes. `bitkv_space_amplification = disk_bytes / live_bytes`. A value of 5
means 80% of the disk is garbage.

## Diagnose

```bash
curl -s http://<host>:9121/metrics | grep -E 'bitkv_(space_amplification|disk_bytes|live_bytes)'
kubectl -n bitkv exec bitkv-0 -- df -h /data
```

Make sure there is free space of at least the live data size: compaction
writes the new files before deleting the old ones.

## Fix

```bash
bitkvctl --addr <host>:6380 compact
```

Compaction is online: reads and writes continue. It is crash-safe: if the
process dies midway, the next startup either discards the partial output or
finishes the cleanup (see DESIGN.md, section 8).

## Verify

`bitkv_space_amplification` close to 1.0 and `bitkv_disk_bytes` dropped.

## Escalate

Not enough free space to compact: expand the volume first, or restore from
backup onto a larger volume ([backup-and-restore.md](backup-and-restore.md)).
