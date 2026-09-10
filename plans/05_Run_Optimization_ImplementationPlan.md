# M8 Run Optimization Implementation Plan

## Summary and completion contract

This plan is the authoritative task inventory for the optimization work. plans/04_M8_Run_Optimization_Evaluation.md remains the evidence source, and the existing architecture, format, milestone plan, and workpack retain their stated authority.

A completed run follows this path:

    detached observed process
      → source verification and metadata-only planning
      → persisted run/checkpoint plan
      → bounded decoded-block cache and spatial work chunks
      → deterministic seam/quantization frontiers
      → streamed cache shards and canonical packs
      → streamed full validation
      → unchanged/changed incremental reconstruction
      → inspection, exports, and continuously updated evidence

Completion requires:

- The 481,225-tile release scale profile completes clean build, full validation, unchanged incremental reconstruction, inspection, and exports on the current approximately 64 GiB Windows host.
- Builder-managed memory and transient scratch remain within explicit per-run budgets and do not grow with level-8 tile count. For the acceptance run, use 16,384 MiB managed memory, an 8,192 MiB decoded-block sub-budget, and 16,384 MiB transient scratch; peak process RSS must remain below 48 GiB.
- Every active command prints and flushes a progress snapshot at least every five seconds, writes structured events, and leaves partial evidence on failure or cancellation.
- Cancellation preserves the last transactional checkpoint and the previously published database; a new detached process can resume the same logical run.
- Performance-only work preserves format-v1 bytes and scientific results. No format-v1 specification or golden fixture changes are permitted.
- SLDEM2015 has one acquisition profile, SLDEM2015, backed only by the existing locked 96-member fullset bundle.
- Alternative A controls invalidation acceptance: content and provenance changes are footprint-confined, while v1 dependency hashes, database identity, and package placement may change globally; restoration must converge exactly.

## Interfaces and operational contracts

- Add Builder-only ResourceBudgets, ProgressEvent, ExecutionOptions, and a borrowed progress-sink interface whose reporting failures return Result<void>. The sink's caller owns it for the complete operation. Pass execution options through scan, plan, build, validation, and benchmark operations; retain std::stop_token cancellation and existing Result<T>/Error behavior.
- Keep LunarTerrainCore APIs and dependencies unchanged. New GDAL, SQLite, process, cache, telemetry, and formatting work remains private to Builder/tooling targets under hosted C++20.
- Add these machine-local CLI options without including them in semantic or package identity:
  - --run-id, --run-state-directory, --event-log, --progress-interval-seconds
  - --memory-budget-mib, --decoded-cache-budget-mib, --scratch-budget-mib
  - --decoded-cache-directory and --resume
- Default ordinary local commands to 2,048 MiB managed memory, 512 MiB decoded blocks, 4,096 MiB transient scratch, and five-second progress. Qualification commands must supply and record explicit budgets.
- Emit final machine-readable command results only on stdout. Emit flushed human progress on stderr and lunar-terrain-progress-v1 NDJSON events to the requested event log. Each event carries run/sequence identity, command and phase, face/level/chunk context, work counts and rate, worker states and queue depth, RSS/commit/page faults, cache residency/hits/misses/evictions, cumulative I/O and staging high-water, categorized timings, checkpoint identity, and any structured error.
- Replace the final-only M7 benchmark writer with an atomically updated lunar-terrain-benchmark-v2 report. It records running, passed, failed, or cancelled; separate scan and plan timing; actual requested source/halo samples; clean, validation, and incremental phases; all resource/I/O counters; budgets; identities; and partial results. Historical M7-v1 records remain unchanged.
- Add tools/run_observed.py, using only Python 3.9 standard-library facilities. Its start command launches an exact argv without a shell in a detached OS session/process group and returns immediately. run, status, follow, and cancel provide foreground execution, atomic status, live logs/events, and graceful interruption. Each attempt owns an immutable invocation record, PID/heartbeat, stdout/stderr logs, and terminal exit state.
- Reshape qualification/m8/run_qualification.py into run, resume, and sweep commands. It streams rather than buffers child output, atomically updates qualification.json after every phase/checkpoint, records the currently active child process, and resumes only from a matching immutable manifest.
- Keep CMake 3.25, the existing target graph, explicit source lists, imported dependencies, and current presets. Add no dependency, install, export, package-config, Unreal, or platform-specific provisioning machinery.

## Step-by-step implementation

### Preparation — Lock the corrected contracts

