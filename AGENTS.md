# Repository Guidance

## Authority and scope

- `specs/01_LunarTerrainBuilder_Architecture_v0.3.md` owns the system architecture and scientific intent.
- `specs/03_LunarTerrainDatabase_Format_v1.md` owns the byte-level v1 `.ltdb`/`.ltp` contract. When an illustrative layout in the architecture is less specific, the format specification controls.
- `plans/01_LunarTerrainCore_LunarTerrainBuilder_Implementation_Plan.md` owns milestone scope and acceptance.
- `.agents/workpacks/lunar-terrain-core-builder/PLAN.md` records only the current milestone, verification state, blockers, and next action. Update it whenever milestone state changes.
- Current implementation scope is `LunarTerrainCore` and the standalone `LunarTerrainBuilder`. Do not add Unreal modules, Unreal representations, Mesh Terrain integration, or runtime/editor code.

## C++ and API rules

- Target hosted C++20. Prefer value semantics and explicit ownership; returned decoded data owns its storage, and borrowed views may not outlive that owner.
- Public Core operations report recoverable failures through `lunar::terrain::Result<T>` and `Error`, not exceptions. Error codes are stable; attach relevant path, byte offset, encoded `LunarTileKey`, or channel context.
- Keep Core free of Unreal, GDAL, PROJ, SQLite, TOML, CLI, and formatting dependencies. Core's allowed external dependencies are Zstandard and OpenSSL.
- Keep Builder-only dependencies private to Builder targets.
- Preserve deterministic ordering and fixed floating-point operation order. Never enable fast-math or scheduling-dependent floating-point reductions.

## Format and serialization rules

- Treat the v1 format specification and committed golden fixtures as frozen compatibility inputs. Change them only with an explicitly approved format revision.
- Read and write fields explicitly in little-endian order. Never serialize or hash native C++ object representations, structure padding, host endianness, container iteration order, or uninitialized bytes.
- Check every externally supplied offset, size, count, range, and arithmetic operation before use.
- Canonical hashes cover exactly the domains defined by the format specification. Reserved and alignment bytes are zero when written and validated as specified.

## Build and verification

The committed presets require CMake 3.25+, a C++20 compiler, and `VCPKG_ROOT` pointing to the pinned vcpkg checkout:

```text
cmake --workflow --preset debug
cmake --workflow --preset release
cmake --workflow --preset linux-asan
```

The Linux sanitizer workflow is available only on Linux with GCC or Clang. Focused commands remain:

```text
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

- Use target-based CMake, explicit source lists, public header file sets, imported dependency targets, and target-scoped warnings/options.
- Do not add install, export, or package-config machinery until a consumer requirement exists.
- Linux/GCC and Windows/MSVC must remain supported. MSVC source compilation uses UTF-8 and precise floating point.

## Working-tree hygiene

- Preserve user changes and keep unrelated edits out of the active patch.
- Keep build trees, dependency installations, caches, staging data, `.ltdb`, and `.ltp` outputs out of source control.
- Committed `tests/golden/format_v1` vectors are intentional compatibility fixtures, not disposable generated output.
- Do not commit machine-local paths, job counts, cache locations, or staging locations.

