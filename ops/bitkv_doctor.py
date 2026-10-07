#!/usr/bin/env python3
"""bitkv-doctor: collect diagnostics, run rule-based checks, and optionally
ask an LLM for a root-cause analysis grounded in this repo's runbooks.

  bitkv_doctor.py [--addr HOST:PORT] [--metrics URL]
                  [--k8s NAMESPACE | --systemd]
                  [--ai] [--dry-run]

Design choices, deliberately conservative for production use:

1. Deterministic checks run first and are useful on their own. The AI step is
   optional and additive; the tool never depends on it to say something is
   wrong.
2. The LLM sees only collected evidence (metrics, pod status, recent logs)
   plus the runbooks, and is told to cite the runbook it relies on and to say
   "insufficient evidence" rather than guess. A human then verifies by
   following that runbook.
3. Stored keys and values are never collected, so no customer data is sent
   to a third-party model.
4. --dry-run prints the exact prompt without sending it, for review.

The AI step uses the Gemini REST API and needs GEMINI_API_KEY.
"""
import argparse
import glob
import json
import os
import subprocess
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bitkv_client import Client, read_metrics  # noqa: E402

RUNBOOK_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "runbooks")


def run(cmd):
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
        return (out.stdout + out.stderr).strip()[-4000:]  # bound what we send anywhere
    except (OSError, subprocess.TimeoutExpired) as e:
        return f"<{cmd[0]} unavailable: {e}>"


def histogram_quantile(metrics, q):
    """p-quantile estimate (upper bucket bound) from cumulative buckets."""
    prefix = 'bitkv_command_duration_seconds_bucket{le="'
    buckets = sorted(
        (float("inf") if k[len(prefix):-2] == "+Inf" else float(k[len(prefix):-2]), v)
        for k, v in metrics.items() if k.startswith(prefix))
    if not buckets or buckets[-1][1] == 0:
        return None
    target = q * buckets[-1][1]
    return next(le for le, count in buckets if count >= target)


def collect(args):
    ev = {"collected_unix": int(time.time()), "addr": args.addr}
    try:
        t0 = time.perf_counter()
        with Client(args.addr, timeout=3) as c:
            ev["ping"] = c.do("PING")
        ev["ping_ms"] = round((time.perf_counter() - t0) * 1000, 2)
    except OSError as e:
        ev["ping_error"] = str(e)
    try:
        m = read_metrics(args.metrics)
        keep = ("bitkv_keys", "bitkv_data_files", "bitkv_disk_bytes", "bitkv_live_bytes",
                "bitkv_space_amplification", "bitkv_recovered_torn_bytes", "bitkv_fsyncs_total",
                "bitkv_writes_total", "bitkv_connected_clients", "bitkv_command_errors_total",
                "bitkv_command_duration_seconds_count", "bitkv_uptime_seconds")
        ev["metrics"] = {k: m[k] for k in keep if k in m}
        p99 = histogram_quantile(m, 0.99)
        if p99 is not None:
            ev["metrics"]["command_p99_seconds_upper_bound"] = p99
    except OSError as e:
        ev["metrics_error"] = str(e)
    if args.k8s:
        sel = ["-n", args.k8s, "-l", "app.kubernetes.io/name=bitkv"]
        ev["k8s_pods"] = run(["kubectl", "get", "pods", "-o", "wide", *sel])
        ev["k8s_describe"] = run(["kubectl", "describe", "pods", *sel])
        ev["k8s_logs"] = run(["kubectl", "logs", "--tail=60", "--all-containers", *sel])
        ev["k8s_previous_logs"] = run(["kubectl", "logs", "--previous", "--tail=60", *sel])
        ev["k8s_events"] = run(["kubectl", "get", "events", "-n", args.k8s, "--sort-by=.lastTimestamp"])
    if args.systemd:
        ev["systemd_status"] = run(["systemctl", "status", "bitkv", "--no-pager"])
        ev["journal"] = run(["journalctl", "-u", "bitkv", "-n", "60", "--no-pager"])
    return ev