1. Correct plans/03_M8_LunarDEMProductQualification.md and the M3/M8 summaries in plans/01_LunarTerrainCore_LunarTerrainBuilder_Implementation_Plan.md:
   - Select Alternative A.
   - Replace footprint-confined v1 dependency-diff acceptance with footprint-confined scientific content/provenance plus exact restoration convergence.
   - Replace subset SLDEM acquisition requirements with one full-set identity used through spatially bounded Builder regions.
2. Correct qualification/m8/README.md and the workpack to say that the first scale run failed/incompletely terminated, whole-level staging was not resource-bounded, and no scale benchmark is currently running unless independently verified.
3. Treat all existing working-tree edits and abandoned external run data as user-owned. Start optimization runs in new roots; never delete old staging/cache data automatically.
4. After every priority gate, update only the workpack's current milestone, verified state, blocker, and next action.

### Priority 0 — Make runs observable, interruptible, and diagnosable

1. Implement a single thread-safe telemetry collector. Workers update counters and instrumented wait states; one reporter assigns event sequence numbers and writes progress, preventing worker scheduling from reordering the event stream.
2. Instrument scan, plan, source hashing/opening, projection, sampling, fusion, seam resolution, quantization, encoding, decoded cache, SQLite, staging, packing, publication, validation, inspection, and export. Distinguish live bytes, cumulative bytes, and high-water bytes.
3. Install CLI signal handling that requests the operation's std::stop_source. Check cancellation in all long loops and bounded waits. Return structured ErrorCode::cancelled; use process exit 130 for cooperative cancellation, 1 for operational failure, and 2 for CLI usage errors.
4. Make benchmark own scan and plan. Remove the qualification driver's duplicate scale scan/plan calls, and reuse one immutable prepared source catalog across benchmark phases.
5. Persist benchmark-v2 and qualification-v2 partial reports by sibling temporary file plus atomic replacement. Append and flush each NDJSON event; synchronize event/report files at checkpoints and terminal states. Readers tolerate only one truncated trailing NDJSON record after process failure.
6. Implement the generic detached observer. Its five-second heartbeat reports elapsed time, child state, last output/event time, and last known progress even when the child has stopped advancing. Graceful cancel is the default; forced termination requires an explicit separate option.
7. Add per-member and periodic byte/rate progress to provisioning/provision.py, including hashing and verification, while preserving it as the sole provisioning implementation.
8. Add qualification/m8/configs/scale_probe.toml: use the complete scale source stack but restrict the region to 40–50°E and 10–20°N. Require a dense level-8 plan spanning multiple work chunks and the SLDEM 45° component boundary.
9. Run the probe, cancel it after at least one committed checkpoint, resume it in a new detached attempt, and verify that reports distinguish advancing, blocked, cancelled, failed, and resumed states.

Priority-0 exit gate: no command remains silent for more than five seconds; scan and plan timings are separate and not duplicated; failure/cancellation leaves structured partial evidence; and the cancellation/restart probe reaches byte-identical output.

### Priority 1 — Remove unbounded storage and memory behavior

1. Split source lifetime into:
   - An immutable metadata descriptor/catalog containing verified artifact identity, GDAL metadata, physical-member coverage, and hierarchy inputs.
   - A sampling session that lazily leases GDAL datasets and decoded blocks only during sampling.
   plan performs zero raster sample reads. scan reads only its explicit source spot checks.
2. Build a physical-member spatial index from normalized declared coverage. Queries return only intersecting members in canonical member order, including longitude wrap and polar cases. A regional full-set SLDEM run must not decode or sample unrelated members.
3. Replace full-raster vectors with a persistent, validated decoded-block cache:
   - Key by artifact SHA-256, member/band/type, and normalized source-native block coordinates.
   - Store explicit little-endian versioned records in bounded-size shard files with checked offsets, lengths, CRC32C, and SHA-256.
   - Share immutable resident blocks and evict through a hard-cap LRU. Pinned blocks count against the decoded sub-budget; if the minimum operation cannot fit, fail before work begins with a contextual resource-limit error.
   - Treat truncation, checksum mismatch, and identity mismatch as errors rather than silently regenerating corrupted cache data.
