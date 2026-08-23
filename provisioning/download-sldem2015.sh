#!/usr/bin/env bash

set -Eeuo pipefail

usage() {
    printf 'Usage: %s Artifact|Fullset\n' "${0##*/}" >&2
}

if (($# != 1)); then
    usage
    exit 2
fi

mode=$1
case "$mode" in
    Artifact | Fullset) ;;
    *)
        usage
        exit 2
        ;;
esac

if [[ -z ${SLDEM2015_ROOT:-} ]]; then
    printf 'error: SLDEM2015_ROOT must name the external SLDEM2015 source root.\n' >&2
    exit 1
fi

for command_name in curl md5sum sha256sum stat chmod find realpath sort; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        printf 'error: required command not found: %s\n' "$command_name" >&2
        exit 1
    fi
done

sldem_root=$(realpath -m -- "$SLDEM2015_ROOT")
if [[ $sldem_root == / ]]; then
    printf 'error: SLDEM2015_ROOT must not be the filesystem root.\n' >&2
    exit 1
fi
source_directory=$sldem_root/tiles/jp2
pds_tile_root=https://pds-geosciences.wustl.edu/lro/lro-l-lola-3-rdr-v1/lrolol_1xxx/data/sldem2015/tiles/jp2
pds_manifest_url=https://pds-geosciences.wustl.edu/lro/lro-l-lola-3-rdr-v1/lrolol_1xxx_260615.md5
artifact_total_bytes=171872390
artifact_bundle_sha256=17810a5b1551a56b865f59c20ae2c78c6aa05112112c557f466e21e19d5b9351
fullset_total_bytes=5466757104

artifact_members=(
    sldem2015_512_00n_30n_000_045.jp2
    sldem2015_512_00n_30n_000_045_aux.xml
    sldem2015_512_00n_30n_000_045_jp2.lbl
)
artifact_bytes=(171864155 3295 4940)
artifact_md5=(
    f009c232030ecd09e21dd67ebafdb02d
    6268c2b44040867f5ddb58992f36f789
    d20e9cb11147652f301cc9f3e492a5ed
)
artifact_sha256=(
    8d6e9bb9687bd19dbc9de57c8044932540bb934e5ef557b473660f5056484a73
    c9c013daa77cfbbd68efb157c51198fda3aed157f0d5ebddaf1a94f9151faa71
    4d8f5edd72cdc486cbe491741a588dc545b7b72baa4cfb0e34cae7389d6305e0
)

fullset_members=()
latitude_bands=(00n_30n 30n_60n 30s_00s 60s_30s)
longitude_spans=(000_045 045_090 090_135 135_180 180_225 225_270 270_315 315_360)
suffixes=(.jp2 _aux.xml _jp2.lbl)

for latitude_band in "${latitude_bands[@]}"; do
    for longitude_span in "${longitude_spans[@]}"; do
        stem=sldem2015_512_${latitude_band}_${longitude_span}
        for suffix in "${suffixes[@]}"; do
            fullset_members+=("$stem$suffix")
        done
    done
done

download_file() {
    local url=$1
    local target=$2
    local partial=$target.part

    if [[ -f $target ]]; then
        return
    fi

    curl \
        --fail \
        --location \
        --retry 5 \
        --continue-at - \
        --output "$partial" \
        "$url"
    mv -- "$partial" "$target"
}

write_u32_le() {
    local value=$1
    printf '%b' "$(printf '\\%03o\\%03o\\%03o\\%03o' \
        "$((value & 255))" \
        "$(((value >> 8) & 255))" \
        "$(((value >> 16) & 255))" \
        "$(((value >> 24) & 255))")"
}

write_u64_le() {
    local value=$1
    local byte
    local escaped=
    local shift
    for shift in 0 8 16 24 32 40 48 56; do
        byte=$(((value >> shift) & 255))
        printf -v escaped '%s\\%03o' "$escaped" "$byte"
    done
    printf '%b' "$escaped"
}

