# LunarTerrainCore / LunarTerrainBuilder workpack

Authoritative scope and milestone definitions: [`plans/01_LunarTerrainCore_LunarTerrainBuilder_Implementation_Plan.md`](../../../plans/01_LunarTerrainCore_LunarTerrainBuilder_Implementation_Plan.md).

Architecture: [`specs/01_LunarTerrainBuilder_Architecture_v0.3.md`](../../../specs/01_LunarTerrainBuilder_Architecture_v0.3.md).

Current milestone: M5 — P3 deterministic fusion, provenance, quality, and transition diagnostics are complete. Development is paused before M6.

Blockers: None known. The pinned three-member SLDEM2015 subset is provisioned outside the repository and matches the locked byte counts and SHA-256 values in [`plans/02_SLDEM2015_Pre-M3_Provisioning_Workflow.md`](../../../plans/02_SLDEM2015_Pre-M3_Provisioning_Workflow.md).

Verification: M2 acceptance is complete, including its Linux ASan/UBSan workflow. M3 passed the Windows/MSVC debug and release workflows, generated-raster catalog/build/Core-roundtrip tests, explicit datum and no-data tests, and the pinned-SLDEM2015 acceptance workflow. M4 passed the Linux debug, release, and ASan/UBSan workflows (45/45 tests each), including deterministic seam/corner ownership, post-seam quantization, materialized and virtual aprons, and dependency-named binary64 staging. M5 passed the Windows/MSVC debug and release workflows (50/50 tests each), including priority/DatasetID ordering independent of configuration order; `Replace`, `BiasCorrectedReplace`, and `ResidualRefinement_v1`; the versioned binomial filter, pass-count, normalized no-data, and 32-sample smoothstep rules; broad-geometry/residual checks; continuous transition derivatives; finalized byte-identical tile edges; multi-dataset LTDB/Core round trips; sorted provenance palettes and 64×64 dominant-source/quality maps; deterministic reordered-source builds; diagnostic PPM/CSV exports; and the pinned SLDEM2015 round trip. The frozen v1 fixture checker passed, and the format fixtures and Core dependency boundary remain unchanged. Linux sanitizer verification was unavailable for M5 on the current Windows host.

Next action: Pause. Begin M6 sparse hierarchy, geometric error, and incremental cache work only after explicit authorization.
