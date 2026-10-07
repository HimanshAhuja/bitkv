# SOP: backup and restore

## Take a backup (server running)

```bash
python3 ops/backup.py backup --addr <host>:6380 --data-dir /var/lib/bitkv \
    --out bitkv-$(date +%Y%m%d-%H%M).tar.gz
python3 ops/backup.py verify bitkv-*.tar.gz
```

The backup triggers a compaction, copies every data file, and records a
SHA-256 for each. Store it off the host (for example, an S3 bucket).

## Restore (disaster recovery)

1. Stop the server. Restore refuses to run while a server holds the
   database lock.
   - systemd: `sudo systemctl stop bitkv`
   - Kubernetes: `kubectl -n bitkv scale statefulset bitkv --replicas=0`
2. Restore. Every checksum is verified before anything is written:
   ```bash
   python3 ops/backup.py restore backup.tar.gz --data-dir /var/lib/bitkv --force
   ```
3. Start the server and confirm the startup log reports the expected key count.
4. Run `python3 ops/smoke_test.py`.

## Test restores regularly

A backup that has never been restored is not a backup. Restore into a scratch
directory and start a server on another port at least monthly.
