#!/usr/bin/env python3
"""Run an exact command with durable status, logs, progress, and cancellation."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time
from typing import Any, BinaryIO, Iterable, Optional
import uuid


SCHEMA = "lunar-terrain-observed-run-v1"
TERMINAL_STATES = frozenset(("passed", "failed", "cancelled"))


class ObserverError(RuntimeError):
    """An operator-actionable observer failure."""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def atomic_write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f"{path.name}.tmp")
    encoded = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
    with temporary.open("wb") as stream:
        stream.write(encoded)
        stream.flush()
        os.fsync(stream.fileno())
    deadline = time.monotonic() + 2.0
    while True:
        try:
            os.replace(temporary, path)
            break
        except PermissionError:
            if os.name != "nt" or time.monotonic() >= deadline:
                raise
            time.sleep(0.01)


def load_json(path: Path) -> dict[str, Any]:
    deadline = time.monotonic() + 2.0
    while True:
        try:
            with path.open("r", encoding="utf-8") as stream:
                value = json.load(stream)
            break
        except PermissionError as error:
            if os.name != "nt" or time.monotonic() >= deadline:
                raise ObserverError(f"could not read {path}: {error}") from error
            time.sleep(0.01)
        except (OSError, json.JSONDecodeError) as error:
            raise ObserverError(f"could not read {path}: {error}") from error
    if not isinstance(value, dict):
        raise ObserverError(f"{path} does not contain a JSON object")
    return value


def require_new_run_directory(path: Path) -> Path:
    resolved = path.resolve()
    if resolved.exists():
        if not resolved.is_dir():
            raise ObserverError(f"run directory is not a directory: {resolved}")
        if any(resolved.iterdir()):
            raise ObserverError(f"run directory is not empty: {resolved}")
    else:
        resolved.mkdir(parents=True)
    return resolved


def command_from_remainder(command: Iterable[str]) -> list[str]:
    result = list(command)
    if result and result[0] == "--":
        result.pop(0)
    if not result:
        raise ObserverError("an exact command argv is required after --")
    if any("\0" in item for item in result):
        raise ObserverError("command arguments must not contain NUL")
    return result


def event_log_from_argv(argv: list[str], cwd: Path) -> Optional[Path]:
    for index, argument in enumerate(argv[:-1]):
        if argument == "--event-log":
            candidate = Path(argv[index + 1])
            return candidate if candidate.is_absolute() else cwd / candidate
    return None


def last_valid_ndjson_event(path: Optional[Path]) -> tuple[Optional[dict[str, Any]], Optional[str]]:
    if path is None or not path.is_file():
        return None, None
    try:
        data = path.read_bytes()
    except OSError as error:
        return None, f"could not read event log: {error}"
    lines = data.splitlines(keepends=True)
    last: Optional[dict[str, Any]] = None
    for index, encoded in enumerate(lines):
        complete = encoded.endswith((b"\n", b"\r"))
        try:
            value = json.loads(encoded.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            if index == len(lines) - 1 and not complete:
                return last, None
            return last, f"invalid NDJSON record {index + 1}: {error}"
        if not isinstance(value, dict):
            return last, f"NDJSON record {index + 1} is not an object"
        last = value
    return last, None


def newest_mtime(paths: Iterable[Optional[Path]]) -> Optional[float]:
    values: list[float] = []
    for path in paths:
        if path is None:
            continue
        try:
            values.append(path.stat().st_mtime)
        except OSError:
            pass
    return max(values) if values else None


def timestamp_from_epoch(value: Optional[float]) -> Optional[str]:
    if value is None:
        return None
    return datetime.fromtimestamp(value, timezone.utc).isoformat().replace("+00:00", "Z")


def child_creation_arguments() -> dict[str, Any]:
    if os.name == "nt":
        return {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP}
    return {"start_new_session": True}


def detached_creation_arguments() -> dict[str, Any]:
    if os.name == "nt":
        startupinfo = subprocess.STARTUPINFO()
        startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startupinfo.wShowWindow = subprocess.SW_HIDE
        return {
            # A hidden private console gives the supervisor a durable control-event
            # channel after the launching terminal exits. The child gets its own group.
            "creationflags": subprocess.CREATE_NEW_CONSOLE,
            "close_fds": True,
            "startupinfo": startupinfo,
        }
    return {"start_new_session": True, "close_fds": True}


def request_child_stop(child: subprocess.Popen[Any], force: bool) -> None:
    if child.poll() is not None:
        return
    if force:
        child.kill()
        return
    if os.name == "nt":
        child.send_signal(signal.CTRL_BREAK_EVENT)
    else:
        os.killpg(child.pid, signal.SIGINT)


def copy_stream(source: BinaryIO, destinations: tuple[BinaryIO, ...]) -> None:
    while True:
        chunk = source.read(64 * 1024)
        if not chunk:
            return
        for destination in destinations:
            destination.write(chunk)
            destination.flush()


def invocation_record(arguments: argparse.Namespace, run_dir: Path) -> dict[str, Any]:
    argv = command_from_remainder(arguments.command)
    return {
        "schema": SCHEMA,
        "attempt_id": str(uuid.uuid4()),
        "name": arguments.name,
        "argv": argv,
        "cwd": str(Path.cwd().resolve()),
        "created_utc": utc_now(),
        "heartbeat_seconds": arguments.heartbeat_seconds,
        "resumed_from": arguments.resume_of,
        "run_directory": str(run_dir),
    }


def initial_status(invocation: dict[str, Any], state: str) -> dict[str, Any]:
    return {
        "schema": SCHEMA,
        "attempt_id": invocation["attempt_id"],
        "name": invocation["name"],
        "state": state,
        "activity": "resumed" if invocation.get("resumed_from") else "starting",
        "resumed_from": invocation.get("resumed_from"),
        "created_utc": invocation["created_utc"],
        "heartbeat_utc": utc_now(),
        "elapsed_seconds": 0.0,
        "pid": None,
        "supervisor_pid": None,
        "exit_code": None,
        "last_output_utc": None,
        "last_event_utc": None,
        "progress": None,
    }


def write_invocation(run_dir: Path, invocation: dict[str, Any]) -> None:
    path = run_dir / "invocation.json"
    if path.exists():
        raise ObserverError(f"immutable invocation already exists: {path}")
    atomic_write_json(path, invocation)


def supervise(run_dir: Path, foreground: bool) -> int:
    invocation = load_json(run_dir / "invocation.json")
    argv = invocation.get("argv")
    if not isinstance(argv, list) or not argv or not all(isinstance(item, str) for item in argv):
        raise ObserverError("invocation argv is invalid")
    cwd = Path(str(invocation["cwd"]))
    heartbeat_seconds = float(invocation.get("heartbeat_seconds", 5.0))
    stdout_path = run_dir / "stdout.log"
    stderr_path = run_dir / "stderr.log"
    event_log = event_log_from_argv(argv, cwd)
    started = time.monotonic()
    status = initial_status(invocation, "starting")
    status["supervisor_pid"] = os.getpid()
    atomic_write_json(run_dir / "status.json", status)

    stdout_file = stdout_path.open("ab", buffering=0)
    stderr_file = stderr_path.open("ab", buffering=0)
    child: Optional[subprocess.Popen[Any]] = None
    copy_threads: list[threading.Thread] = []

    def close_output_streams() -> None:
        for thread in copy_threads:
            thread.join(timeout=5.0)
        if child is not None:
            if child.stdout is not None:
                child.stdout.close()
            if child.stderr is not None:
                child.stderr.close()
        if not stdout_file.closed:
            stdout_file.close()
        if not stderr_file.closed:
            stderr_file.close()

    try:
        if foreground:
            stdout_console = getattr(sys.stdout, "buffer", None)
            stderr_console = getattr(sys.stderr, "buffer", None)
            child = subprocess.Popen(
                argv,
                cwd=cwd,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                **child_creation_arguments(),
            )
            assert child.stdout is not None and child.stderr is not None
            copy_threads = [
                threading.Thread(
                    target=copy_stream,
                    args=(
                        child.stdout,
                        (stdout_file, stdout_console) if stdout_console is not None else (stdout_file,),
                    ),
                    daemon=True,
                ),
                threading.Thread(
                    target=copy_stream,
                    args=(
                        child.stderr,
                        (stderr_file, stderr_console) if stderr_console is not None else (stderr_file,),
                    ),
                    daemon=True,
                ),
            ]
            for thread in copy_threads:
                thread.start()
        else:
            child = subprocess.Popen(
                argv,
                cwd=cwd,
                stdin=subprocess.DEVNULL,
                stdout=stdout_file,
                stderr=stderr_file,
                **child_creation_arguments(),
            )
        status.update({"state": "running", "activity": "resumed" if invocation.get("resumed_from") else "advancing", "pid": child.pid})
        atomic_write_json(run_dir / "status.json", status)

        cancellation_sent = False
        cancellation_requested = False
        cancellation_force = False
        last_cancellation_signal: Optional[float] = None
        previous_progress_key: Optional[tuple[Any, ...]] = None
        previous_output_mtime: Optional[float] = None
        last_progress_change = time.monotonic()
        while child.poll() is None:
            cycle_started = time.monotonic()
            cancel_path = run_dir / "cancel.request.json"
            if cancel_path.is_file() and not cancellation_requested:
                request = load_json(cancel_path)
                if request.get("attempt_id") != invocation["attempt_id"]:
                    raise ObserverError("cancel request does not match this immutable attempt")
                cancellation_requested = True
                cancellation_force = bool(request.get("force"))
                status["state"] = "cancel_requested"
            should_signal = cancellation_requested and (
                not cancellation_sent
                or (
                    os.name == "nt"
                    and not cancellation_force
                    and last_cancellation_signal is not None
                    and cycle_started - last_cancellation_signal >= 1.0
                )
            )
            if (should_signal
                    and (os.name != "nt" or cancellation_force or cycle_started - started >= 0.25)):
                try:
                    request_child_stop(child, cancellation_force)
                    cancellation_sent = True
                    last_cancellation_signal = cycle_started
                    status.pop("cancellation_delivery_error", None)
                except OSError as error:
                    status["cancellation_delivery_error"] = str(error)

            progress, progress_error = last_valid_ndjson_event(event_log)
            progress_key = None if progress is None else (
                progress.get("phase"),
                (progress.get("work") or {}).get("completed"),
                progress.get("checkpoint_id"),
            )
            if progress_key is not None and progress_key != previous_progress_key:
                previous_progress_key = progress_key
                last_progress_change = time.monotonic()
                status["activity"] = "advancing"
            output_mtime = newest_mtime((stdout_path, stderr_path))
            if output_mtime is not None and output_mtime != previous_output_mtime:
                previous_output_mtime = output_mtime
                last_progress_change = time.monotonic()
                status["activity"] = "advancing"
            elif cancellation_requested:
                status["activity"] = "cancelling"
            elif time.monotonic() - last_progress_change >= heartbeat_seconds:
                status["activity"] = "blocked"
            status.update(
                {
                    "heartbeat_utc": utc_now(),
                    "elapsed_seconds": time.monotonic() - started,
                    "last_output_utc": timestamp_from_epoch(output_mtime),
                    "last_event_utc": timestamp_from_epoch(newest_mtime((event_log,))),
                    "progress": progress,
                }
            )
            if progress_error:
                status["event_log_error"] = progress_error
            atomic_write_json(run_dir / "status.json", status)
            remaining = heartbeat_seconds - (time.monotonic() - cycle_started)
            if remaining > 0:
                try:
                    child.wait(timeout=remaining)
                except subprocess.TimeoutExpired:
                    pass

        exit_code = int(child.returncode)
        close_output_streams()
        progress, progress_error = last_valid_ndjson_event(event_log)
        if exit_code == 0:
            state = "passed"
        elif cancellation_sent or exit_code == 130:
            state = "cancelled"
        else:
            state = "failed"
        status.update(
            {
                "state": state,
                "activity": state,
                "exit_code": exit_code,
                "heartbeat_utc": utc_now(),
                "elapsed_seconds": time.monotonic() - started,
                "last_output_utc": timestamp_from_epoch(
                    newest_mtime((stdout_path, stderr_path))
                ),
                "last_event_utc": timestamp_from_epoch(newest_mtime((event_log,))),
                "progress": progress,
            }
        )
        if progress_error:
            status["event_log_error"] = progress_error
        atomic_write_json(run_dir / "status.json", status)
        return exit_code
    except KeyboardInterrupt:
        if child is not None and child.poll() is None:
            request_child_stop(child, False)
            child.wait()
        close_output_streams()
        status.update(
            {
                "state": "cancelled",
                "activity": "cancelled",
                "exit_code": 130,
                "heartbeat_utc": utc_now(),
                "elapsed_seconds": time.monotonic() - started,
            }
        )
        atomic_write_json(run_dir / "status.json", status)
        return 130
    except BaseException as error:
        if child is not None and child.poll() is None:
            try:
                request_child_stop(child, True)
                child.wait(timeout=5.0)
            except OSError:
                pass
            except subprocess.TimeoutExpired:
                pass
        close_output_streams()
        status.update(
            {
                "state": "failed",
                "activity": "failed",
                "heartbeat_utc": utc_now(),
                "elapsed_seconds": time.monotonic() - started,
                "error": f"{type(error).__name__}: {error}",
            }
        )
        atomic_write_json(run_dir / "status.json", status)
        raise
    finally:
        close_output_streams()


def create_attempt(arguments: argparse.Namespace) -> tuple[Path, dict[str, Any]]:
    run_dir = require_new_run_directory(arguments.run_dir)
    invocation = invocation_record(arguments, run_dir)
    write_invocation(run_dir, invocation)
    atomic_write_json(run_dir / "status.json", initial_status(invocation, "created"))
    return run_dir, invocation


def start_command(arguments: argparse.Namespace) -> int:
    run_dir, invocation = create_attempt(arguments)
    supervisor_argv = [
        sys.executable,
        str(Path(__file__).resolve()),
        "_supervise",
        "--run-dir",
        str(run_dir),
    ]
    supervisor = subprocess.Popen(
        supervisor_argv,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        **detached_creation_arguments(),
    )
    supervisor_pid = supervisor.pid
    # The process intentionally outlives this Popen wrapper; releasing the wrapper must
    # neither wait for it nor report an uncollected-child ResourceWarning.
    supervisor.returncode = 0
    result = {
        "schema": SCHEMA,
        "attempt_id": invocation["attempt_id"],
        "run_directory": str(run_dir),
        "supervisor_pid": supervisor_pid,
        "state": "started",
    }
    print(json.dumps(result, sort_keys=True), flush=True)
    return 0


def run_command(arguments: argparse.Namespace) -> int:
    run_dir, _ = create_attempt(arguments)
    return supervise(run_dir, foreground=True)


def status_command(arguments: argparse.Namespace) -> int:
    status = load_json(arguments.run_dir.resolve() / "status.json")
    if arguments.json:
        print(json.dumps(status, sort_keys=True))
    else:
        print(
            f"{status.get('name')}: {status.get('state')} / {status.get('activity')} "
            f"elapsed={float(status.get('elapsed_seconds') or 0.0):.1f}s "
            f"pid={status.get('pid')} heartbeat={status.get('heartbeat_utc')}"
        )
    return 0


def follow_command(arguments: argparse.Namespace) -> int:
    run_dir = arguments.run_dir.resolve()
    offsets = {"stdout.log": 0, "stderr.log": 0}
    while True:
        for name, destination in (
            ("stdout.log", sys.stdout.buffer),
            ("stderr.log", sys.stderr.buffer),
        ):
            path = run_dir / name
            if not path.is_file():
                continue
            with path.open("rb") as stream:
                stream.seek(offsets[name])
                data = stream.read()
                offsets[name] += len(data)
            if data:
                destination.write(data)
                destination.flush()
        status = load_json(run_dir / "status.json")
        if status.get("state") in TERMINAL_STATES:
            return int(status.get("exit_code") or 0)
        time.sleep(arguments.poll_seconds)


def cancel_command(arguments: argparse.Namespace) -> int:
    run_dir = arguments.run_dir.resolve()
    status = load_json(run_dir / "status.json")
    if status.get("state") in TERMINAL_STATES:
        print(json.dumps(status, sort_keys=True))
        return 0
    request = {
        "schema": SCHEMA,
        "attempt_id": status.get("attempt_id"),
        "requested_utc": utc_now(),
        "force": bool(arguments.force),
    }
    atomic_write_json(run_dir / "cancel.request.json", request)
    print(json.dumps(request, sort_keys=True), flush=True)
    return 0


def add_attempt_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--resume-of")
    parser.add_argument("--heartbeat-seconds", type=float, default=5.0)
    parser.add_argument("command", nargs=argparse.REMAINDER)


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="operation", required=True)
    start = subparsers.add_parser("start", help="launch a detached observed attempt")
    add_attempt_arguments(start)
    run = subparsers.add_parser("run", help="run an observed attempt in the foreground")
    add_attempt_arguments(run)
    status = subparsers.add_parser("status", help="read the latest atomic status")
    status.add_argument("--run-dir", type=Path, required=True)
    status.add_argument("--json", action="store_true")
    follow = subparsers.add_parser("follow", help="follow logs until the attempt terminates")
    follow.add_argument("--run-dir", type=Path, required=True)
    follow.add_argument("--poll-seconds", type=float, default=0.25)
    cancel = subparsers.add_parser("cancel", help="request graceful cancellation")
    cancel.add_argument("--run-dir", type=Path, required=True)
    cancel.add_argument(
        "--force",
        action="store_true",
        help="force termination instead of requesting graceful interruption",
    )
    supervise_parser = subparsers.add_parser("_supervise", help=argparse.SUPPRESS)
    supervise_parser.add_argument("--run-dir", type=Path, required=True)
    arguments = parser.parse_args(argv)
    if hasattr(arguments, "heartbeat_seconds") and arguments.heartbeat_seconds <= 0:
        parser.error("--heartbeat-seconds must be positive")
    if hasattr(arguments, "poll_seconds") and arguments.poll_seconds <= 0:
        parser.error("--poll-seconds must be positive")
    return arguments


def main(argv: Optional[list[str]] = None) -> int:
    arguments = parse_args(argv)
    try:
        if arguments.operation == "start":
            return start_command(arguments)
        if arguments.operation == "run":
            return run_command(arguments)
        if arguments.operation == "status":
            return status_command(arguments)
        if arguments.operation == "follow":
            return follow_command(arguments)
        if arguments.operation == "cancel":
            return cancel_command(arguments)
        if arguments.operation == "_supervise":
            return supervise(arguments.run_dir.resolve(), foreground=False)
        raise ObserverError(f"unsupported operation: {arguments.operation}")
    except ObserverError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("error: observer interrupted", file=sys.stderr)
        return 130
    except OSError as error:
        print(f"error: observer operating-system failure: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
