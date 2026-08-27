# M8 Lunar DEM Product Qualification Spike

## Purpose and authority

M8 is a post-M7 qualification spike for the complete `LunarTerrainCore` and standalone `LunarTerrainBuilder` pipeline. M0–M7 establish implementation correctness through frozen format vectors, controlled generated rasters, synthetic hierarchies, and the pinned single-tile SLDEM2015 acceptance artifact. M8 supplies the separate evidence that the completed pipeline can ingest, fuse, attribute, validate, and benchmark multiple heterogeneous real lunar DEM products.

The milestone summary and acceptance in [`01_LunarTerrainCore_LunarTerrainBuilder_Implementation_Plan.md`](01_LunarTerrainCore_LunarTerrainBuilder_Implementation_Plan.md) control M8 scope. The architecture and scientific intent remain owned by [`01_LunarTerrainBuilder_Architecture_v0.3.md`](../specs/01_LunarTerrainBuilder_Architecture_v0.3.md), and the byte-level v1 contract remains owned by [`03_LunarTerrainDatabase_Format_v1.md`](../specs/03_LunarTerrainDatabase_Format_v1.md). This spike does not revise the frozen format or weaken any M0–M7 gate.

M8 begins only after M7 acceptance is complete. Its required qualification profiles use hash-pinned NASA/PDS-first products over representative mid-latitude, SLDEM-coverage-boundary, and polar regions. A larger opt-in scale profile then exercises the same pipeline with the complete 32-tile SLDEM2015 set, a global coverage source, and selected regional refinements.

The portable acquisition foundation is already present before M8. [`provisioning/provision.py`](../provisioning/provision.py) is the single Python 3.9+ standard-library entry point for Windows and Linux, and [`provisioning/provisioning.json`](../provisioning/provisioning.json) is its schema-versioned product, bundle, and profile lock. The current `Artifact` and `Fullset` profiles preserve the established SLDEM2015 behavior and pinned three-member artifact identity. This completed tooling refactor does not begin M8; M8 extends the existing entry point and lock only after each additional product passes preflight.

## Intended result

M8 produces an auditable qualification record for a bounded real-product stack:

- exact external artifact bundles can be acquired, verified, and reopened without committing source data;
- source metadata, datum, projection, no-data, effective resolution, lineage, and quality semantics are explicit;
- real source coverage creates the expected connected sparse scientific hierarchy;
- configured fusion policies preserve intended source roles and produce traceable provenance and quality;
- clean, repeated, reordered-source, and incremental builds remain deterministic within each supported toolchain;
- the complete `scan` through `export` operator path works on the resulting databases;
- scientific diagnostics and the M7 operational metrics are recorded for the required regions and the larger scale profile; and
- every investigated product is classified as qualified, qualified with explicit limitations, or rejected with evidence.

Qualification does not require every candidate product to succeed. It requires a reproducible accepted source set and explicit rejection or deferral of unsuitable candidates.

## End-to-end qualification flow

One M8 run follows this path:

1. A developer runs a named product/bundle provisioning profile for each required source and supplies that product's external root through `--root` or its configured environment variable. These acquisition profiles are distinct from the multi-source Builder Profiles A–C and scale profile below.
2. `provision.py` loads the adjacent JSON lock by default, or an explicitly supplied `--config` file, then downloads or reuses the exact declared members, resumes through temporary `.part` files, verifies upstream checksums where available, computes project SHA-256 values and declared canonical artifact-bundle identities, and makes verified source members read-only. `--verify-only` performs the same identity checks without any network request.
3. A preflight uses the same GDAL/PROJ-capable Builder environment that will perform the build to validate raster access, projection/frame, bounds, dimensions, sample representation, no-data, and required sidecars or quality companions.
4. `scan` and `plan` construct the ordered source registry, coverage index, source-dependent target levels, and connected sparse tile set. Advertised pixel spacing is evidence, not a substitute for effective-resolution qualification.
5. `build` samples and fuses sources in configured priority/DatasetID order, resolves seams, quantizes, generates aprons and auxiliary channels, uses the incremental cache, and transactionally publishes `.ltdb/.ltp` outputs.
6. `validate --full`, `inspect`, `diff`, and diagnostic `export` verify structure, science, source transitions, hierarchy, provenance, quality, and reproducibility.
7. The benchmark harness records the M7 metrics and the qualification report records the product status, chosen policy, actual hierarchy, scientific findings, limitations, and any required follow-up.

