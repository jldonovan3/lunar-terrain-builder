from __future__ import annotations

import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest

import run_qualification


def manifest(binary: Path) -> dict[str, object]:
    return {
        "schema": run_qualification.MANIFEST_SCHEMA,
        "profile": "probe",
        "binary": str(binary),
        "threads": 1,
        "budgets_mib": {
            "managed_memory": 64,
            "decoded_cache": 16,
            "transient_scratch": 64,
        },
        "progress_interval_seconds": 0.05,
    }


class QualificationReportTests(unittest.TestCase):
    @staticmethod
    def encoded_tile_key(face: int, level: int, x: int, y: int) -> bytes:
        morton = 0
        for bit in range(level):
            morton |= ((x >> bit) & 1) << (2 * bit)
            morton |= ((y >> bit) & 1) << (2 * bit + 1)
        encoded = (face << 61) | (level << 56) | morton
        return encoded.to_bytes(8, "little")

    def test_probe_uses_complete_scale_stack_and_locked_cross_boundary_region(self):
        repository = Path(run_qualification.__file__).resolve().parents[2]
        evidence = run_qualification.verify_probe_configuration(
            repository / "qualification" / "m8" / "configs" / "scale_probe.toml")
        self.assertTrue(evidence["complete_scale_source_stack"])
        self.assertEqual(4, evidence["source_count"])
        self.assertEqual(40.0, evidence["region"]["west_longitude_degrees"])
        self.assertEqual(50.0, evidence["region"]["east_longitude_degrees"])
        self.assertEqual(45.0, evidence["sldem_component_boundary_degrees"])

    def test_probe_plan_requires_dense_level_eight_work(self):
        evidence = run_qualification.verify_probe_plan({
            "level_counts": [
                {"level": 7, "tile_count": 50},
                {"level": 8, "tile_count": 129},
            ],
        })
        self.assertEqual(8, evidence["level"])
        self.assertEqual(3, evidence["reference_work_chunk_count"])
        with self.assertRaisesRegex(
                run_qualification.QualificationError, "is not dense"):
            run_qualification.verify_probe_plan({
                "level_counts": [{"level": 8, "tile_count": 127}],
            })

    def test_selected_tile_falls_back_to_highest_published_cache_tile(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            cache_path = Path(temporary_directory) / "cache.sqlite"
            connection = sqlite3.connect(cache_path)
            try:
                connection.execute(
                    "CREATE TABLE tiles("
                    "tile_key BLOB, build_state INTEGER, previous_pack_id INTEGER)")
                connection.executemany(
                    "INSERT INTO tiles VALUES(?, ?, ?)",
                    [
                        (self.encoded_tile_key(4, 7, 80, 81), 2, 0),
                        (self.encoded_tile_key(4, 8, 160, 161), 2, 1),
                        (self.encoded_tile_key(4, 8, 162, 161), 2, 1),
                        (self.encoded_tile_key(4, 8, 163, 161), 1, None),
                    ],
                )
                connection.commit()
            finally:
                connection.close()
            self.assertEqual(
                "QSC/F4/L08/0162/0161",
                run_qualification.selected_tile({}, cache_path),
            )

    def test_selected_tile_prefers_report_prototype_over_cache_fallback(self):
        self.assertEqual(
            "QSC/F2/L12/0042/0099",
            run_qualification.selected_tile(
                {"prototype_tiles": ["QSC/F2/L12/0042/0099"]},
                Path("missing-cache.sqlite"),
            ),
        )

    def test_ndjson_allows_only_a_truncated_trailing_record(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "events.ndjson"
            path.write_text('{"sequence": 1}\n{"sequence":', encoding="utf-8")
            self.assertEqual([{"sequence": 1}], run_qualification._read_ndjson(path))
            path.write_text('{"sequence":\n{"sequence": 2}\n', encoding="utf-8")
            with self.assertRaisesRegex(
                    run_qualification.QualificationError, "non-trailing"):
                run_qualification._read_ndjson(path)

    def test_streamed_command_checkpoints_an_atomic_partial_report(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            context = run_qualification.RunContext(
                root, manifest(Path(sys.executable)), resumed=False)
            context.common_arguments = lambda *_: []  # type: ignore[method-assign]
            report = context.run_command(
                "synthetic-pass",
                ["-c", 'print("{\\"result\\":42}")'],
                expect_json=True,
            )
            self.assertEqual(42, report["result"])
            persisted = json.loads(context.report_path.read_text(encoding="utf-8"))
            self.assertIn("synthetic-pass", persisted["checkpoints"])
            self.assertEqual("passed", persisted["commands"][0]["state"])
            self.assertFalse(context.report_path.with_name("qualification.json.tmp").exists())

    def test_resume_records_a_new_attempt_and_reuses_completed_command(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            base_manifest = manifest(Path(sys.executable))
            context = run_qualification.RunContext(root, base_manifest, resumed=False)
            context.common_arguments = lambda *_: []  # type: ignore[method-assign]
            expected = context.run_command(
                "once",
                ["-c", 'print("{\\"value\\":7}")'],
                expect_json=True,
            )
            resumed = run_qualification.RunContext(root, base_manifest, resumed=True)
            resumed.common_arguments = lambda *_: []  # type: ignore[method-assign]
            actual = resumed.run_command(
                "once",
                ["-c", 'print("{\\"value\\":7}")'],
                expect_json=True,
            )
            self.assertEqual(expected, actual)
            self.assertTrue(resumed.result["resumed"])
            self.assertEqual(2, len(resumed.result["attempts"]))
            self.assertEqual(1, len(resumed.records))


if __name__ == "__main__":
    unittest.main()
