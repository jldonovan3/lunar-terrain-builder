# M8 product qualification spike

This package implements the M8 product locks, required-region and scale configurations, complete real-raster hierarchy path, quality mappings, and reproducible qualification workflow. Raw databases, packs, caches, decoded-raster accelerators, exports, and command records remain outside source control. M8 acceptance is not claimed until the current resource-bounding work, required Windows/Linux matrix, and opt-in scale run are completed with evidence.

## Product preflight and acquisition lock

All selected archives fit the existing schema-version-1 lifecycle: direct HTTP members, optional official PDS MD5 manifest verification, project SHA-256 verification, and ordered canonical bundle hashing. No provisioning schema or acquisition lifecycle change was required.

| Product / selected revision | Original members | Locked bytes | Canonical bundle SHA-256 | Checksum authority |
|---|---:|---:|---|---|
| LOLA GDR `LDEM_256`, product V3.0, data through `LRO_ES_52`, archive `lrolol_1xxx_260615` | 4 lossless JP2 rasters + 8 PDS sidecars | 1,848,004,754 | `8a657dde1f0047bc5cc32a179aee38ba3e36fa081723eae2618e8a181da1909f` | Official PDS MD5 manifest plus project SHA-256 |
| SLDEM2015 512 ppd V2.0, archive `lrolol_1xxx_260615` | 32 lossless JP2 rasters + 64 PDS sidecars | 5,466,757,104 | `b3f04e54c5368111df2fd5699017db29d723e8790bc8821eaa1a562f136d2e29` | Official PDS MD5 manifest plus project SHA-256 |
| LROC `NAC_DTM_MASKELYNE` v1.9, `LROLRC_2001` | elevation GeoTIFF, confidence image, label, readme | 136,882,682 | `c7b56da9de0a50839e4881c32e3cee6c6ad67f1a6629e5875779c418aa3cea36` | Archive HTTP entity MD5 values confirmed against the members; project SHA-256 is authoritative for the lock |
| GSFC adjusted south-polar `80S` bundle, 2023 publication | elevation, count, effective-resolution, and error COG GeoTIFFs | 11,915,218,975 | `a0f79e5293db691a3e06022221137d261e1de322e542c3c45add92a3280d771e` | No published checksum catalog was located; exact project SHA-256 values are authoritative for the lock |

The complete selected original-member set is 19,366,863,515 bytes (about 18.04 GiB), excluding downloaded and generated checksum manifests. `provisioning/provisioning.json` owns every per-member byte count, MD5 where available, SHA-256, role, ordered bundle, total, and canonical identity. The established three-member SLDEM `artifact` bundle and `north_boundary` profile remain only as pre-optimization implementation state until the explicitly deferred Priority-1 consolidation. The corrected acceptance contract has one `SLDEM2015` acquisition profile backed by the existing locked 96-member `fullset` bundle; required-region configurations select spatial coverage without defining smaller acquisition identities.

Authoritative source evidence:

- LOLA GDR dataset and archive: <https://pds-geosciences.wustl.edu/lro/lro-l-lola-3-rdr-v1/lrolol_1xxx/catalog/gdr_ds.cat> and <https://pds-geosciences.wustl.edu/lro/lro-l-lola-3-rdr-v1/lrolol_1xxx/data/lola_gdr/cylindrical/jp2/>.
- SLDEM2015 archive: <https://pds-geosciences.wustl.edu/lro/lro-l-lola-3-rdr-v1/lrolol_1xxx/data/sldem2015/>.
- Maskelyne product page, PDS archive, usage terms, and label/readme: <https://data.lroc.im-ldi.com/lroc/view_rdr/NAC_DTM_MASKELYNE>, <https://pds.lroc.im-ldi.com/data/LRO-L-LROC-5-RDR-V1.0/LROLRC_2001/DATA/SDP/NAC_DTM/MASKELYNE/>, and <https://www.lroc.im-ldi.com/about/terms>. The LROC terms identify products obtained through the listed PDS interfaces as public-domain data and request research citation.
- GSFC south-polar product page and required citation: <https://pgda.gsfc.nasa.gov/products/90>, data DOI <https://doi.org/10.60903/gsfcpgda-lola-spole>, and Barker et al. (2023), <https://doi.org/10.3847/PSJ/acf3e1>.

The NASA/PDS and GSFC endpoints are public, non-authenticated archives. Product citations and archive provenance must accompany scientific use. No credentials, session state, interactive agreement, archive extraction, generated URL discovery, or overwrite behavior is needed.

## Builder-linked GDAL inspection

The checked-in expectations were reconciled against the same GDAL 3.12.4 / PROJ 9.8.1 dependency stack linked into the Builder:

