#!/usr/bin/env python3
"""Check the standalone CLI's stable usage and operational exit codes."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    arguments = parser.parse_args()
    binary = arguments.binary.resolve(strict=True)
    repository = Path(__file__).resolve().parents[1]
    synthetic = repository / "tests" / "data" / "synthetic_p0.toml"

    cases = (
        ("missing required argument", [str(binary), "scan"], 2),
        (
            "invalid resource budget",
            [str(binary), "--memory-budget-mib", "0", "scan", str(synthetic)],
            2,
        ),
    )
    for name, argv, expected in cases:
        completed = subprocess.run(
            argv,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if completed.returncode != expected:
            print(
                f"{name}: expected {expected}, got {completed.returncode}",
                file=sys.stderr,
            )
            return 1

    with tempfile.TemporaryDirectory() as temporary_directory:
        temporary = Path(temporary_directory)
        missing = temporary / "missing.toml"
        completed = subprocess.run(
            [str(binary), "scan", str(missing)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if completed.returncode != 1:
            print(
                f"operational failure: expected 1, got {completed.returncode}",
                file=sys.stderr,
            )
            return 1

        partial_path = temporary / "failed-benchmark.json"
        completed = subprocess.run(
            [
                str(binary),
                "benchmark",
                str(missing),
                "--output",
                str(partial_path),
                "--json",
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if completed.returncode != 1:
            print(
                f"benchmark failure: expected 1, got {completed.returncode}",
                file=sys.stderr,
            )
            return 1
        partial = json.loads(partial_path.read_text(encoding="utf-8"))
        if (partial.get("benchmark_schema") != "lunar-terrain-benchmark-v2"
                or partial.get("status") != "failed"
                or partial.get("active_phase") != "configuration"):
            print("benchmark failure did not preserve a v2 partial report", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