write_hex() {
    local hex=$1
    local escaped=
    local index
    for ((index = 0; index < ${#hex}; index += 2)); do
        escaped+=\\x${hex:index:2}
    done
    printf '%b' "$escaped"
}

verify_artifact() {
    local total_bytes=0
    local index member path actual_bytes actual_md5 actual_sha256

    for index in "${!artifact_members[@]}"; do
        member=${artifact_members[index]}
        path=$source_directory/$member
        if [[ ! -f $path ]]; then
            printf 'error: missing artifact member: %s\n' "$path" >&2
            return 1
        fi

        actual_bytes=$(stat -c '%s' -- "$path")
        actual_md5=$(md5sum -- "$path")
        actual_md5=${actual_md5%% *}
        actual_sha256=$(sha256sum -- "$path")
        actual_sha256=${actual_sha256%% *}

        if [[ $actual_bytes != "${artifact_bytes[index]}" ]]; then
            printf 'error: byte-count mismatch for %s: expected %s, got %s. Delete only this file and re-run.\n' \
                "$member" "${artifact_bytes[index]}" "$actual_bytes" >&2
            return 1
        fi
        if [[ $actual_md5 != "${artifact_md5[index]}" ]]; then
            printf 'error: official PDS MD5 mismatch for %s. Delete only this file and re-run.\n' "$member" >&2
            return 1
        fi
        if [[ $actual_sha256 != "${artifact_sha256[index]}" ]]; then
            printf 'error: SHA-256 mismatch for %s. Delete only this file and re-run.\n' "$member" >&2
            return 1
        fi

        ((total_bytes += actual_bytes))
    done

    if ((total_bytes != artifact_total_bytes)); then
        printf 'error: artifact byte-count mismatch: expected %s, got %s\n' \
            "$artifact_total_bytes" "$total_bytes" >&2
        return 1
    fi

    local bundle_digest
    bundle_digest=$(
        {
            printf 'LTDB_ARTIFACT_BUNDLE_V1\0'
            write_u32_le "${#artifact_members[@]}"
            for index in "${!artifact_members[@]}"; do
                member=tiles/jp2/${artifact_members[index]}
                write_u32_le "${#member}"
                printf '%s' "$member"
                write_u64_le "${artifact_bytes[index]}"
                write_hex "${artifact_sha256[index]}"
            done
        } | sha256sum
    )
    bundle_digest=${bundle_digest%% *}

    if [[ $bundle_digest != "$artifact_bundle_sha256" ]]; then
        printf 'error: artifact bundle SHA-256 mismatch: expected %s, got %s\n' \
            "$artifact_bundle_sha256" "$bundle_digest" >&2
        return 1
    fi

    printf '%s' "$total_bytes"
}

verify_fullset() {
    if ((${#fullset_members[@]} != 96)); then
        printf 'error: internal full-set member list has %s entries; expected 96\n' \
            "${#fullset_members[@]}" >&2
        return 1
    fi

    local pds_manifest_path=$sldem_root/pds-lrolol_1xxx_260615.md5
    download_file "$pds_manifest_url" "$pds_manifest_path"

    declare -A pds_md5_by_name=()
    local line manifest_hash manifest_name
    while IFS= read -r line || [[ -n $line ]]; do
        line=${line%$'\r'}
        if [[ $line =~ ^([[:xdigit:]]{32})[[:space:]]+.*\\data\\sldem2015\\tiles\\jp2\\([^\\]+)$ ]]; then
            manifest_hash=${BASH_REMATCH[1],,}
            manifest_name=${BASH_REMATCH[2],,}
            pds_md5_by_name[$manifest_name]=$manifest_hash
        fi
    done <"$pds_manifest_path"

    local total_bytes=0
    local member path actual_bytes actual_md5
    for member in "${fullset_members[@]}"; do
        path=$source_directory/$member
        if [[ ! -f $path ]]; then
            printf 'error: missing full-set member: %s\n' "$path" >&2
            return 1
        fi
        if [[ -z ${pds_md5_by_name[$member]:-} ]]; then
            printf 'error: no official PDS MD5 found for %s in %s\n' "$member" "$pds_manifest_path" >&2
            return 1
        fi

        actual_md5=$(md5sum -- "$path")
        actual_md5=${actual_md5%% *}
        if [[ $actual_md5 != "${pds_md5_by_name[$member]}" ]]; then
            printf 'error: official PDS MD5 mismatch for %s. Delete only this file and re-run.\n' "$member" >&2
            return 1
        fi

        actual_bytes=$(stat -c '%s' -- "$path")
        ((total_bytes += actual_bytes))
    done

    if ((total_bytes != fullset_total_bytes)); then
        printf 'error: full-set byte-count mismatch: expected %s, got %s\n' \
            "$fullset_total_bytes" "$total_bytes" >&2
        return 1
    fi

    local sha256_manifest_path=$sldem_root/SLDEM2015_512ppd_SHA256SUMS.txt
    local sha256_manifest_partial=$sha256_manifest_path.part
    : >"$sha256_manifest_partial"
    while IFS= read -r member; do
        path=$source_directory/$member
        actual_sha256=$(sha256sum -- "$path")
        actual_sha256=${actual_sha256%% *}
        printf '%s  tiles/jp2/%s\n' "$actual_sha256" "$member" >>"$sha256_manifest_partial"
    done < <(printf '%s\n' "${fullset_members[@]}" | LC_ALL=C sort)
    mv -f -- "$sha256_manifest_partial" "$sha256_manifest_path"

    printf '%s' "$total_bytes"
}

assert_no_partial_files() {
    local partial
    partial=$(find "$sldem_root" -type f -name '*.part' -print -quit)
    if [[ -n $partial ]]; then
        printf 'error: incomplete download remains at %s; re-run the same mode to resume it.\n' "$partial" >&2
        return 1
    fi
}

mkdir -p -- "$source_directory"

if [[ $mode == Artifact ]]; then
    members=("${artifact_members[@]}")
else
    members=("${fullset_members[@]}")
fi

for member in "${members[@]}"; do
    download_file "$pds_tile_root/$member" "$source_directory/$member"
done

verified_artifact_bytes=$(verify_artifact)

if [[ $mode == Fullset ]]; then
    verified_fullset_bytes=$(verify_fullset)
    for member in "${fullset_members[@]}"; do
        chmod a-w -- "$source_directory/$member"
    done
    assert_no_partial_files
    printf 'Verified 32 SLDEM2015 tiles and their 64 sidecars: %s bytes\n' "$verified_fullset_bytes"
    printf 'Wrote SHA-256 manifest: %s/SLDEM2015_512ppd_SHA256SUMS.txt\n' "$sldem_root"
else
    for member in "${artifact_members[@]}"; do
        chmod a-w -- "$source_directory/$member"
    done
    assert_no_partial_files
    printf 'Verified SLDEM2015 artifact: %s bytes\n' "$verified_artifact_bytes"
    printf 'Verified artifact bundle SHA-256: %s\n' "$artifact_bundle_sha256"
fi

printf 'SLDEM2015_ROOT=%s\n' "$sldem_root"