- LOLA and SLDEM open through `JP2OpenJPEG` as `Int16`. Their PDS values use a 0.5 scale. LOLA `LDEM_256` is radius relative to a 1,737,400 m reference and declares a 1,737,400 m offset; SLDEM is elevation relative to that radius with zero offset. The four LOLA files are 46,080 × 23,040 and the 32 SLDEM files are 23,040 × 15,360.
- Maskelyne opens through `GTiff` as 2,370 × 11,542 `Float32`, with no-data `-3.40282265508890445e38`, approximately 5 m pixels, and observed bounds `33.5311475599988..33.922993086289495°E`, `3.258297606427915..5.16145141999987°N`. Its equirectangular CRS is planetocentric/east-positive on a 1,737,400 m sphere.
- Every selected south-polar COG opens through `GTiff` as 30,400 × 30,400 `Float32`, with 20 m projected pixels, NaN no-data, and a south-polar stereographic Moon sphere. The projected square's geographic envelope is `-180..180°E`, `-90..-75.89380428852846°N`; the selected scientific region remains 80°S and poleward.

NaN no-data, omitted default scale/offset tags, full-longitude logical mosaics, and pole-containing projected footprints are now represented and validated explicitly by the Builder. These are compatibility additions to source ingestion and diagnostics; they do not alter format v1.

Pre-fusion spot checks recorded from original elevation members through `scan` are:

| Source | Planetocentric coordinate (latitude, longitude) | Normalized elevation |
|---|---|---:|
| LOLA global mosaic | `(0°, 0°E)` | `-723.0 m` |
| SLDEM artifact | `(15°, 22.5°E)` | `-1346.0 m` |
| SLDEM northern-boundary artifact | `(45°, 22.5°E)` | `-1091.75 m` |
| Maskelyne | `(4.2098745132138925°, 33.72707032314415°E)` | `-781.5782012939478 m` |
| GSFC south-polar adjusted DEM | `(-82.94690214426423°, 0°E)` | `-1768.8586752032345 m` |

These values are input checks, not fusion acceptance values. Required-region acceptance uses decoded output samples, policy-to-policy elevation deltas, transition derivatives, provenance/quality diagnostics, complete structural/scientific validation, and the frozen 0.5 m publication quantization.

## Source lineage, policies, and quality

LOLA is the global foundation. SLDEM combines LOLA and Kaguya Terrain Camera information and is therefore a higher-resolution backbone, not an independent measurement of LOLA-scale geometry. The GSFC polar DEM is LOLA-derived. Starting priorities and policies reflect this lineage:

- global LOLA: priority 0, base, `Replace`;
- SLDEM: priority 100, `Replace` over the foundation;
- Maskelyne NAC: priority 300, `ResidualRefinement_v1` as the required starting hypothesis;
- GSFC adjusted south pole: priority 300, `Replace` as the starting hypothesis.

Maskelyne confidence DN 0 is treated as no-data; DN 1–4 is declared lower-confidence/interpolated; DN 10–15 supports elevation. The exact distinctions among shadow, saturation, suspicious correlation, interpolation/extrapolation, successful correlation, and manual editing cannot be retained in v1 `QUAL` and are recorded as unsupported rather than collapsed silently.

For the polar bundle, zero count or non-finite elevation is declared filled/no-data and lower-confidence. Local effective resolution above the provisional 80 m source level or elevation error above 5 m is declared lower-confidence. Exact count and continuous effective-resolution/error values cannot be encoded losslessly in v1 `QUAL`; the source companions and this limitation remain explicit. Both mappings now feed the fused source grids and published v1 `QUAL` channel.

## Qualification configurations and observed plans

The configurations under `configs/` contain semantic source metadata and only portable external-root environment names. Output, cache, and thread settings remain local and excluded from semantic identity. `verify_builder_locks.py` fails when a Builder stable key, provenance URI, root environment name, ordered artifact member, byte count, SHA-256, bundle total, or bundle hash drifts from the acquisition lock.

| Config | Required region | Source target levels | Expected hierarchy tiles | Estimated uncompressed channel bytes | Semantic SHA-256 |
|---|---|---|---:|---:|---|
| Profile A | `33.55..33.90°E`, `3.30..5.10°N` | LOLA L7; SLDEM L8; Maskelyne L12 | 2,480 | 353,196,640 | `116b2dd77e5ca1353a2c165ed638a4208e5b4f6a3d30c757488732d07951f72b` |
| Profile B | `20..21°E`, `59.5..60.5°N` | LOLA L7; SLDEM L8 through 60°N | 41 | 5,838,482 | `03853ef143a7a9cd3da85e9e4af7550646bd2267674127f2df95929ef0698862` |
| Profile C | all longitudes, `90..89°S` | LOLA L7; polar L8 | 77 | 10,964,954 | `07cbf079a4d8da1d11852e94653cbf7ea1f6501dca459739470a2fc18416e9b9` |
| Scale | global | LOLA L7; SLDEM L8; polar L8; Maskelyne L12 | 481,225 | 68,542,801,650 | `b2a7c7240a48d752bc727e6d6578f478bf11871ecb9d2ff9ffb2e314bab0ecc9` |

