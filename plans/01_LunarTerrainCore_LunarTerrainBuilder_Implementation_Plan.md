# LunarTerrainCore and LunarTerrainBuilder Implementation Plan

## Summary

Implement the complete engine-independent terrain pipeline through deterministic `.ltdb/.ltp` creation and validation. Work proceeds through synthetic terrain, SLDEM2015 ingestion, seam/apron handling, heterogeneous fusion, sparse hierarchy, incremental builds, and scale validation. Unreal modules and Unreal-specific representations remain out of scope.

Success requires:

- A reusable C++20 `LunarTerrainCore` source API with no Unreal, GDAL, PROJ, or SQLite dependency.
- A standalone `lunar-terrain` builder using GDAL/PROJ.
- Byte-complete v1 format documentation and golden vectors before container implementation.
- Reproducible, transactionally published `.ltdb/.ltp` outputs.
- Linux/GCC and Windows/MSVC build and test coverage.

## Foundation and Project Structure

### Planning and repository guidance

- Create root `AGENTS.md` first. It will define:
  - Architecture and format specs as authoritative.
  - The Core/Builder-only scope boundary.
  - C++20, target-based CMake, error-handling, ownership, determinism, and serialization rules.
  - Standard configure/build/test commands.
  - Requirements to preserve user changes, avoid native-struct serialization, prohibit fast-math, and keep generated outputs out of source control.
  - The obligation to update the durable workpack when milestone state changes.
- Create one minimal workpack at `.agents/workpacks/lunar-terrain-core-builder/PLAN.md`. It owns stable milestone IDs, current milestone, blockers, next action, and acceptance links; it must not duplicate the architecture spec or become an activity log.
- Add a byte-complete `specs/03_LunarTerrainDatabase_Format_v1.md` before P0:
  - Resolve chunk framing, provenance structures, `META` canonicalization, padding/alignment, bounds, codec parameters, hash coverage, and compatibility behavior.
  - Define pack hashing with the pack-header hash field zeroed during calculation.
  - Define database-content identity over canonical chunks and ordered full pack hashes without a self-referential header hash.
  - Define deterministic configuration encoding and distinguish semantic/package settings from machine-local paths, job counts, and staging locations.
  - Treat multifile DEM products as ordered artifact bundles; store the bundle byte total and SHA-256 while listing member hashes in `META`.
  - Correct QSC lattice numerator calculation to signed arithmetic, avoiding unsigned underflow for negative `u/v`.
  - Publish hand-authored and generated golden byte vectors for every v1 record and channel.

### Build system

