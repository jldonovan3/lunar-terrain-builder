# Lunar Terrain Database and Pack Format v1.0

## 1. Status and authority

This document is the byte-level contract for `.ltdb` and `.ltp` major version 1, minor version 0. It refines the storage model in [`01_LunarTerrainBuilder_Architecture_v0.3.md`](01_LunarTerrainBuilder_Architecture_v0.3.md). If an illustrative table in the architecture omits a byte offset, framing rule, hash domain, or compatibility rule, this document controls.

The v1.0 writer emits only the canonical forms defined here. Readers may accept compatible later minor versions as described in section 3. Changes to field meaning, canonical ordering, hash coverage, or required decoding behavior require a new major version unless this document explicitly reserves the change.

## 2. Common representation

### 2.1 Primitive values

- All integers are unsigned little-endian unless a field explicitly says otherwise.
- Signed integers use two's-complement little-endian representation.
- `float32` and `float64` are IEEE-754 binary32 and binary64, serialized little-endian. Except for the explicit DSET unknown/sentinel representation in section 6.2, canonical writers reject non-finite values and write positive zero for values equal to zero.
- Byte offsets are relative to the beginning of the file, chunk, or tile named by the field.
- FourCC values and magic values are the four displayed ASCII bytes in display order.
- SHA-256 values are the 32 digest bytes in normal digest order. A prefix is the first stated number of those bytes.
- CRC32C is CRC-32C/Castagnoli with reflected polynomial `0x82F63B78`, initial value `0xFFFFFFFF`, and final XOR `0xFFFFFFFF`. It is the same value commonly named CRC-32C/iSCSI.
- Text is valid shortest-form UTF-8 without a byte-order mark. NUL is not permitted inside a stored string.
- Every reserved byte and every alignment byte is zero in canonical output. A v1.0 reader rejects nonzero reserved bytes in a known record.
- Native C/C++ object representations are never a wire representation.

### 2.2 Alignment and arithmetic

`Align8(n)` is the smallest multiple of 8 greater than or equal to `n`. The LTDB chunk directory, every LTDB chunk, every tile payload in a pack, the tile channel directory, and every stored channel begin at an 8-byte-aligned offset. `STRS` records use their separate 4-byte alignment rule.

Readers perform addition and multiplication with checked unsigned arithmetic before comparing a range with its containing range. A range `[offset, offset + bytes)` is valid only when the addition does not overflow and the result is at most the containing size. No two nonempty chunks, tile payloads, or stored channel ranges may overlap. Canonical files are at most `2^63 - 1` bytes.

### 2.3 Domain-separated hashes

A hash domain shown as a quoted name consists of the displayed ASCII bytes followed by one zero byte. Integers in hash input are encoded using the same fixed-width little-endian representation as the file format. Variable byte strings are framed as `uint64 byte_count` followed by exactly those bytes.

`ChunkHashFrame` is:

|Offset|Size|Field|
|--:|--:|---|
|0|4|FourCC|
|4|2|Chunk version|
|6|2|Chunk flags|
|8|8|Logical byte count|
|16|N|Exact logical chunk bytes|

## 3. Version and compatibility behavior

- A reader that implements major version 1 rejects any other major version with `UnsupportedVersion`.
- The v1.0 writer writes minor version 0, `HeaderBytes = 256`, and only flags defined here.
- A major-v1 reader may open a later minor version when the fixed header is 256 bytes, all required known records use supported versions, no unknown required chunk or channel exists, and no unknown flag changes a known record's interpretation.
- Chunk flag bit 0 is `Required`; bit 1 is `ContentIdentity`. Mandatory v1 chunks set both bits. An unknown chunk with `Required` set causes `UnsupportedFeature`; another unknown chunk may be ignored structurally. Any chunk with `ContentIdentity` set participates in database-content hashing even when its payload is not interpreted.
- Channel flag bit 0 is `Required`. An unknown channel with that bit set causes `UnsupportedFeature`; another unknown channel may be skipped after its range and CRC have been validated.
- Unknown bits in database, pack, tile, dataset, pack-record, tile-index, or known-channel flags cause `UnsupportedFeature`.
- Missing, duplicate, or version-incompatible mandatory chunks cause `InvalidFormat` or `UnsupportedVersion` as appropriate.

## 4. Lunar Tile Key

