# LunarTerrainCore / LunarTerrainBuilder workpack

Authoritative scope and milestone definitions: [`plans/01_LunarTerrainCore_LunarTerrainBuilder_Implementation_Plan.md`](../../../plans/01_LunarTerrainCore_LunarTerrainBuilder_Implementation_Plan.md).

Architecture: [`specs/01_LunarTerrainBuilder_Architecture_v0.3.md`](../../../specs/01_LunarTerrainBuilder_Architecture_v0.3.md).

Current milestone: M4 — P2 seams, corners, quantization, aprons, and dependency-versioned staging are complete. Development is paused before M5.

Blockers: None known. The pinned three-member SLDEM2015 subset is provisioned outside the repository and matches the locked byte counts and SHA-256 values in [`plans/02_SLDEM2015_Pre-M3_Provisioning_Workflow.md`](../../../plans/02_SLDEM2015_Pre-M3_Provisioning_Workflow.md).

Verification: M2 acceptance is complete, including its Linux ASan/UBSan workflow. M3 passed the Windows/MSVC debug and release workflows, generated-raster catalog/build/Core-roundtrip tests, explicit datum and no-data tests, and the opt-in pinned-SLDEM2015 acceptance workflow. M4 passed the Linux debug, release, and ASan/UBSan workflows (45/45 tests each), including lowest-TileKey edge/corner ownership independent of input order; equatorial, polar, and reversed topology; post-seam quantization; materialized-neighbor and sparse virtual aprons; atomic dependency-named binary64 staging; deterministic clean builds; the pinned SLDEM2015 round trip; and canonical seam/apron algorithm identity. The frozen v1 fixtures and Core dependency boundary remain unchanged. Leak detection was disabled for the sanitizer workflow because LeakSanitizer cannot run under the managed ptrace environment; AddressSanitizer and UndefinedBehaviorSanitizer remained enabled.

Next action: Pause. Begin M5 fusion, provenance, and quality work only after explicit authorization.
