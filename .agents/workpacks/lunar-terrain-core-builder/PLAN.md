# LunarTerrainCore / LunarTerrainBuilder workpack

Authoritative scope and milestone definitions: [`plans/01_LunarTerrainCore_LunarTerrainBuilder_Implementation_Plan.md`](../../../plans/01_LunarTerrainCore_LunarTerrainBuilder_Implementation_Plan.md).

Architecture: [`specs/01_LunarTerrainBuilder_Architecture_v0.3.md`](../../../specs/01_LunarTerrainBuilder_Architecture_v0.3.md).

Current milestone: M0 — Core contracts and format lock (complete). Implementation is paused before M1.

Blockers: None.

Verification: Golden regeneration/check, canonical JSON, SHA256SUMS, pack zero-field hashing, JSON/YAML syntax, and the M1 scope audit pass. On Windows/MSVC, the ignored `local-debug` workflow completed the pinned-vcpkg configure, build, and test path; CTest passed all 14 registered Core, Builder, and CLI tests.

Next action: Begin M1 projection, topology, byte-I/O, integrity, decompression, and database-reader work when authorized.
