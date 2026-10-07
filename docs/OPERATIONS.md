# Operating bitkv

The storage engine is half the project. This half is about running it: on
Kubernetes, on cloud VMs, monitored, backed up, and diagnosable at 3 a.m.

```
 developer ── git push ──> GitHub Actions ──> image (GHCR)
                               |   tests, sanitizers, kill -9 harness,
                               |   IaC lint, real Kubernetes end-to-end
                               v
                         main branch  <── Argo CD watches ──> Kubernetes
                                                               StatefulSet + PVC
                                                               NetworkPolicy
                                                               probes: bitkvctl (Go)

 Terraform ──> AWS: EC2 (IMDSv2, encrypted) + least-privilege IAM + S3 backups
     └──> Ansible ──> hardened host: ufw, sshd, sysctl, sandboxed systemd unit

 Prometheus ──> alert rules ──> runbooks <── bitkv-doctor (rules + optional LLM)
     └──> Grafana dashboard
```

| directory | what | tool |
|---|---|---|
| `deploy/helm/bitkv` | Kubernetes deployment | Helm |
| `deploy/argocd` | GitOps sync | Argo CD |
| `deploy/terraform` | AWS infrastructure | Terraform |
| `deploy/ansible` | host configuration, rolling upgrades | Ansible |
| `deploy/monitoring` | scrape config, alerts, alert tests, dashboard | Prometheus, Grafana |
| `ctl/` | operations CLI and readiness probe | Go |
| `ops/` | backup/restore, smoke test, diagnostics | Python |
| `docs/runbooks` | on-call procedures | |

---

## 1. Run it yourself

Do each of these at least once. You cannot defend what you have not run.

### Local stack with dashboards (Docker)

```bash
docker compose up --build              # bitkv + Prometheus + Grafana
docker compose --profile load up -d    # add traffic
open http://localhost:3000             # dashboard "bitkv overview"
open http://localhost:9090/alerts      # alert rules
```

### Kubernetes (kind, on your laptop)

```bash
kind create cluster --name bitkv
docker build -t bitkv:dev .
kind load docker-image bitkv:dev --name bitkv
helm install bitkv deploy/helm/bitkv -n bitkv --create-namespace \
  --set image.repository=bitkv --set image.tag=dev --set image.pullPolicy=Never --wait
helm test bitkv -n bitkv

kubectl -n bitkv port-forward svc/bitkv 6380 9121 &
redis-cli -p 6380 SET hello world
kubectl -n bitkv delete pod bitkv-0          # the StatefulSet recreates it...
kubectl -n bitkv wait pod/bitkv-0 --for=condition=Ready
redis-cli -p 6380 GET hello                  # ...and the PVC kept the data

python3 ops/bitkv_doctor.py --k8s bitkv
```

Things to try deliberately, then diagnose with `bitkv-doctor` and the runbooks:
set `resources.limits.memory=16Mi` and load data (OOMKilled); set
`persistence.storageClass=nope` (Pending); run a client pod in a namespace
without the `bitkv-access=true` label (blocked by the NetworkPolicy).

### AWS (Terraform + Ansible)

Uses free-tier eligible resources. **Destroy it when you are done.**

```bash
cd deploy/terraform
cp terraform.tfvars.example terraform.tfvars   # set your IP and SSH key
terraform init && terraform plan && terraform apply
terraform output -raw ansible_inventory > ../ansible/inventory.ini

cd ../ansible
ansible-galaxy collection install -r requirements.yml
ansible-playbook site.yml                      # provision and verify
ansible-playbook upgrade.yml -e bitkv_version=main

cd ../terraform && terraform destroy
```

---

## 2. Security: defence in depth

The same rule ("only these networks may reach these ports") is enforced at
three independent layers, so one misconfiguration does not expose the data.

| layer | Kubernetes | VM / cloud |
|---|---|---|
| network | NetworkPolicy: default-deny ingress, **zero egress** | AWS security group + `ufw` default-deny |
| identity | no service-account token mounted | IAM role scoped to one S3 prefix; SSM instead of SSH |
| process | non-root, read-only root FS, all capabilities dropped, seccomp | systemd sandbox: `ProtectSystem=strict`, no capabilities, `NoNewPrivileges` |
| data | PVC; backups checksummed | encrypted EBS; S3 encrypted, versioned, public access blocked |
| metadata | | IMDSv2 required (blocks SSRF credential theft) |

The container runs under an arbitrary UID with group-0-writable data, which
is what OpenShift's restricted security context requires.

---

## 3. Observability

`/metrics` (port 9121) exposes Prometheus metrics; `/healthz` and `/readyz`
serve the Kubernetes probes. Every alert in `deploy/monitoring/alerts.yml`
links to a runbook, and `alerts_test.yml` unit-tests the alerts with
`promtool test rules` (they fire when they should and stay quiet when they
should not).

Command names are normalised before becoming metric labels. Echoing whatever
a client sends would let any client create unbounded time series, a classic
way to take down Prometheus (cardinality explosion).

---

