# M8 Scale-Run Optimization Evaluation

## Purpose and conclusion

This report evaluates the incomplete M8 scale qualification run described by the
[M8 qualification guide](../qualification/m8/README.md). It works backward from
that driver through planning, raster access, hierarchy construction, tile staging,
seam resolution, packing, validation, and incremental reconstruction. The
[architecture](../specs/01_LunarTerrainBuilder_Architecture_v0.3.md), frozen
[format v1](../specs/03_LunarTerrainDatabase_Format_v1.md), and
[M8 qualification plan](03_M8_LunarDEMProductQualification.md) remain the controlling
sources for scientific behavior and compatibility.

The run did not expose one unexplained stall. It exposed several strategies that
were correct on small fixtures but scale with the largest level or the complete
database rather than with a bounded work window. The dominant failure is the
whole-level binary64 staging design. Level 8 alone requires 183,528,601,200 bytes
of staged cores before any of those cores can be released. Retained auxiliaries,
parent cores, newly quantized cores, and encoded artifacts take the level well
beyond 200 GB. Encoding then repeatedly reloads and refinalizes overlapping
two-hop neighborhoods. A successful build would encounter another scale cliff in
full validation, which currently retains every decoded tile at once.

The 46.07 GB decoded-raster cache is a separate problem. Its expansion from
compressed source products is expected, but loading every decoded raster into
resident memory and repeating that load for scan, plan, build, and incremental
execution is not. Disabling it is not an adequate remedy because the uncached
GDAL path serializes sampling through a source mutex.

The preferred performance direction is therefore a bounded spatial pipeline:
metadata-only planning, a memory-budgeted decoded block cache, chunked tile work
with a deterministic seam frontier, streamed validation and packing, and batched
cache metadata. More threads or larger hardware can help diagnose the current
path, but cannot correct its asymptotic storage, validation-memory, and repeated
I/O behavior.

This is an evaluation only. It does not change source code, the v1 format,
scientific output, provisioning locks, or milestone acceptance.

## Evidence basis and execution trace

The evaluated implementation is the current working tree based on commit
`64290c6` (`M8 spike first pass`). The full-hierarchy builder, bounded staging
path, decoded-raster cache, and qualification driver are working-tree changes on
top of that commit. This matters because commit `64290c6` planned the complete
scale hierarchy but built only prototype raster tiles; the present disk-backed
path is the first implementation to attempt all 481,225 planned tiles.

The surviving ignored `qualification.json` for the scale attempt records the
following commands. These are the authoritative captured timings; the longer
duration and progress estimate are separately identified as operator observations.

| Command | Captured result | Elapsed |
|---|---:|---:|
| Outer `scan` | success | 76.615 s |
| Outer `plan` | success | 109.539 s |
| `benchmark` child | exit code 1 | 18,570.607 s |
| Complete driver | failed | 18,756.763 s (5 h 12 m 37 s) |

The benchmark child produced six `proj.db` lookup warnings but no structured
Builder error or benchmark record. The operator additionally observed a run of
more than six hours, approximately 350,000 completed tiles, more than 200 GB of
working data, and approximately 100,000 tiles remaining. The evidence does not
establish whether the child was manually interrupted, failed during an unreported
operation, or had stopped making useful progress. It does establish that the
current reporting is insufficient to distinguish those states. The PROJ warning
should be removed before another qualification run, but its small fixed count
does not explain the storage growth.

The effective command path is longer than the qualification guide suggests:

```text
provision and verify locked products
  -> qualification driver: scan
  -> qualification driver: plan
  -> benchmark command
       -> scan again
       -> plan again
       -> clean build
            -> reopen raster sources
            -> plan hierarchy again
            -> prepare every tile in a level
            -> encode every tile in that level
            -> release that level's binary64 core and auxiliary files
            -> assemble and publish packs
       -> full validation
       -> unchanged incremental build and publication
  -> inspect and exports
```

The first-pass scale run ended within `benchmark`, before the driver could reach
inspection or exports. Because the benchmark writes its JSON only after all
listed phases finish, it preserved no internal phase, level, tile, memory, I/O,
or utilization progress.