4. Once metadata-only planning, the component index, and bounded block cache pass, consolidate SLDEM:
   - Keep only the existing sldem2015/fullset 96-member bundle and its locked order, byte total, checksums, and canonical hash.
   - Replace Artifact, Fullset, and SLDEMBoundary profiles with the single case-insensitive profile SLDEM2015; provide no compatibility aliases.
   - Migrate M3, Profiles A–C, scale, probe, lock verification, provisioning tests, and documentation to the complete bundle. Profile C declares SLDEM even though its required region does not intersect it.
   - Keep the M3 scientific region centered on the former 0–30°N, 0–45°E case so its sample assertions remain meaningful, while accepting the intentional configuration/dataset/database identity change.
   - Record newly verified non-format identities; do not update format fixtures.
5. Replace both current raster build paths with one bounded production pipeline:
   - Persist a run plan of canonical face/level/Morton ranges. Select power-of-four Morton-aligned work chunks from checked worst-case memory estimates and record the selection so resume never replans completed work.
   - Sample and fuse a chunk plus required halo, resolve each shared raw binary64 edge/corner once using the lowest-key owner, quantize every tile once, and retain only the raw-edge and quantized-neighbor strips needed by future chunks.
   - Read nearest materialized ancestors from already finalized pack/cache data through a bounded parent cache rather than retaining all ancestor qcores.
   - Preserve explicit QSC cross-face orientation, fixed source and row-major floating-point order, post-seam quantization, ordinary/virtual aprons, and canonical output ordering.
   - Remove the permanent small-versus-large implementation split after equivalence tests pass.
6. Replace per-tile persistence with checkpointed state:
   - Use one ordered cache-writer thread and bounded queue.
   - Write reusable tile/scientific payloads into versioned shard containers and commit bounded batches in one synchronous=FULL SQLite transaction.
   - Synchronize only completed checkpoint shards, pack temporaries, and final publications. Do not call FlushFileBuffers/fsync for individual disposable tile intermediates.
   - On cancellation, ignore or truncate the uncommitted shard/pack tail and resume after the last committed chunk.
   - Use new side-by-side cache-v2 files; leave legacy cache.sqlite, per-tile staging, and full-raster decoded caches untouched and document manual cleanup.
7. Stream canonical packs:
   - Feed ready tile records through a bounded reorder buffer.
   - Stream one pack temporary at a time, finalize its header, calculate the required v1 hash over the completed file, patch the prefix, synchronize, and publish.
   - Retain tile payloads only in reusable cache shards; do not create millions of individual auxiliary/qcore/encoded files.
   - Publish .ltdb last so interruption leaves the prior database usable.
8. Stream validation from sorted TIDX:
   - Check hierarchy and structure from the index without decoded-tile residency.
   - Decode one tile plus only its required parent/neighbors through a bounded validation cache.
   - Compare every owned seam exactly once and preserve all current scientific, provenance, quality, CRC, and hash checks.
   - Checkpoint validation progress so a scale validation can resume.

Priority-1 exit gate: pre-consolidation performance changes reproduce the existing Profile A–C identities; post-consolidation baselines change only for the approved product-identity migration; generated dense hierarchies prove bounded cores/frontiers/files; all chunk/face/corner cases remain byte-identical; and probe peak counters remain within the explicit budgets.

### Priority 2 — Remove serialized and repeated computation

1. Produce one sampling plan per source/work chunk. Transform each unique QSC/geographic coordinate to source pixels once and reuse taps for coverage, dependency windows, elevation, and quality.
2. Assign coordinates directly through the physical-member spatial index instead of testing every coordinate against every mosaic component.
3. Replace nested executors and source-wide mutexes with one global bounded executor. Each physical member supplies a bounded pool of lazily opened GDAL/PROJ handle leases; no handle is used concurrently.
4. Reuse projected coordinates, source samples, fused values, and halos shared by adjacent tiles in a chunk while retaining the exact per-sample source order and floating-point evaluation order.
5. Keep elevation values and validity in separate contiguous arrays. Skip fusion boundary-distance computation when a source is fully valid over the work window, and retain the current optional representation only if telemetry shows the replacement would reduce neither managed memory nor fusion traversal.
6. Keep tile auxiliary data in the active chunk until encoding, share immutable decoded parents among siblings, and remove block-order sorts after block-key grouping already provides locality.
7. Add deterministic counters proving:
   - Candidate-component checks scale with intersecting members, not all 32 SLDEM members.
   - Pixel transforms occur once per coordinate/source/window.
   - Nested executor count is zero.
   - Fully valid sources build no unused distance field.
   - Each tile auxiliary is loaded at most once and each tile is quantized once.

Priority-2 exit gate: post-consolidation output is identical across worker counts, budgets, and chunk boundaries; the repeated-work counters satisfy the rules above; and the probe shows no regression in resource high-water or total workflow time.

