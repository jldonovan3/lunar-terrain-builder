[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet("Artifact", "Fullset")]
    [string]$Mode
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($env:SLDEM2015_ROOT)) {
    throw "SLDEM2015_ROOT must name the external SLDEM2015 source root."
}

$Curl = Get-Command curl.exe -ErrorAction Stop
$SldemRoot = [System.IO.Path]::GetFullPath($env:SLDEM2015_ROOT)
if ($SldemRoot -eq [System.IO.Path]::GetPathRoot($SldemRoot)) {
    throw "SLDEM2015_ROOT must not be a filesystem root."
}
$SourceDirectory = Join-Path $SldemRoot "tiles\jp2"
$PdsTileRoot = "https://pds-geosciences.wustl.edu/lro/lro-l-lola-3-rdr-v1/lrolol_1xxx/data/sldem2015/tiles/jp2"
$PdsManifestUrl = "https://pds-geosciences.wustl.edu/lro/lro-l-lola-3-rdr-v1/lrolol_1xxx_260615.md5"
$ArtifactTotalBytes = [int64]171872390
$ArtifactBundleSHA256 = "17810a5b1551a56b865f59c20ae2c78c6aa05112112c557f466e21e19d5b9351"
$FullsetTotalBytes = [int64]5466757104

$ArtifactMembers = [ordered]@{
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

function Get-FullsetMembers {
    $Members = [System.Collections.Generic.List[string]]::new()
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
                $Members.Add("$Stem$Suffix")
            }
        }
    }

    return $Members.ToArray()
}

function Invoke-ResumableDownload {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Url,
        [Parameter(Mandatory = $true)]
        [string]$Target
    )

    if (Test-Path -LiteralPath $Target -PathType Leaf) {
        return
    }

    $Partial = "$Target.part"
    & $Curl.Source `
        --fail `
        --location `
        --retry 5 `
        --continue-at - `
        --output $Partial `
        $Url
    if ($LASTEXITCODE -ne 0) {
        throw "curl.exe failed for $Url with exit code $LASTEXITCODE; re-run to resume $Partial"
    }

    Move-Item -LiteralPath $Partial -Destination $Target
}

function Assert-NoPartialFiles {
    $PartialFiles = @(Get-ChildItem -LiteralPath $SldemRoot -File -Recurse -Filter "*.part" -ErrorAction Stop)
    if ($PartialFiles.Count -ne 0) {
        throw "Incomplete download remains at $($PartialFiles[0].FullName); re-run the same mode to resume it."
    }
}

function Test-Artifact {
    $TotalBytes = [int64]0

    foreach ($Entry in $ArtifactMembers.GetEnumerator()) {
        $Path = Join-Path $SourceDirectory $Entry.Key
        $File = Get-Item -LiteralPath $Path -ErrorAction Stop
        $ActualMD5 = (Get-FileHash -LiteralPath $Path -Algorithm MD5).Hash.ToLowerInvariant()
        $ActualSHA256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()

        if ($File.Length -ne $Entry.Value.Bytes) {
            throw "Byte-count mismatch for $($Entry.Key): expected $($Entry.Value.Bytes), got $($File.Length). Delete only this file and re-run."
        }
        if ($ActualMD5 -ne $Entry.Value.MD5) {
            throw "Official PDS MD5 mismatch for $($Entry.Key). Delete only this file and re-run."
        }
        if ($ActualSHA256 -ne $Entry.Value.SHA256) {
            throw "SHA-256 mismatch for $($Entry.Key). Delete only this file and re-run."
        }

        $TotalBytes += $File.Length
    }

    if ($TotalBytes -ne $ArtifactTotalBytes) {
        throw "Artifact byte-count mismatch: expected $ArtifactTotalBytes, got $TotalBytes"
    }

    $Stream = [System.IO.MemoryStream]::new()
    $Writer = [System.IO.BinaryWriter]::new($Stream, [System.Text.Encoding]::UTF8, $true)
    try {
        $Writer.Write([System.Text.Encoding]::ASCII.GetBytes("LTDB_ARTIFACT_BUNDLE_V1"))
        $Writer.Write([byte]0)
        $Writer.Write([uint32]$ArtifactMembers.Count)

        foreach ($Entry in $ArtifactMembers.GetEnumerator()) {
            $RelativePath = "tiles/jp2/$($Entry.Key)"
            [byte[]]$NameBytes = [System.Text.Encoding]::UTF8.GetBytes($RelativePath)
            [byte[]]$DigestBytes = for ($Index = 0; $Index -lt $Entry.Value.SHA256.Length; $Index += 2) {
                [Convert]::ToByte($Entry.Value.SHA256.Substring($Index, 2), 16)
            }

            $Writer.Write([uint32]$NameBytes.Length)
            $Writer.Write($NameBytes)
            $Writer.Write([uint64]$Entry.Value.Bytes)
            $Writer.Write($DigestBytes)
        }

        $Writer.Flush()
        $SHA256 = [System.Security.Cryptography.SHA256]::Create()
        try {
            $BundleDigestBytes = $SHA256.ComputeHash($Stream.ToArray())
            $BundleDigest = -join ($BundleDigestBytes | ForEach-Object { $_.ToString("x2") })
        } finally {
            $SHA256.Dispose()
        }
    } finally {
        $Writer.Dispose()
        $Stream.Dispose()
    }

    if ($BundleDigest -ne $ArtifactBundleSHA256) {
        throw "Artifact bundle SHA-256 mismatch: expected $ArtifactBundleSHA256, got $BundleDigest"
    }

    return $TotalBytes
}