## Findings

### F1 — Whole-level staging explains the 200+ GB footprint

The large-hierarchy path in
[`container_writer.cpp`](../src/builder/container_writer.cpp) prepares every tile
at a level before starting any encoding task. Each prepared tile persists a
257×257 binary64 core and a raster auxiliary artifact. Those files remain until
every encoding task at the level completes.

The captured plan contains 347,300 level-8 tiles. A staged core is 528,444 bytes:
52 bytes of header plus 66,049 binary64 values. Level-8 cores therefore consume
183,528,601,200 bytes (170.92 GiB). Before level 8, 131,070 quantized ancestor
cores remain and consume another 17,321,424,780 bytes (16.13 GiB). Depending on
whether per-tile dominant-source and quality maps are present, level-8 auxiliary
files add approximately 2.90–7.17 GB (2.70–6.67 GiB).

The result is 203.75–208.02 GB (189.76–193.73 GiB) before current-level quantized
or encoded artifacts are counted. As level-8 encoding proceeds, quantized cores
can add 45.90 GB (42.75 GiB). Near level completion the derived footprint is
249.65–253.91 GB before encoded tile artifacts. This directly agrees with the
operator's observation and does not depend on the final `.ltdb` or `.ltp` size.

This storage is an execution artifact, not a v1 requirement. The architecture
requires binary64 values until deterministic seam ownership has been resolved;
it does not require every full core in a level to coexist as an individual file.

The qualification guide calls this path "level-bounded," and the current
workpack says the former scale-memory risk is corrected. The implementation is
bounded only in the sense that it retains one level rather than the complete
hierarchy. Because the dominant level contains 347,300 tiles, that is not a
useful resource bound. Both statements should be corrected when milestone state
is next updated.

### F2 — Encoding repeats neighborhood I/O and quantization

For each target tile, `finalize_streaming_raster_tile` builds a two-hop
same-level neighborhood containing as many as 13 tiles. It loads each full
binary64 core, resolves boundaries for the neighborhood, and calls the finalizer
for every loaded core, although it returns only the target. Adjacent target tasks
repeat most of the same reads and work.

For a dense level-8 interior, the upper bound is approximately 4.51 million core
loads, 2.17 TiB of core reads, and 298.2 billion finalized core samples. These are
derived upper bounds rather than measured counters, but the control flow is
confirmed. The target auxiliary is also loaded twice, and each child reloads its
parent quantized core to calculate geometric error.

The two-hop neighborhood is a correctness workaround for independent tile
tasks. It should be replaced by a shared deterministic frontier or journal, not
made faster by increasing the number of tasks performing the same work.

### F3 — The decoded cache trades source serialization for unbounded residency

The four locked scale products total 19,366,863,515 bytes (19.37 GB or 18.04
GiB). The materialized decoded cache contains 42 files totaling 46,066,043,596
bytes (46.07 GB or 42.90 GiB), a 2.379× expansion:

| Decoded data | Files | Resident payload |
|---|---:|---:|
| LOLA global `Int16` mosaics | 4 | 8.49 GB |
| SLDEM2015 `Int16` mosaics | 32 | 22.65 GB |
| Maskelyne elevation and confidence | 2 | 0.14 GB |
| South-polar elevation and companions | 4 | 14.79 GB |

The byte expansion is normal decompression. The scale issue is that
[`raster_source.cpp`](../src/builder/raster_source.cpp) reads and hashes each
cache file in full into an owning `std::vector`, then retains all component
vectors for the source lifetime. LOLA and SLDEM use metadata overrides and are
loaded eagerly during source opening. Large sampling windows later bring the
Maskelyne and polar arrays into memory as well.

The outer driver and inner benchmark independently invoke scan and plan, and the
clean build opens the sources again. Before clean-build sampling completes, this
causes roughly 159 GiB of decoded-payload creation or traversal across repeated
source lifetimes. A completed benchmark would reopen the full 42.90 GiB set for
the unchanged incremental build. Operating-system file caching can reduce device
reads, but not hashing, memory allocation, copying, or resident-memory pressure.