### Priority 3 — Make incremental work proportional to change using Alternative A

1. Add internal scientific-work key domain LTBUILD_SCIENTIFIC_WORK_V1. Serialize it explicitly from:
   - TileKey and relevant datum/projection/tile/quantization/fusion/builder algorithm versions.
   - Sorted active DatasetIDs, artifact bundle hashes, priorities/policies/quality mappings, and exact source-window dependencies.
   - Any neighbor/ancestor inputs required to reconstruct the expensive scientific result.
   Omit unrelated declared sources, complete semantic-configuration hash, database name, packaging settings, paths, budgets, workers, and scheduling.
2. Cache the expensive fused/quantized core plus provenance/quality inputs under this key. On a scientific hit, rerun seam/apron, hierarchy metadata, v1 dependency construction, and encoding so the published payload always contains the correct frozen-v1 dependency identity.
3. Report exact-v1 tile reuse, scientific-work reuse, scientifically recomputed tiles, content-stable re-encodes, pack reuse, and avoided bytes separately.
4. Define the source influence set as tiles sampling the changed source plus the required fusion-transition/filter halo and same-level apron/seam neighbors. Expose its count/hash and bounded representative list in plan/qualification evidence.
5. Update the required-region workflow assertions:
   - Added/removed tiles match hierarchy planning.
   - Content and provenance changes are subsets of the computed influence set.
   - Dependency and package-placement changes may include every surviving tile.
   - Restoration exactly reproduces database and ordered pack identities.
6. Add a pack-composition key over the complete encoded payload identity and size of every canonical member, not merely decoded content hashes. Reuse a previously checkpointed pack directly when composition, path, size, and durable cache record match; otherwise stream-build or verify it. Full validation remains the cryptographic check against later external corruption.

Priority-3 exit gate: refinement removal reuses scientific work outside the influence set despite globally changed v1 dependencies; unchanged incremental publication reuses unchanged packs without duplicate temporary writes or complete existing-pack reads; and all restoration identities converge exactly.

### Priority 4 — Tune concurrency and finish qualification

1. Use the probe with identical explicit budgets for worker counts 1, 2, 4, 8, and 16. For each count, record one cold-cache run and three warm timed runs. Keep eight workers as the comparison baseline.
2. Admit a 32-worker probe only if 16 workers is at least 5% faster than eight, uses less than 80% of each managed budget, keeps RSS below 40 GiB, and does not worsen decoded-cache hit rate or major-page-fault rate by more than 5%.
3. Select the lowest worker count whose median complete workflow time is within 5% of the fastest qualifying result. A candidate is disqualified by any identity difference, budget breach, failed checkpoint, cache-thrash condition, or increased serialized-wait share.
4. Run strict no-network provisioning verification for SLDEM2015, LOLA, Maskelyne, and south-polar products. Run the migrated M3 acceptance and Profiles A–C on Windows and Linux.
5. Launch the complete release scale workflow at the selected count. Require clean build, streamed full validation, unchanged incremental build, inspect, and every required export; continuously retain events, checkpoint state, resource high-water, and partial/final reports.
6. Run debug and release workflows on Windows and debug, release, and ASan/UBSan workflows on Linux. If Linux infrastructure remains unavailable, record it as an active blocker and do not mark M8 complete.
7. Update the M8 qualification report with accepted products/policies, the full-set identity, hierarchy, scientific evidence, resource bounds, concurrency conclusion, limitations, and Alternative-A results. Keep raw run roots, local paths, job counts, caches, staging, databases, and packs external/ignored.
8. Reduce the workpack to the completed milestone, final verification state, any genuine platform blocker, and the next action.

No wall-time target is introduced. Completion is based on deterministic correctness, bounded resources, continuous observability, and successful execution.

## Verification matrix

