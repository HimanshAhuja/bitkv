# Runbook: unclean shutdown detected

**Symptoms:** startup log says `truncated N torn bytes`;
`bitkv_recovered_torn_bytes` above 0.

**Impact:** normally none. This is recovery working as designed: the previous
process died mid-write, and the incomplete record (which was never
acknowledged to any client) was discarded.

## Diagnose why it died

- Kubernetes: `kubectl -n bitkv describe pod bitkv-0` (look for `OOMKilled`,
  liveness probe failures, node eviction) and `kubectl get events`.
- systemd: `journalctl -u bitkv -b -1` and `dmesg | grep -i -E 'oom|killed'`.
- A deliberate `SIGKILL` from a too-short `terminationGracePeriodSeconds`
  also causes this: the server needs time to fsync on `SIGTERM`.

## Fix

Address the cause of death (memory limit, probe thresholds, grace period).
No data repair is needed.

## Verify

The next deploy or restart logs no torn bytes.

## Escalate

Torn bytes on *every* restart, including clean ones, suggests the server is
being killed before it can sync: escalate with the shutdown logs.
