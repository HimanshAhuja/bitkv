# Runbook: pod not starting

**Symptoms:** pod in `CrashLoopBackOff`, `Pending`, `OOMKilled` or `ImagePullBackOff`.

**Impact:** instance unavailable. With one replica this is an outage.

## Diagnose

```bash
kubectl -n bitkv get pods -l app.kubernetes.io/name=bitkv -o wide
kubectl -n bitkv describe pod bitkv-0        # read the Events section at the bottom
kubectl -n bitkv logs bitkv-0 --previous     # logs from the crashed container
```

| what you see | likely cause |
|---|---|
| `Pending`, event `FailedScheduling` | no node can satisfy the CPU/memory request, or the PVC cannot bind (no StorageClass) |
| `Pending`, PVC `Pending` | `kubectl -n bitkv get pvc`; no default StorageClass, or wrong `persistence.storageClass` |
| `OOMKilled` | the in-memory index outgrew `resources.limits.memory`. bitkv keeps every key in RAM |
| `ImagePullBackOff` | wrong image tag, or missing pull secret |
| `CrashLoopBackOff`, log says `already open by another process` | two pods mounted the same volume; check `replicaCount` is 1 |
| `CrashLoopBackOff`, log shows an `IOError` | disk full or read-only: `kubectl -n bitkv exec bitkv-0 -- df -h /data` |
| readiness probe failing, liveness fine | server is up but slow; see [high-latency.md](high-latency.md) |

## Fix

- **OOMKilled:** raise `resources.limits.memory` in Helm values. Rough sizing:
  keys x (average key length + about 80 bytes of index overhead). Commit and
  let Argo CD sync.
- **PVC pending:** set `persistence.storageClass` to a class that exists
  (`kubectl get storageclass`).
- **Disk full:** run a compaction once the pod is up
  ([high-space-amplification.md](high-space-amplification.md)), or expand the
  PVC if the StorageClass allows expansion.
- **Image:** correct `image.tag`.

Never delete the PVC to "fix" a crash loop: that deletes the data.

## Verify

Pod is `Running` and `1/1 Ready`; `bitkvctl health` passes from inside the
cluster; the startup log line reports the expected key count.

## Escalate

A crash with no clear cause in `--previous` logs: keep the PVC, scale to 0,
copy the data directory off for analysis, and escalate.
