"""Tests for the ops tools.

Unit tests always run. Integration tests start a real server and run when
BITKV_SERVER points at a bitkv-server binary:

    BITKV_SERVER=build/bitkv-server python3 -m unittest discover -s ops/tests -v
"""
import os
import shutil
import socket
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
OPS = os.path.dirname(HERE)
sys.path.insert(0, OPS)

import bitkv_doctor  # noqa: E402
from bitkv_client import Client  # noqa: E402

SERVER = os.environ.get("BITKV_SERVER")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class DoctorChecks(unittest.TestCase):
    def test_histogram_quantile_picks_first_bucket_reaching_target(self):
        m = {
            'bitkv_command_duration_seconds_bucket{le="0.001"}': 90,
            'bitkv_command_duration_seconds_bucket{le="0.01"}': 99,
            'bitkv_command_duration_seconds_bucket{le="0.1"}': 100,
            'bitkv_command_duration_seconds_bucket{le="+Inf"}': 100,
        }
        self.assertEqual(bitkv_doctor.histogram_quantile(m, 0.50), 0.001)
        self.assertEqual(bitkv_doctor.histogram_quantile(m, 0.99), 0.01)
        self.assertIsNone(bitkv_doctor.histogram_quantile({}, 0.99))

    def test_unreachable_is_critical(self):
        f = bitkv_doctor.checks({"ping_error": "connection refused"})
        self.assertEqual(f[0][0], "CRITICAL")
        self.assertEqual(f[0][2], "server-unreachable.md")

    def test_healthy_server_has_no_findings(self):
        ev = {"ping": "PONG", "ping_ms": 0.4, "metrics": {
            "bitkv_space_amplification": 1.2, "bitkv_recovered_torn_bytes": 0,
            "bitkv_command_duration_seconds_count": 10000, "bitkv_command_errors_total": 2}}
        self.assertEqual(bitkv_doctor.checks(ev), [])

    def test_high_space_amplification_points_at_compaction_runbook(self):
        f = bitkv_doctor.checks({"metrics": {"bitkv_space_amplification": 6.0}})
        self.assertIn("high-space-amplification.md", [x[2] for x in f])

    def test_error_ratio_threshold(self):
        bad = {"metrics": {"bitkv_command_duration_seconds_count": 1000, "bitkv_command_errors_total": 50}}
        ok = {"metrics": {"bitkv_command_duration_seconds_count": 1000, "bitkv_command_errors_total": 5}}
        self.assertIn("command-errors.md", [x[2] for x in bitkv_doctor.checks(bad)])
        self.assertNotIn("command-errors.md", [x[2] for x in bitkv_doctor.checks(ok)])

    def test_kubernetes_crashloop_detected(self):
        f = bitkv_doctor.checks({"k8s_pods": "bitkv-0  0/1  CrashLoopBackOff  5  3m"})
        self.assertIn(("CRITICAL", "pod state/event: CrashLoopBackOff", "pod-not-starting.md"), f)

    def test_every_referenced_runbook_exists(self):
        books = bitkv_doctor.load_runbooks()
        referenced = {"server-unreachable.md", "high-latency.md", "high-space-amplification.md",
                      "unclean-shutdown.md", "command-errors.md", "pod-not-starting.md"}
        self.assertTrue(referenced <= set(books), referenced - set(books))

    def test_prompt_contains_evidence_and_rules_but_no_values(self):
        p = bitkv_doctor.build_prompt({"ping": "PONG"}, [], {"x.md": "runbook text"})
        self.assertIn("Use ONLY the evidence", p)
        self.assertIn("runbook text", p)


@unittest.skipUnless(SERVER, "set BITKV_SERVER to run integration tests")
class Integration(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="bitkv-ops-")
        self.data = os.path.join(self.tmp, "data")
        self.port, self.mport = free_port(), free_port()
        self.addr = f"127.0.0.1:{self.port}"
        self.metrics = f"http://127.0.0.1:{self.mport}"
        self.proc = self.start(self.data, self.port, self.mport)

    def start(self, data, port, mport):
        p = subprocess.Popen([SERVER, "--dir", data, "--port", str(port), "--metrics-port", str(mport)],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(50):
            try:
                with Client(f"127.0.0.1:{port}") as c:
                    if c.do("PING") == "PONG":
                        return p
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("server did not start")

    def tearDown(self):
        self.proc.terminate()
        self.proc.wait(timeout=10)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_tool(self, *args):
        return subprocess.run([sys.executable, *args], capture_output=True, text=True, cwd=OPS)

    def test_smoke_test_passes(self):
        r = self.run_tool("smoke_test.py", "--addr", self.addr, "--metrics", self.metrics)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_backup_restore_round_trip(self):
        with Client(self.addr) as c:
            for i in range(300):
                c.do("SET", f"k{i}", f"v{i}")
            c.do("DEL", "k7")
        archive = os.path.join(self.tmp, "b.tar.gz")
        r = self.run_tool("backup.py", "backup", "--addr", self.addr, "--data-dir", self.data, "--out", archive)
        self.assertEqual(r.returncode, 0, r.stderr)

        # Restore must refuse while the source server holds the lock.
        r = self.run_tool("backup.py", "restore", archive, "--data-dir", self.data, "--force")
        self.assertNotEqual(r.returncode, 0)

        restored = os.path.join(self.tmp, "restored")
        r = self.run_tool("backup.py", "restore", archive, "--data-dir", restored)
        self.assertEqual(r.returncode, 0, r.stderr)
        p2, m2 = free_port(), free_port()
        proc = self.start(restored, p2, m2)
        try:
            with Client(f"127.0.0.1:{p2}") as c:
                self.assertEqual(c.do("DBSIZE"), 299)
                self.assertEqual(c.do("GET", "k123"), "v123")
                self.assertIsNone(c.do("GET", "k7"))
        finally:
            proc.terminate()
            proc.wait(timeout=10)

    def test_tampered_backup_is_rejected(self):
        with Client(self.addr) as c:
            c.do("SET", "a", "b" * 200)
        archive = os.path.join(self.tmp, "b.tar.gz")
        self.run_tool("backup.py", "backup", "--addr", self.addr, "--data-dir", self.data, "--out", archive)
        bad = os.path.join(self.tmp, "bad.tar.gz")
        with tarfile.open(archive) as src, tarfile.open(bad, "w:gz") as dst:
            for m in src.getmembers():
                data = src.extractfile(m).read()
                if m.name.endswith(".data") and len(data) > 30:
                    data = data[:30] + bytes([data[30] ^ 0xFF]) + data[31:]
                m.size = len(data)
                import io
                dst.addfile(m, io.BytesIO(data))
        r = self.run_tool("backup.py", "verify", bad)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("checksum mismatch", r.stderr)

    def test_doctor_reports_healthy_then_unreachable(self):
        r = self.run_tool("bitkv_doctor.py", "--addr", self.addr, "--metrics", self.metrics)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.proc.terminate()
        self.proc.wait(timeout=10)
        r = self.run_tool("bitkv_doctor.py", "--addr", self.addr, "--metrics", self.metrics)
        self.assertEqual(r.returncode, 2)
        self.assertIn("server-unreachable.md", r.stdout)
        self.proc = self.start(self.data, self.port, self.mport)  # for tearDown


if __name__ == "__main__":
    unittest.main()