The completed regional workflows show the magnitude of the cold-versus-warm
effect. Their two clean builds use separate empty tile caches but share the
decoded cache; the second run therefore receives warm decoded files and operating
system pages rather than tile reuse.

| Profile | Tiles | First clean build | Second clean build | Speedup |
|---|---:|---:|---:|---:|
| A | 2,480 | 723.08 s | 125.11 s | 5.8× |
| B | 41 | 1,476.23 s | 61.54 s | 24.0× |
| C | 77 | 661.24 s | 88.93 s | 7.4× |

The fixed source-opening/cache cost can therefore dominate a small tile plan.
The table does not isolate storage-device reads, decoding, hashing, allocation,
or page-cache effects; Priority 0 instrumentation is needed to divide those
costs.

Without the decoded cache, `GdalRasterSource::SampleBatch` holds a source mutex
around GDAL access. Disabling the cache would reduce resident memory but collapse
parallel sampling. A bounded concurrent reader/cache design is required.

### F4 — Full validation is not scale-bounded

[`builder.cpp`](../src/builder/builder.cpp) implements validation through
`read_all_tiles`, which reads and retains every `DecodedTerrainTile` before
scientific, hierarchy, and seam checks begin. This happens even for non-full
validation; `--full` adds the scientific and seam traversals.

The planner estimates 68,542,801,650 uncompressed channel bytes (63.84 GiB).
Elevation alone is 64,562,108,450 bytes (60.13 GiB). Containers, metadata,
auxiliary channels, decompression buffers, the database index, and allocator
overhead push the live set beyond the current host's approximately 64 GiB memory
envelope. Even if the clean build completed, full validation would likely page
heavily or fail.

This behavior predates M8 and was safe only because earlier fixtures contained
few tiles. M7 made full validation part of every benchmark without adding a
scale-representative validation case.

### F5 — Per-tile durability and metadata updates serialize workers

Disposable auxiliary, quantized-core, and encoded-tile artifacts are written
through `write_file_synced`; on Windows this calls `FlushFileBuffers` for every
file. A clean 481,225-tile build can therefore request approximately 1.44 million
durable file flushes before pack and database publication.

[`build_cache.cpp`](../src/builder/build_cache.cpp) uses one SQLite connection in
WAL mode with `synchronous=FULL`. Clean tiles perform `MarkTileBuilding` and
`StoreCompletedTile` as independent autocommit statements. Publication then
calls `UpdatePacking` once per tile in a serial loop. This is at least another
1.44 million serialized database updates and commit opportunities. Concurrent
workers share the same full-mutex connection, so they queue at this boundary.

At peak, the flat staging directory can approach 1.65 million files: current
level cores, auxiliaries, quantized cores, encoded artifacts, and retained
ancestor artifacts. Directory lookup, creation, rename, flush, and cleanup costs
therefore grow along with payload I/O.

These behaviors are strong explanations for low CPU utilization, but the failed
run did not capture active-worker, filesystem-wait, or SQLite-wait time. They
must be reported as code-supported bottleneck hypotheses rather than measured
utilization results.

### F6 — Unchanged incremental publication remains proportional to the product

The tile cache avoids scientific reconstruction when dependencies match, but it
does not make publication cheap. Preparation loads and verifies every encoded
tile artifact. Packing reloads those artifacts, reconstructs all temporary packs,
and writes them. If the final content-addressed pack already exists, publication
reads that pack in full to verify its hash and removes the newly written
temporary pack.

Consequently, 100% tile reuse can still mean a complete encoded-artifact read, a
complete temporary pack write, and a complete existing-pack read. The benchmark
also reloads the decoded raster caches before discovering that the tile work is
reusable. Incremental reuse ratio alone does not describe the remaining work.

### F7 — Raster sampling contains avoidable repeated computation

The current source path has several multiplicative costs:

- `MosaicRasterSource::SampleBatch` scans the entire coordinate batch against
  every component to discover relevant components. A SLDEM tile batch can test
  roughly 100,000 coordinates against each of 32 physical rasters before
  sampling the one or small number that intersect.
- A component calculates pixel coordinates to form a window and ordering, sorts
  covered indices by GDAL block, and then calls a sampling function that repeats
  coverage and pixel-coordinate work. The sort is of limited value after the
  complete decoded array is already resident.
- `WindowDependency` visits all mosaic components for every logical SLDEM tile,
  including components that do not intersect the tile.
- Fusion constructs optional-valued grids and computes a complete valid-boundary
  distance field for every active source, including fully valid sources for
  which no transition boundary exists.
- Most tiles use a 327×327 fusion grid for a 257×257 core, approximately 62%
  sampling overhead. Adjacent tile tasks independently transform and sample the
  overlapping halos.
- The outer tile executor may contain eight tasks while a mosaic source creates
  nested executors of up to four tasks. This can oversubscribe decode work and
  makes global concurrency difficult to control.

These are meaningful throughput opportunities after the unbounded staging,
cache residency, and validation problems are corrected.

### F8 — Eight workers are neither the sole problem nor a validated optimum

The run selected eight Builder workers on a host exposing 32 logical processors.
`run_bounded_tasks` uses a dynamic atomic task index and `std::jthread`; task
distribution itself does not impose an obvious eight-way serial schedule.
However, filesystem flushes, the single SQLite connection, source locks, nested
component executors, and level-wide barriers can leave workers blocked or idle.

Raising the worker count before those waits are measured can increase cache
pressure, page faults, SQLite contention, and random I/O. Eight workers should
remain the comparison baseline. Thread-count sweeps should occur only after the
pipeline has bounded resources and exposes phase-specific utilization.

### F9 — The benchmark cannot explain or safely resume a scale run

The M7 benchmark record was designed around six synthetic tiles and one pinned
SLDEM tile. Its current metrics do not represent the new workload accurately:

- catalog time measures scan but excludes plan;
- sampling throughput divides 257×257 core vertices by total clean-build time,
  even though that time includes sampling halos, fusion, seams, encoding,
  filesystem work, cache transactions, packing, and publication;
- staging I/O is the live size of the staging directory after cleanup, not
  cumulative bytes read/written or high-water usage;
- peak RSS is process-wide, but there is no phase attribution;
- no progress or partial benchmark record is written until the entire clean,
  validation, and incremental sequence completes.

The resulting silence made normal long-running work, an I/O-bound tail, paging,
deadlock, and cancellation indistinguishable to the operator.

### F10 — Correctness coverage did not exercise the scale shape

The full hierarchy test contains 30 tiles backed by tiny generated rasters. The
decoded-cache test uses one approximately 8 MB raster. Existing tests prove many
important invariants—deterministic ordering, cache reuse, hierarchy connectivity,
seams, corruption handling, and byte identity—but none asserts staging
high-water, resident-memory budget, file count, cumulative I/O, or behavior with
a level containing hundreds of thousands of tiles.

The green 73-test debug and release suites therefore do not contradict the scale
failure. They validate output correctness for small inputs, not bounded resource
use.

### F11 — SLDEM provisioning modes no longer match the intended product model

The provisioning lock currently exposes three SLDEM acquisition shapes: a
three-member `Artifact`, a 96-member `Fullset`, and a three-member northern
boundary bundle. Profile A embeds the first subset, Profile B embeds the boundary
subset, and scale embeds the full set. This duplicates product identity across
provisioning, Builder configurations, lock verification, documentation, and
tests.

The intended future model is one canonical SLDEM2015 full-set profile and bundle,
used by Profiles A–C, scale, and the migrated M3 path. Regional Builder runs can
remain spatially bounded through `required_region`; acquisition and semantic
source identity do not need to be regional subsets.

This consolidation is not itself a performance optimization. With the current
eager decoded-cache behavior, changing small profiles to the full set would make
them load all 32 SLDEM arrays. Full-set migration should therefore follow or
travel with metadata-only planning, spatial component selection, and bounded
decoded caching. It also intentionally revises older M3 and M8 statements that
preserve the three-member identity, so it must be an explicit provisioning and
acceptance migration rather than an incidental configuration edit.

