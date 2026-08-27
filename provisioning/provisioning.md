# Lunar DEM provisioning

`provision.py` is the portable Windows and Linux entry point for acquiring and verifying external lunar DEM products. It requires Python 3.9 or newer and uses only the Python standard library. Product locks, bundle membership, provisioning profiles, and network settings live in the adjacent `provisioning.json`; source data and machine-local roots remain outside the repository.

The checked-in configuration provides the established SLDEM2015 `Artifact` and `Fullset` profiles plus the M8 `SLDEMBoundary`, `LOLAFoundation`, `Maskelyne`, and `SouthPolar80S` profiles. Profile names are case-insensitive. Product qualification evidence and exact Builder usage are recorded in [`../qualification/m8/README.md`](../qualification/m8/README.md).

## SLDEM2015 root

Pass the external root explicitly with `--root`, or omit it and set `SLDEM2015_ROOT`. The script resolves the path, rejects a filesystem root, and preserves the official source-relative `tiles/jp2/` layout.

Windows PowerShell examples:

```powershell
python .\provisioning\provision.py Artifact --root D:\LunarTerrainData\SLDEM2015
python .\provisioning\provision.py Fullset --root D:\LunarTerrainData\SLDEM2015

$env:SLDEM2015_ROOT = "D:\LunarTerrainData\SLDEM2015"
python .\provisioning\provision.py Artifact
```

Linux examples:

```bash
python3 ./provisioning/provision.py Artifact --root /data/LunarTerrainData/SLDEM2015
python3 ./provisioning/provision.py Fullset --root /data/LunarTerrainData/SLDEM2015

export SLDEM2015_ROOT=/data/LunarTerrainData/SLDEM2015
python3 ./provisioning/provision.py Artifact
```

`Artifact` downloads and verifies the pinned JP2 tile, detached PDS label, and auxiliary XML sidecar. It checks every member's byte count, official PDS MD5, SHA-256, and the canonical artifact-bundle SHA-256 used by the Builder.

`Fullset` downloads all 32 JP2 tiles and their 64 official sidecars. It verifies all 96 members against the official PDS MD5 manifest and locked total byte count, verifies the pinned artifact subset, and atomically writes `SLDEM2015_512ppd_SHA256SUMS.txt` under the external root.

`SLDEMBoundary` selects the three original members for the `30–60°N, 0–45°E` tile used by M8 Profile B. It reuses the full-set member identities and does not replace or modify the established `Artifact` definition.

## M8 product roots

Each M8 product remains a separate provisioning invocation and external root:

| Profile | Environment variable | Locked contents |
|---|---|---|
| `LOLAFoundation` | `LOLA_GDRDEM_ROOT` | Four lossless global `LDEM_256` JP2 rasters and their eight PDS sidecars |
| `Maskelyne` | `LROC_NAC_MASKELYNE_ROOT` | NAC elevation GeoTIFF, confidence image, PDS label, and readme |
| `SouthPolar80S` | `LOLA_SOUTH_POLAR_80S_ROOT` | Adjusted elevation, count, effective-resolution, and error GeoTIFFs |

Examples use the same portable contract on Windows and Linux:

```text
python provisioning/provision.py LOLAFoundation --root <external-lola-root>
python provisioning/provision.py Maskelyne --root <external-maskelyne-root>
python provisioning/provision.py SouthPolar80S --root <external-polar-root>
```

Add `--verify-only` to any invocation to prohibit network access. The provisioning profile acquires one product/bundle only; Builder configurations combine the resulting environment-variable roots.

## Verification-only operation

Add `--verify-only` to prohibit all network requests and verify files already present:

```text
python provisioning/provision.py Artifact --root <external-root> --verify-only
python provisioning/provision.py Fullset --root <external-root> --verify-only
```

The full-set check also requires the previously acquired `pds-lrolol_1xxx_260615.md5` under the external root. Verification-only operation may regenerate the project SHA-256 manifest and reapply read-only permissions to verified original members, but it never downloads a missing member or checksum manifest.

## Transfer and failure behavior

- An existing final member is never overwritten or deleted. It is verified in place; a mismatch names the file that must be made writable if necessary, deleted manually, and reacquired.
- New transfers use a neighboring `.part` file and an HTTP range request when resuming. A server that does not honor the requested byte range causes a failure rather than a silent restart or concatenation.
- A completed transfer is atomically renamed from `.part` to its final name. Any remaining `.part` file under the external root fails the profile.
- Verified original bundle members are made read-only. Generated checksum manifests and the downloaded upstream checksum manifest are not members of the canonical artifact bundle.
- Exit status `0` means the selected profile passed. Command-line usage errors return `2`; configuration, transfer, filesystem, or verification failures return `1`.

## Configuration

`provisioning.json` schema version 1 separates reusable product locks from the profiles that select them:

- `network` controls retry count, request timeout, streaming chunk size, and user agent.
- `products` defines the stable dataset key, archive revision, provenance and download URLs, root environment variable, optional per-member source-URL/byte/checksum/role metadata, upstream checksum manifests, and ordered bundles.
- A bundle declares its ordered source-relative members and byte total. It may also declare an upstream checksum manifest, a canonical domain and bundle SHA-256, and a generated SHA-256 manifest path.
- `profiles` selects one product bundle to acquire and the bundle identities that must be verified. The `Fullset` profile consequently verifies both the pinned three-member artifact and the full 96-member set.

The script validates the entire configuration before accessing an external root. Unknown fields, unsafe relative paths, duplicate members, unresolved references, unpinned members, and a canonical hash inconsistent with configured member order, sizes, or SHA-256 values are rejected. Use `--config PATH` to exercise another schema-version-1 file:

```text
python provisioning/provision.py <profile> --config provisioning/another-lock.json --root <external-root>
```

Future products should be added only after product preflight has locked their authoritative URLs, revision, required source members and companions, byte counts, published checksums where available, computed SHA-256 values, and canonical bundle identity. Do not add credentials, session state, generated derivatives, source data, checksum output, or machine-local paths to the checked-in configuration.

The focused Builder-lock consistency check is also standard-library-only:

```text
python provisioning/verify_builder_locks.py --lock provisioning/provisioning.json qualification/m8/configs/profile_a.toml qualification/m8/configs/profile_b.toml qualification/m8/configs/profile_c.toml qualification/m8/configs/scale.toml
```

## Focused tests

The provisioning tests use temporary files and mocked HTTP responses; they do not contact an archive:

```text
python -m unittest discover -s provisioning -p "test_*.py" -v
```
