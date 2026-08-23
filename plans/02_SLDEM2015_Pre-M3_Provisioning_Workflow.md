# SLDEM2015 Pre-M3 Provisioning Workflow

## Purpose

Provision an exact, locally readable SLDEM2015 artifact bundle before M3 implementation. Acquisition is separate from terrain builds: files are downloaded once from NASA PDS, verified, made read-only, and kept outside the repository. M3 receives a machine-local root path separately from the stable dataset key and provenance URI.

The initial M3 acceptance bundle is one 512-pixels-per-degree (ppd) JP2 tile plus its detached PDS label and auxiliary XML sidecar. The same layout can later expand to all 32 official 512-ppd tiles without changing the source-relative member names.

Authoritative project requirements remain in the [implementation plan](01_LunarTerrainCore_LunarTerrainBuilder_Implementation_Plan.md) and [v1 format specification](../specs/03_LunarTerrainDatabase_Format_v1.md). Official source documentation is the [PDS SLDEM description](https://ode.rsl.wustl.edu/moon/pagehelp/Content/Missions_Instruments/LRO/LOLA/SLDEM.htm), [PDS JP2 directory](https://pds-geosciences.wustl.edu/lro/lro-l-lola-3-rdr-v1/lrolol_1xxx/data/sldem2015/tiles/jp2/), and [PDS checksum guidance](https://pds-geosciences.wustl.edu/dataserv/checksums.html).

## Locked source bundle

Use these identity values for the initial M3 subset:

- Stable dataset key: `nasa.sldem2015.512ppd.v1`
- Provenance URI: `https://pds-geosciences.wustl.edu/lro/lro-l-lola-3-rdr-v1/lrolol_1xxx/data/sldem2015/`
- Local root contents: the official tree beginning at `tiles/jp2/`
- Artifact bundle bytes: `171872390`
- Artifact bundle SHA-256: `6846f7a9b1955d38e2fd14e6a5181720eaec9f8fc450bd28a3af247d5b4c79fc`

Members are ordered by unsigned UTF-8 bytes of the following source-relative paths, as required by the v1 format:

| Source-relative member | Bytes | Official PDS MD5 | SHA-256 |
|---|---:|---|---|
| `tiles/jp2/sldem2015_512_00n_30n_000_045.jp2` | 171864155 | `f009c232030ecd09e21dd67ebafdb02d` | `8d6e9bb9687bd19dbc9de57c8044932540bb934e5ef557b473660f5056484a73` |
| `tiles/jp2/sldem2015_512_00n_30n_000_045_aux.xml` | 3295 | `6268c2b44040867f5ddb58992f36f789` | `c9c013daa77cfbbd68efb157c51198fda3aed157f0d5ebddaf1a94f9151faa71` |
| `tiles/jp2/sldem2015_512_00n_30n_000_045_jp2.lbl` | 4940 | `d20e9cb11147652f301cc9f3e492a5ed` | `4d8f5edd72cdc486cbe491741a588dc545b7b72baa4cfb0e34cae7389d6305e0` |

These values were verified against the official PDS files on 2026-08-22. Keep the three source files byte-for-byte unchanged. In particular, do not rename `_aux.xml` inside the provenance tree; M3 must explicitly associate that sidecar or create a non-provenance GDAL alias when needed.

## Windows prerequisites

- Windows 10 or 11 with Windows PowerShell 5.1 or PowerShell 7.
- `curl.exe` available on `PATH`. Invoke `curl.exe`, not `curl`, because Windows PowerShell may define `curl` as an alias.
- At least 250 MiB free for the M3 subset. Reserve at least 6 GiB before downloading all 32 JP2 tile bundles.
- A data directory outside the repository. This workflow defaults to `%USERPROFILE%\LunarTerrainData\SLDEM2015`; another persistent local or mounted path is acceptable.

Check the prerequisite before downloading:

```powershell
Get-Command curl.exe -ErrorAction Stop
```

Do not install a separate system GDAL for this workflow. The repository's vcpkg manifest enables `gdal[openjpeg]`; M3 must validate `JP2OpenJPEG` through the same GDAL build linked into the Builder.

## Download the M3 subset on Windows

Run the following in PowerShell. Downloads use a `.part` file, resume after interruption, and move into place only after `curl.exe` succeeds.

```powershell
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($env:SLDEM2015_ROOT)) {
    $SldemRoot = Join-Path $env:USERPROFILE "LunarTerrainData\SLDEM2015"
} else {
    $SldemRoot = $env:SLDEM2015_ROOT
}
$SourceDirectory = Join-Path $SldemRoot "tiles\jp2"
$PdsTileRoot = "https://pds-geosciences.wustl.edu/lro/lro-l-lola-3-rdr-v1/lrolol_1xxx/data/sldem2015/tiles/jp2"

New-Item -ItemType Directory -Force -Path $SourceDirectory | Out-Null

$Members = @(
    "sldem2015_512_00n_30n_000_045.jp2",
    "sldem2015_512_00n_30n_000_045_aux.xml",
    "sldem2015_512_00n_30n_000_045_jp2.lbl"
)

foreach ($Member in $Members) {
    $Target = Join-Path $SourceDirectory $Member
    if (Test-Path -LiteralPath $Target) {
        continue
    }

    $Partial = "$Target.part"
    $CurlArguments = @(
        "--fail",
        "--location",
        "--retry", "5",
        "--continue-at", "-",
        "--output", $Partial,
        "$PdsTileRoot/$Member"
    )

    & curl.exe @CurlArguments
    if ($LASTEXITCODE -ne 0) {
        throw "curl.exe failed for $Member with exit code $LASTEXITCODE"
    }

    Move-Item -LiteralPath $Partial -Destination $Target
}

$env:SLDEM2015_ROOT = $SldemRoot
Write-Host "SLDEM2015_ROOT=$env:SLDEM2015_ROOT"
```

`SLDEM2015_ROOT` above applies to the current PowerShell process. Persisting it is optional:

```powershell
[Environment]::SetEnvironmentVariable("SLDEM2015_ROOT", $SldemRoot, "User")
```

The environment variable and resolved path are machine-local inputs. Do not commit either value to this repository or include them in scientific identity hashes.

## Verify and protect the subset

Run this verification from the same PowerShell session. It rejects missing files, wrong sizes, corrupt downloads, and unexpected source revisions before making the verified files read-only.

```powershell
$Expected = [ordered]@{
    "sldem2015_512_00n_30n_000_045.jp2" = [pscustomobject]@{
        Bytes = [int64]171864155
        MD5 = "f009c232030ecd09e21dd67ebafdb02d"
        SHA256 = "8d6e9bb9687bd19dbc9de57c8044932540bb934e5ef557b473660f5056484a73"
    }
    "sldem2015_512_00n_30n_000_045_aux.xml" = [pscustomobject]@{
        Bytes = [int64]3295
        MD5 = "6268c2b44040867f5ddb58992f36f789"
        SHA256 = "c9c013daa77cfbbd68efb157c51198fda3aed157f0d5ebddaf1a94f9151faa71"
    }
    "sldem2015_512_00n_30n_000_045_jp2.lbl" = [pscustomobject]@{
        Bytes = [int64]4940
        MD5 = "d20e9cb11147652f301cc9f3e492a5ed"
        SHA256 = "4d8f5edd72cdc486cbe491741a588dc545b7b72baa4cfb0e34cae7389d6305e0"
    }
}

$TotalBytes = [int64]0
foreach ($Entry in $Expected.GetEnumerator()) {
    $Path = Join-Path $SourceDirectory $Entry.Key
    $File = Get-Item -LiteralPath $Path -ErrorAction Stop
    $ActualMD5 = (Get-FileHash -LiteralPath $Path -Algorithm MD5).Hash.ToLowerInvariant()
    $ActualSHA256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()

    if ($File.Length -ne $Entry.Value.Bytes) {
        throw "Byte-count mismatch for $($Entry.Key): expected $($Entry.Value.Bytes), got $($File.Length)"
    }
    if ($ActualMD5 -ne $Entry.Value.MD5) {
        throw "PDS MD5 mismatch for $($Entry.Key)"
    }
    if ($ActualSHA256 -ne $Entry.Value.SHA256) {
        throw "SHA-256 mismatch for $($Entry.Key)"
    }

    $TotalBytes += $File.Length
}

if ($TotalBytes -ne 171872390) {
    throw "Artifact bundle byte-count mismatch: expected 171872390, got $TotalBytes"
}

foreach ($Member in $Expected.Keys) {
    (Get-Item -LiteralPath (Join-Path $SourceDirectory $Member)).IsReadOnly = $true
}

Write-Host "Verified M3 SLDEM2015 subset: $TotalBytes bytes"
```

### Verify the canonical artifact-bundle hash

This check uses the v1 framing from the format specification. Source-relative names always contain `/`, even on Windows; local Windows paths never enter the hash.

```powershell
$BundleMembers = @(
    [pscustomobject]@{
        RelativePath = "tiles/jp2/sldem2015_512_00n_30n_000_045.jp2"
        Bytes = [uint64]171864155
        SHA256 = "8d6e9bb9687bd19dbc9de57c8044932540bb934e5ef557b473660f5056484a73"
    },
    [pscustomobject]@{
        RelativePath = "tiles/jp2/sldem2015_512_00n_30n_000_045_aux.xml"
        Bytes = [uint64]3295
        SHA256 = "c9c013daa77cfbbd68efb157c51198fda3aed157f0d5ebddaf1a94f9151faa71"
    },
    [pscustomobject]@{
        RelativePath = "tiles/jp2/sldem2015_512_00n_30n_000_045_jp2.lbl"
        Bytes = [uint64]4940
        SHA256 = "4d8f5edd72cdc486cbe491741a588dc545b7b72baa4cfb0e34cae7389d6305e0"
    }
)

$Stream = [System.IO.MemoryStream]::new()
$Writer = [System.IO.BinaryWriter]::new($Stream, [System.Text.Encoding]::UTF8, $true)

$Writer.Write([System.Text.Encoding]::ASCII.GetBytes("LTDB_ARTIFACT_BUNDLE_V1"))
$Writer.Write([uint32]$BundleMembers.Count)

foreach ($Member in $BundleMembers) {
    [byte[]]$NameBytes = [System.Text.Encoding]::UTF8.GetBytes($Member.RelativePath)
    [byte[]]$DigestBytes = for ($Index = 0; $Index -lt $Member.SHA256.Length; $Index += 2) {
        [Convert]::ToByte($Member.SHA256.Substring($Index, 2), 16)
    }

    $Writer.Write([uint32]$NameBytes.Length)
    $Writer.Write($NameBytes)
    $Writer.Write([uint64]$Member.Bytes)
    $Writer.Write($DigestBytes)
}

$Writer.Flush()
$SHA256 = [System.Security.Cryptography.SHA256]::Create()
$BundleDigestBytes = $SHA256.ComputeHash($Stream.ToArray())
$BundleDigest = -join ($BundleDigestBytes | ForEach-Object { $_.ToString("x2") })
$SHA256.Dispose()
$Writer.Dispose()
$Stream.Dispose()

$ExpectedBundleDigest = "6846f7a9b1955d38e2fd14e6a5181720eaec9f8fc450bd28a3af247d5b4c79fc"
if ($BundleDigest -ne $ExpectedBundleDigest) {
    throw "Artifact bundle SHA-256 mismatch: expected $ExpectedBundleDigest, got $BundleDigest"
}

Write-Host "Verified artifact bundle SHA-256: $BundleDigest"
```

## M3 handoff and acceptance

When M3 configuration gains real-dataset entries, join the following values by stable dataset key:

- The semantic configuration uses `nasa.sldem2015.512ppd.v1` and the provenance URI above.
- Machine-local configuration resolves `SLDEM2015_ROOT` to the external source root.
- The initial artifact bundle contains exactly the three locked members above. Do not include `.part` files, checksum manifests, generated GDAL aliases, or directory metadata.
- The PDS label's `PRODUCT_VERSION_ID = V2.0` is preserved as source metadata; it is distinct from the project's stable-key version.

The first M3 `scan`/acceptance path must confirm:

- `JP2OpenJPEG` is available in the linked GDAL runtime;
- raster dimensions are 23040 by 15360 at 512 ppd;
- coverage is 0-30 degrees north and 0-45 degrees east in a simple cylindrical, planetocentric coordinate system;
- no-data DN is `-32768` and sample scale is `0.5`;
- values represent meters relative to the 1,737,400 m reference radius, rather than absolute lunar radii;
- member sizes, member hashes, bundle bytes, bundle hash, and recorded source-relative paths match this document; and
- the same scan succeeds with network access disabled.

Ordinary CI continues to use generated GDAL fixtures. The hash-pinned SLDEM2015 test remains opt-in and must report a clear skip when `SLDEM2015_ROOT` is unset.

## Optional full 512-ppd expansion

The complete official 512-ppd JP2 elevation product contains 32 geographic tiles, each with one JP2, one `_aux.xml`, and one `_jp2.lbl` file. It covers 60 degrees south to 60 degrees north. The 96-file bundle occupies `5466757104` bytes (5.467 GB, 5.091 GiB); the JP2 payloads alone occupy `5466491027` bytes.

Run this only when full coverage is needed. It preserves an already verified subset and resumes interrupted `.part` files.

```powershell
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($env:SLDEM2015_ROOT)) {
    $SldemRoot = Join-Path $env:USERPROFILE "LunarTerrainData\SLDEM2015"
} else {
    $SldemRoot = $env:SLDEM2015_ROOT
}
$SourceDirectory = Join-Path $SldemRoot "tiles\jp2"
$PdsTileRoot = "https://pds-geosciences.wustl.edu/lro/lro-l-lola-3-rdr-v1/lrolol_1xxx/data/sldem2015/tiles/jp2"

$LatitudeBands = @("00n_30n", "30n_60n", "30s_00s", "60s_30s")
$LongitudeSpans = @(
    "000_045", "045_090", "090_135", "135_180",
    "180_225", "225_270", "270_315", "315_360"
)
$Suffixes = @(".jp2", "_aux.xml", "_jp2.lbl")

foreach ($LatitudeBand in $LatitudeBands) {
    foreach ($LongitudeSpan in $LongitudeSpans) {
        $Stem = "sldem2015_512_${LatitudeBand}_${LongitudeSpan}"
        foreach ($Suffix in $Suffixes) {
            $Member = "$Stem$Suffix"
            $Target = Join-Path $SourceDirectory $Member
            if (Test-Path -LiteralPath $Target) {
                continue
            }

            $Partial = "$Target.part"
            $CurlArguments = @(
                "--fail",
                "--location",
                "--retry", "5",
                "--continue-at", "-",
                "--output", $Partial,
                "$PdsTileRoot/$Member"
            )
            & curl.exe @CurlArguments
            if ($LASTEXITCODE -ne 0) {
                throw "curl.exe failed for $Member with exit code $LASTEXITCODE"
            }

            Move-Item -LiteralPath $Partial -Destination $Target
        }
    }
}
```

The expected expansion result is exactly 96 source files and `5466757104` total bytes:

```powershell
$FullFiles = Get-ChildItem -LiteralPath $SourceDirectory -File |
    Where-Object { $_.Name -match '^sldem2015_512_.*(?:\.jp2|_aux\.xml|_jp2\.lbl)$' }
$FullBytes = [int64](($FullFiles | Measure-Object -Property Length -Sum).Sum)

if ($FullFiles.Count -ne 96) {
    throw "Expected 96 full-set files, got $($FullFiles.Count)"
}
if ($FullBytes -ne 5466757104) {
    throw "Expected 5466757104 full-set bytes, got $FullBytes"
}

Write-Host "Full SLDEM2015 512-ppd JP2 set is present: $FullBytes bytes"
```

Verify every expanded file against the pinned official PDS volume manifest, then generate a local SHA-256 manifest outside `tiles/jp2/`:

```powershell
$PdsManifestUrl = "https://pds-geosciences.wustl.edu/lro/lro-l-lola-3-rdr-v1/lrolol_1xxx_260615.md5"
$PdsManifestPath = Join-Path $SldemRoot "pds-lrolol_1xxx_260615.md5"

if (-not (Test-Path -LiteralPath $PdsManifestPath)) {
    $PdsManifestPartial = "$PdsManifestPath.part"
    & curl.exe --fail --location --retry 5 --continue-at - --output $PdsManifestPartial $PdsManifestUrl
    if ($LASTEXITCODE -ne 0) {
        throw "curl.exe failed for the PDS checksum manifest with exit code $LASTEXITCODE"
    }
    Move-Item -LiteralPath $PdsManifestPartial -Destination $PdsManifestPath
}

$PdsMD5ByName = @{}
foreach ($Line in [System.IO.File]::ReadLines($PdsManifestPath)) {
    if ($Line -match '^(?<Hash>[0-9A-Fa-f]{32})\s+.*\\data\\sldem2015\\tiles\\jp2\\(?<Name>[^\\]+)$') {
        $PdsMD5ByName[$Matches.Name.ToLowerInvariant()] = $Matches.Hash.ToLowerInvariant()
    }
}

foreach ($File in $FullFiles) {
    $Name = $File.Name.ToLowerInvariant()
    if (-not $PdsMD5ByName.ContainsKey($Name)) {
        throw "No official PDS MD5 found for $($File.Name)"
    }

    $ActualMD5 = (Get-FileHash -LiteralPath $File.FullName -Algorithm MD5).Hash.ToLowerInvariant()
    if ($ActualMD5 -ne $PdsMD5ByName[$Name]) {
        throw "Official PDS MD5 mismatch for $($File.Name)"
    }
}

$SHA256ManifestPath = Join-Path $SldemRoot "SLDEM2015_512ppd_SHA256SUMS.txt"
[string[]]$SHA256Lines = $FullFiles |
    Sort-Object -Property Name |
    ForEach-Object {
        $Digest = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        "$Digest  tiles/jp2/$($_.Name)"
    }
[System.IO.File]::WriteAllLines($SHA256ManifestPath, $SHA256Lines, [System.Text.Encoding]::ASCII)

foreach ($File in $FullFiles) {
    $File.IsReadOnly = $true
}

Write-Host "Verified all PDS MD5 values and wrote $SHA256ManifestPath"
```

The dated PDS manifest pins the archive revision used to establish this workflow. If PDS replaces or removes it, stop and review a newer official manifest rather than silently accepting different bytes. Do not treat byte count alone as integrity verification, and do not add the expanded data, checksum manifests, or machine-local path to git.

## Failure recovery

- A failed transfer leaves only its `.part` file. Re-run the same download block to resume it.
- A size or checksum mismatch is not an accepted source revision. Clear the read-only attribute if necessary, delete only the named failed file, and download it again.
- If the second download still differs from this document, stop before M3 and review the official PDS product and checksum manifests.
- Do not rename the official sidecars to make a manual GDAL check pass. Preserve the provenance tree and handle any GDAL-compatible alias outside the artifact bundle.

## Completion criteria

Preparation on a development machine is complete when:

- the three initial source files exist at the documented source-relative paths;
- their byte counts, PDS MD5 values, SHA-256 values, and canonical bundle hash all pass;
- the source files are read-only and no `.part` file remains;
- `SLDEM2015_ROOT` resolves to the source root for the local M3 process;
- this workflow has added no dataset bytes or machine-local configuration to the repository; and
- full-set expansion remains optional until a milestone requires more than the initial M3 subset.
