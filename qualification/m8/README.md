# M8 product qualification spike — Work Stages 1–3

This evidence package implements the M8 spike through Work Stage 3. It deliberately stops before Work Stage 4: no required-region build, full validation, publication, deterministic rebuild, reordered-source build, incremental invalidation, or database comparison has been performed.

## Product preflight and acquisition lock

All selected archives fit the existing schema-version-1 lifecycle: direct HTTP members, optional official PDS MD5 manifest verification, project SHA-256 verification, and ordered canonical bundle hashing. No provisioning schema or acquisition lifecycle change was required.

| Product / selected revision | Original members | Locked bytes | Canonical bundle SHA-256 | Checksum authority |
|---|---:|---:|---|---|
| LOLA GDR `LDEM_256`, product V3.0, data through `LRO_ES_52`, archive `lrolol_1xxx_260615` | 4 lossless JP2 rasters + 8 PDS sidecars | 1,848,004,754 | `8a657dde1f0047bc5cc32a179aee38ba3e36fa081723eae2618e8a181da1909f` | Official PDS MD5 manifest plus project SHA-256 |
| SLDEM2015 512 ppd V2.0, archive `lrolol_1xxx_260615` | 32 lossless JP2 rasters + 64 PDS sidecars | 5,466,757,104 | `b3f04e54c5368111df2fd5699017db29d723e8790bc8821eaa1a562f136d2e29` | Official PDS MD5 manifest plus project SHA-256 |
| LROC `NAC_DTM_MASKELYNE` v1.9, `LROLRC_2001` | elevation GeoTIFF, confidence image, label, readme | 136,882,682 | `c7b56da9de0a50839e4881c32e3cee6c6ad67f1a6629e5875779c418aa3cea36` | Archive HTTP entity MD5 values confirmed against the members; project SHA-256 is authoritative for the lock |
| GSFC adjusted south-polar `80S` bundle, 2023 publication | elevation, count, effective-resolution, and error COG GeoTIFFs | 11,915,218,975 | `a0f79e5293db691a3e06022221137d261e1de322e542c3c45add92a3280d771e` | No published checksum catalog was located; exact project SHA-256 values are authoritative for the lock |

The complete selected original-member set is 19,366,863,515 bytes (about 18.04 GiB), excluding downloaded and generated checksum manifests. `provisioning/provisioning.json` owns every per-member byte count, MD5 where available, SHA-256, role, ordered bundle, total, and canonical identity. The established three-member SLDEM `artifact` bundle remains unchanged. The full 96-member SLDEM bundle now has complete per-member metadata and a canonical identity; `north_boundary` selects its `30–60°N, 0–45°E` artifact without duplicating member identity.

Authoritative source evidence:

- LOLA GDR dataset and archive: <https://pds-geosciences.wustl.edu/lro/lro-l-lola-3-rdr-v1/lrolol_1xxx/catalog/gdr_ds.cat> and <https://pds-geosciences.wustl.edu/lro/lro-l-lola-3-rdr-v1/lrolol_1xxx/data/lola_gdr/cylindrical/jp2/>.
- SLDEM2015 archive: <https://pds-geosciences.wustl.edu/lro/lro-l-lola-3-rdr-v1/lrolol_1xxx/data/sldem2015/>.
- Maskelyne product page, PDS archive, usage terms, and label/readme: <https://data.lroc.im-ldi.com/lroc/view_rdr/NAC_DTM_MASKELYNE>, <https://pds.lroc.im-ldi.com/data/LRO-L-LROC-5-RDR-V1.0/LROLRC_2001/DATA/SDP/NAC_DTM/MASKELYNE/>, and <https://www.lroc.im-ldi.com/about/terms>. The LROC terms identify products obtained through the listed PDS interfaces as public-domain data and request research citation.
- GSFC south-polar product page and required citation: <https://pgda.gsfc.nasa.gov/products/90>, data DOI <https://doi.org/10.60903/gsfcpgda-lola-spole>, and Barker et al. (2023), <https://doi.org/10.3847/PSJ/acf3e1>.

The NASA/PDS and GSFC endpoints are public, non-authenticated archives. Product citations and archive provenance must accompany scientific use. No credentials, session state, interactive agreement, archive extraction, generated URL discovery, or overwrite behavior is needed.

## Builder-linked GDAL inspection

The checked-in expectations were reconciled against the same GDAL 3.12.4 / PROJ 9.8.1 dependency stack linked into the Builder:

- LOLA and SLDEM open through `JP2OpenJPEG` as `Int16`. Their PDS values use a 0.5 scale. LOLA `LDEM_256` is radius relative to a 1,737,400 m reference and declares a 1,737,400 m offset; SLDEM is elevation relative to that radius with zero offset. The four LOLA files are 46,080 × 23,040 and the 32 SLDEM files are 23,040 × 15,360.
- Maskelyne opens through `GTiff` as 2,370 × 11,542 `Float32`, with no-data `-3.40282265508890445e38`, approximately 5 m pixels, and observed bounds `33.5311475599988..33.922993086289495°E`, `3.258297606427915..5.16145141999987°N`. Its equirectangular CRS is planetocentric/east-positive on a 1,737,400 m sphere.
- Every selected south-polar COG opens through `GTiff` as 30,400 × 30,400 `Float32`, with 20 m projected pixels, NaN no-data, and a south-polar stereographic Moon sphere. The projected square's geographic envelope is `-180..180°E`, `-90..-75.89380428852846°N`; the selected scientific region remains 80°S and poleward.