function Test-Fullset {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Members
    )

    if ($Members.Count -ne 96) {
        throw "Internal full-set member list has $($Members.Count) entries; expected 96"
    }

    $PdsManifestPath = Join-Path $SldemRoot "pds-lrolol_1xxx_260615.md5"
    Invoke-ResumableDownload -Url $PdsManifestUrl -Target $PdsManifestPath

    $PdsMD5ByName = @{}
    foreach ($Line in [System.IO.File]::ReadLines($PdsManifestPath)) {
        if ($Line -match '^(?<Hash>[0-9A-Fa-f]{32})\s+.*\\data\\sldem2015\\tiles\\jp2\\(?<Name>[^\\]+)$') {
            $PdsMD5ByName[$Matches.Name.ToLowerInvariant()] = $Matches.Hash.ToLowerInvariant()
        }
    }

    $TotalBytes = [int64]0
    $Files = [System.Collections.Generic.List[System.IO.FileInfo]]::new()
    foreach ($Member in $Members) {
        $Path = Join-Path $SourceDirectory $Member
        $File = Get-Item -LiteralPath $Path -ErrorAction Stop
        $Name = $File.Name.ToLowerInvariant()
        if (-not $PdsMD5ByName.ContainsKey($Name)) {
            throw "No official PDS MD5 found for $($File.Name) in $PdsManifestPath"
        }

        $ActualMD5 = (Get-FileHash -LiteralPath $File.FullName -Algorithm MD5).Hash.ToLowerInvariant()
        if ($ActualMD5 -ne $PdsMD5ByName[$Name]) {
            throw "Official PDS MD5 mismatch for $($File.Name). Delete only this file and re-run."
        }

        $TotalBytes += $File.Length
        $Files.Add($File)
    }

    if ($TotalBytes -ne $FullsetTotalBytes) {
        throw "Full-set byte-count mismatch: expected $FullsetTotalBytes, got $TotalBytes"
    }

    $SHA256ManifestPath = Join-Path $SldemRoot "SLDEM2015_512ppd_SHA256SUMS.txt"
    $SHA256ManifestPartial = "$SHA256ManifestPath.part"
    [string[]]$SHA256Lines = $Files |
        Sort-Object -Property Name |
        ForEach-Object {
            $Digest = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            "$Digest  tiles/jp2/$($_.Name)"
        }
    [System.IO.File]::WriteAllLines($SHA256ManifestPartial, $SHA256Lines, [System.Text.Encoding]::ASCII)
    Move-Item -LiteralPath $SHA256ManifestPartial -Destination $SHA256ManifestPath -Force

    return [pscustomobject]@{
        Bytes = $TotalBytes
        Files = $Files.ToArray()
        SHA256ManifestPath = $SHA256ManifestPath
    }
}

New-Item -ItemType Directory -Force -Path $SourceDirectory | Out-Null

if ($Mode -eq "Artifact") {
    $Members = [string[]]$ArtifactMembers.Keys
} else {
    $Members = Get-FullsetMembers
}

foreach ($Member in $Members) {
    $Target = Join-Path $SourceDirectory $Member
    Invoke-ResumableDownload -Url "$PdsTileRoot/$Member" -Target $Target
}

$VerifiedArtifactBytes = Test-Artifact

if ($Mode -eq "Fullset") {
    $Fullset = Test-Fullset -Members $Members
    foreach ($File in $Fullset.Files) {
        $File.IsReadOnly = $true
    }
    Assert-NoPartialFiles
    Write-Host "Verified 32 SLDEM2015 tiles and their 64 sidecars: $($Fullset.Bytes) bytes"
    Write-Host "Wrote SHA-256 manifest: $($Fullset.SHA256ManifestPath)"
} else {
    foreach ($Member in $ArtifactMembers.Keys) {
        (Get-Item -LiteralPath (Join-Path $SourceDirectory $Member)).IsReadOnly = $true
    }
    Assert-NoPartialFiles
    Write-Host "Verified SLDEM2015 artifact: $VerifiedArtifactBytes bytes"
    Write-Host "Verified artifact bundle SHA-256: $ArtifactBundleSHA256"
}

Write-Host "SLDEM2015_ROOT=$SldemRoot"
