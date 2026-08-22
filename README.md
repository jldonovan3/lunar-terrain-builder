# Lunar-Terrain-Builder Pipeline

An offline toolchain for transforming heterogeneous lunar DEM products into a deterministic, fused, provenance-preserving canonical terrain database for ultimate conversion into Unreal Engine 5.8 mesh terrain.

## The Ultimate Overly-Ambitious Goal
Streaming (or packaging / compressing) a 1:1 full-scale, continuous, spherical Moon with a walkable surface, streamed via World Partition in UE

## Overall Goals

- scientific DEM data as authoritative macrogeometry
- QSC projection
- fusion of heterogeneous DEM resolutions
- deterministic procedural enhancement downstream from the scientific base
- complete offline distribution
- preservation of DEM provenance, and data attribution
- Unreal Engine 5.8 mesh terrain as the intended terrain realization layer, rather than scientific storage formats

## Pipeline

Four implementation units:


### `LunarTerrainCore` (M1 complete)
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

The M2 synthetic P0 path is available through the committed
`tests/data/synthetic_p0.toml` configuration:

```text
lunar-terrain scan tests/data/synthetic_p0.toml --json
lunar-terrain plan tests/data/synthetic_p0.toml --json
lunar-terrain build tests/data/synthetic_p0.toml --json
lunar-terrain validate out/m2-synthetic/MoonSynthetic.ltdb --full --json
lunar-terrain inspect out/m2-synthetic/MoonSynthetic.ltdb QSC/F0/L00/0000/0000 --json
```

It publishes six deterministic level-zero QSC face packs and the LTDB manifest.
Generated `.ltdb`/`.ltp` outputs remain untracked build products.

### `LunarTerrainEditor`
    Unreal Engine 5.8 editor plugin
    reads .ltdb custom database
    constructs spherical Mesh Partition base geometry
    manages rebuild/invalidation
    invokes deterministic geological PCG
    builds Mesh Terrain

### `LunarTerrainRuntime`
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

## SLDEM2015 Data Attribution
Barker, M. K., Mazarico, E., Neumann, G. A., Zuber, M. T., Haruyama, J., Smith, D. E. "A new lunar digital elevation model from the Lunar Orbiter Laser Altimeter and SELENE Terrain Camera," Icarus, Volume 273, p. 346-355. http://dx.doi.org/10.1016/j.icarus.2015.07.039