### F12 — The v1 invalidation conflict is real but orthogonal to the clean run

Format v1 defines each tile dependency document with
`semantic_configuration_sha256` and fusion and quantization configuration
hashes. The implementation supplies the same complete semantic configuration
hash to all three fields. Removing a refinement changes that hash, so every
surviving tile receives a different published dependency hash even when its
sampled content and provenance are unchanged.

This makes the M8 Stage 4 requirement that dependency diffs remain confined to
the removed refinement footprint impossible under the frozen v1 contract. The
observed Profile B result—all 33 surviving tiles rebuilt, none reused, removed
tiles absent, and exact convergence after restoration—is the expected behavior
of the current contract.

The conflict can make changed-configuration incremental runs unnecessarily
expensive, but it did not cause the clean scale build's whole-level staging,
decoded-cache residency, or validation behavior. Resolving it is not a
prerequisite for a bounded clean scale run.

## Analysis of findings

### Expected expansion versus avoidable amplification

The comparison between 19.37 GB of source products and more than 200 GB of work
data mixes four different categories:

| Category | Size | Interpretation |
|---|---:|---|
| Locked source artifacts | 19.37 GB / 18.04 GiB | Compressed JP2/GeoTIFF products and sidecars |
| Decoded raster cache | 46.07 GB / 42.90 GiB | Expected raw-sample expansion; avoidable as an all-resident prerequisite |
| Planned logical channels | 68.54 GB / 63.84 GiB | Planner estimate for the 481,225-tile v1 product before Zstandard compression |
| Level-8 binary64 cores | 183.53 GB / 170.92 GiB | Avoidable transient whole-level representation |
| Level-8 pre-encode staging floor | 203.75–208.02 GB | Avoidable cores, auxiliaries, and retained ancestor qcores |
| Level-8 near-completion staging floor | 249.65–253.91 GB | Avoidable prior footprint plus current qcores; encoded artifacts excluded |

The final logical product is expected to be larger than its compressed inputs:
it resamples sources into QSC tiles, stores ancestors and multiple target levels,
duplicates shared borders into serialized aprons, and adds provenance and quality
channels. Zstandard should reduce the published pack size, but no completed scale
pack set exists from which to measure the final ratio.

The 68.54 GB channel value is deliberately described as a planner estimate. The
estimator budgets provenance and quality from the total configured source count,
whereas an individual tile can contain fewer contributors or no nonzero quality
map. It is suitable for showing the validation-memory order of magnitude, not as
a prediction of exact published logical bytes.

The binary64 level staging is different. It duplicates intermediate values at
tile granularity, preserves all tiles until a level barrier, then adds quantized
and encoded representations without retiring the originals. It is the immediate
cause of the observed disk usage and is not required by the scientific or byte
format contracts.

### Why apparent progress can flatten

Level 8 contains 72% of all planned tiles. Its preparation phase creates every
core and auxiliary before encoding starts. Its encoding phase then performs
large overlapping reads, CPU-heavy repeated finalization, per-tile flushes, and
serialized SQLite updates while the full preparation set remains live. A count
near 350,000 therefore does not imply that most total work or storage has been
retired. It can mark entry into the most expensive tail of the dominant level.

The operating system must also balance a roughly 42.90 GiB resident decoded
source set against per-worker coordinates, optional sample grids, fused grids,
neighborhood cores, compression buffers, and filesystem cache on an approximately
64 GiB host. Paging can turn otherwise sequential work into low-throughput I/O.
Because the run did not record page faults, active time, queue depth, or phase
progress, paging remains a plausible inference rather than a measured cause.

### Architecture boundary for a correction

The architecture's determinism rules constrain the solution but do not require
the current storage design. A corrected implementation must still:

- preserve fixed floating-point operation order and prohibit fast-math;
- copy the lowest-key owner's raw binary64 edge before quantization;
- construct ordinary aprons from the same-level neighbor's quantized interior;
- use explicit QSC topology at face boundaries and corners;
- derive geometric error from the nearest materialized ancestor;
- emit tiles and packs in canonical key order; and
- preserve v1 dependency, content, serialization, hash, and compression behavior.

A spatial window, stripe, or chunk can satisfy these rules by retaining a bounded
frontier of unresolved raw edges and quantized neighbor rows. It does not need to
retain every 257×257 binary64 core. The hierarchy plan also reveals which tiles
will be parents of future materialized nodes, allowing unrelated quantized cores
to be retired immediately after their output and neighbor obligations are met.

### How small-workload assumptions accumulated

The scale behavior is the interaction of several earlier, locally reasonable
choices rather than one isolated M8 regression:

1. Early container work introduced `read_all_tiles` when validation inputs were
   only a few synthetic or prototype tiles.
2. M4 established the scientifically necessary per-tile binary64 staging and
   raw-edge ownership rule, but did not require level-wide retention.
3. M6 added sparse hierarchy, SQLite reuse, bounded task dispatch, and canonical
   repacking. Its generated hierarchies were small enough that per-tile cache
   transactions and complete artifact collections remained inexpensive.
4. M7 made full validation and a clean-plus-incremental benchmark standard, but
   its committed records cover six synthetic tiles and one SLDEM tile.
5. Commit `64290c6` added the four real products and the 481,225-tile scale plan
   while the build still selected prototype tiles.
6. The current M8 working changes scaled that prototype model by spilling an
   entire level to per-tile files and adding an all-resident decoded accelerator.
   The 30-tile equivalence test proves the new path's correctness but not its
   resource bound.

This history explains why the correctness gates remained green: each milestone
tested its new invariant, but no gate modeled the simultaneous level width,
source corpus, file count, validation corpus, and repeated benchmark lifecycle of
the scale configuration.

## Options for future action

### Priority 0 — Make the next run observable and interruptible

Add a versioned event/checkpoint stream written atomically throughout scan,
plan, build, validation, incremental publication, inspection, and export. Record:

- phase, face, level, chunk, planned/completed/built/reused tiles, and rate;
- active, runnable, and blocked worker counts plus queue depth;
- process RSS, commit size, page faults, decoded-cache residency, and cache
  hit/miss/eviction counts;
- cumulative bytes read, written, flushed, deleted, and packed;
- live and high-water staging bytes and file count;
- sampling, projection, fusion, seam, quantization, encoding, SQLite, packing,
  and validation time; and
- the last completed transactional checkpoint and cancellation reason.

Split scan and plan timing. Count actual requested halo/source samples rather
than treating only core vertices as sampling work. Keep live staging size,
cumulative staging I/O, and staging high-water as separate metrics. Ensure a
failed or cancelled command emits a structured `Error` and a partial benchmark
record.

The qualification driver should either pass its scan/plan results into the
benchmark workflow or let the benchmark own those phases, not execute both
copies. The first instrumented run should be a bounded representative level-8
region, followed by a cancellation/restart exercise, before attempting the full
scale product.

### Priority 1 — Remove the scale blockers

1. **Separate metadata and sampling lifetimes.** Open source descriptors for
   catalog and hierarchy planning without materializing decoded pixels. `scan`
   may read a small explicit spot-check window; `plan` should perform no raster
   sample read, matching the architecture's coverage-index intent.
2. **Use a memory-budgeted decoded block cache.** Key chunks by source artifact,
   band/type, and normalized source-native block coordinates. Validate each
   chunk, share immutable blocks, and evict them through a deterministic-capacity
   LRU or equivalent bounded policy. Keep the budget machine-local and outside
   semantic identity. A memory mapping of the current full cache is a useful
   interim experiment, but does not provide a dependable resident-set bound
   under multi-source random access.
3. **Build spatial work windows.** Process canonical face/level regions with a
   bounded halo and raw-edge frontier. Resolve edge ownership once, quantize each
   tile once, retain only neighbor rows needed for aprons, and preserve raw or
   quantized parent cores only for planned descendants. Cross-face edges and
   corners must use the existing QSC topology mapping.