## 4. Interview questions on the operations side

### Kubernetes

**Why a StatefulSet and not a Deployment?** A database needs a stable identity
and its own volume. A StatefulSet gives the pod a fixed name (`bitkv-0`), a
stable DNS record through the headless Service, and a PersistentVolumeClaim
that is re-attached when the pod is rescheduled. A Deployment treats pods as
interchangeable, which is wrong for something that owns data.

**Why exactly one replica?** bitkv is a single-node engine. Two pods writing
the same data directory would interleave records and corrupt it; the server's
`flock` would also make the second one crash-loop. The chart refuses
`replicaCount != 1`. Scaling out needs replication (Raft), which is the next
project, not a config flag.

**Why three probes?** The startup probe allows a long recovery (replaying
every data file) without liveness killing it midway. Liveness (`/healthz`)
restarts a wedged process. Readiness (`bitkvctl health`, in Go) checks the
real data path with a RESP PING, so a process whose HTTP side answers but
whose data path is stuck is removed from the Service without being killed.

**What happens when you delete the pod?** Kubernetes sends SIGTERM; the server
fsyncs and exits within `terminationGracePeriodSeconds`. The StatefulSet
creates `bitkv-0` again, the same PVC is mounted, recovery rebuilds the index,
and readiness admits it back. CI does exactly this, gracefully and with
`--force`, and checks all 500 keys survive.

**What does the headless Service do?** `clusterIP: None` makes DNS return the
pod's own IP, giving `bitkv-0.bitkv-headless.bitkv.svc.cluster.local`. The
StatefulSet requires one.

**How does the NetworkPolicy work?** It selects the bitkv pod and allows
ingress on 6380 only from namespaces labelled `bitkv-access=true`, and on 9121
only from `monitoring`. `egress: []` denies all outbound traffic. Probes still
work because the kubelet connects from the node. CI verifies the policy is
actually enforced: a pod in an unlabelled namespace is refused, a labelled one
connects.

**A pod is in CrashLoopBackOff. What do you do?** `kubectl describe pod` for
the events, then `kubectl logs --previous` for the crashed container's output.
Walk through `docs/runbooks/pod-not-starting.md`.

### Helm and GitOps

**Why Helm?** One templated chart instead of hand-maintained YAML per
environment, with values for what differs (image tag, storage class, memory).

**What is GitOps, and why Argo CD?** Git is the source of truth for what runs.
Argo CD continuously compares the cluster with the repository and syncs it.
Deploys are merges, rollbacks are reverts, there is an audit trail, and
`selfHeal` undoes manual changes made directly in the cluster.

### Ansible

**What does idempotent mean, and how do you know your playbook is?** Running
it again changes nothing if the host is already in the desired state. Run it
twice: the second run must report `changed=0`.

**How does the rolling upgrade avoid taking everything down?** `serial: 1`
upgrades one host at a time; `max_fail_percentage: 0` aborts the rollout the
moment any host fails; each host is backed up first and must pass the smoke
test before the next one starts.

**Why is the build a conditional task instead of a handler?** If a
handler-based build failed, the source checkout would already count as done,
and re-running the playbook would never retry the build. The lint exception is
documented in the role.

### Terraform and AWS

**What is the state file?** Terraform's record of what it created, mapped to
real resource IDs. Teams store it remotely (S3) with locking so two people
cannot apply at once. Never commit it: it can contain secrets.

**Why IMDSv2?** The instance metadata endpoint hands out the IAM role's
credentials. IMDSv1 answers any GET, so an SSRF bug in an app could leak them.
IMDSv2 requires a session token obtained with a PUT, which SSRF typically
cannot do.

**What is least privilege here?** The instance role can read and write objects
under `backups/` in one bucket and register with SSM. No other bucket, no IAM,
no EC2 API.

**`plan` vs `apply`?** `plan` shows the diff between the code and reality
without changing anything. Review it before every `apply`.

### Monitoring

**Why a histogram rather than an average for latency?** Averages hide tail
latency. Buckets let Prometheus compute p99 across instances with
`histogram_quantile`.

**How do you know your alerts work?** `promtool test rules` feeds synthetic
series and asserts each alert fires after its `for` duration and not before.

### GenAI in operations

**How do you stop an LLM from hallucinating during an incident?**
`bitkv-doctor` runs deterministic checks first and is useful without any AI.
The model sees only collected evidence plus the runbooks, must cite the
runbook it relies on, and must say "insufficient evidence" rather than guess;
a human verifies by following that runbook. It never sees stored values, so
no customer data leaves the system. `--dry-run` shows the exact prompt.

---

## 5. Honest limitations

- Single node: no replication or failover. A node failure means downtime
  until the pod is rescheduled and the volume re-attached.
- Backups are logical snapshots taken after a compaction; there is no
  point-in-time recovery.
- The S3 bucket and IAM policy exist, but `backup.py` writes to local disk;
  uploading to S3 is a scheduled-job wiring step left as an exercise.
- The kill -9 tests do not prove power-loss durability (DESIGN.md, section 12).