The 64-bit key is:

```text
bits 63..61  Face, 0 through 5
bits 60..56  Level, 0 through 28
bits 55..0   Morton code
```

Morton bit `2k` is X bit `k`; bit `2k + 1` is Y bit `k`. For level `L`, every Morton bit at or above `2L` is zero. X and Y are each less than `2^L`. A reader rejects unused nonzero Morton bits. Parent and child operations use the lowest two Morton bits as quadrant `x_bit | (y_bit << 1)`.

The canonical text form is `QSC/Ff/Lll/xxxx/yyyy`: face is one decimal digit, level is exactly two decimal digits, and X/Y use at least four digits with leading zeroes, expanding without leading zeroes when more than four digits are needed. Parsers reject non-canonical spellings.

The lattice numerator is calculated in signed arithmetic:

```text
gx          = uint64(X) * 256 + i
denominator = uint64(256) << Level
numerator   = int64(2 * gx) - int64(denominator)
u           = double(numerator) / double(denominator)
```

The same rule applies to Y/v. Converting `2 * gx - denominator` as unsigned is invalid because it underflows for negative u or v.

## 5. LTDB file

### 5.1 Canonical layout

The canonical v1.0 file layout is:

```text
offset 0                         LTDBHeaderV1, 256 bytes
offset 256                       chunk directory
Align8(end of directory)         first chunk
Align8(end of preceding chunk)   each later chunk
```

Directory entries are sorted by raw FourCC bytes. Mandatory chunks therefore appear as `DSET`, `META`, `PACK`, `STRS`, `TIDX`. The physical chunk bytes use the same order. There is no chunk wrapper outside the directory record.

Canonical v1 chunks are stored without chunk-level compression, so `StoredBytes == LogicalBytes`. Chunk CRC32C covers exactly the stored chunk bytes, excluding inter-chunk alignment. Chunks do not include alignment at their end.

### 5.2 LTDBHeaderV1 — 256 bytes

|Offset|Size|Field|Canonical v1.0 value or rule|
|--:|--:|---|---|
|0|4|Magic|`LTDB`|
|4|2|Major version|1|
|6|2|Minor version|0|
|8|4|Header bytes|256|
|12|4|Endian tag|`0x01020304`|
|16|4|Database flags|0|
|20|4|Chunk directory count|At least 5; at most 4096|
|24|8|Chunk directory offset|256|
|32|16|Database ID|Section 11.6|
|48|8|Reference radius, meters|Positive finite binary64; prototype `1737400.0`|
|56|8|Elevation origin, meters|Finite binary64; profile 1 is `-16384.0`|
|64|8|Elevation step, meters|Positive finite binary64; profile 1 is `0.5`|
|72|2|Tile cells|256|
|74|2|Core vertices|257|
|76|1|Apron samples|1|
|77|1|Maximum emitted level|0 through 28|
|78|1|Projection ID|1 (`LunarQSC_v1`)|
|79|1|Quantization ID|1 (`Global_U16_0p5m`)|
|80|8|Tile count|Number of TIDX records|
|88|4|Dataset count|Number of DSET records|
|92|4|Pack count|Number of PACK records|
|96|32|Builder configuration SHA-256|Section 11.2|
|128|32|Dataset registry SHA-256|Section 11.3|
|160|32|Tile-index SHA-256|Section 11.4|
|192|32|Database-content SHA-256|Section 11.5|
|224|32|Reserved|Zero|

The database header is not protected by a separate CRC. Readers validate its fixed fields and recompute the identity hashes when full validation is requested.

### 5.3 ChunkDirectoryEntryV1 — 40 bytes

|Offset|Size|Field|
|--:|--:|---|
|0|4|FourCC tag|
|4|2|Chunk version; 1 for v1 chunks|
|6|2|Flags; mandatory chunks use `0x0003`|
|8|8|File offset|
|16|8|Stored bytes|
|24|8|Logical bytes|
|32|4|CRC32C of stored bytes|
|36|4|Reserved; zero|

The complete directory range must fit the file, must not overlap any chunk, and must end before the first chunk. Each mandatory chunk appears exactly once.

## 6. LTDB chunks

### 6.1 STRS string table

`STRS` is a sequence of records:

|Relative offset|Size|Field|
|--:|--:|---|
|0|4|UTF-8 byte count|
|4|N|UTF-8 bytes|
|4+N|0..3|Zero padding to a 4-byte boundary|