Acquisition remains separate from terrain builds. A build never downloads a source, accepts an unverified revision, or treats a generated alias, clipped derivative, checksum manifest, directory metadata, or temporary file as an original source member unless the dataset definition explicitly makes it part of a new derived dataset.

## Scope and boundaries

### In scope

- Extend the portable `provision.py` and `provisioning.json` contract with locked LOLA, SLDEM2015, LROC NAC, and polar product bundles, then verify the same entry point on Windows and Linux.
- Add external-data configurations and opt-in acceptance/benchmark workflows for the required regions and scale profile.
- Exercise existing M0–M7 capabilities with multiple real sources, projections, resolutions, footprints, no-data patterns, and quality companions.
- Make scoped Core, Builder, test, and provisioning corrections needed to honor existing architecture, format, fusion, hierarchy, validation, and tooling contracts.
- Select and record dataset-specific v1 fusion policies and quality mappings through semantic configuration.
- Produce qualification and benchmark evidence without committing source rasters, terrain databases, packs, caches, or staging output.

### Out of scope

- Replacing golden vectors, generated-raster tests, synthetic hierarchy tests, or the pinned M3 SLDEM2015 run.
- Changing the frozen v1 `.ltdb/.ltp` contract or existing v1 fusion semantics without separately approved versioning work.
- Adding Unreal, Mesh Terrain, editor/runtime, World Partition, PCG, cook, or streaming implementation.
- Building a general remote-data service, mirroring mission archives, or promising support for every lunar elevation product.
- Treating image pixel spacing, output grid spacing, or an interpolated grid as measured effective terrain resolution without supporting metadata or diagnostics.
- Performing undocumented horizontal or vertical coregistration that cannot be represented in source metadata and semantic configuration.

If a required product exposes a missing capability that cannot be added without changing the v1 format or scientific algorithm semantics, stop that product's qualification, record the evidence, and raise a separately approved revision. Do not silently reinterpret the product or alter v1 output.

## Source roles and starting policy hypotheses

The labels below describe the M8 source stack; they do not introduce new public format enums.

### Coverage foundation

