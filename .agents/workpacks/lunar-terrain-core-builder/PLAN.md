# LunarTerrainCore / LunarTerrainBuilder workpack

Authoritative scope and milestone definitions: [`plans/01_LunarTerrainCore_LunarTerrainBuilder_Implementation_Plan.md`](../../../plans/01_LunarTerrainCore_LunarTerrainBuilder_Implementation_Plan.md).

Architecture: [`specs/01_LunarTerrainBuilder_Architecture_v0.3.md`](../../../specs/01_LunarTerrainBuilder_Architecture_v0.3.md).

Current milestone: M3 — SLDEM2015 P1 ingestion is complete. Development is paused before M4.

Blockers: None known. The pinned three-member SLDEM2015 subset is provisioned outside the repository and matches the locked byte counts and SHA-256 values in [`plans/02_SLDEM2015_Pre-M3_Provisioning_Workflow.md`](../../../plans/02_SLDEM2015_Pre-M3_Provisioning_Workflow.md).

Verification: M2 acceptance is complete, including its Linux ASan/UBSan workflow. M3 passed the Windows/MSVC debug and release workflows, generated-raster catalog/build/Core-roundtrip tests, explicit datum and no-data tests, and the opt-in pinned-SLDEM2015 acceptance workflow. The real-data acceptance rechecked all three member hashes and the canonical ordered bundle hash, measured the locked source sample, reproduced the selected QSC tile byte-for-byte across two builds, and reconstructed it through Core within the configured quantization bound. The format-fixture check, frozen-fixture scope, Core dependency scope, deterministic-floating-point scope, and machine-local-path audit passed.

Next action: Pause. Begin M4 multi-source fusion, hierarchy, seam, and apron work only after explicit authorization.