- Require CMake 3.25 and C++20. Preset schema 6 supplies shared configure/build/test workflows at this floor. [`FILE_SET` is already available from CMake 3.23](https://cmake.org/cmake/help/v3.23/command/target_sources.html), while [workflow presets require CMake 3.25](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html).
- Use a committed vcpkg manifest with a builtin baseline. Configure it through `VCPKG_ROOT`/`CMakeUserPresets.json`, not machine-specific paths committed to the repo, following the [official vcpkg CMake integration](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/cmake-integration) and [manifest versioning guidance](https://learn.microsoft.com/en-us/vcpkg/concepts/manifest-mode).
- Create these targets:
  - `lunar_terrain_core` / `LunarTerrain::Core`: public Core headers and implementation.
  - `lunar_terrain_builder_lib`: testable Builder pipeline, private to this repository.
  - `lunar-terrain`: thin CLI executable.
  - Separate Core unit, Builder unit, and CLI/integration test targets registered with CTest.
- Use explicit source lists, target-scoped warnings/options, public header file sets, and imported dependency targets. Do not add install/export/package-config support yet.
- Pin Zstd and OpenSSL for Core; GDAL, PROJ, SQLite, toml++, CLI11, and fmt for Builder; Catch2 for tests. Implement CRC32C internally with portable deterministic test vectors.
- Provide generator-neutral debug/release workflows plus a Linux ASan/UBSan workflow. CI covers GCC on Linux and MSVC on Windows; MSVC receives UTF-8 and precise-floating-point options. No target enables fast-math.

## Implementation Changes and Milestones

### M0 — Core contracts and format lock

- Establish namespace `lunar::terrain` and a project-owned `Result<T>`/`Error` API. Recoverable public Core failures return results rather than throwing; errors carry a stable code and relevant file offset, path, TileKey, or channel context.
- Define value types for `LunarTileKey`, geodetic coordinates, dataset/pack IDs, SHA-256 digests, format enums, database headers, index entries, decoded channels, and provenance.
- Make `LunarTileKey` construction validating-only; support parse/format, parent, children, ordering, Morton access, hashing, and explicit rejection of unused Morton bits.
- Freeze wire-format fixtures before implementing readers or writers.

Acceptance: all format tables reconcile to their declared byte sizes, golden vectors are reviewed, and invalid-key/error contracts are explicit.

### M1 — LunarTerrainCore projection and reader foundation

- Implement QSC forward/inverse projection and cross-face topology in Core without calling PROJ. Builder tests use PROJ only as the reference oracle.
- Keep projection input/output in binary64; define longitude normalization, pole behavior, face-edge ownership, edge orientation/reversal, and cube-corner ownership.
- Implement explicit little-endian byte readers, checked offset/length arithmetic, CRC32C, SHA-256, Delta2D U16 reversal, Zstd decompression, and format-version validation.
- Implement move-only `LunarTerrainDatabase` with:
  - `Open(path) -> Result<Database>`
  - `Header()`
  - `FindTile(TileKey) -> optional<TileIndexEntry>`
  - `ReadTile(TileKey) -> Result<DecodedTerrainTile>`
  - Child enumeration and sparse regional/leaf queries.
- Return index records by value and decoded tiles as owning objects; borrowed `span` views may not outlive their owning tile.

Acceptance: TileKey, projection, topology, predictor, byte-I/O, corruption, and golden-format tests pass on Linux and Windows.

### M2 — Synthetic P0 end-to-end container

- Add typed TOML configuration parsing and validation. Canonicalize semantic settings into fixed-order bytes; exclude local source paths, thread counts, and cache directories from scientific identity.
- Add a synthetic sampler and the initial tile planning/build stages.
- Implement deterministic ELEV quantization, channel encoding, pack writing, `DSET/PACK/TIDX/STRS/META` assembly, and manifest writing.
- Write output to sibling temporary files, fsync/close them, rename packs first, and publish `.ltdb` last. A failed build must leave the previous database usable.
- Pack in Face/Level/Morton order with deterministic sequential Pack IDs. Defaults are 1 GiB rollover, Zstd level 3, no dictionary, and one independently compressed channel frame per tile.
- Deliver initial `scan`, `plan`, `build`, `validate`, and `inspect` commands with text output and `--json` for automation.

Acceptance: six complete synthetic QSC faces round-trip through writer and Core reader without cracks; two empty-cache builds in the pinned environment produce identical database and pack hashes.

### M3 — SLDEM2015 P1 ingestion

- Implement `IRasterSource`, GDAL-backed raster access, cataloging, datum/elevation normalization, no-data policy, metadata overrides, footprint extraction, and a coverage index.
- Require stable dataset key and source URI separately from the local path. Hash the exact external SLDEM2015 subset and all required sidecars as one deterministic artifact bundle.
- Validate that the active GDAL build exposes the needed JP2/OpenJPEG capability before accepting a JP2 source.
- Convert radius-valued products to elevation relative to 1,737,400 m only through explicit configuration/metadata rules; silent datum or no-data assumptions are errors.
- Keep generated GDAL rasters in ordinary CI. Run the hash-pinned SLDEM2015 subset as an opt-in acceptance preset because the source data remains outside git.

Acceptance: the pinned subset builds into `.ltdb/.ltp`, reconstructs through Core, and passes measured elevation, coverage, provenance, source-hash, and quantization spot checks.

### M4 — P2 seams, corners, quantization, and aprons

- Stage each 257×257 binary64 core independently before quantization.
- Build a deterministic adjacency graph. For every shared edge/corner, the lowest TileKey owns the values; collect patches in sorted order and apply once per receiving tile to avoid parallel write races.
- Quantize only after edge resolution. Build the 259×259 serialized elevation channel afterward.
- Source ordinary aprons from the neighboring quantized core. At absent sparse neighbors, sample a virtual strip at target spacing. At cube boundaries, use explicit topology mappings rather than extending face coordinates.
- Persist staged artifacts atomically under `.ltbuild/staging`; they remain disposable and versioned by dependency hash.

Acceptance: equatorial edges, polar edges, reversed edges, all cube corners, sparse boundaries, and aprons compare byte-for-byte where required.

### M5 — P3 fusion, provenance, and quality

- Implement deterministic source ordering by priority followed by DatasetID.
- Support `Replace`, `BiasCorrectedReplace`, and `ResidualRefinement_v1`; use residual refinement as the configured default for regional refinement sources.
- Lock the initial residual filter as a separable 5-tap binomial kernel `[1,4,6,4,1]/16`, repeated deterministically according to the coarse/fine resolution ratio. Use normalized valid taps near no-data and fall back to the coarse source when fewer than half the kernel weights are valid.
- Use a fixed smoothstep transition over 32 target samples from the valid fine-data boundary. Coefficients, pass-count rule, source order, and no-data behavior are part of the algorithm version and dependency hash.
- Generate a deterministically sorted provenance palette, optional 64×64 dominant-source map, and optional 64×64 quality mask. Primary DatasetID must appear in the palette.
- Add provenance and quality visualization exports plus derivative diagnostics across transitions.

Acceptance: overlapping coarse/fine fixtures preserve broad coarse geometry, retain measurable fine residuals, produce valid provenance/quality data, and have continuous transition and byte-identical tile edges.

### M6 — Sparse hierarchy, geometric error, and incremental cache

- Plan the coarsest QSC level whose worst-case local spacing does not underrepresent each source’s effective resolution; recursively subdivide intersecting coverage and materialize all ancestors.
- Compute child masks from the final tile set and reject orphaned nodes.
- Compute geometric error against the nearest materialized ancestor using the frozen reconstruction rule.
- Implement `.ltbuild/cache.sqlite` with dataset hashes, TileKeys, dependency/content hashes, build state, staging artifact location, and previous packing data.
- Reuse tiles only when the complete dependency hash matches. Repack in canonical order so incremental and clean builds converge on identical outputs.
- Support cancellation and bounded parallel tile tasks, but keep floating-point reductions, seam ownership, registry construction, and packing order deterministic.

Acceptance: an L8/L10/L12 synthetic hierarchy is connected and sparse; unchanged rebuilds reuse staged tiles; changed source windows invalidate only dependent tiles; incremental and clean rebuilds are byte-identical.

### M7 — Complete operator tooling and scale gate

- Complete `validate --full`, `diff`, and `export`.
- Validation covers structure, hashes/CRCs, projection, scientific values, seams, provenance, hierarchy, and deterministic rebuilds.
- `diff` distinguishes dataset, dependency, uncompressed content, provenance, and package-layout changes.
- `export` supports PLY, OBJ, diagnostic raster, CSV, raw U16, provenance, and quality outputs without introducing Unreal formats.
- Benchmark representative regions for catalog time, sampling throughput, staging I/O, peak memory, compression ratio, pack count, validation time, and incremental reuse.
- Keep 1 GiB packs, Zstd level 3, 64×64 auxiliary masks, and the v1 fusion parameters unless benchmark evidence creates a separately versioned change; never silently alter v1 output semantics.

Acceptance: a complete configured build publishes validated `.ltdb/.ltp` outputs, survives corruption tests and interrupted builds, supports inspection/diff/export, and records reproducible benchmark results.

## Test Plan

- Unit tests:
  - TileKey boundaries through level 28, parsing, Morton bit order, parents/children, and invalid encodings.
  - Signed lattice coordinates, QSC round trips, face topology, reversed edges, poles, and corners.
  - Little-endian serialization, checked arithmetic, SHA-256, CRC32C, predictor wraparound, quantization limits, and Result/Error propagation.
- Format tests:
  - Golden byte fixtures for every record/channel.
  - Unknown compatible chunks, incompatible majors, truncation, overlap, overflow, invalid strings, corrupt compressed data, CRC mismatch, and hash mismatch.
- Builder tests:
  - Generated GDAL rasters for projected/geographic inputs, no-data, sidecars, radius/elevation conventions, overlapping coverage, and invalid metadata.
  - Sparse planning, seam ownership, aprons, fusion, provenance, geometric error, cache invalidation, and deterministic pack rollover.
- End-to-end tests:
  - Synthetic six-face clean rebuild comparison.
  - Hash-pinned external SLDEM2015 acceptance run.
  - Incremental-versus-clean equivalence.
  - Interrupted publication leaves the prior database readable.
  - Linux/GCC and Windows/MSVC each require deterministic repeated builds; cross-platform byte identity remains a measured target, not the initial release gate.

## Assumptions and Locked Defaults

- `LunarTerrainCore` is source-consumed with public headers but has no ABI, install, or package compatibility promise in this phase.
- Core is hosted C++20 with allocation, RTTI, and threading available, but recoverable public failures do not require exceptions.
- Core may depend on Zstd and OpenSSL; it must not depend on GDAL, PROJ, SQLite, TOML/CLI libraries, or Unreal.
- `.ltbuild` is disposable; `.ltdb/.ltp` alone contain everything required to interpret the canonical database.
- The P1 real-data artifact is an externally stored, SHA-256-pinned SLDEM2015 subset.
- No Unreal Editor, Runtime, Mesh Terrain, World Partition, PCG, cooking, or engine asset work is included.
