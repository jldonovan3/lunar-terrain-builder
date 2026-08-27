# M7 controlled benchmark records

These records close the M7 operator-tooling scale gate with the controlled data available before the M8 multi-product qualification spike:

- `synthetic-windows-msvc-debug.json` covers all six level-zero QSC faces from `tests/data/synthetic_p0.toml`.
- `sldem2015-pinned-windows-msvc-debug.json` covers the externally provisioned, hash-pinned SLDEM2015 acceptance region from `tests/data/sldem2015_p1.toml`.

Both records were produced on 2026-08-26 with the committed `lunar-terrain benchmark` command and schema `lunar-terrain-m7-v1`. They contain no machine-local paths or source bytes. Reproduce a record with:

```text
lunar-terrain benchmark <configuration.toml> --output <record.json> --json
```

Metric definitions:

- catalog time measures configuration identity, artifact cataloging/hashing, metadata validation, and representative source access;
- sampling throughput is the number of newly built canonical 257×257 cores divided by clean-build wall time, so it is an intentionally end-to-end effective rate that also includes staging, encoding, packing, and publication;
- staging I/O is the total dependency-named staging artifact size after the clean build, with throughput normalized by clean-build wall time;
- compression ratio is decoded logical channel bytes divided by complete pack-file bytes, including pack headers and alignment;
- peak resident memory is the process high-water mark reported by the host operating system;
- validation time covers `validate --full` over the published database and packs;
- incremental reuse is the reused-tile fraction from an immediate dependency-identical incremental rebuild;
- deterministic rebuild requires matching database content identity and ordered pack identities between the clean and incremental publications.

Elapsed time, throughput, staging throughput, and peak memory are measurements, not frozen acceptance values. Configuration hashes, tile/pack counts, complete validation, full incremental reuse, and deterministic publication are the reproducibility anchors. These runs retained the frozen 1 GiB pack target, Zstandard level 3, v1 64×64 auxiliary maps, fusion rules, format fixtures, and output semantics.