4. **Stream validation.** Validate structure from the tile index, then decode one
   tile and a bounded set of neighbors/parents at a time. Cache only the small
   validation frontier required for seams and hierarchy checks. Basic and full
   validation must not retain the product's decoded channel corpus.
5. **Batch disposable state.** Send cache metadata to one ordered writer and
   commit tile/chunk updates in bounded transactions. Checkpoint completed chunks
   atomically. Do not call `FlushFileBuffers` for each disposable auxiliary,
   qcore, or encoded artifact; retain durable synchronization for checkpoints
   and final `.ltdb`/`.ltp` publication.
6. **Stream canonical packing.** Feed completed canonical tile records to a
   bounded reorder buffer and pack writer. Avoid an individual encoded file plus
   a later reload when the tile is not needed as reusable work state. Shard or
   containerize retained cache data so directory entries scale by chunks rather
   than transient tile representations.

This priority should make peak RSS and transient scratch proportional to the
decoded-cache budget, active spatial window, necessary parent frontier, and a
bounded number of packs in flight—not to the number of tiles in level 8 or in the
database.

### Priority 2 — Remove serialized and repeated work

- Build a spatial index for physical mosaic members and assign coordinates to
  the relevant member before sampling. Do not scan a full coordinate batch
  against every SLDEM component.
- Transform geographic coordinates to source pixels once per source/work window
  and reuse the result for coverage, dependency, ordering, elevation, and quality
  sampling.
- Use one global bounded executor. Represent GDAL access with a bounded pool of
  per-worker or leased dataset handles rather than a global source lock or nested
  thread pools.
- Reuse overlapping projected and fused halo regions across adjacent tiles while
  retaining each tile's fixed row-major floating-point evaluation order.
- Replace optional-heavy dense grids with explicit value and validity storage
  where profiling confirms allocation or traversal cost.
- Fast-path sources that are entirely valid over a work window so fusion does not
  calculate an unused boundary-distance field.
- Load a target auxiliary once, share immutable parent cores among sibling work,
  and remove block-order sorts when the bounded decoded representation already
  provides locality.

These are private Builder changes under hosted C++20. They should preserve the
existing `Result<T>`/`Error` contract, explicit ownership, cancellation through
`std::stop_token`, deterministic ordering, and the Core dependency boundary.

### Priority 3 — Make incremental publication proportional to change

Introduce an internal scientific-work key that contains only the source members,
windows, algorithms, and parameters relevant to a tile's expensive sampling and
fusion work. It is local cache metadata and does not replace the dependency hash
published by format v1. This allows unchanged content outside a refinement to be
reused even when a v1-wide semantic configuration change requires a new published
dependency field and database identity.

Reuse unchanged canonical packs directly when their ordered tile membership,
payloads, and pack identity are unchanged. Do not write a duplicate temporary
pack merely to discover that the content-addressed final pack already exists.
Track tile reuse, scientific-work reuse, pack reuse, and bytes avoided as separate
metrics.

### Priority 4 — Tune concurrency and complete qualification

After Priorities 0–3, compare one, two, four, eight, sixteen, and—if memory and
I/O remain healthy—higher worker counts on the same bounded workload. Choose the
default from phase throughput, CPU time, device throughput, page faults, cache
hit rate, and SQLite wait time rather than logical processor count alone.

The acceptance run remains the 481,225-tile release scale profile on the current
approximately 64 GiB/32-logical-processor Windows host, with eight workers as the
baseline. It must complete clean build, streamed full validation, unchanged
incremental reconstruction, inspection, and export while continuously recording
resource high-water and progress. No wall-time target should be fixed until the
instrumented bounded path establishes a credible baseline.

### SLDEM2015 full-set consolidation

Retire the provisioning `Artifact`, `Fullset`, and northern-boundary modes in
favor of one clearly named SLDEM2015 profile whose only canonical bundle contains
all 32 JP2 tiles and 64 sidecars in locked order. Migrate Profiles A–C, scale, the
single-source M3 acceptance path, provisioning tests, lock verification, and
documentation to that full-set identity. Remove subset bundle definitions and
tests once no supported path references them.

