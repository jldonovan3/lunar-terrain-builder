## 1. Scope

`LunarTerrainBuilder` is the offline toolchain responsible for transforming heterogeneous lunar DEM products into an engine-independent, deterministic, provenance-preserving canonical terrain database.

The governing requirements are:

- a continuous, full-scale 1:1 spherical Moon;
    
- scientific DEM data as authoritative macrogeometry;
    
- fusion of heterogeneous DEM resolutions;
    
- deterministic procedural enhancement downstream from the scientific base;
    
- complete offline distribution;
    
- preservation of DEM provenance;
    
- Unreal Engine 5.8 Mesh Terrain as the intended terrain realization layer rather than the canonical scientific storage format.
    

The source material already establishes that lunar DEM inputs may arrive in substantially different resolutions and formats, including PDS IMG/LBL and JPEG-2000 products.  
GDAL provides native JPEG-2000 access through its OpenJPEG driver, including georeferencing and multithreaded decoding support. The prototype builder should therefore use GDAL/PROJ as its scientific-data interoperability layer rather than implementing PDS/JP2 raster decoding internally.

---

# 2. System boundary

The pipeline is divided into four implementation units:

```text
LunarTerrainCore
    Pure C++ data structures and algorithms
    Tile IDs
    database reader
    hashing conventions
    coordinate primitives
    no Unreal dependency

LunarTerrainBuilder
    Standalone C++20 command-line program
    GDAL / PROJ
    DEM ingestion
    reprojection
    fusion
    validation
    

LunarTerrainEditor
    Unreal Engine 5.8 editor plugin
    reads .ltdb
    constructs spherical Mesh Partition base geometry
    manages rebuild/invalidation
    invokes deterministic geological PCG
    builds Mesh Terrain

LunarTerrainRuntime
    Unreal runtime module
    lunar geographic coordinate types
    Moon-centered transforms
    gravity/local tangent calculations
    gameplay spatial queries
    does NOT require GDAL or raw DEM data
```

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

Mesh Terrain's current UE 5.8 architecture supports this separation well: editor preview sections are distinct from runtime compiled sections, and compiled-section transformer pipelines can independently generate static-mesh/Nanite representations, collision, subsections, skirts, RVTs and World Partition properties.

Therefore:

> **`.ltdb` is the canonical lunar terrain database. It is not the primary shipping runtime terrain representation.**

---

# 3. Projection and tile space

## 3.1 Canonical geographic frame

Scientific locations are represented as:

```cpp
struct LunarGeodeticCoordinate
{
    double LatitudeRadians;
    double LongitudeRadians;
    double ElevationMeters;
};
```

with:

```text
Reference lunar radius = 1,737,400 m
Surface radius = ReferenceRadius + ElevationMeters
```

Persistent scientific terrain data is conceptually Moon-centered and radial.

## 3.2 Quadrilateralized Spherical Cube

The tile hierarchy uses **Quadrilateralized Spherical Cube (QSC)** projection.

PROJ describes QSC as an equal-area projection with limited angular distortion, equal treatment of all six cube faces, and specifically identifies planetary terrain rendering with per-face quadtrees as an intended application.

The six faces are fixed as:

|Face ID|Projection center|
|--:|---|
|0|Lat 0°, Lon 0°|
|1|Lat 0°, Lon +90°|
|2|Lat 0°, Lon 180°|
|3|Lat 0°, Lon −90°|
|4|North pole|
|5|South pole|

These correspond directly to PROJ's QSC face centers.

PROJ's QSC cube-face extent is `[-R,+R]` on both axes for a sphere of radius `R`.

Internally, LunarTerrainBuilder normalizes this to:

```text
u ∈ [-1,+1]
v ∈ [-1,+1]
```

per face.

---

# 4. Tile lattice

## 4.1 Fixed tile resolution

The v1 terrain tile is:

```text
256 × 256 cells
257 × 257 core samples
1-sample apron
259 × 259 serialized elevation samples
```

The distinction is:

```text
257 core vertices
      ↓
256 intervals/cells
```

This gives exact binary subdivision when moving between quadtree levels.

An uncompressed U16 elevation channel therefore occupies:

```text
259 × 259 × 2
= 134,162 bytes
≈ 131.0 KiB
```

before compression.

## 4.2 Exact sample coordinates

For level `L`:

```text
TilesPerAxis = 2^L
FaceCells    = 256 × 2^L
```

For tile `(X,Y)` and core vertex `(i,j)`, where `0 ≤ i,j ≤ 256`:

```cpp
uint64 gx = X * 256 + i;
uint64 gy = Y * 256 + j;

uint64 denominator = 256ull << L;

double u =
    double(2 * gx - denominator)
    / double(denominator);

double v =
    double(2 * gy - denominator)
    / double(denominator);
```

The important feature is that adjacent tiles calculate a shared boundary from exactly the same integer face-grid coordinate.

Example:

```text
Tile A final column:

gx = (X × 256) + 256

Tile B first column:

gx = ((X + 1) × 256) + 0
```

These are numerically identical before QSC inversion.

QSC inversion then provides latitude/longitude, and the DEM fusion sampler supplies radial elevation.

---

# 5. Tile hierarchy

The format permits levels `0–28`.

The initial Moon database is expected to use approximately `L0–L13`.

Representative levels remain:

|Level|Approximate nominal face-grid pitch|Intended source scale|
|--:|--:|---|
|L7|~106 m|global ~100 m-class DEM|
|L8|~53 m|SLDEM2015 512 ppd|
|L9|~26.5 m|intermediate|
|L10|~13.3 m|~20 m products|
|L11|~6.6 m|~10 m products|
|L12|~3.3 m|~5 m products|
|L13|~1.7 m|~2 m products|

