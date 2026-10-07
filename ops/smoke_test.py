#!/usr/bin/env python3
"""End-to-end smoke test against a running bitkv server.

Used by Ansible after deploys and upgrades, and by CI against the Kubernetes
deployment. Exits non-zero on the first failed check, so it can gate a
rollout.

  smoke_test.py [--addr HOST:PORT] [--metrics URL] [--keys N]
"""
import argparse
import os
import sys
import uuid

from bitkv_client import Client, read_metrics


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--addr", default=os.environ.get("BITKV_ADDR", "127.0.0.1:6380"))
    p.add_argument("--metrics", default=os.environ.get("BITKV_METRICS", "http://127.0.0.1:9121"))
    p.add_argument("--keys", type=int, default=200)
    args = p.parse_args()

    run = uuid.uuid4().hex[:8]  # isolate this run's keys from real data
    failures = []

    def check(name, ok, detail=""):
        print(f"[{'PASS' if ok else 'FAIL'}] {name}{(': ' + detail) if detail and not ok else ''}")
        if not ok:
            failures.append(name)

    with Client(args.addr) as c:
        check("PING", c.do("PING") == "PONG")
        for i in range(args.keys):
            c.do("SET", f"smoke:{run}:{i}", f"value-{i}")
        bad = [i for i in range(args.keys) if c.do("GET", f"smoke:{run}:{i}") != f"value-{i}"]
        check(f"SET/GET {args.keys} keys", not bad, f"{len(bad)} mismatched")
        c.do("SET", f"smoke:{run}:ctr", "0")
        check("INCR", [c.do("INCR", f"smoke:{run}:ctr") for _ in range(3)] == [1, 2, 3])
        deleted = sum(c.do("DEL", f"smoke:{run}:{i}") for i in range(args.keys))
        c.do("DEL", f"smoke:{run}:ctr")
        check("DEL", deleted == args.keys, f"deleted {deleted}")
        check("deleted keys are gone", c.do("GET", f"smoke:{run}:0") is None)

    try:
        m = read_metrics(args.metrics)
        check("metrics endpoint", m.get("bitkv_up") == 1.0)
        check("command counter moving", m.get('bitkv_commands_total{command="set"}', 0) >= args.keys)
    except OSError as e:
        check("metrics endpoint", False, str(e))

    if failures:
        sys.exit(f"smoke test FAILED: {', '.join(failures)}")
    print("smoke test passed")


if __name__ == "__main__":
    main()
