# Format v1 golden vectors

These vectors freeze the byte contract in [`specs/03_LunarTerrainDatabase_Format_v1.md`](../../../specs/03_LunarTerrainDatabase_Format_v1.md) before the M1 reader/writer implementation.

The fixture dataset is `fixture.dem.v1`, with two ordered artifact members (`dem.img` containing `DEM` and `dem.lbl` containing `L1`). Its one tile is `QSC/F2/L08/0147/0092`. The configuration and tile-dependency JCS documents lock their hash inputs. The small channel samples deliberately exercise encoding rules:

```text
decoded ELEV U16 codes:  100 101 103 / 99 101 104 / 98 102 106
Delta2D residual codes:  100   1   2 / -1   1   1 / -1   2   1
PRVN dominant map:         0   0     /  1   1
QUAL bytes:                0   4     /  1  12
```

The 3x3 and 2x2 algorithm fixtures are intentionally smaller than semantic production dimensions. `database_file_v1.bin` and `pack_file_v1.bin` are complete structural fixtures around those test records; they are golden parser inputs, not valid scientific production tiles.

For every vector:

- `.hex` is the reviewed, offset-labelled representation used for byte-level review;
- `.bin` is the exact generated input used by tests;
- `SHA256SUMS` detects accidental fixture changes.

Regenerate only as part of an approved format change:

```text
py -3 tools/generate_format_v1_fixtures.py --write
```

Normal verification is non-mutating:

```text
py -3 tools/generate_format_v1_fixtures.py --check
```