These values are planning approximations because QSC is equal-area rather than equidistant. The builder will calculate actual local/geodesic sampling distances when deciding whether a particular level adequately represents a source product.

The hierarchy is **sparse**.

```text
L0
│
├── ...
│
├── L7 global coverage
│     │
│     └── L8 SLDEM refinement
│             │
│             ├── L10 regional DEM
│             │      └── L11
│             │            └── L12
│             │
│             └── no child where no finer
│                 measured data exists
```

An absent child explicitly means:

> no finer canonical scientific terrain node exists.

It does not imply missing terrain.

Its ancestor supplies the available measured representation.

---

# 6. `LunarTileKey`

Every terrain node has a 64-bit stable ID.

```text
63        61 60      56 55                             0
+-----------+----------+--------------------------------+
| Face: 3   | Level: 5 | Morton XY code: 56            |
+-----------+----------+--------------------------------+
```

Encoding:

```cpp
uint64 MakeTileKey(
    uint8 face,
    uint8 level,
    uint32 x,
    uint32 y)
{
    uint64 morton = MortonInterleave(x, y);

    return
        (uint64(face)  << 61) |
        (uint64(level) << 56) |
        morton;
}
```

Morton encoding is explicitly:

```text
bit 2k     = X bit k
bit 2k + 1 = Y bit k
```

Validation requirements:

```text
Face  <= 5
Level <= 28

X < 2^Level
Y < 2^Level

unused Morton bits must equal zero
```

Parent:

```cpp
ParentMorton = Morton >> 2;
ParentLevel  = Level - 1;
```

Children:

```cpp
ChildMorton = (Morton << 2) | Quadrant;
ChildLevel  = Level + 1;
```

A human-readable form is also standardized:

```text
QSC/F2/L08/0147/0092
```

`LunarTileKey` should become a project-wide primitive rather than a builder-private type.

---

# 7. Proposed storage architecture

Two related formats are introduced:

```text
Moon.ltdb
    database manifest
    schema
    source registry
    pack registry
    searchable tile index

*.ltp
    Lunar Terrain Pack
    actual tile channel payloads
```

Example:

```text
Moon.ltdb

Packs/
    Moon_F0_L07_P0000.ltp
    Moon_F0_L08_P0000.ltp
    Moon_F1_L07_P0000.ltp
    ...
    Moon_F4_L12_P0003.ltp
```

Packs contain a single `(Face, Level)` group and are ordered by Morton code.

The prototype pack rollover target is:

```text
1 GiB
```

This is a tuning parameter, not a format invariant.

---

# 8. `.ltdb` file

All integers are **little-endian**.

IEEE-754 binary32/binary64 are used for floats.

No serialized structure may rely upon native C++ structure padding.

## 8.1 Header

`LTDBHeaderV1` is exactly **256 bytes**.

|Offset|Size|Field|
|--:|--:|---|
|0|4|Magic `"LTDB"`|
|4|2|Major version|
|6|2|Minor version|
|8|4|Header bytes = `256`|
|12|4|Endian tag = `0x01020304`|
|16|4|Database flags|
|20|4|Chunk directory count|
|24|8|Chunk directory offset|
|32|16|Deterministic Database ID|
|48|8|Reference radius, meters|
|56|8|Elevation origin, meters|
|64|8|Elevation step, meters|
|72|2|Tile cells = `256`|
|74|2|Core vertices = `257`|
|76|1|Apron = `1`|
|77|1|Maximum emitted level|
|78|1|Projection ID|
|79|1|Quantization ID|
|80|8|Tile count|
|88|4|Dataset count|
|92|4|Pack count|
|96|32|Builder configuration SHA-256|
|128|32|Dataset registry SHA-256|
|160|32|Tile-index SHA-256|
|192|32|Database-content SHA-256|
|224|32|Reserved|

Current enums:

```cpp
ProjectionID:
    1 = LunarQSC_v1

QuantizationID:
    1 = Global_U16_0p5m
```

The header contains no creation timestamp so the canonical build can be byte-reproducible.

`DatabaseID` is deterministically derived from canonical configuration plus the ordered source registry rather than randomly generated.

---

# 9. Chunk directory

Immediately after the header is normally a chunk directory, although the header carries its explicit offset.

Each entry is exactly **40 bytes**:

|Field|Type|
|---|---|
|FourCC tag|`char[4]`|
|Chunk version|`uint16`|
|Flags|`uint16`|
|File offset|`uint64`|
|Stored bytes|`uint64`|
|Logical bytes|`uint64`|
|CRC32C|`uint32`|
|Reserved|`uint32`|

Mandatory v1 chunks:

```text
DSET    source dataset registry
PACK    terrain-pack registry
TIDX    tile index
STRS    string table
META    extended metadata
```

Unknown chunk types must be ignored when the file major version remains compatible.

This provides forward extension without changing the fixed header.

---

# 10. String table

`STRS` contains deduplicated UTF-8 strings.

String ID is a **byte offset inside STRS**, not an array index.

Representation:

```text
uint32 ByteLength
uint8  UTF8[ByteLength]
padding to 4-byte boundary
```

Offset `0` represents an empty string.

Strings are sorted deterministically before serialization.

---

# 11. Dataset registry

Every scientific source product receives a stable `DatasetID`.

A suggested stable source key is:

```text
nasa.sldem2015.512ppd.v1
```

`DatasetID` is derived from the first 32 bits of:

```text
SHA256(
    "LTDB_DATASET_V1"
    +
    StableDatasetKey
)
```

The builder must detect a collision and fail rather than silently remap IDs.

New scientific product revisions receive new stable source keys/IDs, preserving historical provenance.

## 11.1 Dataset record