Level selection is the existing deterministic worst-case QSC sample-spacing rule, so these measured L7/L8/L12 results supersede filename and nominal-grid hypotheses. `plan --json` lists per-source clipped coverage, effective resolution, selected level, fusion policy, counts at every materialized level, and at most 256 representative tile keys. `scan --json` lists the complete logical source registry, physical raster count, artifact identity, coverage, center spot check, priority, policy, quality mapping, and unsupported quality values.

## Work Stages 4–5 implementation and evidence state

`materialize_hierarchy = true` selects the complete sparse hierarchy path without changing established synthetic or single-tile configurations. Small hierarchies retain the direct in-memory path. Larger plans currently stage one whole level at a time, reconstruct deterministic seams from two-hop same-level neighborhoods, persist compact quantized parent cores for geometric error, and assemble canonical packs from dependency-named encoded artifacts. The generated 30-tile hierarchy proves deterministic equivalence at that test size, but whole-level staging is not resource-bounded with respect to the level-8 tile count and must not be described as bounded scale execution.

`LUNAR_TERRAIN_DECODED_CACHE` may name an absolute machine-local directory shared by otherwise empty tile caches. It stores artifact-hash-keyed, explicit little-endian decoded rasters with a payload SHA-256. It is an execution accelerator, not semantic input; corruption fails validation. The qualification driver uses a work-root-local directory unless the caller supplies a shared external root.

The complete Windows/MSVC Profile A–C matrices passed. For every profile, two clean builds, reversed source declarations, and the restored incremental build have identical database and ordered pack identities. Every clean, reordered, removed, restored, and policy-variant database passed `validate --full`; inspect, classified diff, elevation, provenance, quality, and transition exports also completed.

| Profile | Clean database SHA-256 | Tiles / packs | Fully verified hierarchy / seams | Removed refinement | Restoration |
|---|---|---|---|---|---|
| A | `52ce12f6d19523aa2e388a6cc81d5e5c177050b5da9d84abdfab086d01865d09` | 2,480 / 26 | 2,480 / 4,479 | 51 survivors built, zero reused | 51 built, 2,429 reused; exact convergence |
| B | `3cf457349f07b123e70a11e9e836f0e88f4e3da975a304fc4e23faea4ec63ae4` | 41 / 9 | 41 / 29 | 33 survivors built, zero reused | 33 built, 8 reused; exact convergence |
| C | `4ee43670c0a74539e4b472af15bfe4ff4e3ded91be930bc6607cbd1be3c7fc7f` | 77 / 9 | 77 / 108 | 41 survivors built, zero reused | 41 built, 36 reused; exact convergence |

The policy evidence supports the configured starting choices, with explicit qualifications:

| Pairing | Accepted policy | Comparison evidence and qualification |
|---|---|---|
| LOLA → SLDEM | `Replace` | `BiasCorrectedReplace` validated but changed 21 of 41 tile contents. At the transition diagnostic tile it shifted mean elevation by `-0.617 m` (RMS `0.690 m`, maximum `1.0 m`), changed first-derivative RMS only from `4.111` to `4.103 m/sample`, and increased second-derivative RMS from `1.678` to `1.715 m`. The extra bias step therefore did not provide a material continuity benefit over the source's intended absolute geometry. |
| SLDEM → Maskelyne | `ResidualRefinement_v1` | `Replace` and `BiasCorrectedReplace` both validated and each changed 2,179 tile contents. At the explicit L12 prototype, direct replacement shifted the residual result by `3.638 m` mean (RMS `4.770 m`, maximum `18.5 m`); bias-corrected replacement reduced the mean shift to `0.595 m` but retained `3.136 m` RMS and a `21.5 m` maximum. The residual policy keeps the related backbone's broad geometry while admitting the NAC contribution. Confidence DN limitations remain encoded as interpolated/lower-confidence v1 quality rather than treating the advertised 5 m grid as uniformly authoritative. |
| LOLA → south-polar LOLA | `Replace`, lower-confidence qualification | `BiasCorrectedReplace` validated and changed 75 of 77 tile contents, but at the pole-region diagnostic tile changed only one quantized sample by `0.5 m` and left the measured transition derivatives unchanged. Count/effective-resolution/error companions mark the contribution as filled or lower-confidence where applicable. The polar product is therefore accepted as a sparse, quality-qualified contribution at the provisional L8 treatment, not as uniformly dominant 20 m terrain. |