A StringID is the byte offset of the record's length field. Record zero is always the four-byte empty-string record, so StringID 0 means empty text. All other strings are nonempty, unique, and sorted by unsigned UTF-8 byte sequence. References must point to the first byte of a record. String length is at most `2^32 - 1` and the complete record must fit STRS.

### 6.2 DSET dataset registry

DSET records are sorted by ascending DatasetID and IDs are unique.

#### DatasetRecordV1 — 128 bytes

|Offset|Size|Field|
|--:|--:|---|
|0|4|Dataset ID|
|4|4|Flags|
|8|4|Product-name StringID|
|12|4|Producer StringID|
|16|4|Mission StringID|
|20|4|Instrument StringID|
|24|4|Version StringID|
|28|4|Source URI StringID|
|32|4|Original CRS StringID|
|36|4|License StringID|
|40|8|Nominal resolution, meters|
|48|8|Horizontal accuracy, meters|
|56|8|Vertical accuracy, meters|
|64|8|Source no-data value|
|72|8|Ordered artifact-bundle byte total|
|80|32|Ordered artifact-bundle SHA-256|
|112|4|META byte offset|
|116|4|META byte count|
|120|4|Quality schema ID|
|124|4|Reserved; zero|

Dataset flag bit 0 means samples represent radius and require declared datum normalization; bit 1 means the source declares a no-data value. Other bits are reserved. Accuracy fields use positive finite meters or the canonical quiet-NaN binary64 bit pattern `0x7ff8000000000000` when the source does not state an accuracy. The no-data field uses that same canonical NaN when flag bit 1 is clear; otherwise it contains the source value, which may itself be NaN only when the exact canonical NaN is the product's declared sentinel.

DatasetID is the little-endian `uint32` represented by digest bytes 0 through 3 of:

```text
SHA256(domain "LTDB_DATASET_V1" + uint64(key_utf8_bytes) + key_utf8_bytes)
```

The stable key is nonempty, normalized Unicode NFC UTF-8 and is recorded in the dataset META object as `stable_key`. A collision between different keys is an error.

`ArtifactBundleBytes` is the sum of all member byte counts with checked `uint64` arithmetic. `ArtifactBundleSha256` is defined in section 11.1. Even a one-file product is represented as a one-member ordered bundle.

### 6.3 META canonical JSON

META concatenates zero or more canonical JSON documents without separators or implicit framing. Each DSET record points to its exact JSON byte range. Empty metadata is represented by byte count zero and offset zero. Nonempty documents are deduplicated, sorted by unsigned canonical UTF-8 bytes, and concatenated in that order; DSET offsets are assigned after sorting.

Canonical JSON is RFC 8785 JSON Canonicalization Scheme (JCS): valid I-JSON, no insignificant whitespace, deterministic object-member ordering and number serialization, and UTF-8 output. NaN and infinity are not JSON values. Product values that cannot be represented without loss are stored as strings with an explicit schema key.

Every source META object contains:

- `stable_key`: the stable DatasetID key;
- `artifact_members`: an array in artifact-bundle order;
- for every member: `name`, `bytes`, and lowercase 64-digit `sha256`;
- explicit datum, radius/elevation, no-data, and metadata-override declarations needed to interpret the source.

Member names are stable source-relative URI path strings, not machine-local paths.

### 6.4 PACK registry

PACK records are sorted by ascending PackID and IDs are unique. Canonical builders assign PackIDs sequentially from zero after sorting tiles by Face, Level, and Morton and applying deterministic rollover.

#### PackRecordV1 — 80 bytes

|Offset|Size|Field|
|--:|--:|---|
|0|4|Pack ID|
|4|4|Flags; zero|
|8|4|Path StringID|
|12|2|Default codec ID|
|14|2|Reserved; zero|
|16|8|Tile count|
|24|8|Pack file bytes|
|32|8|First TileKey|
|40|8|Last TileKey|
|48|32|Full Pack SHA-256|

The path is a nonempty portable relative UTF-8 path using `/`, with no empty, `.` or `..` segment and no drive/authority/root prefix. The default codec is advisory; each channel record remains authoritative. A nonempty pack contains one Face/Level group, has `FirstTileKey <= LastTileKey`, and its tile count agrees with TIDX.

