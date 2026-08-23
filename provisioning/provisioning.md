# SLDEM2015 provisioning

Set `SLDEM2015_ROOT` to a persistent directory outside this repository. The scripts require `curl` plus the platform's standard checksum tools, preserve the official `tiles/jp2/` member names, resume interrupted `.part` downloads, and skip files already present.

## Windows

Run from PowerShell 5.1 or PowerShell 7:

```powershell
.\provisioning\download-sldem2015.ps1 Artifact
.\provisioning\download-sldem2015.ps1 Fullset
```

## Linux

Run with Bash 4 or newer:

```bash
./provisioning/download-sldem2015.sh Artifact
./provisioning/download-sldem2015.sh Fullset
```

`Artifact` downloads and verifies the pinned JP2 tile, detached PDS label, and auxiliary XML sidecar. It checks each file's byte count, official PDS MD5, SHA-256, and the canonical artifact-bundle hash.

`Fullset` downloads all 32 JP2 tiles and their 64 official sidecars. It verifies all 96 files against the pinned PDS MD5 manifest and total byte count, verifies the artifact subset, and writes `SLDEM2015_512ppd_SHA256SUMS.txt` under `SLDEM2015_ROOT`.

Verified source files are made read-only. A failed transfer retains its `.part` file; run the same command again to resume. For a reported size or checksum mismatch, make only the named file writable if needed, delete it, and retry. If the replacement still differs, stop and review the official PDS product and checksum manifest. Do not rename sidecars or add dataset files, manifests, or machine-local paths to git.