The first large opt-in scale attempt failed or was incompletely terminated before it produced an accepted benchmark report. Its abandoned external staging/cache data is user-owned and is not removed automatically. Whole-level staging was not resource-bounded, and no scale benchmark is currently claimed to be running without independent process, heartbeat, and report verification. Linux evidence is pending because the current host has no Linux or WSL environment.

Preparation and Priority 0 add continuously flushed progress-v1 events, benchmark-v2 and qualification-v2 partial reports, explicit budgets, cooperative cancellation, the detached observer lifecycle, and resumable attempts. The probe is locked to the complete four-source scale stack and the 40–50°E, 10–20°N region crossing the 45° SLDEM component boundary; the driver rejects a completed report unless its level-8 plan contains at least 128 tiles. The real plan contains 1,448 hierarchy tiles, including 1,021 level-8 tiles across 16 reference 64-tile work chunks.

The real detached cancellation/restart gate passes. The first attempt durably checkpointed scan, plan, and a clean 27-pack publication, then cooperatively cancelled during full validation with exit 130 and structured benchmark, qualification, observer, and terminal-event evidence. The resumed benchmark fully validated, completed an unchanged incremental rebuild with 1,448/1,448 reuse, and exactly reproduced database identity `b590ed620d4931a7023a08de5d726d3625685af0a6028dd6b85e315e7cb069a6` plus all 27 ordered pack identities. Its clean build, full validation, and unchanged incremental phases took 52.492 s, 20.642 s, and 1.518 s respectively; inspection and all five required exports passed, and qualification records `resume_byte_identical = true`. An empty benchmark-prototype assumption caused one preserved downstream failed attempt after the resumed benchmark had passed; the driver now has tested prototype precedence with a read-only published-cache fallback, and a distinct resumed attempt completed the remaining checkpoints.

The probe also confirms the Priority-1 resource blocker rather than resolving it: 38 retained full-raster artifacts occupied 31,279,479,900 decoded-cache bytes against the 8,192 MiB sub-budget. Peak process RSS was 31,642,853,376 bytes (29.47 GiB), below the 48 GiB process ceiling but not bounded by the configured cache budget. These figures are diagnostic P0 evidence, not an accepted scale-performance result. The self-contained synthetic lifecycle separately verifies checkpointed exit 130 and byte-identical detached restart behavior.

Alternative A resolves the former acceptance conflict without changing format v1. Scientific content and provenance changes must remain within the computed source-influence footprint, while frozen-v1 dependency hashes, database identity, and package placement may change globally. Restoration must reproduce the clean database and ordered pack identities exactly. The existing removal runs demonstrate global dependency invalidation and exact restoration; footprint-confined scientific assertions remain part of the optimization acceptance work.

## Portable commands

Provision products separately into external roots:

```text
python provisioning/provision.py LOLAFoundation --root <lola-root>
python provisioning/provision.py Artifact --root <sldem-root>
python provisioning/provision.py SLDEMBoundary --root <sldem-root>
python provisioning/provision.py Maskelyne --root <maskelyne-root>
python provisioning/provision.py SouthPolar80S --root <south-polar-root>
```

Use `Fullset` instead of `Artifact`/`SLDEMBoundary` for the scale profile. Add `--verify-only` for strict no-network verification. Set the environment names printed by provisioning, then run:

```text
lunar-terrain scan qualification/m8/configs/profile_a.toml --json
lunar-terrain plan qualification/m8/configs/profile_a.toml --json
python provisioning/verify_builder_locks.py --lock provisioning/provisioning.json qualification/m8/configs/profile_a.toml qualification/m8/configs/profile_b.toml qualification/m8/configs/profile_c.toml qualification/m8/configs/scale.toml
```

Run one required-region matrix into a new external or ignored directory with explicit budgets:

```text
python qualification/m8/run_qualification.py run a --binary <lunar-terrain> --work-root <empty-work-root> --threads <local-count> --memory-budget-mib 16384 --decoded-cache-budget-mib 8192 --scratch-budget-mib 16384
```

Use `b` or `c` for the other required profiles, `probe` for the spatially restricted scale stack, and `scale` for the full opt-in benchmark. Resume a matching interrupted root with `run_qualification.py resume --work-root <work-root>`; use `sweep` for probe worker-count comparisons. The v2 driver refuses a nonempty new root, streams child output, atomically checkpoints partial evidence and active-child state, validates every database fully, compares clean/reordered/restored identities, runs policy variants and operator exports, and keeps all machine-local paths and job counts in the raw work root rather than the committed summary.
