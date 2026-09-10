from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest

import run_observed


class NdjsonTests(unittest.TestCase):
    def test_one_truncated_trailing_record_is_ignored(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "events.ndjson"
            path.write_bytes(b'{"sequence":1}\n{"sequence":')
            event, error = run_observed.last_valid_ndjson_event(path)
            self.assertEqual(1, event["sequence"])
            self.assertIsNone(error)

    def test_invalid_non_trailing_record_is_reported(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "events.ndjson"
            path.write_bytes(b'{"sequence":\n{"sequence":2}\n')
            event, error = run_observed.last_valid_ndjson_event(path)
            self.assertIsNone(event)
            self.assertIn("invalid NDJSON record 1", error)


class ObserverLifecycleTests(unittest.TestCase):
    def test_foreground_run_preserves_exact_argv_and_writes_terminal_status(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory) / "attempt"
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                exit_code = run_observed.main([
                    "run",
                    "--run-dir", str(root),
                    "--name", "argv-test",
                    "--heartbeat-seconds", "0.05",
                    "--",
                    sys.executable,
                    "-c", "import sys; print(sys.argv[1])",
                    "argument with spaces",
                ])
            self.assertEqual(0, exit_code)
            invocation = json.loads((root / "invocation.json").read_text(encoding="utf-8"))
            status = json.loads((root / "status.json").read_text(encoding="utf-8"))
            self.assertEqual("argument with spaces", invocation["argv"][-1])
            self.assertEqual("passed", status["state"])
            self.assertEqual(0, status["exit_code"])
            self.assertIn("argument with spaces", (root / "stdout.log").read_text())

    def test_detached_attempt_is_gracefully_cancelled(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory) / "attempt"
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(0, run_observed.main([
                    "start",
                    "--run-dir", str(root),
                    "--name", "cancel-test",
                    "--heartbeat-seconds", "0.05",
                    "--",
                    sys.executable,
                    "-c", "import time; time.sleep(60)",
                ]))
            deadline = time.monotonic() + 10.0
            status = {}
            while time.monotonic() < deadline:
                status = run_observed.load_json(root / "status.json")
                if status.get("state") == "running":
                    break
                time.sleep(0.05)
            self.assertEqual("running", status.get("state"))
            while time.monotonic() < deadline:
                status = run_observed.load_json(root / "status.json")
                if status.get("activity") == "blocked":
                    break
                time.sleep(0.05)
            self.assertEqual("blocked", status.get("activity"), status)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(0, run_observed.main([
                    "cancel", "--run-dir", str(root)]))
            deadline = time.monotonic() + 10.0
            while time.monotonic() < deadline:
                status = run_observed.load_json(root / "status.json")
                if status.get("state") in run_observed.TERMINAL_STATES:
                    break
                time.sleep(0.05)
            self.assertEqual("cancelled", status.get("state"), status)
            self.assertEqual("cancelled", status.get("activity"), status)

    def test_failure_and_resumed_attempt_are_distinct_terminal_evidence(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            failed = root / "failed"
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(7, run_observed.main([
                    "run",
                    "--run-dir", str(failed),
                    "--name", "failure-test",
                    "--",
                    sys.executable,
                    "-c", "raise SystemExit(7)",
                ]))
            failed_status = run_observed.load_json(failed / "status.json")
            self.assertEqual("failed", failed_status["state"])
            self.assertEqual(7, failed_status["exit_code"])

            resumed = root / "resumed"
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(0, run_observed.main([
                    "run",
                    "--run-dir", str(resumed),
                    "--name", "resume-test",
                    "--resume-of", failed_status["attempt_id"],
                    "--",
                    sys.executable,
                    "-c", "pass",
                ]))
            resumed_status = run_observed.load_json(resumed / "status.json")
            self.assertEqual("passed", resumed_status["state"])
            self.assertEqual(failed_status["attempt_id"], resumed_status["resumed_from"])


if __name__ == "__main__":
    unittest.main()
