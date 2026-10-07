# bitkv runbooks

Standard operating procedures for on-call. Each runbook has the same shape:
**Symptoms, Impact, Diagnose, Fix, Verify, Escalate.**

Start any incident with:

```bash
python3 ops/bitkv_doctor.py --k8s bitkv        # Kubernetes
python3 ops/bitkv_doctor.py --systemd          # VM / bare metal
```

It runs rule-based checks and points at the right runbook. Add `--ai` for an
LLM root-cause summary grounded in these documents (needs `GEMINI_API_KEY`).
Always verify an AI suggestion by following the runbook it cites.

| runbook | when |
|---|---|
| [server-unreachable.md](server-unreachable.md) | PING fails or times out |
| [pod-not-starting.md](pod-not-starting.md) | CrashLoopBackOff, Pending, OOMKilled, ImagePullBackOff |
| [high-latency.md](high-latency.md) | p99 above SLO, slow PING |
| [high-space-amplification.md](high-space-amplification.md) | disk growing faster than data |
| [unclean-shutdown.md](unclean-shutdown.md) | torn bytes recovered at startup |
| [command-errors.md](command-errors.md) | error ratio above 1% |
| [backup-and-restore.md](backup-and-restore.md) | taking backups, disaster recovery |
| [upgrade.md](upgrade.md) | rolling out a new version |