def checks(ev):
    """Rule-based findings: (severity, message, runbook)."""
    f = []
    m = ev.get("metrics", {})
    if "ping_error" in ev:
        f.append(("CRITICAL", f"data plane unreachable: {ev['ping_error']}", "server-unreachable.md"))
    elif ev.get("ping_ms", 0) > 50:
        f.append(("WARNING", f"PING took {ev['ping_ms']} ms", "high-latency.md"))
    if "metrics_error" in ev:
        f.append(("WARNING", f"metrics endpoint unreachable: {ev['metrics_error']}", "server-unreachable.md"))
    amp = m.get("bitkv_space_amplification", 0)
    if amp > 3:
        f.append(("WARNING", f"space amplification {amp:.1f}x: most of the disk is dead records",
                  "high-space-amplification.md"))
    if m.get("bitkv_recovered_torn_bytes", 0) > 0:
        f.append(("INFO", f"last startup truncated {int(m['bitkv_recovered_torn_bytes'])} torn bytes "
                          "(the previous run did not shut down cleanly)", "unclean-shutdown.md"))
    total = m.get("bitkv_command_duration_seconds_count", 0)
    if total > 100 and m.get("bitkv_command_errors_total", 0) / total > 0.01:
        f.append(("WARNING", f"{m['bitkv_command_errors_total'] / total:.1%} of commands returned errors",
                  "command-errors.md"))
    p99 = m.get("command_p99_seconds_upper_bound")
    if p99 is not None and p99 > 0.01:
        f.append(("WARNING", f"p99 command latency up to {p99 * 1000:.1f} ms", "high-latency.md"))
    pods = ev.get("k8s_pods", "") + ev.get("k8s_describe", "")
    for state in ("CrashLoopBackOff", "OOMKilled", "ImagePullBackOff", "Pending", "FailedScheduling"):
        if state in pods:
            f.append(("CRITICAL", f"pod state/event: {state}", "pod-not-starting.md"))
    return f


def load_runbooks():
    books = {}
    for path in sorted(glob.glob(os.path.join(RUNBOOK_DIR, "*.md"))):
        with open(path) as fh:
            books[os.path.basename(path)] = fh.read()
    return books


def build_prompt(ev, findings, runbooks):
    rb = "\n\n".join(f"=== runbook: {name} ===\n{text}" for name, text in runbooks.items())
    return f"""You are assisting an on-call engineer with a bitkv key-value store incident.

Rules:
- Use ONLY the evidence and runbooks below. Do not invent metrics, logs or commands.
- Name the single most likely root cause, the evidence lines that support it,
  and the runbook (by filename) whose steps the engineer should follow.
- If the evidence does not support a conclusion, say "insufficient evidence"
  and list what to collect next.
- Keep it under 200 words.

Rule-based findings already detected:
{json.dumps(findings, indent=2)}

Collected evidence:
{json.dumps(ev, indent=2)}

Runbooks:
{rb}
"""


def ask_gemini(prompt, model):
    key = os.environ.get("GEMINI_API_KEY")
    if not key:
        return "(AI analysis skipped: set GEMINI_API_KEY)"
    url = f"https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent"
    body = json.dumps({"contents": [{"parts": [{"text": prompt}]}],
                       "generationConfig": {"temperature": 0.1}}).encode()
    req = urllib.request.Request(url, data=body, headers={
        "Content-Type": "application/json", "x-goog-api-key": key})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            data = json.load(r)
        return data["candidates"][0]["content"]["parts"][0]["text"].strip()
    except Exception as e:  # the AI step must never break the diagnostic run
        return f"(AI analysis failed: {e})"


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--addr", default=os.environ.get("BITKV_ADDR", "127.0.0.1:6380"))
    p.add_argument("--metrics", default=os.environ.get("BITKV_METRICS", "http://127.0.0.1:9121"))
    target = p.add_mutually_exclusive_group()
    target.add_argument("--k8s", metavar="NAMESPACE", help="also collect Kubernetes pod state, logs, events")
    target.add_argument("--systemd", action="store_true", help="also collect systemd status and journal")
    p.add_argument("--ai", action="store_true", help="ask an LLM for a root-cause analysis")
    p.add_argument("--dry-run", action="store_true", help="print the AI prompt instead of sending it")
    p.add_argument("--model", default="gemini-2.5-flash")
    p.add_argument("--json", action="store_true", help="machine-readable output")
    args = p.parse_args()

    ev = collect(args)
    findings = checks(ev)
    if args.json:
        print(json.dumps({"evidence": ev, "findings": findings}, indent=2))
    else:
        print("bitkv-doctor findings")
        if not findings:
            print("  [OK] no problems detected by rule-based checks")
        for sev, msg, book in findings:
            print(f"  [{sev}] {msg}\n      -> docs/runbooks/{book}")
    if args.ai or args.dry_run:
        prompt = build_prompt(ev, findings, load_runbooks())
        if args.dry_run:
            print("\n--- prompt that would be sent ---\n" + prompt)
        else:
            print("\n--- AI root-cause analysis (verify against the cited runbook) ---")
            print(ask_gemini(prompt, args.model))
    sys.exit(2 if any(s == "CRITICAL" for s, _, _ in findings) else 0)


if __name__ == "__main__":
    main()