This migration changes semantic configuration and artifact-bundle identities for
the former subset configurations, even though it does not revise the v1 byte
layout. Update the M3 and M8 acceptance documents explicitly and regenerate only
the non-format expectations authorized by that revision. Frozen `.ltdb`/`.ltp`
golden fixtures remain unchanged unless a separate format revision is approved.

### Neutral alternatives for the dependency-hash conflict

The following choices resolve different product requirements; this report does
not prefer one.

#### Alternative A — Correct the M8 acceptance gate and preserve v1

Keep the published v1 dependency algorithm. Revise Stage 4 to require that
scientific content and provenance changes are confined to the refinement's
actual influence, while acknowledging that the published dependency field,
database identity, and package placement may change globally. Require exact
restoration convergence. Use the internal scientific-work key described above
to recover footprint-confined computation without misrepresenting v1 identity.

This is the smaller compatibility change. It preserves existing readers and
golden fixtures but gives up footprint-confined published dependency diffs for
v1 databases.

#### Alternative B — Define a versioned dependency schema

Design a new format version whose tile dependency identity includes only
per-tile-relevant semantic settings, sources, and windows. Specify how global
datum, projection, quantization, and algorithm versions remain mandatory while
irrelevant source declarations are excluded. Add new golden fixtures, reader
compatibility behavior, cache migration, diff semantics, and mixed-version
operator tests.

This preserves the original Stage 4 meaning but is a format and compatibility
project, not an M8 performance patch. It must not be implemented by silently
changing hashes while continuing to label the result format v1.

## Verification and acceptance for follow-up work

Before another full run, require the following focused evidence:

- byte-identical v1 output against existing golden fixtures and Profiles A–C for
  every performance-only change;
- seam, apron, corner, and geometric-error tests whose boundaries deliberately
  cross spatial chunks, stripes, worker assignments, and all cube faces;
- a dense generated hierarchy large enough to force multiple work windows and
  assert bounded live cores, qcores, encoded buffers, scratch bytes, and file
  count;
- cold, warm, eviction, concurrent-read, truncated-chunk, checksum-corruption,
  and memory-budget tests for the decoded block cache;
- streaming validation tests that detect structural, channel, hierarchy, and
  seam corruption while asserting a bounded decoded-tile frontier;
- batched SQLite and pack-publication tests covering cancellation before and
  after a checkpoint, restart, stale temporary cleanup, and exact final identity;
- benchmark-schema tests proving that progress and partial evidence survive
  cancellation or process failure and that reported counters measure their named
  work; and
- strict no-network provisioning verification for the single full SLDEM set,
  including the explicitly migrated M3 acceptance path.

The full scale gate then requires a successful clean release build, full streamed
validation, unchanged incremental build, and deterministic published identity on
the current host. Peak memory and transient scratch must be bounded by configured
work budgets rather than grow with total level size. Linux debug, release, and
sanitizer verification remains a separate platform gate when a Linux host is
available.

## Summary of disposition

- **Immediate root cause:** whole-level binary64 staging, followed by repeated
  neighborhood reads and finalization.
- **Next hard failure if the build completes:** all-at-once decoded validation.
- **Major memory and startup cost:** eager, repeated, all-resident decoded raster
  caches.
- **Likely utilization constraints:** per-file flushes, serialized autocommit
  SQLite work, flat-directory metadata, nested executors, and paging; telemetry
  is required to rank them empirically.
- **Recommended engineering sequence:** instrumentation, bounded source/cache and
  tile/validation lifetimes, batched/streamed persistence, sampling optimization,
  incremental pack reuse, then thread tuning and scale rerun.
- **Provisioning direction:** one full SLDEM2015 product identity, including an
  explicit M3 migration; subset modes are retired.
- **Dependency-hash disposition:** a genuine acceptance conflict with two valid
  policy paths, neither of which explains or blocks correction of the clean-run
  resource failures.