A global, lower-priority LOLA GDRDEM source supplies terrain where no preferred or regional source is available. `LDEM_256` is the initial candidate because its four global tiles provide a 118.45 m grid suitable for the approximate L7 foundation anticipated by the architecture. LOLA GDRDEM pixels are binned and interpolated altimetry, so source metadata and any density/quality evidence must constrain the declared effective resolution. See the [official LOLA GDRDEM description](https://ode.rsl.wustl.edu/moon/pagehelp/Content/Missions_Instruments/LRO/LOLA/GDR/GDRDEM.htm).

### Preferred backbone

SLDEM2015 512 ppd is the preferred backbone within its official ±60° latitude coverage. It combines SELENE Terrain Camera DEMs with LOLA control, has about 60 m effective resolution at the equator, and maps naturally to the approximate L8 band. It cannot be the sole global coverage source. See the [official SLDEM2015 description](https://ode.rsl.wustl.edu/moon/pagehelp/Content/Missions_Instruments/LRO/LOLA/SLDEM.htm).

Within ordinary SLDEM coverage, SLDEM should normally become the tile's `PrimaryDatasetID` when no finer qualified product dominates. This is distinct from the global coverage-foundation role and from the per-tile `PrimaryDatasetID` chosen after fusion.

The initial LOLA-to-SLDEM policy hypothesis is `Replace`, with `BiasCorrectedReplace` evaluated at the coverage boundary. `ResidualRefinement_v1` is not the default hypothesis for this pairing because SLDEM already incorporates LOLA and is intended to supply its own broad geometry. M8 selects the policy from measured transition and reconstruction evidence without changing the M5 defaults for regional refinement sources.

### Regional and polar refinements

Higher-resolution LROC NAC and polar LOLA products are sparse refinements. `ResidualRefinement_v1` is the initial SLDEM-to-NAC policy because it preserves the backbone's broad geometry while retaining measured high-frequency structure. `BiasCorrectedReplace` must also be evaluated when published registration, offset, or error metadata indicates that the refinement's low-frequency geometry should supersede the backbone.

For a LOLA foundation refined by a denser LOLA polar grid, begin with `Replace` or `BiasCorrectedReplace`; use residual refinement only if effective-resolution and derivative evidence shows that the finer grid contributes measured high-frequency terrain rather than interpolation artifacts.

Every selected policy, priority, source order, declared metadata override, and quality mapping remains part of canonical semantic configuration and dependency identity. A source revision receives a new stable key/ID as required by the architecture.

## Candidate product set

### Required qualification products

| Product | Purpose | Expected hierarchy | Required members and checks |
|---|---|---:|---|
| LOLA GDRDEM `LDEM_256` | Global coverage foundation and non-SLDEM fallback | Approximately L7 | Exact elevation tiles and detached labels; scaling/offset, radius/elevation meaning, global bounds, no-data/interpolation behavior, and official checksums must be pinned. |
| SLDEM2015 512 ppd | Preferred ±60° backbone | Approximately L8 | Reuse the pinned `0–30°N, 0–45°E` artifact for the mid-latitude case; add exact northern-boundary members for the ±60° case; retain the existing full 32-tile provisioning mode for scale. Include the official data-quality product only after its alignment and semantics are qualified. |
| LROC NAC `MASKELYNE` DTM | First real high-resolution regional refinement | Approximately L12 | Pin the elevation DTM, confidence map, label/readme, and only the sidecars required to interpret them. Verify the published 5 m grid, 3.26–5.16°N by 33.53–33.92°E footprint, datum, no-data, confidence semantics, and error metadata. See the [official Maskelyne product](https://data.lroc.im-ldi.com/lroc/view_rdr/NAC_DTM_MASKELYNE). |
| NASA GSFC south-polar LOLA adjusted DEM | Polar projection, coverage, effective resolution, and quality | Approximately L10 for the 20 m candidate, subject to effective resolution | Begin with the `80S` adjusted elevation, count, effective-resolution, and elevation-error GeoTIFFs; exclude hillshade, slope, and roughness products unless a later test requires them. Pin exact revisions and hashes during provisioning qualification. See the [official product listing](https://pgda.gsfc.nasa.gov/products/90). |

Expected levels are validation hypotheses. The M6 rule remains authoritative: choose the coarsest QSC level whose worst-case local spacing does not underrepresent the source's qualified effective resolution, then materialize only intersecting coverage and required ancestors. The qualification report records planned and actual levels and explains any difference.

### Scale-profile products

The scale profile uses:

- all 32 SLDEM2015 512-ppd elevation tiles and their 64 official sidecars already selected by the `Fullset` profile in `provisioning.json`;
- the qualified global LOLA coverage foundation;
- the qualified Maskelyne refinement; and
- the qualified south-polar elevation and quality bundle.

The scale profile is opt-in because its source bytes, staging, output, cache, and runtime are unsuitable for ordinary CI. It must complete at least once for M8 and remain reproducible, but it is not an every-change regression gate.

### Secondary NASA/PDS candidates

- **LROC GLD100:** evaluate as an alternative approximately 100 m L7 coverage bridge across the SLDEM ±60° boundary and toward the poles. Its value is coverage and independent camera-derived behavior, not increased resolution inside SLDEM coverage. See an [official 100 m polar product](https://data.lroc.im-ldi.com/lroc/view_rdr_product/WAC_GLD100_P900N0000_100M).
- **Additional LROC NAC DTMs:** use the [official footprint and error catalog](https://data.lroc.im-ldi.com/lroc/view_rdr/SHAPEFILE_NAC_DTMS) to select one 2–3 m product only after the required Maskelyne case passes. It may exercise L13, different no-data geometry, or a different error/registration profile.
- **High-resolution LOLA landing-site DEMs:** use as a quality stress case rather than assuming their output grid spacing is effective measured resolution. NASA reports that some 5 m products are highly interpolated; measurement density and uncertainty companions control qualification. See the [NASA GSFC high-resolution LOLA documentation](https://pgda.gsfc.nasa.gov/products/78).

Secondary candidates do not gate M8 unless explicitly promoted after the required profile is operational.

### Deferred multi-agency candidates

- **CE2TMap2015 20 m DEM:** potentially supplies broad L10-class coverage, but acquisition conditions, packaging, datum, licensing, and reported coregistration behavior require a separate preflight. Its persistent dataset reference is documented by the [CNSA-backed data citation](https://doi.org/10.12350/CLPDS.GRAS.CE2.DEM-20m.vA). It is not an M8 dependency.
- **JAXA SLDEM2013:** useful for lineage and comparative validation, but it contributes to the SLDEM2015 source family and is not assumed to be an independent refinement. See the [JAXA DARTS SLNDEM catalog](https://darts.isas.jaxa.jp/en/app/about/kaguya-seldem-app).

These candidates may receive a feasibility note, but automated acquisition or Builder support is follow-up work unless separately authorized.

## Provisioning tooling

Acquisition remains outside `lunar-terrain` and uses the existing [`provision.py`](../provisioning/provision.py) entry point on both platforms. Do not add PowerShell/Bash equivalents or product-specific wrappers. The script uses only the Python 3.9 standard library; `provisioning.json` is the one authoritative acquisition lock, so platform behavior and product identities cannot drift.

The schema-version-1 baseline provides:

- configurable retry count, timeout, streaming chunk size, and user agent;
- products with a stable dataset key, archive revision, provenance URI, download base URL, and external-root environment variable;
- optional member-specific source URLs, byte counts, MD5/SHA-256 values, and roles for elevation, labels, metadata, and quality companions;
- upstream PDS-style MD5 manifests, with an optional pinned manifest SHA-256;
- ordered bundles with a declared byte total, optional upstream manifest, canonical hash domain and SHA-256, and optional generated SHA-256 manifest; and
- case-insensitive named profiles that select one product's download bundle and the bundle identities that must be verified.

Provisioning profiles remain product/bundle acquisition units. A required-region or scale workflow invokes the necessary acquisition profiles separately, with one external root per invocation, then combines those roots through the Builder semantic configurations. Do not turn `provisioning.json` into Builder orchestration or duplicate scientific fusion policy there.

Each concern has one defining location:

- `provisioning.json` owns acquisition identity and behavior: dataset key, archive revision, provenance/download location, external-root environment name, ordered original members and roles, byte counts, published and project checksums, bundle totals and canonical identities, checksum-manifest handling, and provisioning-profile membership;
- Builder semantic configuration owns the source metadata needed for canonical builds and publication: product/producer/mission/instrument/version/license strings, GDAL driver/capability expectations, raster type, dimensions, bands, projection/frame, bounds, datum/reference radius, sample scale/offset, no-data interpretation, effective resolution, priorities, and fusion/quality policy; and
- the qualification record owns cited licensing/data-policy evidence, access constraints, authoritative metadata evidence, spot checks, limitations, and accepted or rejected status.

Builder semantic configuration also carries the stable key, provenance URI, ordered artifact members, byte counts, SHA-256 values, and bundle identity required by the Builder's canonical input contract. Derive those repeated values from the acquisition lock or verify their exact agreement with a focused consistency check; do not maintain an unchecked second product identity. An archive revision and a published scientific product version may use different identifiers, but both must be explicit and supported by the qualification evidence.

M8 may extend the versioned JSON schema and its validation only when a qualified required product needs an acquisition fact or archive behavior that schema version 1 cannot represent. Keep a compatible schema version when adding ordinary products, members, bundles, or profiles already supported by the current contract. A new manifest syntax or archive transport requires focused parser/schema tests; authentication, interactive acceptance, archive extraction, generated discovery, or another materially different lifecycle requires an explicit design decision before implementation.

All extensions preserve the established behavior:

- require an external root through `--root` or the product's configured environment variable, resolve it, and reject filesystem roots and configured paths that escape it;
- validate the full JSON configuration before accessing an external root, rejecting unknown fields, unsafe paths, duplicate members, unresolved references, unpinned members, and canonical identities inconsistent with configured member order, sizes, or SHA-256 values;
- stream new downloads through neighboring `.part` files, resume with HTTP range requests, reject a server that ignores or misstates the requested range, and atomically rename only a completed transfer;
- never overwrite or delete an existing final member; verify it in place and require explicit operator removal after a reported mismatch;
- reject missing members, size/checksum drift, undeclared or incomplete `.part` files, and unexpected archive revisions;
- compute project SHA-256 values even when the archive publishes another checksum and verify every declared canonical artifact-bundle identity used by the Builder;
- make verified original members read-only while keeping downloaded checksum manifests and generated SHA-256 manifests outside the original artifact bundle unless explicitly declared; and
- make `--verify-only` prohibit all network requests, including retrieval of a missing upstream checksum manifest.

The committed provisioning tests remain network-free and cover the locked SLDEM profile definitions, canonical member ordering, filesystem-root rejection, existing-file verification, corruption and incomplete-transfer rejection, PDS manifest parsing, and safe range resume. M8 extends this suite for each new schema behavior and product lock, then runs representative acquisition and verification with the same script on Windows and Linux.

If an archive requires credentials or session state, unstable generated URLs, interactive acceptance, or an unsupported lifecycle that has not been explicitly approved, classify it as deferred rather than embedding those mechanisms in the tooling.

## Required real-data profiles

### Profile A — Mid-latitude regional refinement

Use the existing pinned `sldem2015_512_00n_30n_000_045` bundle with the Maskelyne NAC DTM at 4.21°N, 33.73°E.

This profile must exercise:

- two real product families with different resolution, footprint, lineage, and quality metadata;
- L8 SLDEM coverage with an embedded approximately L12 sparse refinement and all required ancestors;
- `ResidualRefinement_v1` as the starting policy plus a controlled `Replace`/`BiasCorrectedReplace` comparison;
- valid and invalid/confidence-boundary behavior from the NAC companion map;
- transition derivatives, seam ownership, quantization, ordinary and virtual aprons;
- multi-entry provenance palettes, dominant-source mapping, contribution fractions, and primary-source selection; and
- source-quality mapping to the existing v1 `QUAL` bits without inventing new meanings.

The comparison determines the accepted Maskelyne policy. A policy is accepted only when the source reconstruction, transition, provenance, and quality evidence agree; visual plausibility alone is insufficient.

### Profile B — SLDEM coverage boundary

Use the global LOLA foundation with a selected SLDEM2015 `30–60°N` tile and build a region spanning below and above 60°N at a longitude away from an unrelated source edge.

This profile must exercise:

- global fallback where SLDEM is absent;
- the end of SLDEM's official coverage rather than an artificial no-data mask;
- `Replace` as the initial LOLA-to-SLDEM policy and a `BiasCorrectedReplace` comparison;
- continuity across the broad source transition and byte-identical final shared tile edges;
- correct `PrimaryDatasetID`, provenance, and quality on both sides and through the transition; and
- hierarchy behavior where L8 scientific leaves end and the L7 foundation remains.

GLD100 may be evaluated beside LOLA as a non-gating alternative after the required two-source boundary passes.

### Profile C — South-polar refinement

Use the global LOLA foundation with the selected `80S` adjusted polar elevation, count, effective-resolution, and error products.

This profile must exercise:

- polar stereographic ingestion and transformation through the same Builder GDAL/PROJ boundary;
- coverage through the pole and across QSC polar face edges/corners;
- distinction between 20 m output spacing, local measurement density, and effective resolution;
- lower-confidence/interpolated quality mapping supported by the v1 `QUAL` schema;
- source-dependent sparse level planning rather than a blanket L10 assignment; and
- `Replace`/`BiasCorrectedReplace` comparison without assuming residual refinement adds measured detail.

If the companion data show that a region is too interpolated to justify the nominal level, the correct result is a coarser declared effective resolution, a lower-confidence qualification, or source rejection—not an artificially fine scientific hierarchy.

## Work stages

### 1. Product preflight and lock

- Confirm the required product URLs, revision identifiers, licensing, official checksums, exact source members, byte totals, and expected storage before adding a product/bundle profile to `provisioning.json`.
- Inspect every product with the Builder-linked GDAL stack and reconcile labels/metadata with authoritative product documentation.
- Determine whether each archive fits the existing direct-HTTP, per-member checksum, and PDS-MD5-manifest contract. Raise any required schema or lifecycle decision before acquisition code changes.
- Define source lineage and starting priority/policy without treating related LOLA-derived products as independent measurements.
- Record exact spot-check coordinates and expected source values from the original products before fusion.

Result: each required product has reviewable acquisition-lock fields, Builder metadata expectations, and qualification evidence, or an explicit blocking qualification failure. Do not continue to provisioning or pipeline testing with an unresolved revision, datum, frame, no-data rule, archive lifecycle, or artifact identity.

### 2. Reproducible provisioning

- Add the preflight-approved products, members, bundles, and acquisition profiles to `provisioning.json`; extend `provision.py` only for approved archive behavior the existing schema cannot express.
- Reuse the existing SLDEM `Artifact` and `Fullset` profiles. Preserve the three-member artifact identity and the full-set member order, total-byte check, official PDS MD5 verification, and generated SHA-256 manifest.
- During SLDEM scale preflight, lock the remaining full-set per-member byte counts and SHA-256 values and declare its canonical bundle identity without changing the existing three-member artifact definition.
- Add network-free tests for every new schema rule and archive parser, then verify clean acquisition, interrupted/resumed acquisition, strict no-network reuse, corruption rejection, and upstream-revision rejection with the same CLI contract on Windows and Linux.
- Document only portable `--root`, configured environment-variable, `--config`, and `--verify-only` contracts; keep source bytes and machine-local locations out of git.

Result: another developer can use the same Python entry point and checked-in lock on Windows or Linux to provision byte-identical required inputs and receive the same artifact-bundle identities.

### 3. Qualification configurations and diagnostics

- Add real multi-source semantic configurations for Profiles A–C and a separate scale profile.
- Populate the Builder's repeated stable-key, provenance, artifact-member, byte-count, SHA-256, and bundle-identity fields from the accepted acquisition locks, and add a focused consistency check that prevents the provisioning and Builder identities from drifting.
- Preserve local paths, job counts, output, cache, and staging locations outside semantic identity as required by format v1.
- Map accepted source quality companions to existing v1 quality meanings and record unsupported source flags rather than silently dropping or redefining them.
- Add or extend diagnostic exports only where M7 tooling cannot expose the required elevation differences, derivatives, provenance, quality, effective level, or source-coverage evidence.

Result: `scan`, `plan`, and diagnostic commands explain the source registry, coverage, expected hierarchy, and selected policy before an expensive build begins.

### 4. Required-region pipeline verification

- Run Profiles A–C through `scan`, `plan`, `build`, `validate --full`, `inspect`, `diff`, and relevant exports.
- Build each profile twice from empty caches in the same pinned environment and compare database and pack hashes.
- Repeat with the source declarations reversed; canonical source ordering and outputs must remain unchanged.
- Remove and restore one refinement through configuration, then verify incremental invalidation and `diff` remain confined to its dependency footprint and that restoration converges with a clean build.
- Run the required profile on Windows/MSVC and Linux/GCC or Clang. Require deterministic repeats within each platform; record cross-platform hash comparison as a measured target under the existing project policy.

Result: all required real-product combinations satisfy structural and scientific validation, or the qualification report rejects the product/policy with the failing evidence.

### 5. Scale run and qualification report

- Run the opt-in scale profile at least once in a supported pinned environment after all regional profiles pass.
- Capture catalog time, sampling throughput, staging I/O, peak memory, compression ratio, pack count, clean and incremental build time, validation time, export time, and incremental reuse.
- Compare the scale results with the M7 representative-region baseline without changing v1 defaults merely to improve a benchmark.
- Produce the final product/policy matrix, source identities, hierarchy summary, scientific diagnostics, operational metrics, limitations, rejected candidates, and follow-up recommendations.
- Keep raw outputs and machine-local details outside source control; any committed summary omits local paths, job counts, cache locations, and staging locations.

Result: M8 has a reproducible evidence package and a bounded qualified source stack, while nonessential candidates remain explicitly deferred.

## Verification requirements

### Provisioning and source integrity

- Exact member counts, relative paths, byte counts, upstream checksums, SHA-256 values, bundle totals, and bundle hashes match the locked definitions.
- The same `provision.py` entry point and checked-in JSON lock reproduce each required identity on Windows and Linux; no platform-specific acquisition wrapper participates.
- `--verify-only` makes no network request. Missing members or required upstream checksum manifests fail instead of being fetched.
- Invalid configuration, missing sidecars, `.part` files, corrupted members, archive drift, wrong roots, unsafe root targets, and servers that do not honor resume ranges fail before Builder execution.
- Every Builder source definition agrees with its acquisition lock on stable key, provenance URI, ordered artifact members, byte counts, SHA-256 values, bundle total, and bundle hash.
- GDAL driver, raster type, dimensions, projection/frame, bounds, datum, scale/offset, and no-data match the declared source metadata.

### Scientific behavior

- Source-only regions reconstruct the selected input within declared sampling and 0.5 m quantization tolerances.
- Overlap regions preserve the accepted policy's broad geometry and measurable refinement detail.
- Transition diagnostics satisfy the continuity and derivative criteria established by M5; the seam resolver is not used to hide a poor fusion transition.
- No undeclared no-data or non-finite elevation enters staging or published output.
- Published source error, confidence, count, or effective-resolution information is reflected in qualification and in v1 quality flags where the schema supports it.
- A product whose registration, artifacts, or interpolation dominate the intended refinement is rejected or qualified at a coarser effective resolution.

### Hierarchy, provenance, and quality

- Planned levels follow qualified effective resolution and QSC spacing, not filename or grid-spacing assumptions.
- All refinement tiles have the required ancestors; child masks, sparse boundaries, geometric error, virtual aprons, and neighbor seams validate.
- Every output DatasetID resolves to a pinned source registry entry and exact artifact bundle.
- Provenance palettes are sorted and unique, contribution fractions are valid, dominant-source indices are in range, and each tile's primary source appears in its palette.
- Provenance and quality exports agree with source coverage, selected fusion policy, confidence/no-data boundaries, and transition regions.

### Determinism and operations

- Same-platform empty-cache rebuilds are byte-identical for every required profile.
- Reversing configuration source declaration order does not change output.
- Incremental and clean builds converge to identical output after the same source/configuration state is restored.
- Interrupted publication leaves the prior database usable.
- `validate --full`, `inspect`, `diff`, and required exports complete successfully and report the real multi-source distinctions introduced by the profiles.

### Benchmark reporting

- Metrics use the M7 definitions so regional and scale results are comparable.
- Dataset bundle hashes, semantic/package configuration hashes, Builder version, toolchain, and run type identify every reported measurement.
- Results distinguish acquisition verification, catalog/plan, clean build, unchanged incremental build, changed-source incremental build, validation, and export.
- Benchmark evidence may justify follow-up work, but no v1 codec, fusion, mask, hierarchy, or packaging semantic changes during M8 without explicit versioned approval.

## Deliverables

M8 implementation produces:

- extensions to the existing portable Python provisioning/verification entry point and JSON lock for the required NASA/PDS-first product bundles and acquisition profiles;
- locked acquisition definitions with exact artifact identities, paired Builder metadata expectations, and qualification evidence;
- opt-in required-region and scale configurations/workflows;
- scientific, provenance, quality, hierarchy, determinism, and operator-path acceptance coverage;
- a reproducible benchmark result for the required regions and at least one scale run; and
- a qualification report classifying every required and investigated source/policy combination.

Source rasters, `.ltdb`, `.ltp`, `.ltbuild`, generated aliases, clipped rasters, caches, staging data, and raw benchmark output remain external or ignored.

## M8 acceptance

M8 is complete when all of the following hold:

- M7 acceptance remains green and its controlled golden, generated-raster, synthetic, and single-source SLDEM tests remain unchanged except for compatible additions.
- The required LOLA foundation, SLDEM backbone, Maskelyne NAC refinement, and south-polar LOLA bundle are externally provisioned from locked definitions and reproduce their declared artifact identities through the same `provision.py` and checked-in JSON lock on Windows and Linux, including strict no-network verification-only reuse.
- Profiles A–C complete the full Builder/Core/operator flow and pass structural, scientific, seam, hierarchy, provenance, quality, publication, and deterministic-rebuild validation.
- Each required pairing has an evidence-backed configured fusion policy and declared effective-resolution treatment; no product is accepted solely from visual output or advertised pixel spacing.
- Same-platform clean repeats and reordered-source builds are byte-identical, and incremental builds converge with clean builds.
- The opt-in scale profile completes at least once and records the M7 operational metrics without silently changing v1 semantics.
- The final qualification report identifies accepted products and policies, limitations, rejected/deferred candidates, actual L7–L13 coverage, source hashes, configuration identities, and follow-up work.

Failure of a candidate does not fail M8 when the bounded required terrain roles still have a qualified product and the rejection is documented. Failure of a required role with no qualified replacement, any unresolved source identity/datum ambiguity, or any unapproved v1 semantic change blocks M8 completion.

## Assumptions and locked defaults

- M8 is a post-M7 spike; it does not begin while M5–M7 implementation or acceptance remains incomplete.
- The portable Python provisioning foundation and locked SLDEM `Artifact`/`Fullset` profiles predate M8 and do not start the spike. M8 extends them only after product preflight; it does not restore platform-specific entry points.
- The required gate is the three representative real-data profiles. The larger scale profile is opt-in and must be run once, but is not an ordinary CI gate.
- NASA/PDS and NASA GSFC products form the mandatory first source set. JAXA and CNSA candidates remain non-gating qualification opportunities.
- LOLA supplies global coverage, SLDEM2015 is the preferred ±60° backbone, and qualified NAC/polar products supply sparse refinements.
- Expected L7/L8/L10/L12/L13 mappings are hypotheses; qualified effective resolution and QSC distortion determine actual levels.
- Existing v1 format, hash domains, fusion algorithms, 32-sample transition, provenance/quality dimensions, quantization, Zstd level, and pack target remain unchanged unless separately versioned and approved.
- External artifact revisions are never accepted implicitly. New scientific product revisions receive new stable source keys/IDs and new pinned bundles.
- Unreal and downstream terrain-realization work remain out of scope.