### 6.5 TIDX tile index

TIDX records are sorted by ascending encoded TileKey and keys are unique.

#### TileIndexRecordV1 — 80 bytes

|Offset|Size|Field|
|--:|--:|---|
|0|8|TileKey|
|8|4|Pack ID|
|12|4|Flags|
|16|8|Payload file offset|
|24|4|Stored payload bytes|
|28|4|Logical channel bytes|
|32|2|Minimum U16 elevation code|
|34|2|Maximum U16 elevation code|
|36|4|Primary DatasetID|
|40|4|Effective resolution, millimeters|
|44|4|Geometric error, millimeters|
|48|1|Materialized child mask|
|49|1|Channel count|
|50|2|Reserved; zero|
|52|4|CRC32C of the complete stored LTIL payload|
|56|16|Content SHA-256 prefix|
|72|8|Dependency SHA-256 prefix|

Flag bit 0 means PRVN is present; bit 1 means QUAL is present. Other bits are reserved. Child-mask bits are quadrants 0 through 3; upper four bits are zero. `LogicalChannelBytes` is the checked sum of ChannelRecordV1 `LogicalBytes`; it excludes LTIL headers, directories, and alignment. Payload ranges do not include inter-tile pack alignment. The index flags, channel count, hash prefixes, and scientific summaries must agree with the referenced tile header and decoded channels.

## 7. LTP pack file

### 7.1 Canonical layout

The first tile payload starts at offset 64. Later payloads start at `Align8` of the preceding payload end. Inter-tile bytes are zero. Packs contain at least one tile, exactly one Face/Level group, and ascending Morton order. TIDX is the authoritative directory.

### 7.2 PackHeaderV1 — 64 bytes

|Offset|Size|Field|
|--:|--:|---|
|0|4|Magic `LTPK`|
|4|2|Major version; 1|
|6|2|Minor version; 0|
|8|4|Header bytes; 64|
|12|4|Endian tag; `0x01020304`|
|16|4|Pack ID|
|20|4|Flags; zero|
|24|8|Tile count|
|32|8|Payload region offset; 64|
|40|8|Complete file bytes|
|48|16|Pack SHA-256 prefix|

The full pack hash is SHA-256 of the complete pack file after replacing header bytes 48 through 63 with sixteen zero bytes for the calculation. The stored prefix is the first 16 bytes of that digest, and PACK stores the full digest. This zeroing rule removes self-reference; no other byte is excluded.

## 8. LTIL tile payload

### 8.1 Canonical layout

The tile payload comprises its 128-byte header, an ascending ChannelID directory, zero alignment, and ascending ChannelID stored data ranges. The directory begins at 128. Each stored range begins at an 8-byte-aligned tile-relative offset. Alignment bytes are included in `DataRegionBytes` but not any channel's `StoredBytes`. No trailing alignment belongs to the tile payload.

### 8.2 TileHeaderV1 — 128 bytes

|Offset|Size|Field|
|--:|--:|---|
|0|4|Magic `LTIL`|
|4|2|Record version; 1|
|6|2|Header bytes; 128|
|8|8|TileKey|
|16|4|Flags; same known bits as TIDX|
|20|2|Channel count|
|22|2|Tile cells; 256|
|24|2|Core vertices; 257|
|26|1|Apron; 1|
|27|1|Encoding profile|
|28|4|Effective resolution, meters|
|32|4|Geometric error, meters|
|36|4|Minimum elevation, meters|
|40|4|Maximum elevation, meters|
|44|4|Primary DatasetID|
|48|2|Provenance palette count|
|50|2|Reserved; zero|
|52|4|Channel directory offset; 128|
|56|4|Channel directory bytes; count times 40|
|60|4|Data region offset; `Align8(128 + directory bytes)`|
|64|4|Data region bytes|
|68|16|Dependency SHA-256 prefix|
|84|16|Content SHA-256 prefix|
|100|28|Reserved; zero|

Encoding profile 1 is the U16 global quantization in section 9.1. Encoding profile 2 reserves I32 millimeters relative to reference radius; a v1.0 writer does not emit it. Summary float values must be finite; their millimeter-rounded values agree with TIDX or cause a validation error. Primary DatasetID appears in the PRVN palette.

### 8.3 ChannelRecordV1 — 40 bytes

