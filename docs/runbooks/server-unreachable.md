# Runbook: server unreachable

**Symptoms:** `bitkvctl health` fails; `BitkvDown` alert; clients get connection refused or timeouts.

**Impact:** total outage for every client of this instance.

## Diagnose

1. Is the process running?
   - Kubernetes: `kubectl -n bitkv get pods -l app.kubernetes.io/name=bitkv`
   - systemd: `systemctl status bitkv`
2. If it is not running, go to [pod-not-starting.md](pod-not-starting.md).
3. If it is running, test each plane separately:
   - data plane: `bitkvctl --addr <host>:6380 ping`
   - control plane: `curl -s http://<host>:9121/healthz`
   A healthy `/healthz` with a failing PING means the event loop is up but
   the client port is blocked: check the network path, not the server.
4. Check the network path:
   - Kubernetes: `kubectl -n bitkv get networkpolicy` and confirm the client's
     namespace is in the allowed list. Service DNS:
     `kubectl run -it --rm dbg --image=busybox:1.36 --restart=Never -- nslookup bitkv.bitkv.svc.cluster.local`
   - VM: `sudo ufw status verbose` and the AWS security group.
5. If both planes hang, the event loop is blocked. In `--sync always` mode a
   very slow disk stalls every client: check `iostat -x 1` for `%util` near
   100 and a high `w_await`.

## Fix

- Process down: follow [pod-not-starting.md](pod-not-starting.md).
- Network policy or firewall: correct the rule through Git (Helm values or
  Ansible variables), never by hand on the host, so the fix survives the next
  deploy.
- Disk stall: move to `--sync everysec` if the durability contract allows it,
  or move the volume to faster storage.

## Verify

`bitkvctl health` prints `healthy`, and `python3 ops/smoke_test.py` passes.

## Escalate

If the process is up, the network path is open, and PING still hangs after a
restart, capture `kubectl logs --previous` / `journalctl -u bitkv` and escalate.