Each `DSET` record is exactly **128 bytes**.

|Field|Type|
|---|---|
|Dataset ID|`uint32`|
|Flags|`uint32`|
|Product-name StringID|`uint32`|
|Producer StringID|`uint32`|
|Mission StringID|`uint32`|
|Instrument StringID|`uint32`|
|Version StringID|`uint32`|
|Source URI StringID|`uint32`|
|Original CRS StringID|`uint32`|
|License StringID|`uint32`|
|Nominal resolution m|`double`|
|Horizontal accuracy m|`double`|
|Vertical accuracy m|`double`|
|Source no-data value|`double`|
|Original source bytes|`uint64`|
|Original source SHA-256|`uint8[32]`|
|META offset|`uint32`|
|META bytes|`uint32`|
|Quality schema ID|`uint32`|
|Reserved|`uint32`|

`META` holds uncommon source-specific metadata as canonical UTF-8 JSON.

The complete source SHA-256 makes scientific provenance traceable back to an exact input artifact.

This matters for SLDEM-style products because the initial source material indicates that source pixels may encode lunar radius directly and carry separate labels/quality information rather than already representing the project's normalized elevation convention.

---

# 12. Pack registry

Each `PACK` entry is **80 bytes**.

|Field|Type|
|---|---|
|Pack ID|`uint32`|
|Flags|`uint32`|
|Path StringID|`uint32`|
|Default codec|`uint16`|
|Reserved|`uint16`|
|Tile count|`uint64`|
|File bytes|`uint64`|
|First TileKey|`uint64`|
|Last TileKey|`uint64`|
|Pack SHA-256|`uint8[32]`|

A pack is independently hashable and replaceable.

---

# 13. Tile index

`TIDX` consists of fixed-size **80-byte records**, sorted ascending by `TileKey`.

This permits binary search or direct memory mapping.

|Offset|Field|Type|
|--:|---|---|
|0|TileKey|`uint64`|
|8|Pack ID|`uint32`|
|12|Flags|`uint32`|
|16|Payload offset|`uint64`|
|24|Stored bytes|`uint32`|
|28|Logical bytes|`uint32`|
|32|Minimum U16 elevation code|`uint16`|
|34|Maximum U16 elevation code|`uint16`|
|36|Primary DatasetID|`uint32`|
|40|Effective resolution, mm|`uint32`|
|44|Geometric error, mm|`uint32`|
|48|Materialized child mask|`uint8`|
|49|Channel count|`uint8`|
|50|Reserved|`uint16`|
|52|Payload CRC32C|`uint32`|
|56|Content SHA-256 prefix|`uint8[16]`|
|72|Dependency SHA-256 prefix|`uint8[8]`|

`ChildMask` uses four bits corresponding to the four possible quadtree children.

No explicit parent pointer is required because the parent follows directly from `TileKey`.

---

# 14. `.ltp` pack format

Each Lunar Terrain Pack begins with a 64-byte header.

## 14.1 Pack header

|Offset|Size|Field|
|--:|--:|---|
|0|4|`"LTPK"`|
|4|2|Major|
|6|2|Minor|
|8|4|Header bytes = 64|
|12|4|Endian tag|
|16|4|Pack ID|
|20|4|Flags|
|24|8|Tile count|
|32|8|Payload region offset|
|40|8|File bytes|
|48|16|SHA-256 prefix|
|64|—|Tile payloads begin|

`.ltdb/TIDX` remains the authoritative random-access directory.

Each tile payload nevertheless contains its own TileKey so a pack can be inspected or partially recovered independently.

---

# 15. Tile payload

Every tile begins with a fixed **128-byte `LTIL` header**.

## 15.1 Tile header

|Field|Type|
|---|---|
|Magic `"LTIL"`|`char[4]`|
|Version|`uint16`|
|Header bytes = `128`|`uint16`|
|TileKey|`uint64`|
|Flags|`uint32`|
|Channel count|`uint16`|
|Tile cells|`uint16`|
|Core vertices|`uint16`|
|Apron|`uint8`|
|Encoding profile|`uint8`|
|Effective resolution m|`float`|
|Geometric error m|`float`|
|Minimum elevation m|`float`|
|Maximum elevation m|`float`|
|Primary DatasetID|`uint32`|
|Provenance palette count|`uint16`|
|Reserved|`uint16`|
|Channel directory offset|`uint32`|
|Channel directory bytes|`uint32`|
|Data region offset|`uint32`|
|Data region bytes|`uint32`|
|Dependency hash prefix|`uint8[16]`|
|Content hash prefix|`uint8[16]`|
|Reserved|`uint8[28]`|

---

# 16. Channel model

A tile is a collection of independently encoded channels:

```text
LTIL
│
├── Channel directory
│
├── ELEV
│
├── PRVN
├── QUAL      optional
│
└── future channels
```

Each channel-directory record is **40 bytes**:

```cpp
struct ChannelRecordV1
{
    uint16 ChannelID;
    uint16 Version;

    uint8  ElementType;
    uint8  Components;
    uint8  Codec;
    uint8  Predictor;

    uint16 Width;
    uint16 Height;

    uint32 Flags;

    uint32 DataOffset;
    uint32 StoredBytes;
    uint32 LogicalBytes;

    uint32 CRC32C;

    uint32 Parameter0;
    uint32 Parameter1;
};
```

Initial channel IDs:

```text
0x0001    ELEV    elevation
0x0002    PRVN    provenance
0x0003    QUAL    quality/fusion flags
```

Initial element types:

```text
1   U8
2   U16
3   I16
4   U32
5   F32
6   F64
255 opaque structured blob
```

Initial codecs:

```text
0   none
1   Zstandard
```

Initial predictors:

```text
0   none
1   Delta2D_U16
```

The codec ID is deliberately part of every channel so a future format revision can mix codecs without changing the enclosing database.

---

# 17. Elevation encoding

## 17.1 Primary v1 encoding

Use a global database-wide quantization:

```text
ElevationOrigin = -16,384.0 m
ElevationStep   = 0.5 m
```

This represents:

```text
-16,384.0 m
through
+16,383.5 m
```

using U16.

Encoding:

```cpp
uint16 EncodeElevation(double elevation)
{
    return round(
        (elevation + 16384.0) / 0.5
    );
}
```

Decoding:

```cpp
double elevation =
    -16384.0
    +
    double(q) * 0.5;
```

The builder must scan every input product and fail if a measured elevation exceeds this range.

The existing DEM discussion places the relevant lunar elevation range comfortably inside a roughly −9 km to +11 km range, so this representation provides substantial safety margin while retaining finer vertical quantization than the broad DEM's scientific precision.

## 17.2 High-precision escape profile

The format also reserves:

```text
EncodingProfile 2:

I32 millimeters relative to reference radius
```

for future scientific products whose precision would make 0.5 m quantization undesirable.

The initial database should not require it.

---

# 18. Reversible elevation predictor

Before Zstandard compression, U16 terrain values pass through a deterministic 2D predictor.

For sample `S(x,y)`:

```text
(0,0):
    predicted = 0

first row:
    predicted = left

first column:
    predicted = above

interior:
    predicted =
        left
        + above
        - upperLeft
```

Residual:

```cpp
uint16 residual =
    uint16(sample - predicted);
```

Arithmetic is modulo `2^16`, making the operation exactly reversible.

Decoding reverses the same recurrence.

This is intentionally simple. More advanced predictors are a later benchmarking concern.

Every tile remains independently decodable.

There is **no parent-tile dependency** in the elevation codec.

---

# 19. Provenance channel

`PRVN` records both tile-wide contributing sources and an optional coarse spatial source map.

Suggested payload:

```text
ProvenanceHeader
    Version
    PaletteCount
    MapWidth
    MapHeight
    IndexWidth
    Flags

Palette[PaletteCount]

Optional spatial map
```

Prototype map resolution:

```text
64 × 64
```

A palette record:

```cpp
struct ProvenancePaletteEntry
{
    uint32 DatasetID;
    uint32 Flags;

    float ContributionFraction;
    float NativeResolutionMeters;
};
```

If one dataset supplies the whole tile:

```text
PaletteCount = 1
no spatial map required
```

For fused tiles:

```text
Palette:

0 = global LOLA
1 = SLDEM2015
2 = regional high-resolution DTM

Map:

0000000011111111
0000000111111111
0000001222222222
...
```

The map represents the **dominant measured contributor**, not synthetic PCG.

PCG does not alter `.ltdb`; its provenance belongs to the later Unreal-derived terrain layer.

---

# 20. Quality channel

`QUAL` is optional.

Initial representation:

```text
64 × 64 U8
```

Suggested bit meanings:

```text
bit 0    source product marks interpolated sample
bit 1    builder filled source no-data
bit 2    DEM fusion transition region
bit 3    low-frequency datum/bias correction applied
bit 4    lower-confidence region
bit 5-7  reserved
```

This is particularly useful because the referenced source discussion identifies an accompanying SLDEM data-quality layer.

---

# 21. Tile dependency identity

Every tile build receives two hashes.

## Dependency hash

```text
SHA256(
    "LTDB_TILE_DEP_V1"

    TileKey

    datum specification
    QSC implementation version
    tile schema version
    quantization configuration
    fusion algorithm version
    fusion configuration

    sorted source DatasetIDs
    sorted source SHA256 hashes
    relevant source windows

    builder algorithm version
)
```

If this hash is unchanged, the previous tile is reusable.

## Content hash

After all channels have been built:

```text
SHA256(
    TileKey
    +
    canonical uncompressed channel directory
    +
    uncompressed channels in ChannelID order
)
```

This separates:

```text
"should rebuilding this tile be necessary?"
```

from:

```text
"did rebuilding actually change its result?"
```

---

# 22. Build cache

Incremental state should **not** live inside `.ltdb`.

Use:

```text
.ltbuild/
    cache.sqlite
    staging/
```

`cache.sqlite` records approximately:

```text
DatasetID
SourceSHA256

TileKey
DependencySHA256
ContentSHA256

BuildState
StagingPath

PreviousPackID
PreviousPackOffset
```

This is disposable build infrastructure.

Deleting `.ltbuild` must never destroy information required to understand an existing `.ltdb`.

---

# 23. Builder configuration

A prototype configuration could resemble:

```toml
[database]
name = "Moon"
format_major = 1
format_minor = 0

[datum]
reference_radius_m = 1737400.0
elevation_origin_m = -16384.0
elevation_step_m = 0.5

[projection]
type = "qsc"
version = 1

[tiles]
cells = 256
apron = 1
max_level = 13

[packaging]
target_pack_bytes = 1073741824
codec = "zstd"

[fusion]
algorithm = "residual_refinement_v1"
transition_width_samples = 32

[[dataset]]
key = "global-base"
role = "base"
priority = 100
path = "..."

[[dataset]]
key = "sldem2015-512ppd"
role = "refinement"
priority = 200
path = "..."

[[dataset]]
key = "regional-dtm-example"
role = "refinement"
priority = 300
path = "..."
```

Configuration is parsed into a canonical internal representation before hashing; comments, whitespace and textual key ordering therefore do not influence `BuilderConfigHash`.

---

# 24. Builder pipeline

The executable should implement the following explicit DAG.

