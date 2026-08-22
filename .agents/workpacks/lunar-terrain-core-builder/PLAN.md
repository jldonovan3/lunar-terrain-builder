# LunarTerrainCore / LunarTerrainBuilder workpack

Authoritative scope and milestone definitions: [`plans/01_LunarTerrainCore_LunarTerrainBuilder_Implementation_Plan.md`](../../../plans/01_LunarTerrainCore_LunarTerrainBuilder_Implementation_Plan.md).

Architecture: [`specs/01_LunarTerrainBuilder_Architecture_v0.3.md`](../../../specs/01_LunarTerrainBuilder_Architecture_v0.3.md).

Current milestone: M2 — Synthetic P0 end-to-end container. Implementation and Windows/MSVC verification are complete; M2 awaits the Linux sanitizer acceptance workflow.

Blockers: The M2 Linux sanitizer workflow cannot run on this Windows host; no implementation blocker is known.

Verification: M1 acceptance is complete, including its Linux sanitizer workflow. For M2, `cmake --workflow --preset debug` and `cmake --workflow --preset release` pass on Windows/MSVC with all 39 registered Core, Builder, and CLI tests. M2 coverage includes typed TOML rejection and identity exclusions, six canonical root-face plans, ELEV/PRVN encoding and Core-reader round trips, all 12 unique face seams, two clean byte-identical builds, failed-publication preservation, and `scan`/`plan`/`build`/`validate`/`inspect` JSON entry points. `py -3 tools/generate_format_v1_fixtures.py --check`, `git diff --check`, frozen-fixture scope, Core dependency scope, and fast-math audits pass. The M2 `linux-asan` workflow remains unexecuted.

Next action: On Linux with GCC or Clang, run `cmake --workflow --preset linux-asan`. If it passes, mark M2 complete and remain paused until M3 is explicitly authorized.
