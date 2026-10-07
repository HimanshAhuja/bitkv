# SOP: upgrading bitkv

## Kubernetes (GitOps)

1. Bump `image.tag` in `deploy/helm/bitkv/values.yaml` in a pull request.
2. CI deploys the chart into a throwaway kind cluster and runs the smoke test.
3. Merge. Argo CD syncs the change.
4. The StatefulSet replaces the pod. On `SIGTERM` the server syncs to disk
   before exiting; `terminationGracePeriodSeconds` gives it time to.
5. The readiness probe (`bitkvctl health`) keeps the new pod out of the
   Service until it has recovered and answers PING.

Rollback: revert the commit; Argo CD syncs back.

## VMs (Ansible)

```bash
ansible-playbook -i inventory.ini deploy/ansible/upgrade.yml -e bitkv_version=<git ref>
```

The upgrade playbook works through hosts one at a time (`serial: 1`): take a
backup, stop, install, start, wait for health, run the smoke test, and stop
the whole rollout if any host fails.

## Before any upgrade

Take and verify a backup ([backup-and-restore.md](backup-and-restore.md)).