```text
Catalog
   ↓
Validate Sources
   ↓
Normalize Metadata
   ↓
Build Coverage Index
   ↓
Plan Sparse QSC Hierarchy
   ↓
Build Core Samples
   ↓
Fuse Overlapping DEMs
   ↓
Resolve Same-Level Edges
   ↓
Quantize
   ↓
Build Aprons
   ↓
Build Provenance / Quality
   ↓
Encode Channels
   ↓
Write Packs
   ↓
Write LTDB
   ↓
Full Validation
```

## Stage 1 — Catalog

For every source:

- open with GDAL;
    
- parse raster dimensions;
    
- resolve georeferencing;
    
- inspect no-data handling;
    
- obtain source datum/projection;
    
- calculate SHA-256;
    
- capture source metadata;
    
- assign deterministic DatasetID.
    

GDAL's JP2OpenJPEG driver exposes georeferencing from several JPEG-2000 metadata sources and supports virtual I/O and threaded decoding.

No terrain is generated yet.

## Stage 2 — Source validation

Reject or explicitly override:

```text
unknown datum
missing projection
ambiguous radius/elevation convention
invalid pixel scale
unexpected sample type
incomplete sidecar metadata
unresolved no-data convention
```

Every override is written into dataset metadata.

Silent assumptions are prohibited.

## Stage 3 — Coverage index

Convert each dataset footprint to canonical lunar coordinates.

Generate:

```cpp
DatasetCoverage
{
    DatasetID;

    spherical footprint;

    nominal resolution;
    usable resolution;

    priority;
    fusion policy;
}
```

The coverage index answers:

```text
Which scientific datasets intersect QSC tile T?
```

without reading their raster contents.

## Stage 4 — Sparse hierarchy planning

Determine target maximum quadtree level for every source from measured source resolution and QSC distortion.

The rule is:

> choose the coarsest level whose worst-case sample spacing over that source footprint does not underrepresent the source's effective resolution.

Then materialize only the QSC nodes justified by source coverage.

All required ancestors are emitted.

This guarantees a connected sparse hierarchy.

---

# 25. Core tile generation

Each tile first produces only:

```text
257 × 257 floating-point core elevations
```

No apron yet.

For every lattice coordinate:

```text
TileKey
    ↓
face-grid coordinate
    ↓
normalized QSC (u,v)
    ↓
QSC inverse
    ↓
lunar lat/lon
    ↓
CanonicalSampler
    ↓
double elevation
```

All geodesy and DEM fusion operates in binary64.

Quantization occurs only after fusion and seam resolution.

---

# 26. `CanonicalSampler`

The central builder abstraction is:

```cpp
class ICanonicalTerrainSampler
{
public:

    TerrainSample Sample(
        LunarGeodeticCoordinate coordinate,
        const SamplingContext& context);
};
```

Result:

```cpp
struct TerrainSample
{
    double ElevationMeters;

    DatasetID PrimarySource;

    SmallVector<SourceContribution> Contributions;

    uint32 QualityFlags;
};
```

This abstraction is significant.

Neither tile generation nor the Unreal adapter should understand SLDEM, LOLA or LROC file formats.

They request:

```text
the canonical terrain at this lunar coordinate
```

and the fusion subsystem decides how that answer is produced.

---

# 27. DEM fusion engine

Fusion is intentionally pluggable:

```cpp
enum class EFusionPolicy
{
    Replace,
    ResidualRefinement,
    BiasCorrectedReplace
};
```

The initial default for high-resolution regional data is:

```text
ResidualRefinement_v1
```

Conceptually:

```text
coarse authoritative terrain C

fine measured terrain F

low-frequency component:
    LF = LowPass(F)

fine residual:
    R = F - LF

canonical result:
    H = C + wR
```

where `w` smoothly approaches zero across the fine-data transition region.

This prevents a hard high/low-resolution boundary while retaining measured higher-frequency structure.

The important implementation requirement is that the filter kernel, transition function and source ordering are all versioned and hashed.

A future investigation may show that particular scientific products should instead use `BiasCorrectedReplace`, allowing their low-frequency geometry to supersede the global base. That does **not** require a file-format change.

---

# 28. Deterministic floating-point rules

The prototype guarantees reproducibility for:

```text
same source bytes
same configuration
same builder version
same pinned GDAL/PROJ versions
same supported build toolchain
```

Requirements:

```text
binary64 geodesy and fusion

no fast-math compiler option

fixed filter kernels

fixed iteration ordering

sources sorted by DatasetID/priority

no parallel floating-point reductions whose ordering
depends on worker scheduling
```

Tile tasks themselves may execute in parallel.

Packing occurs later in deterministic TileKey order.

Cross-platform bit-identical scientific builds should be treated as a validation target rather than assumed initially.

---

# 29. Seam resolution

There are separate cases.

## Same face, same level

Shared QSC lattice coordinates are intrinsically identical.

After both tile cores exist:

```text
lowest TileKey = edge owner
```

The owner's binary64 edge is copied into the neighbor.

## Different QSC faces

The same ownership rule applies.

The builder's face-topology module determines corresponding edge orientation and possible index reversal.

One edge is authoritative.

```text
owner edge
    ↓
neighbor edge overwritten
```

Cube corners similarly have one lowest-TileKey owner.

## Source-data seam

The fusion engine handles broad transition continuity.

The seam resolver is **not** a substitute for DEM fusion.

It guarantees only that the final shared boundary itself is identical.

Sequence:

```text
DEM fusion
    ↓
source-transition smoothing
    ↓
boundary ownership
    ↓
quantization
```

The database invariant becomes:

> Every same-level shared tile boundary contains identical quantized elevations.

---

# 30. Apron generation

The one-sample apron is generated **after** core seam resolution and quantization.

For an ordinary neighbor:

```text
tile A apron
    =
corresponding interior/core row from tile B
```

This means slope, curvature and normal calculations can operate locally without requiring another pack read.

At a sparse-refinement boundary where no same-level neighboring tile exists, the builder requests a **virtual neighboring sample strip** from `CanonicalSampler` using the same target sample spacing.

No fake child tile is created merely to provide an apron.

At cube-face boundaries, apron samples are obtained through the explicit QSC face-topology mapping; QSC coordinates are never blindly extended outside `[-1,+1]`.

---

# 31. Geometric-error metadata

For every non-root tile, calculate:

```text
maximum difference between:

this tile's surface

and

the reconstruction obtainable from its
nearest materialized ancestor
```

Store this as:

```text
GeometricErrorMeters
```

This is not itself an Unreal LOD decision.

It gives the later Unreal importer quantitative information about how much geometric information would be lost if a finer scientific node were omitted.

---

# 32. Packing

After tiles are finalized:

```text
sort by:

Face
Level
Morton
```

For each `(Face,Level)`:

```text
append tiles to current .ltp

if target size would exceed ~1 GiB:
    close pack
    hash pack
    open next pack
```

Each tile channel is individually compressed.

No compression dictionary is shared across tiles in v1.

This deliberately preserves independent decoding and makes incremental pack rewriting simpler.

---

# 33. Database finalization

Once packs exist:

1. build `PACK`;
    
2. build sorted `TIDX`;
    
3. build `DSET`;
    
4. construct `STRS`;
    
5. serialize `META`;
    
6. compute chunk CRCs;
    
7. compute pack hashes;
    
8. compute dataset-registry hash;
    
9. compute tile-index hash;
    
10. compute final database-content hash;
    
11. write deterministic header.
    

No build-time timestamps enter the canonical hash.

---

# 34. Proposed command line

The initial CLI should expose:

```text
lunar-terrain scan
lunar-terrain plan
lunar-terrain build
lunar-terrain validate
lunar-terrain inspect
lunar-terrain diff
lunar-terrain export
```

Examples:

```bash
lunar-terrain scan lunar-terrain.toml
```

Displays source metadata, resolution, coverage, datum and potential problems.

```bash
lunar-terrain plan lunar-terrain.toml
```

Produces:

```text
planned tile count
tiles per face/level
source overlap statistics
estimated uncompressed bytes
estimated build workload
```

without actually generating terrain.

```bash
lunar-terrain build lunar-terrain.toml --incremental
```

Builds only nodes whose dependency hashes changed.

```bash
lunar-terrain validate Moon.ltdb --full
```

Runs complete structural/scientific/seam validation.

```bash
lunar-terrain inspect Moon.ltdb QSC/F2/L08/0147/0092
```

Reports:

```text
tile geometry
parent/children
source datasets
resolution
elevation range
quality flags
hashes
pack location
```

```bash
lunar-terrain diff Moon_A.ltdb Moon_B.ltdb
```

Reports which:

```text
datasets changed
tiles changed
elevation content changed
provenance changed
```

This will become particularly useful when scientific datasets are revised.

---

# 35. Core C++ interfaces

A prototype `LunarTerrainBuilder` source tree should resemble:

```text
Source/
    Core/
        LunarTileKey.h
        LunarCoordinates.h
        Hashing.h
        BinaryIO.h

    Projection/
        QscProjection.h
        QscTopology.h

    Sources/
        RasterSource.h
        GdalRasterSource.cpp
        DatasetCatalog.cpp

    Build/
        CoverageIndex.cpp
        TilePlanner.cpp
        CanonicalSampler.cpp
        FusionEngine.cpp
        SeamResolver.cpp
        ApronBuilder.cpp
        TileEncoder.cpp
        DatabaseWriter.cpp

    Validation/
        DatabaseValidator.cpp
        ProjectionValidator.cpp
        SeamValidator.cpp

    Cli/
        Main.cpp
```

Core interfaces:

```cpp
class IRasterSource
{
public:
    DatasetID GetDatasetID() const;

    SourceMetadata GetMetadata() const;

    bool Covers(
        LunarGeodeticCoordinate p) const;

    RawTerrainSample Sample(
        LunarGeodeticCoordinate p) const;
};
```

```cpp
class TilePlanner
{
public:
    TileBuildPlan Plan(
        LunarTileKey key,
        const CoverageIndex& coverage);
};
```

```cpp
class FusionEngine
{
public:
    TerrainSample Resolve(
        LunarGeodeticCoordinate coordinate,
        std::span<const SourceSample> sources,
        const FusionContext& context);
};
```

```cpp
class TileBuilder
{
public:
    CanonicalTile BuildCore(
        const TileBuildPlan& plan);

    void ResolveEdges(
        CanonicalTile& tile);

    EncodedTile Finalize(
        CanonicalTile&& tile);
};
```

```cpp
class LunarTerrainDatabaseWriter
{
public:
    void AddDataset(...);
    void AddTile(...);
    void Finalize(...);
};
```

A separate read-only library exposes:

```cpp
class LunarTerrainDatabase
{
public:
    static LunarTerrainDatabase Open(path);

    const DatabaseHeader& Header() const;

    const TileIndexEntry*
        FindTile(LunarTileKey key) const;

    DecodedTerrainTile
        ReadTile(LunarTileKey key) const;

    void EnumerateChildren(...);

    void EnumerateRegion(...);
};
```

---

# 36. Unreal Editor boundary

The Unreal plugin should consume the database through a small adapter API.

Conceptually:

```cpp
FLunarTerrainRegionRequest
{
    spherical bounds;

    desired scientific resolution;

    optional max QSC level;
};
```

The plugin asks the database for the finest scientifically justified sparse leaf coverage of that region.

