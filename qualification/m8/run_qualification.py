#!/usr/bin/env python3
"""Run, resume, or sweep the opt-in M8 qualification workflows."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import signal
import sqlite3
import subprocess
import sys
import threading
import time
from typing import Any, Optional


SCHEMA = "lunar-terrain-m8-qualification-v2"
MANIFEST_SCHEMA = "lunar-terrain-m8-qualification-manifest-v1"
EXPORTS = (
    ("elevation-pgm", ".pgm"),
    ("csv", ".csv"),
    ("provenance-ppm", "-provenance.ppm"),
    ("quality-ppm", "-quality.ppm"),
    ("transition-csv", "-transition.csv"),
)
DEFAULT_PROGRESS_SECONDS = 5.0
PROBE_PLAN_LEVEL = 8
PROBE_MIN_LEVEL_TILE_COUNT = 128
PROBE_REFERENCE_WORK_CHUNK_TILES = 64


class QualificationError(RuntimeError):
    """A qualification operation failed."""


class QualificationCancelled(QualificationError):
    """A child cooperatively stopped after cancellation."""


def _raise_keyboard_interrupt(_signal_number: int, _frame: Any) -> None:
    raise KeyboardInterrupt


def _install_console_signal_handlers() -> None:
    if os.name == "nt" and hasattr(signal, "SIGBREAK"):
        signal.signal(signal.SIGBREAK, _raise_keyboard_interrupt)


def _atomic_write(path: Path, data: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(data)
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


def _atomic_json(path: Path, value: Any) -> None:
    _atomic_write(path, json.dumps(value, indent=2, sort_keys=True) + "\n")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def _read_ndjson(path: Path) -> list[dict[str, Any]]:
    """Read complete records, tolerating only one truncated trailing record."""
    if not path.exists():
        return []
    lines = path.read_text(encoding="utf-8", errors="strict").splitlines()
    records: list[dict[str, Any]] = []
    for index, line in enumerate(lines):
        if not line.strip():
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            if index == len(lines) - 1:
                break
            raise QualificationError(f"invalid non-trailing NDJSON record: {path}:{index + 1}")
        if not isinstance(value, dict):
            raise QualificationError(f"NDJSON record is not an object: {path}:{index + 1}")
        records.append(value)
    return records


def _add_common_run_options(parser: argparse.ArgumentParser, *, threads_many: bool) -> None:
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--threads", type=int, nargs="+" if threads_many else None, required=True)
    parser.add_argument("--memory-budget-mib", type=int, required=True)
    parser.add_argument("--decoded-cache-budget-mib", type=int, required=True)
    parser.add_argument("--scratch-budget-mib", type=int, required=True)
    parser.add_argument(
        "--progress-interval-seconds", type=float, default=DEFAULT_PROGRESS_SECONDS)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    run = commands.add_parser("run", help="start a new qualification run")
    run.add_argument("profile", choices=("a", "b", "c", "probe", "scale"))
    _add_common_run_options(run, threads_many=False)
    resume = commands.add_parser("resume", help="resume a matching interrupted run")
    resume.add_argument("--work-root", type=Path, required=True)
    sweep = commands.add_parser("sweep", help="run the probe at several worker counts")
    sweep.add_argument("profile", nargs="?", choices=("probe",), default="probe")
    _add_common_run_options(sweep, threads_many=True)
    return parser.parse_args()


def _configuration_path(repository: Path, profile: str) -> Path:
    if profile == "probe":
        return repository / "qualification" / "m8" / "configs" / "scale_probe.toml"
    if profile == "scale":
        return repository / "qualification" / "m8" / "configs" / "scale.toml"
    return repository / "qualification" / "m8" / "configs" / f"profile_{profile}.toml"


def _manifest(
    profile: str,
    binary: Path,
    configuration: Path,
    threads: int,
    budgets: dict[str, int],
    progress_interval_seconds: float,
) -> dict[str, Any]:
    return {
        "schema": MANIFEST_SCHEMA,
        "profile": profile,
        "binary": str(binary),
        "binary_sha256": _sha256(binary),
        "configuration": str(configuration),
        "configuration_sha256": _sha256(configuration),
        "threads": threads,
        "budgets_mib": budgets,
        "progress_interval_seconds": progress_interval_seconds,
    }


def _request_interrupt(process: subprocess.Popen[str]) -> None:
    try:
        if os.name == "nt":
            process.send_signal(signal.CTRL_BREAK_EVENT)
        else:
            os.killpg(process.pid, signal.SIGINT)
    except (ProcessLookupError, OSError):
        return


def _stream_reader(
    stream: Any,
    name: str,
    messages: "queue.Queue[tuple[str, Optional[str]]]",
) -> None:
    try:
        for line in iter(stream.readline, ""):
            messages.put((name, line))
    finally:
        messages.put((name, None))


class RunContext:
    def __init__(self, root: Path, manifest: dict[str, Any], *, resumed: bool) -> None:
        self.root = root
        self.manifest = manifest
        self.binary = Path(manifest["binary"])
        self.profile = str(manifest["profile"])
        self.threads = int(manifest["threads"])
        self.progress_interval = float(manifest["progress_interval_seconds"])
        self.report_path = root / "qualification.json"
        self.started_monotonic = time.monotonic()
        self.cancel_requested = False
        if resumed and self.report_path.exists():
            self.result = json.loads(self.report_path.read_text(encoding="utf-8"))
            self.result["status"] = "resumed"
            self.result["resumed"] = True
            self.result.pop("error", None)
        else:
            self.result: dict[str, Any] = {
                "schema": SCHEMA,
                "status": "running",
                "profile": self.profile,
                "platform": sys.platform,
                "resumed": False,
                "budgets_mib": manifest["budgets_mib"],
                "commands": [],
                "checkpoints": [],
            }
        attempts = self.result.setdefault("attempts", [])
        self.attempt_number = len(attempts) + 1
        attempts.append({
            "number": self.attempt_number,
            "resumed": resumed,
            "started_unix_seconds": time.time(),
            "state": "running",
        })
        self.result["attempt_started_unix_seconds"] = attempts[-1]["started_unix_seconds"]
        self.persist()

    @property
    def records(self) -> list[dict[str, Any]]:
        return self.result.setdefault("commands", [])

    def persist(self) -> None:
        self.result["attempt_elapsed_seconds"] = time.monotonic() - self.started_monotonic
        _atomic_json(self.report_path, self.result)

    def checkpoint(self, identity: str) -> None:
        checkpoints = self.result.setdefault("checkpoints", [])
        if identity not in checkpoints:
            checkpoints.append(identity)
        self.result["last_checkpoint"] = identity
        self.persist()

    def completed_record(self, command_id: str, arguments: list[str]) -> Optional[dict[str, Any]]:
        for record in self.records:
            if (record.get("command_id") == command_id
                    and record.get("operation_arguments") == arguments
                    and record.get("state") == "passed"):
                return record
        return None

    def common_arguments(self, command_id: str, event_path: Path, state_path: Path) -> list[str]:
        budgets = self.manifest["budgets_mib"]
        return [
            "--run-id", f"{self.profile}-attempt-{self.attempt_number}-{command_id}",
            "--run-state-directory", str(state_path),
            "--event-log", str(event_path),
            "--progress-interval-seconds", str(self.progress_interval),
            "--memory-budget-mib", str(budgets["managed_memory"]),
            "--decoded-cache-budget-mib", str(budgets["decoded_cache"]),
            "--scratch-budget-mib", str(budgets["transient_scratch"]),
            "--decoded-cache-directory", str(self.root / "decoded-raster-cache"),
        ] + (["--resume"] if self.result.get("resumed") else [])

    def run_command(
        self,
        command_id: str,
        arguments: list[str],
        *,
        expect_json: bool,
    ) -> Any:
        event_path = (
            self.root / "events" / f"attempt-{self.attempt_number}" / f"{command_id}.ndjson")
        state_path = self.root / "command-state" / f"attempt-{self.attempt_number}" / command_id
        complete_arguments = self.common_arguments(command_id, event_path, state_path) + arguments
        previous = self.completed_record(command_id, arguments)
        if previous is not None:
            self.checkpoint(command_id)
            return previous.get("report")

        index = len(self.records)
        log_root = self.root / "logs"
        log_root.mkdir(parents=True, exist_ok=True)
        stdout_path = log_root / f"{index:03d}-{command_id}.stdout.log"
        stderr_path = log_root / f"{index:03d}-{command_id}.stderr.log"
        invocation = [str(self.binary), *complete_arguments]
        print(f"[m8] {command_id}: {' '.join(complete_arguments)}", file=sys.stderr, flush=True)
        creationflags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        started = time.monotonic()
        process = subprocess.Popen(
            invocation,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            creationflags=creationflags,
            start_new_session=os.name != "nt",
        )
        record: dict[str, Any] = {
            "command_id": command_id,
            "arguments": complete_arguments,
            "operation_arguments": arguments,
            "pid": process.pid,
            "state": "advancing",
            "stdout_log": str(stdout_path.relative_to(self.root)),
            "stderr_log": str(stderr_path.relative_to(self.root)),
            "event_log": str(event_path.relative_to(self.root)),
        }
        self.records.append(record)
        self.result["active_child"] = record.copy()
        self.persist()

        messages: "queue.Queue[tuple[str, Optional[str]]]" = queue.Queue()
        assert process.stdout is not None and process.stderr is not None
        readers = [
            threading.Thread(
                target=_stream_reader,
                args=(process.stdout, "stdout", messages),
                daemon=True),
            threading.Thread(
                target=_stream_reader,
                args=(process.stderr, "stderr", messages),
                daemon=True),
        ]
        for reader in readers:
            reader.start()
        stdout_parts: list[str] = []
        streams_open = 2
        last_activity = time.monotonic()
        last_persisted = 0.0
        last_event_sequence: Optional[int] = None
        with stdout_path.open("w", encoding="utf-8", newline="") as stdout_log, \
                stderr_path.open("w", encoding="utf-8", newline="") as stderr_log:
            try:
                while streams_open or process.poll() is None:
                    try:
                        stream_name, line = messages.get(timeout=0.2)
                    except queue.Empty:
                        stream_name, line = "", ""
                    now = time.monotonic()
                    if line is None:
                        streams_open -= 1
                    elif line:
                        last_activity = now
                        target = stdout_log if stream_name == "stdout" else stderr_log
                        target.write(line)
                        target.flush()
                        if stream_name == "stdout":
                            stdout_parts.append(line)
                        else:
                            sys.stderr.write(line)
                            sys.stderr.flush()
                    events = _read_ndjson(event_path)
                    if events:
                        sequence = events[-1].get("sequence")
                        if sequence != last_event_sequence:
                            last_event_sequence = sequence
                            last_activity = now
                    if now - last_persisted >= min(self.progress_interval, 1.0):
                        state = (
                            "advancing"
                            if now - last_activity <= self.progress_interval
                            else "blocked"
                        )
                        active = self.result["active_child"]
                        active.update({
                            "state": state,
                            "elapsed_seconds": now - started,
                            "heartbeat_unix_seconds": time.time(),
                            "last_output_age_seconds": now - last_activity,
                            "last_event_sequence": last_event_sequence,
                        })
                        if events:
                            active["last_progress"] = events[-1]
                        record.update(active)
                        self.persist()
                        last_persisted = now
            except KeyboardInterrupt:
                self.cancel_requested = True
                self.result["status"] = "cancelling"
                self.result["active_child"]["state"] = "cancelling"
                self.persist()
                _request_interrupt(process)
                process.wait()
            finally:
                for reader in readers:
                    reader.join(timeout=1.0)
                process.stdout.close()
                process.stderr.close()

        exit_code = process.wait()
        elapsed = time.monotonic() - started
        record.update({"exit_code": exit_code, "elapsed_seconds": elapsed})
        self.result.pop("active_child", None)
        output = "".join(stdout_parts)
        if expect_json and output.strip():
            try:
                record["report"] = json.loads(output)
            except json.JSONDecodeError:
                record["stdout_parse_error"] = "stdout did not contain one JSON value"
        if arguments and arguments[0] == "benchmark" and "--output" in arguments:
            benchmark_path = Path(arguments[arguments.index("--output") + 1])
            if benchmark_path.exists():
                try:
                    record["partial_report"] = json.loads(
                        benchmark_path.read_text(encoding="utf-8"))
                except (OSError, json.JSONDecodeError):
                    record["partial_report_error"] = "benchmark partial report is unreadable"
        if exit_code == 0 and (not expect_json or "report" in record):
            record["state"] = "passed"
            self.checkpoint(command_id)
            print(
                f"[m8] {command_id}: passed in {elapsed:.3f}s",
                file=sys.stderr,
                flush=True,
            )
            return record.get("report")
        record["state"] = (
            "cancelled" if exit_code == 130 or self.cancel_requested else "failed")
        self.persist()
        if record["state"] == "cancelled":
            raise QualificationCancelled(f"{command_id} was cancelled")
        raise QualificationError(f"{command_id} failed with exit code {exit_code}")


def raster_blocks(text: str) -> tuple[str, list[str]]:
    starts = [match.start() for match in re.finditer(r"(?m)^\[\[raster\]\]\s*$", text)]
    if not starts:
        raise QualificationError("configuration contains no [[raster]] declarations")
    prefix = text[: starts[0]]
    blocks = [
        text[start : starts[index + 1] if index + 1 < len(starts) else len(text)]
        for index, start in enumerate(starts)
    ]
    return prefix, blocks


def _configuration_number(section: str, name: str) -> float:
    match = re.search(rf"(?m)^{re.escape(name)}\s*=\s*([^#\s]+)", section)
    if match is None:
        raise QualificationError(f"configuration is missing {name}")
    try:
        return float(match.group(1))
    except ValueError as error:
        raise QualificationError(f"configuration has invalid {name}") from error


def verify_probe_configuration(configuration: Path) -> dict[str, Any]:
    text = configuration.read_text(encoding="utf-8")
    _, probe_sources = raster_blocks(text)
    scale = configuration.with_name("scale.toml")
    _, scale_sources = raster_blocks(scale.read_text(encoding="utf-8"))
    if probe_sources != scale_sources:
        raise QualificationError("probe source stack differs from the complete scale stack")

    region_match = re.search(
        r"(?ms)^\[region\]\s*$\s*(.*?)(?=^\[|\Z)", text)
    if region_match is None:
        raise QualificationError("probe configuration has no [region] section")
    region = region_match.group(1)
    bounds = {
        "west_longitude_degrees": _configuration_number(
            region, "west_longitude_degrees"),
        "east_longitude_degrees": _configuration_number(
            region, "east_longitude_degrees"),
        "south_latitude_degrees": _configuration_number(
            region, "south_latitude_degrees"),
        "north_latitude_degrees": _configuration_number(
            region, "north_latitude_degrees"),
    }
    required = {
        "west_longitude_degrees": 40.0,
        "east_longitude_degrees": 50.0,
        "south_latitude_degrees": 10.0,
        "north_latitude_degrees": 20.0,
    }
    if bounds != required:
        raise QualificationError("probe region is not the locked 40-50E, 10-20N region")
    component_boundary = 45.0
    if not (
        bounds["west_longitude_degrees"]
        < component_boundary
        < bounds["east_longitude_degrees"]
    ):
        raise QualificationError("probe does not span the SLDEM 45-degree boundary")
    return {
        "complete_scale_source_stack": True,
        "source_count": len(probe_sources),
        "region": bounds,
        "sldem_component_boundary_degrees": component_boundary,
    }


def verify_probe_plan(report: dict[str, Any]) -> dict[str, Any]:
    level_counts = report.get("level_counts")
    if not isinstance(level_counts, list):
        raise QualificationError("probe benchmark contains no level-count plan evidence")
    count = None
    for entry in level_counts:
        if (isinstance(entry, dict)
                and entry.get("level") == PROBE_PLAN_LEVEL
                and isinstance(entry.get("tile_count"), int)):
            count = int(entry["tile_count"])
            break
    if count is None:
        raise QualificationError(f"probe plan contains no level-{PROBE_PLAN_LEVEL} count")
    if count < PROBE_MIN_LEVEL_TILE_COUNT:
        raise QualificationError(
            f"probe level-{PROBE_PLAN_LEVEL} plan is not dense: "
            f"{count} < {PROBE_MIN_LEVEL_TILE_COUNT}")
    reference_chunks = (
        count + PROBE_REFERENCE_WORK_CHUNK_TILES - 1
    ) // PROBE_REFERENCE_WORK_CHUNK_TILES
    if reference_chunks < 2:
        raise QualificationError("probe plan does not span multiple reference work chunks")
    return {
        "level": PROBE_PLAN_LEVEL,
        "tile_count": count,
        "minimum_tile_count": PROBE_MIN_LEVEL_TILE_COUNT,
        "reference_work_chunk_tiles": PROBE_REFERENCE_WORK_CHUNK_TILES,
        "reference_work_chunk_count": reference_chunks,
    }


def write_variants(configuration: Path, variants: Path) -> tuple[Path, Path, dict[str, Path]]:
    text = configuration.read_text(encoding="utf-8")
    prefix, blocks = raster_blocks(text)
    variants.mkdir(parents=True, exist_ok=True)
    reversed_path = variants / "reversed.toml"
    _atomic_write(reversed_path, prefix + "".join(reversed(blocks)))

    refinement = next(
        (index for index in range(len(blocks) - 1, -1, -1)
         if re.search(r'(?m)^role\s*=\s*"refinement"\s*$', blocks[index])),
        len(blocks) - 1,
    )
    removed_path = variants / "refinement-removed.toml"
    _atomic_write(
        removed_path,
        prefix + "".join(
            block for index, block in enumerate(blocks) if index != refinement),
    )
    policy_paths: dict[str, Path] = {}
    selected_block = blocks[refinement]
    policy_match = re.search(r'(?m)^fusion_policy\s*=\s*"([^"]+)"\s*$', selected_block)
    if not policy_match:
        raise QualificationError("selected refinement has no explicit fusion_policy")
    baseline_policy = policy_match.group(1)
    for policy in ("Replace", "BiasCorrectedReplace"):
        if policy == baseline_policy:
            continue
        policy_block = (
            selected_block[: policy_match.start(1)]
            + policy
            + selected_block[policy_match.end(1) :]
        )
        policy_text = prefix + "".join(
            policy_block if index == refinement else block
            for index, block in enumerate(blocks)
        )
        policy_path = variants / f"policy-{policy.lower()}.toml"
        _atomic_write(policy_path, policy_text)
        policy_paths[policy] = policy_path
    return reversed_path, removed_path, policy_paths


def local_build_arguments(output: Path, cache: Path, threads: int) -> list[str]:
    result = ["--output-directory", str(output), "--cache-directory", str(cache)]
    if threads:
        result += ["--threads", str(threads)]
    return result


def database_path(configuration: Path, output: Path) -> Path:
    text = configuration.read_text(encoding="utf-8")
    database = re.search(r"(?ms)^\[database\]\s*$\s*(.*?)(?=^\[|\Z)", text)
    name = None if database is None else re.search(
        r'(?m)^name\s*=\s*"([^"]+)"\s*$', database.group(1))
    if name is None:
        raise QualificationError(f"configuration has no [database] name: {configuration}")
    return output / f"{name.group(1)}.ltdb"


def _format_encoded_tile_key(encoded: int) -> str:
    face = encoded >> 61
    level = (encoded >> 56) & 0x1F
    morton_mask = (1 << 56) - 1
    morton = encoded & morton_mask
    used_mask = morton_mask if level == 28 else (1 << (2 * level)) - 1
    if face > 5 or level > 28 or morton & ~used_mask:
        raise QualificationError("cache contains an invalid encoded tile key")
    x = 0
    y = 0
    for bit in range(level):
        x |= ((morton >> (2 * bit)) & 1) << bit
        y |= ((morton >> (2 * bit + 1)) & 1) << bit
    return f"QSC/F{face}/L{level:02d}/{x:04d}/{y:04d}"


def selected_cached_tile(cache_path: Path) -> str:
    if not cache_path.is_file():
        raise QualificationError(f"build cache does not exist: {cache_path}")
    try:
        connection = sqlite3.connect(
            f"{cache_path.resolve().as_uri()}?mode=ro", uri=True)
        try:
            rows = connection.execute(
                "SELECT tile_key FROM tiles "
                "WHERE build_state=2 AND previous_pack_id IS NOT NULL"
            ).fetchall()
        finally:
            connection.close()
    except sqlite3.Error as error:
        raise QualificationError(f"could not read completed tiles from build cache: {error}") from error
    tiles = []
    for row in rows:
        key = row[0]
        if not isinstance(key, bytes) or len(key) != 8:
            raise QualificationError("cache contains a malformed tile key")
        tiles.append(_format_encoded_tile_key(int.from_bytes(key, "little")))
    if not tiles:
        raise QualificationError("build cache did not contain a published tile")
    return selected_tile({"tiles": tiles})


def selected_tile(
    report: dict[str, Any],
    fallback_cache_path: Optional[Path] = None,
) -> str:
    tiles = report.get("prototype_tiles") or report.get("tiles", [])
    if not tiles:
        if fallback_cache_path is not None:
            return selected_cached_tile(fallback_cache_path)
        raise QualificationError("report did not contain a representative tile")
    return max(
        tiles,
        key=lambda value: (
            int((re.search(r"/L(\d+)/", value) or [None, "0"])[1]),
            value,
        ),
    )


def canonical_published_identity(report: dict[str, Any]) -> tuple[str, tuple[str, ...]]:
    database_hash = report.get("database_content_sha256")
    pack_hashes = report.get("ordered_pack_sha256")
    if pack_hashes is None:
        pack_hashes = [pack["sha256"] for pack in report.get("packs", [])]
    return str(database_hash), tuple(pack_hashes)


def required_region_workflow(
    context: RunContext,
    configuration: Path,
) -> dict[str, Any]:
    reversed_configuration, removed_configuration, policy_configurations = write_variants(
        configuration, context.root / "variants")

    scan = context.run_command(
        "scan", ["scan", str(configuration), "--json"], expect_json=True)
    plan = context.run_command(
        "plan", ["plan", str(configuration), "--json"], expect_json=True)

    reports: dict[str, dict[str, Any]] = {}
    databases: dict[str, Path] = {}
    for name, source, incremental, cache_name in (
        ("clean_1", configuration, False, "clean-1"),
        ("clean_2", configuration, False, "clean-2"),
        ("reversed", reversed_configuration, False, "reversed"),
        ("removed", removed_configuration, True, "clean-1"),
        ("restored", configuration, True, "clean-1"),
    ):
        output = context.root / "outputs" / name
        cache = context.root / "caches" / cache_name
        arguments = ["build", str(source)]
        if incremental:
            arguments.append("--incremental")
        arguments += local_build_arguments(output, cache, context.threads)
        arguments.append("--json")
        reports[name] = context.run_command(
            f"build-{name}", arguments, expect_json=True)
        databases[name] = database_path(source, output)
        context.run_command(
            f"validate-{name}",
            ["validate", str(databases[name]), "--full", "--json"],
            expect_json=True,
        )

    policy_reports: dict[str, dict[str, Any]] = {}
    for policy, source in policy_configurations.items():
        name = f"policy_{policy.lower()}"
        output = context.root / "outputs" / name
        cache = context.root / "caches" / name
        policy_reports[policy] = context.run_command(
            f"build-{name}",
            [
                "build",
                str(source),
                *local_build_arguments(output, cache, context.threads),
                "--json",
            ],
            expect_json=True,
        )
        databases[name] = database_path(source, output)
        context.run_command(
            f"validate-{name}",
            ["validate", str(databases[name]), "--full", "--json"],
            expect_json=True,
        )

    baseline = canonical_published_identity(reports["clean_1"])
    for name in ("clean_2", "reversed", "restored"):
        if canonical_published_identity(reports[name]) != baseline:
            raise QualificationError(
                f"{name} did not reproduce the baseline published identity")

    diffs: dict[str, Any] = {}
    for name in ("clean_2", "reversed", "removed", "restored"):
        diffs[name] = context.run_command(
            f"diff-{name}",
            ["diff", str(databases["clean_1"]), str(databases[name]), "--json"],
            expect_json=True,
        )
    for policy in policy_configurations:
        name = f"policy_{policy.lower()}"
        diffs[name] = context.run_command(
            f"diff-{name}",
            ["diff", str(databases["clean_1"]), str(databases[name]), "--json"],
            expect_json=True,
        )

    tile = selected_tile(plan)
    inspection = context.run_command(
        "inspect",
        ["inspect", str(databases["clean_1"]), tile, "--json"],
        expect_json=True,
    )
    export_root = context.root / "exports"
    export_root.mkdir(parents=True, exist_ok=True)
    for export_format, suffix in EXPORTS:
        context.run_command(
            f"export-{export_format}",
            [
                "export",
                str(databases["clean_1"]),
                tile,
                "--format",
                export_format,
                "--output",
                str(export_root / f"tile{suffix}"),
            ],
            expect_json=False,
        )

    return {
        "scan": scan,
        "plan": plan,
        "builds": reports,
        "policy_builds": policy_reports,
        "diffs": diffs,
        "incremental_scope": {
            "removed_built_tiles": reports["removed"]["built_tile_count"],
            "removed_reused_tiles": reports["removed"]["reused_tile_count"],
            "removed_tile_count": reports["removed"]["tile_count"],
            "configuration_wide_invalidation": (
                reports["removed"]["tile_count"] > 0
                and reports["removed"]["reused_tile_count"] == 0
            ),
        },
        "inspection": inspection,
        "selected_tile": tile,
    }


def scale_workflow(context: RunContext, configuration: Path) -> dict[str, Any]:
    label = "probe" if context.profile == "probe" else "scale"
    probe_configuration = None
    if context.profile == "probe":
        probe_configuration = verify_probe_configuration(configuration)
        context.result["probe_configuration"] = probe_configuration
        context.persist()
    output = context.root / "outputs" / label
    cache = context.root / "caches" / label
    benchmark_path = context.root / f"m8-{label}-benchmark.json"
    report = context.run_command(
        "benchmark",
        [
            "benchmark",
            str(configuration),
            "--output",
            str(benchmark_path),
            *local_build_arguments(output, cache, context.threads),
            "--json",
        ],
        expect_json=True,
    )
    if report.get("benchmark_schema") != "lunar-terrain-benchmark-v2":
        raise QualificationError("benchmark did not produce the v2 schema")
    probe_plan = verify_probe_plan(report) if context.profile == "probe" else None
    database = database_path(configuration, output)
    tile = selected_tile(report, cache / "cache.sqlite")
    inspection = context.run_command(
        "inspect", ["inspect", str(database), tile, "--json"], expect_json=True)
    export_root = context.root / "exports"
    export_root.mkdir(parents=True, exist_ok=True)
    export_started = time.monotonic()
    for export_format, suffix in EXPORTS:
        context.run_command(
            f"export-{export_format}",
            [
                "export",
                str(database),
                tile,
                "--format",
                export_format,
                "--output",
                str(export_root / f"tile{suffix}"),
            ],
            expect_json=False,
        )
    benchmark_record = json.loads(benchmark_path.read_text(encoding="utf-8"))
    if canonical_published_identity(report) != canonical_published_identity(benchmark_record):
        raise QualificationError("streamed and persisted benchmark identities differ")
    resumed_identities = [
        canonical_published_identity(record["partial_report"])
        for record in context.records
        if record.get("state") == "cancelled"
        and isinstance(record.get("partial_report"), dict)
        and record["partial_report"].get("database_content_sha256")
    ]
    current_identity = canonical_published_identity(report)
    if any(identity != current_identity for identity in resumed_identities):
        raise QualificationError("resumed publication differs from its cancelled checkpoint")
    return {
        "benchmark": report,
        "benchmark_record": benchmark_record,
        "export_seconds": time.monotonic() - export_started,
        "inspection": inspection,
        "probe": (
            {"configuration": probe_configuration, "plan": probe_plan}
            if context.profile == "probe" else None
        ),
        "resume_byte_identical": (
            all(identity == current_identity for identity in resumed_identities)
            if resumed_identities else None
        ),
        "selected_tile": tile,
    }


def _run_context(context: RunContext) -> int:
    configuration = Path(context.manifest["configuration"])
    try:
        if context.profile in ("probe", "scale"):
            evidence = scale_workflow(context, configuration)
        else:
            evidence = required_region_workflow(context, configuration)
        context.result["evidence"] = evidence
        context.result["status"] = "passed"
        context.result["attempts"][-1]["state"] = "passed"
        context.result.pop("error", None)
        context.checkpoint("qualification-complete")
        return 0
    except QualificationCancelled as error:
        context.result["status"] = "cancelled"
        context.result["attempts"][-1]["state"] = "cancelled"
        context.result["error"] = f"{type(error).__name__}: {error}"
        context.persist()
        return 130
    except KeyboardInterrupt:
        context.result["status"] = "cancelled"
        context.result["attempts"][-1]["state"] = "cancelled"
        context.result["error"] = "KeyboardInterrupt: qualification was cancelled"
        context.persist()
        return 130
    except Exception as error:  # Evidence must survive all operational failures.
        context.result["status"] = "failed"
        context.result["attempts"][-1]["state"] = "failed"
        context.result["error"] = f"{type(error).__name__}: {error}"
        context.persist()
        return 1


def _validate_positive(arguments: argparse.Namespace) -> None:
    thread_values = arguments.threads if isinstance(arguments.threads, list) else [arguments.threads]
    if any(value <= 0 for value in thread_values):
        raise QualificationError("worker counts must be positive")
    for name in ("memory_budget_mib", "decoded_cache_budget_mib", "scratch_budget_mib"):
        if getattr(arguments, name) <= 0:
            raise QualificationError(f"--{name.replace('_', '-')} must be positive")
    if arguments.progress_interval_seconds <= 0:
        raise QualificationError("--progress-interval-seconds must be positive")


def _budgets(arguments: argparse.Namespace) -> dict[str, int]:
    return {
        "managed_memory": arguments.memory_budget_mib,
        "decoded_cache": arguments.decoded_cache_budget_mib,
        "transient_scratch": arguments.scratch_budget_mib,
    }


def _new_context(
    profile: str,
    binary: Path,
    work_root: Path,
    threads: int,
    budgets: dict[str, int],
    progress_interval_seconds: float,
) -> RunContext:
    if work_root.exists() and any(work_root.iterdir()):
        raise QualificationError(f"work root is not empty: {work_root}")
    work_root.mkdir(parents=True, exist_ok=True)
    repository = Path(__file__).resolve().parents[2]
    configuration = _configuration_path(repository, profile).resolve(strict=True)
    manifest = _manifest(
        profile,
        binary.resolve(strict=True),
        configuration,
        threads,
        budgets,
        progress_interval_seconds,
    )
    _atomic_json(work_root / "manifest.json", manifest)
    return RunContext(work_root, manifest, resumed=False)


def _resume_context(work_root: Path) -> RunContext:
    manifest_path = work_root / "manifest.json"
    if not manifest_path.is_file():
        raise QualificationError(f"resume manifest does not exist: {manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schema") != MANIFEST_SCHEMA:
        raise QualificationError("resume manifest schema does not match")
    if manifest.get("kind") == "worker-sweep":
        raise QualificationError("resume an individual sweep run, not the sweep root")
    binary = Path(manifest["binary"]).resolve(strict=True)
    configuration = Path(manifest["configuration"]).resolve(strict=True)
    if _sha256(binary) != manifest.get("binary_sha256"):
        raise QualificationError("resume binary does not match the immutable manifest")
    if _sha256(configuration) != manifest.get("configuration_sha256"):
        raise QualificationError("resume configuration does not match the immutable manifest")
    return RunContext(work_root, manifest, resumed=True)


def _configure_decoder_threads(threads: int) -> None:
    decoder_threads = str(min(threads, 4))
    os.environ.setdefault("GDAL_NUM_THREADS", decoder_threads)
    os.environ.setdefault("OPJ_NUM_THREADS", decoder_threads)


def _run_sweep(arguments: argparse.Namespace) -> int:
    root = arguments.work_root.resolve()
    if root.exists() and any(root.iterdir()):
        raise QualificationError(f"work root is not empty: {root}")
    root.mkdir(parents=True, exist_ok=True)
    binary = arguments.binary.resolve(strict=True)
    budgets = _budgets(arguments)
    sweep_manifest = {
        "schema": MANIFEST_SCHEMA,
        "kind": "worker-sweep",
        "profile": "probe",
        "binary": str(binary),
        "binary_sha256": _sha256(binary),
        "threads": arguments.threads,
        "budgets_mib": budgets,
        "progress_interval_seconds": arguments.progress_interval_seconds,
    }
    _atomic_json(root / "manifest.json", sweep_manifest)
    report: dict[str, Any] = {
        "schema": SCHEMA,
        "kind": "worker-sweep",
        "status": "running",
        "runs": [],
    }
    report_path = root / "qualification.json"
    _atomic_json(report_path, report)
    exit_code = 0
    for threads in arguments.threads:
        run_root = root / f"threads-{threads}"
        context = _new_context(
            "probe",
            binary,
            run_root,
            threads,
            budgets,
            arguments.progress_interval_seconds,
        )
        _configure_decoder_threads(threads)
        child_exit = _run_context(context)
        report["runs"].append({
            "threads": threads,
            "status": context.result["status"],
            "report": str(context.report_path.relative_to(root)),
        })
        _atomic_json(report_path, report)
        if child_exit != 0:
            exit_code = child_exit
            break
    report["status"] = "passed" if exit_code == 0 else (
        "cancelled" if exit_code == 130 else "failed")
    _atomic_json(report_path, report)
    print(report_path)
    return exit_code


def main() -> int:
    try:
        _install_console_signal_handlers()
        arguments = parse_args()
        if arguments.action == "resume":
            context = _resume_context(arguments.work_root.resolve())
            _configure_decoder_threads(context.threads)
            exit_code = _run_context(context)
            print(context.report_path)
            return exit_code
        _validate_positive(arguments)
        if arguments.action == "sweep":
            return _run_sweep(arguments)
        context = _new_context(
            arguments.profile,
            arguments.binary,
            arguments.work_root.resolve(),
            arguments.threads,
            _budgets(arguments),
            arguments.progress_interval_seconds,
        )
        _configure_decoder_threads(context.threads)
        exit_code = _run_context(context)
        if exit_code != 0:
            print(context.result.get("error", "qualification failed"), file=sys.stderr)
        print(context.report_path)
        return exit_code
    except QualificationError as error:
        print(f"qualification error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
