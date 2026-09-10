#!/usr/bin/env python3
"""Verify checkpointed cancellation and a byte-identical detached restart."""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import time
from typing import Any, Callable

import run_observed


def _wait_status(
    run_directory: Path,
    predicate: Callable[[dict[str, Any]], bool],
    timeout_seconds: float = 20.0,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_seconds
    status: dict[str, Any] = {}
    while time.monotonic() < deadline:
        try:
            status = run_observed.load_json(run_directory / "status.json")
        except run_observed.ObserverError:
            time.sleep(0.01)
            continue
        if predicate(status):
            return status
        time.sleep(0.01)
    raise RuntimeError(f"timed out waiting for observed status: {status}")


def _checkpoint_seen(path: Path, identity: str) -> bool:
    if not path.is_file():
        return False
    for line in path.read_text(encoding="utf-8", errors="strict").splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("checkpoint_id") == identity:
            return True
    return False


def _wait_checkpoint(path: Path, identity: str, run_directory: Path) -> None:
    deadline = time.monotonic() + 20.0
    while time.monotonic() < deadline:
        if _checkpoint_seen(path, identity):
            return
        try:
            status = run_observed.load_json(run_directory / "status.json")
            if status.get("state") in run_observed.TERMINAL_STATES:
                raise RuntimeError(
                    f"attempt terminated before checkpoint {identity}: {status}")
        except run_observed.ObserverError:
            pass
        time.sleep(0.005)
    raise RuntimeError(f"timed out waiting for checkpoint {identity}")


def _tree_hashes(root: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for path in sorted(candidate for candidate in root.rglob("*") if candidate.is_file()):
        result[path.relative_to(root).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def _start(arguments: list[str]) -> None:
    with contextlib.redirect_stdout(io.StringIO()):
        if run_observed.main(arguments) != 0:
            raise RuntimeError("could not start observed attempt")


def _cancel(run_directory: Path, *, force: bool = False) -> None:
    arguments = ["cancel", "--run-dir", str(run_directory)]
    if force:
        arguments.append("--force")
    with contextlib.redirect_stdout(io.StringIO()):
        run_observed.main(arguments)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    arguments = parser.parse_args()
    binary = arguments.binary.resolve(strict=True)
    repository = Path(__file__).resolve().parents[1]

    with tempfile.TemporaryDirectory() as temporary_directory:
        root = Path(temporary_directory)
        configuration = root / "synthetic_cancel.toml"
        configuration.write_text(
            (repository / "tests" / "data" / "synthetic_p0.toml")
            .read_text(encoding="utf-8"),
            encoding="utf-8",
        )
        shared_output = root / "shared-output"
        shared_cache = root / "shared-cache"
        cancelled_attempt = root / "attempt-cancelled"
        cancelled_events = root / "events-cancelled.ndjson"
        cancelled_report = root / "benchmark-cancelled.json"
        attempts = [cancelled_attempt]
        try:
            _start([
                "start",
                "--run-dir", str(cancelled_attempt),
                "--name", "cli-cancel-test",
                "--heartbeat-seconds", "0.02",
                "--",
                str(binary),
                "benchmark", str(configuration),
                "--output", str(cancelled_report),
                "--output-directory", str(shared_output),
                "--cache-directory", str(shared_cache),
                "--run-id", "cli-cancel-test",
                "--run-state-directory", str(root / "state-cancelled"),
                "--event-log", str(cancelled_events),
                "--progress-interval-seconds", "0.02",
                "--memory-budget-mib", "256",
                "--decoded-cache-budget-mib", "64",
                "--scratch-budget-mib", "512",
                "--threads", "1",
                "--json",
            ])
            _wait_checkpoint(
                cancelled_events, "benchmark-scan", cancelled_attempt)
            _cancel(cancelled_attempt)
            cancelled_status = _wait_status(
                cancelled_attempt,
                lambda value: value.get("state") in run_observed.TERMINAL_STATES,
            )
            if (cancelled_status.get("state"), cancelled_status.get("exit_code")) != (
                    "cancelled", 130):
                raise RuntimeError(f"CLI did not cooperatively exit 130: {cancelled_status}")
            partial = json.loads(cancelled_report.read_text(encoding="utf-8"))
            event, event_error = run_observed.last_valid_ndjson_event(cancelled_events)
            if (partial.get("status") != "cancelled"
                    or not partial.get("phase_complete", {}).get("scan")
                    or event_error is not None
                    or event is None
                    or event.get("state") != "cancelled"
                    or event.get("error", {}).get("code") != "cancelled"):
                raise RuntimeError("cancelled attempt did not retain structured partial evidence")

            resumed_attempt = root / "attempt-resumed"
            attempts.append(resumed_attempt)
            resumed_report = root / "benchmark-resumed.json"
            _start([
                "start",
                "--run-dir", str(resumed_attempt),
                "--name", "cli-resume-test",
                "--resume-of", str(cancelled_status["attempt_id"]),
                "--heartbeat-seconds", "0.02",
                "--",
                str(binary),
                "benchmark", str(configuration),
                "--output", str(resumed_report),
                "--output-directory", str(shared_output),
                "--cache-directory", str(shared_cache),
                "--run-id", "cli-resume-test",
                "--run-state-directory", str(root / "state-resumed"),
                "--event-log", str(root / "events-resumed.ndjson"),
                "--progress-interval-seconds", "0.02",
                "--memory-budget-mib", "256",
                "--decoded-cache-budget-mib", "64",
                "--scratch-budget-mib", "512",
                "--threads", "1",
                "--resume",
                "--json",
            ])
            resumed_status = _wait_status(
                resumed_attempt,
                lambda value: value.get("state") in run_observed.TERMINAL_STATES,
            )
            if (resumed_status.get("state") != "passed"
                    or resumed_status.get("resumed_from") != cancelled_status["attempt_id"]):
                raise RuntimeError(f"resumed attempt did not pass: {resumed_status}")
            resumed = json.loads(resumed_report.read_text(encoding="utf-8"))
            if resumed.get("status") != "passed" or resumed.get("resumed") is not True:
                raise RuntimeError("benchmark did not record its resumed state")

            fresh_output = root / "fresh-output"
            fresh_report = root / "benchmark-fresh.json"
            completed = subprocess.run(
                [
                    str(binary),
                    "benchmark", str(configuration),
                    "--output", str(fresh_report),
                    "--output-directory", str(fresh_output),
                    "--cache-directory", str(root / "fresh-cache"),
                    "--threads", "1",
                    "--json",
                ],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            if completed.returncode != 0:
                raise RuntimeError(f"fresh benchmark failed with {completed.returncode}")
            fresh = json.loads(fresh_report.read_text(encoding="utf-8"))
            if (resumed.get("database_content_sha256") != fresh.get("database_content_sha256")
                    or resumed.get("ordered_pack_sha256") != fresh.get("ordered_pack_sha256")
                    or _tree_hashes(shared_output) != _tree_hashes(fresh_output)):
                raise RuntimeError("resumed publication is not byte-identical to a fresh build")
        finally:
            for attempt in attempts:
                try:
                    status = run_observed.load_json(attempt / "status.json")
                    if status.get("state") not in run_observed.TERMINAL_STATES:
                        _cancel(attempt, force=True)
                        _wait_status(
                            attempt,
                            lambda value: value.get("state") in run_observed.TERMINAL_STATES,
                            5.0,
                        )
                except (OSError, RuntimeError, run_observed.ObserverError):
                    pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
