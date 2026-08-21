# LunarTerrainCore / LunarTerrainBuilder workpack

Authoritative scope and milestone definitions: [`plans/01_LunarTerrainCore_LunarTerrainBuilder_Implementation_Plan.md`](../../../plans/01_LunarTerrainCore_LunarTerrainBuilder_Implementation_Plan.md).

Architecture: [`specs/01_LunarTerrainBuilder_Architecture_v0.3.md`](../../../specs/01_LunarTerrainBuilder_Architecture_v0.3.md).

Current milestone: M1 — LunarTerrainCore projection and reader foundation. Implementation and Windows/MSVC verification are complete; work is paused before M2 pending the Linux acceptance workflow.

Blockers: The required Linux sanitizer workflow cannot run on this Windows host; WSL and Docker are unavailable. No implementation blocker is known.

Verification: `cmake --workflow --preset debug` and `cmake --workflow --preset release` pass on Windows/MSVC with the pinned vcpkg baseline; each passes all 30 registered Core, Builder, and CLI tests. Coverage includes TileKey, signed QSC lattice coordinates, six-face PROJ-oracle agreement, topology/reversal/poles/corners, checked little-endian I/O, CRC32C, SHA-256, Delta2D reversal, Zstandard decoding, golden LTDB/LTP reads, ownership, sparse queries, and corruption/version failures. `py -3 tools/generate_format_v1_fixtures.py --check`, `git diff --check`, Core dependency scope, and fast-math audits pass. `linux-asan` remains unexecuted.

Next action: On Linux with GCC or Clang, run `cmake --workflow --preset linux-asan`. If it passes, mark M1 complete and remain paused until M2 is explicitly authorized.