```text
LTDB leaf set
     ↓
adaptive mesh builder
     ↓
Moon-centered Cartesian vertices
     ↓
watertight local surface mesh
     ↓
Mesh Partition base
```

The scientific tile itself must **not** automatically become an Unreal Actor or World Partition cell.

Instead:

```text
many LTDB tiles
      ↓
one terrain build region
      ↓
Mesh Partition sections
      ↓
compiled sections
      ↓
World Partition
```

This maintains the architectural separation between:

```text
scientific spatial subdivision
```

and:

```text
engine runtime streaming subdivision
```

Epic's current Mesh Terrain tooling can convert arbitrary Static Mesh geometry into a Mesh Partition, can automatically split converted geometry into base sections, and supports very large base creation workflows that save/unload sections while building.

For the prototype, therefore, the lowest-risk import path is:

```text
LTDB
 ↓
FDynamicMesh / temporary Static Mesh
 ↓
Mesh Terrain Convert Mesh
```

rather than attempting to immediately depend on undocumented internal Mesh Partition construction APIs.

A second prototype should investigate bypassing the temporary Static Mesh stage once the public/plugin API surface has been evaluated.

---

# 37. Scientific LOD versus rendering LOD

These must remain separate concepts.

## Scientific hierarchy

```text
L7 global DEM
L8 SLDEM
L12 regional DTM
```

means:

> progressively better measured information exists.

## Mesh Terrain LOD

means:

> simplify already-selected geometry for efficient rendering.

Mesh Terrain's Static Mesh Transformer can create platform-specific LOD/Nanite representations and its simplification system exposes world-space geometric-error tolerances.

Therefore the Unreal adapter should first produce a surface from the **best available scientific leaves**.

Only afterward should Mesh Terrain produce:

```text
render LOD 0
render LOD 1
render LOD 2
...
```

Do not map:

```text
LTDB L8 = render LOD 0
LTDB L7 = render LOD 1
```

directly.

Those hierarchies express fundamentally different information.

---

# 38. Sparse-resolution boundaries in Unreal

A region containing:

```text
L8 scientific terrain
beside
L12 scientific terrain
```

must produce one topologically compatible mesh.

The proposed Unreal adapter therefore performs a **2:1 balancing pass** over the selected scientific leaf set.

Where neighboring scientific nodes differ by more than one level, the coarse side may be subdivided geometrically:

```text
coarse scientific data
        ↓
interpolated topology subdivision
        ↓
compatible mesh density
```

This subdivision does **not** create a new `.ltdb` scientific node.

It is marked as:

```text
Derived / interpolated geometry
```

and exists solely in the Unreal realization layer.

This distinction protects the meaning of the sparse terrain database.

---

# 39. Deterministic geological PCG handoff

`.ltdb` ends at the scientific/derived DEM surface.

The downstream Unreal build fingerprint should be conceptually:

```text
SHA256(
    LTDB DatabaseContentHash
    +
    imported TileKeys
    +
    LunarTerrainEditor version
    +
    Mesh Partition Definition version
    +
    geological PCG graph versions
    +
    world geological seed
)
```

Thus:

```text
scientific source revision
       OR
fusion algorithm revision
       OR
PCG revision
       OR
Mesh Terrain build-definition revision

                ↓

affected Unreal terrain region becomes stale
```

This gives the editor tooling a principled basis for selective rebuilds.

---

# 40. Shipping compression boundary

The `.ltp` Zstandard compression described above exists for the **engine-independent canonical database and build cache**.

Once terrain has been transformed into Unreal assets, it should not remain manually precompressed for shipping.

Epic's UE 5.8 Oodle API documentation explicitly recommends allowing pak/IoStore to choose disk compression rather than manually compressing data that will be stored in shipping packages.

The shipping flow is therefore:

```text
.ltdb / .ltp
Zstd
development/build artifact

        ↓ import

Unreal asset data
uncompressed logical asset representation

        ↓ cook

IoStore / platform compression
```

---

# 41. Validation suite

`lunar-terrain validate --full` must check several classes of invariant.

## Structural

```text
valid LTDB/LTP versions
valid endian tags
valid chunk bounds
valid pack bounds
sorted unique TileKeys
valid parent-child relationships
no illegal Morton bits
valid channel offsets
valid CRCs
valid SHA hashes
```

## Projection

Generate a large deterministic test set and verify:

```text
lat/lon
 → QSC
 → lat/lon
```

within the declared tolerance.

Prototype testing should compare the project's QSC wrapper directly against PROJ's documented implementation. PROJ supports both forward and inverse QSC transformations and defines the six face configurations explicitly.

## Scientific

```text
all samples finite
no undeclared no-data
elevation within encoding range
source coverage consistent with provenance
nominal/effective resolutions valid
```

## Seam

For every materialized same-level neighbor:

```text
edge U16 arrays must compare byte-for-byte
```

No epsilon test.

## Provenance

```text
all DatasetIDs exist
all palette indices valid
PrimaryDataset exists in palette
all contributing-source hashes available
```

## Hierarchy

```text
every non-root tile has a materialized ancestor
child mask matches actual index
no orphan tile
```

## Determinism

A CI test builds a representative region twice from an empty cache.

Required result:

```text
identical .ltdb SHA-256
identical .ltp SHA-256
```

for the pinned build environment.

---

# 42. Debugging/export support

The builder should support exporting any terrain region as:

```text
OBJ
PLY
GeoTIFF-equivalent diagnostic raster
CSV sample set
raw U16
provenance visualization
quality visualization
```

These are debugging products only.

For example:

```bash
lunar-terrain export Moon.ltdb \
    --tile QSC/F2/L08/0147/0092 \
    --format ply
```

