#!/usr/bin/env python3
"""Online backup and offline restore for bitkv.

  backup.py backup  --addr HOST:PORT --data-dir DIR --out FILE.tar.gz
  backup.py verify  FILE.tar.gz
  backup.py restore FILE.tar.gz --data-dir DIR

BACKUP runs while the server is live. It first asks the server to COMPACT, so
nearly all data sits in immutable files, then copies every data and hint file
and records a SHA-256 for each in manifest.json.

The active file may be appended to while it is being copied, so the copy can
end in the middle of a record. That is fine: it looks exactly like a torn
write after a crash, and bitkv's recovery truncates it on startup. The backup
is therefore consistent as of the COMPACT, plus possibly some later writes.

A concurrent merge can delete a file mid-copy; the whole backup then retries.

RESTORE refuses to run while a server holds the database LOCK, verifies every
checksum before writing anything, and only then extracts.
"""
import argparse
import fcntl
import hashlib
import io
import json
import os
import sys
import tarfile
import time

from bitkv_client import Client

DATA_SUFFIXES = (".data", ".hint")


def sha256_bytes(b):
    return hashlib.sha256(b).hexdigest()


def snapshot(data_dir):
    files = {}
    for name in sorted(os.listdir(data_dir)):
        if name.endswith(DATA_SUFFIXES):
            with open(os.path.join(data_dir, name), "rb") as f:
                files[name] = f.read()
    return files


def backup(args):
    with Client(args.addr, timeout=args.timeout) as c:
        c.do("COMPACT")
        keys = c.do("DBSIZE")
    for attempt in range(1, 4):
        try:
            files = snapshot(args.data_dir)
            break
        except FileNotFoundError:
            print(f"a file vanished mid-copy (concurrent merge); retry {attempt}/3", file=sys.stderr)
            time.sleep(0.5)
    else:
        sys.exit("backup failed: data directory kept changing")

    manifest = {
        "created_unix": int(time.time()),
        "source_addr": args.addr,
        "keys_at_compact": keys,
        "files": {n: {"bytes": len(b), "sha256": sha256_bytes(b)} for n, b in files.items()},
    }
    with tarfile.open(args.out, "w:gz") as tar:
        for name, data in [("manifest.json", json.dumps(manifest, indent=2).encode())] + list(files.items()):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mtime = manifest["created_unix"]
            tar.addfile(info, io.BytesIO(data))
    total = sum(len(b) for b in files.values())
    print(f"backup ok: {len(files)} files, {total} bytes, {keys} keys -> {args.out}")


def load_and_verify(path):
    with tarfile.open(path, "r:gz") as tar:
        members = {m.name: tar.extractfile(m).read() for m in tar.getmembers() if m.isfile()}
    if "manifest.json" not in members:
        raise ValueError("no manifest.json in archive")
    manifest = json.loads(members.pop("manifest.json"))
    for name, meta in manifest["files"].items():
        if name not in members:
            raise ValueError(f"{name} listed in manifest but missing from archive")
        if os.path.basename(name) != name:
            raise ValueError(f"refusing path with directories: {name}")  # no ../ traversal
        if sha256_bytes(members[name]) != meta["sha256"]:
            raise ValueError(f"checksum mismatch for {name}")
    extra = set(members) - set(manifest["files"])
    if extra:
        raise ValueError(f"files not in manifest: {sorted(extra)}")
    return manifest, members


def verify(args):
    manifest, members = load_and_verify(args.archive)
    print(f"archive ok: {len(members)} files verified, {manifest['keys_at_compact']} keys at backup time")


def restore(args):
    manifest, members = load_and_verify(args.archive)  # verify everything before touching disk
    os.makedirs(args.data_dir, exist_ok=True)
    lock_path = os.path.join(args.data_dir, "LOCK")
    with open(lock_path, "a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            sys.exit("restore refused: a running server holds the LOCK; stop it first")
        existing = [n for n in os.listdir(args.data_dir) if n.endswith(DATA_SUFFIXES)]
        if existing and not args.force:
            sys.exit(f"restore refused: {args.data_dir} already has {len(existing)} data files (use --force)")
        for n in existing:
            os.unlink(os.path.join(args.data_dir, n))
        for name, data in members.items():
            tmp = os.path.join(args.data_dir, name + ".tmp")
            with open(tmp, "wb") as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, os.path.join(args.data_dir, name))
        dfd = os.open(args.data_dir, os.O_RDONLY)
        os.fsync(dfd)  # make the new directory entries durable
        os.close(dfd)
    print(f"restore ok: {len(members)} files into {args.data_dir}; start the server to recover")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("backup")
    b.add_argument("--addr", default=os.environ.get("BITKV_ADDR", "127.0.0.1:6380"))
    b.add_argument("--data-dir", required=True)
    b.add_argument("--out", required=True)
    b.add_argument("--timeout", type=float, default=60.0)
    v = sub.add_parser("verify")
    v.add_argument("archive")
    r = sub.add_parser("restore")
    r.add_argument("archive")
    r.add_argument("--data-dir", required=True)
    r.add_argument("--force", action="store_true")
    args = p.parse_args()
    try:
        {"backup": backup, "verify": verify, "restore": restore}[args.cmd](args)
    except (ValueError, OSError) as e:
        sys.exit(f"{args.cmd} failed: {e}")


if __name__ == "__main__":
    main()