- Progress and process control: schema/version validation, monotonic event sequence, five-second heartbeats, atomic status/report replacement, foreground/detached equivalence, stale-PID protection, graceful cancel, forced-cancel opt-in, and resume from a new process.
- Source/cache: metadata-only plan with zero sample reads; explicit scan spot checks; cold/warm/eviction/concurrent block access; hard budget; truncated/corrupt/wrong-identity blocks; longitude-wrap and polar component selection.
- Pipeline: dense multi-chunk generated hierarchies; chunk and worker boundary seams; reversed cross-face edges and all cube corners; apron and nearest-ancestor error; cancellation before/after checkpoint; stale-tail recovery; byte identity against the former direct path.
- Persistence/packing: bounded queue backpressure, batched SQLite rollback/commit, shard corruption, streamed pack rollover, interrupted publication, direct pack reuse, and final LTDB-last publication.
- Validation: structural, channel, hierarchy, provenance, quality, and seam corruption detection with asserted maximum decoded frontier.
- Alternative A: unrelated-source declaration, refinement removal, transition-halo changes, global v1 dependency changes, scientific reuse outside influence, and exact restoration.
- Compatibility: tools/generate_format_v1_fixtures.py --check, unchanged golden files, synthetic/M3/Profile A–C output checks, reordered sources, multiple budgets/chunk sizes, and multiple worker counts.

## Documented operator commands

Generic detached lifecycle:

    python tools/run_observed.py start --run-dir <attempt-root> --name <name> -- <command> <arguments...>
    python tools/run_observed.py status --run-dir <attempt-root> --json
    python tools/run_observed.py follow --run-dir <attempt-root>
    python tools/run_observed.py cancel --run-dir <attempt-root>

Provision or strictly verify the consolidated SLDEM product through the unchanged provisioning entry point:

    python tools/run_observed.py start --run-dir <attempt-root> --name provision-sldem -- python provisioning/provision.py SLDEM2015 --root <sldem-root>
    python provisioning/provision.py SLDEM2015 --root <sldem-root> --verify-only

Direct observable benchmark:

    lunar-terrain benchmark qualification/m8/configs/scale_probe.toml --output <benchmark-json> --output-directory <output-root> --cache-directory <cache-root> --decoded-cache-directory <decoded-cache-root> --run-id <run-id> --run-state-directory <state-root> --event-log <events-ndjson> --progress-interval-seconds 5 --memory-budget-mib 16384 --decoded-cache-budget-mib 8192 --scratch-budget-mib 16384 --threads <count> --json

Detached probe, sweep, scale, and resume:

    python tools/run_observed.py start --run-dir <probe-attempt> --name m8-probe -- python qualification/m8/run_qualification.py run probe --binary <lunar-terrain> --work-root <probe-work-root> --threads <count> --memory-budget-mib 16384 --decoded-cache-budget-mib 8192 --scratch-budget-mib 16384

    python tools/run_observed.py start --run-dir <sweep-attempt> --name m8-sweep -- python qualification/m8/run_qualification.py sweep --binary <lunar-terrain> --work-root <sweep-work-root> --threads 1 2 4 8 16 --memory-budget-mib 16384 --decoded-cache-budget-mib 8192 --scratch-budget-mib 16384

    python tools/run_observed.py start --run-dir <scale-attempt> --name m8-scale -- python qualification/m8/run_qualification.py run scale --binary <lunar-terrain> --work-root <scale-work-root> --threads <selected-count> --memory-budget-mib 16384 --decoded-cache-budget-mib 8192 --scratch-budget-mib 16384

    python tools/run_observed.py start --run-dir <new-resume-attempt> --name m8-scale-resume -- python qualification/m8/run_qualification.py resume --work-root <scale-work-root>

Repository verification, also runnable through run_observed.py:

    cmake --workflow --preset debug
    cmake --workflow --preset release
    cmake --workflow --preset linux-asan
    cmake --workflow --preset sldem2015-acceptance
    python tools/generate_format_v1_fixtures.py --check
    python provisioning/verify_builder_locks.py --lock provisioning/provisioning.json tests/data/sldem2015_p1.toml qualification/m8/configs/profile_a.toml qualification/m8/configs/profile_b.toml qualification/m8/configs/profile_c.toml qualification/m8/configs/scale_probe.toml qualification/m8/configs/scale.toml

## Assumptions and fixed defaults

- The output filename includes the repository-standard .md extension: plans/05_Run_Optimization_ImplementationPlan.md.
- The consolidated profile is named SLDEM2015; its sole bundle remains named fullset so its existing member order and canonical hash are preserved.
- Format v1, its dependency algorithm, fusion semantics, compression parameters, and golden fixtures remain frozen.
- Legacy caches are disposable but are never silently removed; cache v2 is created alongside them.
- Resource budgets and run state are machine-local and excluded from every canonical identity.
- Raw observability artifacts remain outside source control; committed summaries contain no machine-local paths, cache/staging locations, or selected local job count.
- Python tooling remains standard-library-only, and all C++ work remains within LunarTerrainCore and standalone LunarTerrainBuilder.