NaN no-data, omitted default scale/offset tags, full-longitude logical mosaics, and pole-containing projected footprints are now represented and validated explicitly by the Builder. These are compatibility additions to source ingestion and diagnostics; they do not alter format v1.

Pre-fusion spot checks recorded from original elevation members through `scan` are:

| Source | Planetocentric coordinate (latitude, longitude) | Normalized elevation |
|---|---|---:|
| LOLA global mosaic | `(0°, 0°E)` | `-723.0 m` |
| SLDEM artifact | `(15°, 22.5°E)` | `-1346.0 m` |
| SLDEM northern-boundary artifact | `(45°, 22.5°E)` | `-1091.75 m` |
| Maskelyne | `(4.2098745132138925°, 33.72707032314415°E)` | `-781.5782012939478 m` |
| GSFC south-polar adjusted DEM | `(-82.94690214426423°, 0°E)` | `-1768.8586752032345 m` |

These values are input checks, not fusion acceptance values. Stage 4 must add required-region comparison samples and tolerances before accepting a policy.

## Source lineage, policies, and quality

LOLA is the global foundation. SLDEM combines LOLA and Kaguya Terrain Camera information and is therefore a higher-resolution backbone, not an independent measurement of LOLA-scale geometry. The GSFC polar DEM is LOLA-derived. Starting priorities and policies reflect this lineage:

- global LOLA: priority 0, base, `Replace`;
- SLDEM: priority 100, `Replace` over the foundation;
- Maskelyne NAC: priority 300, `ResidualRefinement_v1` as the required starting hypothesis;
- GSFC adjusted south pole: priority 300, `Replace` as the starting hypothesis.

Maskelyne confidence DN 0 is treated as no-data; DN 1–4 is declared lower-confidence/interpolated; DN 10–15 supports elevation. The exact distinctions among shadow, saturation, suspicious correlation, interpolation/extrapolation, successful correlation, and manual editing cannot be retained in v1 `QUAL` and are recorded as unsupported rather than collapsed silently.

For the polar bundle, zero count or non-finite elevation is declared filled/no-data and lower-confidence. Local effective resolution above the provisional 80 m source level or elevation error above 5 m is declared lower-confidence. Exact count and continuous effective-resolution/error values cannot be encoded losslessly in v1 `QUAL`; the source companions and this limitation remain explicit. Applying these mappings to published tiles is Stage 4 work.

## Qualification configurations and observed plans

The configurations under `configs/` contain semantic source metadata and only portable external-root environment names. Output, cache, and thread settings remain local and excluded from semantic identity. `verify_builder_locks.py` fails when a Builder stable key, provenance URI, root environment name, ordered artifact member, byte count, SHA-256, bundle total, or bundle hash drifts from the acquisition lock.

| Config | Required region | Source target levels | Expected hierarchy tiles | Estimated uncompressed channel bytes | Semantic SHA-256 |
|---|---|---|---:|---:|---|
| Profile A | `33.55..33.90°E`, `3.30..5.10°N` | SLDEM L8; Maskelyne L12 | 2,480 | 353,156,960 | `29952b5504f82a05bbafd1ef09f0f2c5dbbd1c6d08040aa5a7fbae098af569f6` |
| Profile B | `20..21°E`, `59.5..60.5°N` | LOLA L7; SLDEM L8 through 60°N | 41 | 5,838,482 | `71e76e36e2dfd109588880964a28b98926b7aa270aabfc36aa3c57fdcc5cf75a` |
| Profile C | all longitudes, `90..89°S` | LOLA L7; polar L8 | 77 | 10,964,954 | `bfff21e26ad7daf62ffdeb535c28e81bc92bff2ed997ccd5add71a100b50ba82` |
| Scale | global | LOLA L7; SLDEM L8; polar L8; Maskelyne L12 | 481,225 | 68,542,801,650 | `b2a7c7240a48d752bc727e6d6578f478bf11871ecb9d2ff9ffb2e314bab0ecc9` |

Level selection is the existing deterministic worst-case QSC sample-spacing rule, so these measured L7/L8/L12 results supersede filename and nominal-grid hypotheses. `plan --json` lists per-source clipped coverage, effective resolution, selected level, fusion policy, counts at every materialized level, and at most 256 representative tile keys. `scan --json` lists the complete logical source registry, physical raster count, artifact identity, coverage, center spot check, priority, policy, quality mapping, and unsupported quality values.

## Portable commands

Provision products separately into external roots:

```text
python provisioning/provision.py LOLAFoundation --root <lola-root>
python provisioning/provision.py Artifact --root <sldem-root>
python provisioning/provision.py SLDEMBoundary --root <sldem-root>
python provisioning/provision.py Maskelyne --root <maskelyne-root>
python provisioning/provision.py SouthPolar80S --root <south-polar-root>
```

Use `Fullset` instead of `Artifact`/`SLDEMBoundary` for the scale profile. Add `--verify-only` for strict no-network verification. Set the environment names printed by provisioning, then run:

```text
lunar-terrain scan qualification/m8/configs/profile_a.toml --json
lunar-terrain plan qualification/m8/configs/profile_a.toml --json
python provisioning/verify_builder_locks.py --lock provisioning/provisioning.json qualification/m8/configs/profile_a.toml qualification/m8/configs/profile_b.toml qualification/m8/configs/profile_c.toml qualification/m8/configs/scale.toml
```

The same commands apply to Profiles B, C, and scale. Do not run `build`, `validate`, `inspect`, `diff`, or post-build exports as evidence for this workpack until Work Stage 4 is authorized.