would reconstruct actual Moon-centered Cartesian geometry.

A provenance debug image could expose:

```text
blue     global base
green    SLDEM
yellow   regional DTM
orange   fusion transition
```

so source behavior can be inspected before Unreal ever becomes involved.

---

# 43. Prototype implementation sequence

The first implementation should deliberately avoid trying to build the entire Moon.

### Prototype P0 — projection and binary container

Implement:

```text
TileKey
QSC conversion
LTDB header
chunk directory
TIDX
one LTP pack
ELEV channel
database reader
```

Generate synthetic spherical terrain rather than DEM data.

Success condition:

```text
six complete QSC faces
no cracks
stable TileKeys
byte-identical rebuild
```

### Prototype P1 — one real DEM source

Add:

```text
GDAL source adapter
scientific datum normalization
one SLDEM/LOLA source region
U16 quantization
Zstd channel compression
```

Success:

```text
source → LTDB → reconstructed mesh
```

with measured elevation spot checks.

### Prototype P2 — face boundaries and aprons

Build regions crossing:

```text
equatorial face edge
polar face edge
cube corner
```

Verify edge identity and apron correctness.

### Prototype P3 — heterogeneous DEM fusion

Use:

```text
coarse DEM
+
higher-resolution overlapping DTM
```

Implement `ResidualRefinement_v1`.

Produce:

```text
provenance map
quality map
transition diagnostics
```

and measure seam derivatives.

### Prototype P4 — Unreal importer

Read a small LTDB region.

Generate Moon-centered spherical mesh geometry.

Feed it into Mesh Terrain's currently documented Convert Mesh workflow. Epic documents Convert Mesh as one of the three supported methods of establishing Mesh Partition base geometry.

Validate:

```text
Mesh Partition sections
collision
Mesh Terrain modifier
PCG read/write
compiled sections
```

PCG can currently read and write Mesh Partition data, including terrain-deforming use cases such as erosion.

### Prototype P5 — sparse hierarchy

Construct:

```text
L8 region
adjacent L10 region
embedded L12 region
```

Validate:

```text
scientific sparse hierarchy
2:1 importer balancing
Mesh Terrain topology
no cracks
LOD simplification
```

### Prototype P6 — streaming-scale benchmark

Only after P0–P5 succeed should a sufficiently large region be generated to measure:

```text
editor import throughput
Mesh Terrain build throughput
compiled asset size
World Partition cell count
cook size
runtime streaming latency
collision cost
```

These measurements should determine the eventual mapping between LTDB tiles, Mesh Partition base sections and World Partition cells.

---

# 44. Decisions now sufficiently concrete to lock for the prototype

The prototype should proceed with:

```text
Projection
    QSC

Scientific tile
    256 × 256 cells

Core lattice
    257 × 257

Apron
    1 sample

Elevation
    global U16
    0.5 m increments
    -16,384 m origin

Tile identifier
    64-bit Face/Level/Morton

Hierarchy
    sparse
    approximately L0–L13 initially

Database
    Moon.ltdb

Payload
    *.ltp

Index
    fixed 80-byte Morton-sorted records

Elevation codec
    Delta2D → Zstandard

Integrity
    CRC32C + SHA-256-derived identity hashes

Source identity
    full SHA-256

Provenance
    tile palette + optional 64×64 source map

Quality
    optional 64×64 U8 mask

Build
    deterministic, incremental, offline

Runtime mutation
    none

Unreal boundary
    LTDB → adaptive spherical mesh
         → Mesh Partition
         → deterministic PCG
         → compiled Mesh Terrain
         → IoStore
```

---

# 45. Items intentionally not locked yet

The following should now be answered through prototypes rather than further abstract design:

```text
Zstandard compression level

exact DEM fusion filter kernel

transition-band width

whether 64×64 provenance masks are sufficient

1 GiB versus smaller LTP packs

best Mesh Partition section size

best LTDB→Mesh Partition region grouping

whether temporary Static Mesh conversion can
be replaced with a direct Mesh Partition API

Nanite settings by platform

collision simplification tolerances

World Partition runtime-grid dimensions

maximum ahead-of-train streaming distance
```

These are performance or implementation variables rather than foundational data-model questions.

The architecture no longer depends on choosing them in advance.

---

# 46. Resulting Part One architecture

At this point the complete scientific build chain can be expressed as:

```text
PDS / JP2 / mission DEM products
            │
            ▼
       DatasetCatalog
            │
            ▼
     GDAL source adapters
            │
            ▼
  Canonical lunar geodesy
            │
            ▼
      CoverageIndex
            │
            ▼
   Sparse QSC TilePlanner
            │
            ▼
     CanonicalSampler
            │
      ┌─────┴─────┐
      │           │
      ▼           ▼
 coarse DEM    fine DEM
      │           │
      └─────┬─────┘
            ▼
       FusionEngine
            │
            ▼
      257² core tile
            │
            ▼
      SeamResolver
            │
            ▼
        Quantizer
            │
            ▼
       ApronBuilder
            │
      ┌─────┼─────────┐
      ▼     ▼         ▼
    ELEV   PRVN      QUAL
      └─────┼─────────┘
            ▼
       TileEncoder
            │
            ▼
      Morton packing
            │
       ┌────┴────┐
       ▼         ▼
   Moon.ltdb    *.ltp
       │
       ▼
LunarTerrainEditor
       │
       ▼
adaptive spherical mesh
       │
       ▼
Mesh Partition scientific base
       │
       ▼
deterministic geological PCG
       │
       ▼
Mesh Terrain compiled sections
       │
       ▼
World Partition / IoStore
```

This is sufficiently concrete to begin implementing **P0–P2 without making additional architectural decisions**.