|Offset|Size|Field|
|--:|--:|---|
|0|2|Channel ID|
|2|2|Channel version; 1|
|4|1|Element type|
|5|1|Component count|
|6|1|Codec ID|
|7|1|Predictor ID|
|8|2|Width|
|10|2|Height|
|12|4|Flags|
|16|4|Tile-relative data offset|
|20|4|Stored bytes|
|24|4|Logical bytes after decompression, before inverse prediction|
|28|4|CRC32C of exact stored bytes|
|32|4|Parameter 0|
|36|4|Parameter 1|

Channel IDs are ELEV `0x0001`, PRVN `0x0002`, and QUAL `0x0003`. Element types are U8 `1`, U16 `2`, I16 `3`, U32 `4`, F32 `5`, F64 `6`, I32 `7`, and opaque structured blob `255`. Codec IDs are none `0` and Zstandard `1`. Predictor IDs are none `0` and Delta2D_U16 `1`.

ELEV is required exactly once. PRVN is required by the prototype and its presence agrees with tile flag bit 0. QUAL is optional and agrees with tile flag bit 1.

Parameter 0 is codec-specific and Parameter 1 is channel-specific. For codec none, `StoredBytes == LogicalBytes` and Parameter 0 is zero. For Zstandard, the frame decompresses to exactly `LogicalBytes`, contains its content size, uses no dictionary, and has the Zstandard frame checksum disabled because CRC32C covers the stored frame. The canonical v1 encoder uses the pinned Zstandard library, compression level 3, one frame per channel, no long-distance matching, and no multithreaded frame compression. Parameter 0 contains signed compression level 3 reinterpreted as `uint32`. Decoders do not require a particular compression level but reject dictionaries and output-size disagreement.

## 9. Channel payloads

### 9.1 ELEV

For profile 1, ELEV has U16 elements, one component, width and height 259, predictor Delta2D_U16, required flag set, and Parameter 1 equal to QuantizationID 1. Parameter 0 follows the selected codec.

```text
q = roundTiesToEven((elevation_m - (-16384.0)) / 0.5)
elevation_m = -16384.0 + double(q) * 0.5
```

The encoder rejects non-finite values and values outside U16 range before rounding. Samples are row-major with X varying fastest. Predictor arithmetic is modulo `2^16`:

```text
at (0,0):      predicted = 0
first row:     predicted = left
first column:  predicted = above
interior:      predicted = left + above - upper_left
residual = sample - predicted
```

Residual U16 values are serialized little-endian and then compressed. Inverse prediction reconstructs samples in the same row-major order using modulo arithmetic.

### 9.2 PRVN

PRVN uses opaque element type, one component, no predictor, required flag set, and parameters zero. Width and height repeat the map dimensions or are zero when there is no map. Its decompressed payload starts with:

#### ProvenanceHeaderV1 — 16 bytes

|Offset|Size|Field|
|--:|--:|---|
|0|2|Version; 1|
|2|2|Palette count; at least 1|
|4|2|Map width; 0 or 64 in canonical production output|
|6|2|Map height; 0 or 64 in canonical production output|
|8|1|Index width; 0, 1, or 2 bytes|
|9|1|Reserved; zero|
|10|2|Flags; bit 0 means map present|
|12|4|Reserved; zero|

#### ProvenancePaletteEntryV1 — 16 bytes

|Offset|Size|Field|
|--:|--:|---|
|0|4|DatasetID|
|4|4|Flags; zero in v1.0|
|8|4|Contribution fraction, finite F32 in [0,1]|
|12|4|Native resolution meters, positive finite F32|

Palette entries are sorted by DatasetID and unique. Contribution fractions sum to 1 within `1e-6` when evaluated in binary64. With no map, width, height, index width, and flags are zero. With a map, dimensions are both 64 in production, flag bit 0 is set, and the canonical index width is 1 for at most 256 palette entries and 2 otherwise. Row-major unsigned indices follow the palette and each is less than PaletteCount. The dominant contributor uses the smallest DatasetID to break an exact tie.

### 9.3 QUAL

QUAL uses U8, one component, dimensions 64 by 64, no predictor, required flag clear, and parameters zero. Its decompressed payload is one row-major byte per cell:

```text
bit 0  source product marks an interpolated sample
bit 1  builder filled source no-data
bit 2  DEM fusion transition region
bit 3  low-frequency datum/bias correction applied
bit 4  lower-confidence region
bits 5..7 reserved and zero
```

## 10. Scientific and package identity

### 10.1 Tile dependency hash

The builder projects tile dependencies into a JCS document with this schema before hashing:

```json
{
  "builder_algorithm_version": 1,
  "datum_version": "...",
  "fusion": {"algorithm": "...", "configuration_sha256": "<64 lowercase hex>"},
  "projection": {"id": 1, "implementation_version": 1},
  "quantization": {"configuration_sha256": "<64 lowercase hex>", "id": 1},
  "semantic_configuration_sha256": "<64 lowercase hex>",
  "sources": [
    {
      "artifact_bundle_sha256": "<64 lowercase hex>",
      "dataset_id": 0,
      "windows": [{"dependency_sha256": "<64 lowercase hex>"}]
    }
  ],
  "tile_key": "<16 lowercase hex digits>",
  "tile_schema_version": 1
}
```

Sources are sorted by DatasetID and window dependencies by digest bytes. A source adapter's versioned window-dependency digest covers its normalized, source-native integer window/band descriptor and every source/configuration value that can change samples returned for that window. The fixed-width TileKey text is the numeric encoded value in most-significant hexadecimal digit first, not the human-readable QSC form.

```text
TileDependencySha256 =
  SHA256(domain "LTDB_TILE_DEP_V1" + uint64 JcsByteCount + JcsBytes)
```

Machine paths, job counts, worker scheduling, cache paths, and staging paths never enter this document. The full hash is build/cache metadata; LTIL stores 16 bytes and TIDX stores 8 bytes.

### 10.2 Tile content hash

Tile content identifies decoded scientific channel content independently of codec and pack placement:

```text
SHA256(
  domain "LTDB_TILE_CONTENT_V1"
  + uint64 TileKey
  + uint16 ChannelCount
  + for each channel in ChannelID order:
      uint16 ChannelID
      uint16 Version
      uint8  ElementType
      uint8  Components
      uint8  Predictor
      uint8  zero
      uint16 Width
      uint16 Height
      uint32 ChannelRecord Flags
      uint32 decoded byte count
      uint32 zero (codec Parameter0 is excluded)
      uint32 Parameter1
      decoded channel bytes
)
```

For ELEV, decoded bytes are reconstructed row-major U16 little-endian samples after inverse prediction. For PRVN and QUAL they are the canonical decompressed bytes. Codec ID, offsets, stored sizes, CRCs, compression level, and alignment do not enter the content hash. The full hash is build/cache metadata; LTIL stores 16 bytes and TIDX stores 16 bytes.

## 11. Database identity inputs

### 11.1 Ordered artifact bundles

Members are ordered by unsigned UTF-8 bytes of their stable source-relative URI path. Names are unique and normalized Unicode NFC, use `/`, and contain no `.` or `..` segment. Each member's SHA-256 covers its exact file bytes.

The bundle digest is:

```text
SHA256(
  domain "LTDB_ARTIFACT_BUNDLE_V1"
  + uint32 MemberCount
  + for each ordered member:
      uint32 NameByteCount
      Name UTF-8 bytes
      uint64 FileByteCount
      uint8  FileSha256[32]
)
```

The local source path is not hashed. META lists the same ordered members and their individual hashes so the bundle can be audited.

### 11.2 Deterministic builder configuration

TOML is parsed into a typed configuration and projected into this JSON object before JCS encoding:

```json
{"package":{...},"semantic":{...}}
```

`semantic` contains every value that can change sampled scientific values, tile selection, provenance, hierarchy, or algorithm behavior: datum, projection/version, tile shape, quantization, fusion/version, stable dataset keys, source URIs, declared metadata/overrides, priority/policy, and algorithm version.

`package` contains values that can change canonical package bytes without changing decoded science: format major/minor, portable database name, target pack bytes, codec/version, codec level, and canonical pack naming policy.

Machine-local dataset paths, cache/staging/output directories, temporary names, job counts, memory limits, logging, progress display, and cancellation state are excluded. Dataset local paths are joined to semantic entries only after hashing by stable dataset key.

```text
BuilderConfigurationSha256 =
  SHA256(domain "LTDB_BUILDER_CONFIG_V1" + uint64 JcsByteCount + JcsBytes)
```

