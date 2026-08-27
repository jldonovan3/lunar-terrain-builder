# Lunar Terrain Builder

An offline toolchain for transforming heterogeneous lunar DEM products into a deterministic, fused, provenance-preserving canonical terrain database for ultimate conversion into Unreal Engine 5.8 mesh terrain.

## The Ultimate Overly-Ambitious Goal
Streaming (or packaging / compressing) a 1:1 full-scale, continuous, spherical Moon with a walkable surface, streamed via World Partition in UE

## Overall Goals

- scientific DEM products as authoritative macrogeometry
- QSC projection
- fusion of heterogeneous DEM resolutions from different products
- deterministic procedural enhancement downstream from the scientific base (PCG in Unreal)
- complete offline distribution
- preservation of DEM provenance, and data attribution
- Unreal Engine 5.8 mesh terrain as the intended terrain realization layer, rather than scientific storage formats

## Pipeline

Four implementation units:

### `LunarTerrainCore`
    Pure C++ data structures and algorithms
    Tile IDs
    database reader
    hashing conventions
    coordinate primitives
    no Unreal dependency

### `LunarTerrainBuilder`
    Standalone C++20 CLI
    GDAL / PROJ
    DEM ingestion
    reprojection to quadrilateralized spherical cube
    fusion
    validation - test OBJ mesh tile

#### `LunarTerrainBuilder` temp sythetic source data pathing

The synthetic P0 path is available through the committed
`tests/data/synthetic_p0.toml` configuration:

```text
lunar-terrain scan tests/data/synthetic_p0.toml --json
lunar-terrain plan tests/data/synthetic_p0.toml --json
lunar-terrain build tests/data/synthetic_p0.toml --json
lunar-terrain validate out/m2-synthetic/MoonSynthetic.ltdb --full --json
lunar-terrain inspect out/m2-synthetic/MoonSynthetic.ltdb QSC/F0/L00/0000/0000 --json
```

`Builder` outputs six deterministic level-zero QSC face packs and the LTDB manifest.
Generated `.ltdb`/`.ltp` outputs remain untracked build products.

M7 completes the standalone operator path:

```text
lunar-terrain validate Moon.ltdb --full --json
lunar-terrain inspect Moon.ltdb QSC/F0/L00/0000/0000 --json
lunar-terrain diff Moon-before.ltdb Moon-after.ltdb --json
lunar-terrain export Moon.ltdb QSC/F0/L00/0000/0000 --format ply --output tile.ply
lunar-terrain benchmark lunar-terrain.toml --output benchmark.json --json
```

`diff` separately reports dataset-registry, dependency, decoded-content, provenance,
and package-placement changes. Export formats are `ply`, `obj`, `elevation-pgm`,
`csv`, `raw-u16`, `provenance-ppm`, `quality-ppm`, and `transition-csv`.
The raw elevation product is the 257×257 core in explicit little-endian U16 order;
the 16-bit PGM diagnostic raster uses the Netpbm-required big-endian sample order.
Controlled M7 benchmark records and metric definitions live in `benchmarks/m7`.

The SLDEM2015 P1 path is configured by
`tests/data/sldem2015_p1.toml`. It catalogs and verifies the pinned raster
artifact bundle, applies explicit datum and no-data rules, selects a fully
covered QSC tile, and publishes a deterministic database through the same
Core reader path. Set `SLDEM2015_ROOT` to the provisioned artifact root, then
run:

```text
cmake --workflow --preset sldem2015-acceptance
lunar-terrain scan tests/data/sldem2015_p1.toml --json
lunar-terrain plan tests/data/sldem2015_p1.toml --json
lunar-terrain build tests/data/sldem2015_p1.toml --json
```

> Without `SLDEM2015_ROOT`, the external-data acceptance test is reported as skipped by the normal debug and release workflows. 

### `LunarTerrainEditor` (deferred)
    Unreal Engine 5.8 editor plugin
    reads .ltdb custom database
    constructs spherical Mesh Partition base geometry
    manages rebuild/invalidation
    invokes deterministic geological PCG
    builds Mesh Terrain

### `LunarTerrainRuntime` (deferred)
    Unreal runtime module
    lunar geographic coordinate types
    Moon-centered transforms
    gravity/local tangent calculations
    gameplay spatial queries
    does NOT require GDAL or raw DEM data

The dependency direction is:

```text
Scientific source products
        ↓
LunarTerrainBuilder
        ↓
Moon.ltdb + *.ltp
        ↓
LunarTerrainEditor
        ↓
Mesh Partition base geometry
        ↓
Mesh Terrain modifiers / PCG
        ↓
Compiled Mesh Terrain sections
        ↓
Unreal cook / IoStore
```

## SLDEM2015 Provisioning

`provisioning/provision.py` is the standard-library Python 3.9+ entry point on Windows and Linux. Its adjacent `provisioning.json` contains the locked product, bundle, and profile definitions for the single-tile `Artifact` and 32-tile `Fullset` acquisitions. Pass the external destination with `--root` or `SLDEM2015_ROOT`; add `--verify-only` to prohibit network requests. See [`provisioning/provisioning.md`](provisioning/provisioning.md) for commands, integrity guarantees, and configuration guidance.

## SLDEM2015 Data Attribution
Barker, M. K., Mazarico, E., Neumann, G. A., Zuber, M. T., Haruyama, J., Smith, D. E. "A new lunar digital elevation model from the Lunar Orbiter Laser Altimeter and SELENE Terrain Camera," Icarus, Volume 273, p. 346-355. http://dx.doi.org/10.1016/j.icarus.2015.07.039