The builder may also hash the `semantic` subobject alone for tile dependencies; it uses domain `LTDB_SEMANTIC_CONFIG_V1` with the same length framing.

### 11.3 Dataset registry hash

The registry identity is independent of unrelated strings and their physical StringIDs. It is:

```text
SHA256(
  domain "LTDB_DATASET_REGISTRY_V1"
  + uint32 DatasetCount
  + for every DSET record in DatasetID order:
      uint32 DatasetID
      uint32 Flags
      for each of the eight StringID fields in record order:
          uint64 UTF8ByteCount
          exact UTF-8 bytes
      exact bytes of the four binary64 fields
      uint64 ArtifactBundleBytes
      uint8  ArtifactBundleSha256[32]
      uint64 MetadataByteCount
      exact canonical META JSON bytes
      uint32 QualitySchemaID
)
```

Physical StringIDs, META offsets, and reserved fields are excluded. This covers the complete decoded source registry while preventing a pack-path string from changing dataset identity.

### 11.4 Tile-index hash

```text
SHA256(domain "LTDB_TILE_INDEX_V1" + uint64 TidxBytes + exact TIDX bytes)
```

### 11.5 Database-content hash

This identity deliberately excludes the LTDB header, chunk-directory file offsets and CRCs, physical LTDB alignment, and its own header field:

```text
SHA256(
  domain "LTDB_DATABASE_CONTENT_V1"
  + uint16 Major
  + uint16 Minor
  + BuilderConfigurationSha256
  + uint32 IdentityChunkCount
  + ChunkHashFrame for every ContentIdentity chunk in directory order
  + uint32 PackCount
  + for every PACK record in PackID order:
      uint32 PackID
      uint8  FullPackSha256[32]
)
```

All five mandatory chunks have `ContentIdentity` set. Including complete pack hashes, rather than prefixes, prevents pack changes from being hidden. The header's database-content field is not input, so the definition is not self-referential.

### 11.6 Database ID

DatabaseID is the first 16 digest bytes of:

```text
SHA256(
  domain "LTDB_DATABASE_ID_V1"
  + BuilderConfigurationSha256
  + DatasetRegistrySha256
)
```

It is an opaque deterministic 128-bit identifier, not a UUID and not subject to UUID bit rewriting.

## 12. Required validation

A structural reader rejects at least:

- bad magic, endian tag, fixed sizes, record versions, flags, or reserved bytes;
- any invalid/overflowing/out-of-file/overlapping or misaligned range;
- count-to-byte-size disagreement for fixed-record chunks and channel directories;
- invalid UTF-8, non-canonical STRS ordering, invalid StringID, path, JSON range, or META overlap;
- duplicate or unsorted datasets, packs, tile keys, channels, or provenance entries;
- invalid TileKey face, level, coordinates, or unused Morton bits;
- TIDX-to-pack, TIDX-to-LTIL, summary, range, flag, or hash-prefix disagreement;
- invalid channel dimensions, element type, predictor combination, decoded length, Zstandard dictionary, CRC, or decompression result;
- PRVN palette/index errors, missing PrimaryDataset, QUAL reserved bits, or non-finite scientific values;
- chunk, tile payload, pack, registry, index, database-content, or full-validation hash disagreement.

The implementation uses specific stable `ErrorCode` values and attaches the most relevant path, byte offset, TileKey, and channel context.

## 13. Golden vectors

[`tests/golden/format_v1`](../tests/golden/format_v1) contains the v1.0 compatibility vectors. For each named vector, the reviewed `.hex` file is the human-readable byte contract and the `.bin` file is its generated binary counterpart. [`tools/generate_format_v1_fixtures.py`](../tools/generate_format_v1_fixtures.py) serializes the field values defined in that directory, checks the reviewed hex, regenerates binary files, and produces `SHA256SUMS`.

The set covers every v1 record, STRS/META/config framing, all three channel payloads, a complete LTIL payload, a complete one-tile pack with the zeroed-field pack hash rule, and a complete LTDB with all mandatory chunks and identity hashes. Tiny 3x3 ELEV and 2x2 PRVN/QUAL algorithm vectors intentionally exercise byte rules without pretending to be production tile dimensions; the complete record fixtures label those test dimensions explicitly and are not accepted as production terrain by a semantic validator.
