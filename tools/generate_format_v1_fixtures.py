#!/usr/bin/env python3
"""Generate or verify the reviewed Lunar Terrain format v1 golden vectors."""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "tests" / "golden" / "format_v1"


def u8(value: int) -> bytes:
    return struct.pack("<B", value)


def u16(value: int) -> bytes:
    return struct.pack("<H", value)


def u32(value: int) -> bytes:
    return struct.pack("<I", value)


def u64(value: int) -> bytes:
    return struct.pack("<Q", value)


def f32(value: float) -> bytes:
    return struct.pack("<f", value)


def f64(value: float) -> bytes:
    return struct.pack("<d", value)


def domain(name: str) -> bytes:
    return name.encode("ascii") + b"\0"


def sha256(data: bytes) -> bytes:
    return hashlib.sha256(data).digest()


def align8(value: int) -> int:
    return (value + 7) & ~7


def pad_to(data: bytes, size: int) -> bytes:
    if len(data) > size:
        raise ValueError(f"record is {len(data)} bytes, expected no more than {size}")
    return data + bytes(size - len(data))


def crc32c(data: bytes) -> int:
    value = 0xFFFFFFFF
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0x82F63B78 if value & 1 else 0)
    return value ^ 0xFFFFFFFF


def morton_interleave(x: int, y: int, level: int) -> int:
    morton = 0
    for bit in range(level):
        morton |= ((x >> bit) & 1) << (2 * bit)
        morton |= ((y >> bit) & 1) << (2 * bit + 1)
    return morton


def tile_key(face: int, level: int, x: int, y: int) -> int:
    return (face << 61) | (level << 56) | morton_interleave(x, y, level)


def string_table(strings: list[str]) -> tuple[bytes, dict[str, int]]:
    ordered = [""] + sorted(set(strings) - {""}, key=lambda value: value.encode("utf-8"))
    data = bytearray()
    offsets: dict[str, int] = {}
    for value in ordered:
        encoded = value.encode("utf-8")
        offsets[value] = len(data)
        data += u32(len(encoded))
        data += encoded
        while len(data) % 4:
            data.append(0)
    return bytes(data), offsets


def channel_record(
    channel_id: int,
    element_type: int,
    codec: int,
    predictor: int,
    width: int,
    height: int,
    flags: int,
    offset: int,
    payload: bytes,
    logical_bytes: int,
    parameter0: int,
    parameter1: int,
) -> bytes:
    return b"".join(
        (
            u16(channel_id),
            u16(1),
            u8(element_type),
            u8(1),
            u8(codec),
            u8(predictor),
            u16(width),
            u16(height),
            u32(flags),
            u32(offset),
            u32(len(payload)),
            u32(logical_bytes),
            u32(crc32c(payload)),
            u32(parameter0),
            u32(parameter1),
        )
    )


def content_channel_frame(
    channel_id: int,
    element_type: int,
    predictor: int,
    width: int,
    height: int,
    flags: int,
    parameter1: int,
    decoded: bytes,
) -> bytes:
    return b"".join(
        (
            u16(channel_id),
            u16(1),
            u8(element_type),
            u8(1),
            u8(predictor),
            u8(0),
            u16(width),
            u16(height),
            u32(flags),
            u32(len(decoded)),
            u32(0),
            u32(parameter1),
            decoded,
        )
    )


def chunk_hash_frame(tag: bytes, payload: bytes) -> bytes:
    return tag + u16(1) + u16(0x0003) + u64(len(payload)) + payload


def hex_text(data: bytes) -> str:
    lines = [f"# size: {len(data)}"]
    for offset in range(0, len(data), 16):
        line = " ".join(f"{byte:02x}" for byte in data[offset : offset + 16])
        lines.append(f"{offset:08x}: {line}")
    return "\n".join(lines) + "\n"


def make_vectors() -> dict[str, bytes]:
    key_text = "fixture.dem.v1"
    key_bytes = key_text.encode("utf-8")
    dataset_id = int.from_bytes(
        sha256(domain("LTDB_DATASET_V1") + u64(len(key_bytes)) + key_bytes)[:4], "little"
    )

    members = [
        ("dem.img", b"DEM"),
        ("dem.lbl", b"L1"),
    ]
    bundle_input = bytearray(domain("LTDB_ARTIFACT_BUNDLE_V1") + u32(len(members)))
    member_metadata: list[tuple[str, int, bytes]] = []
    for name, contents in members:
        name_bytes = name.encode("utf-8")
        member_hash = sha256(contents)
        bundle_input += u32(len(name_bytes)) + name_bytes + u64(len(contents)) + member_hash
        member_metadata.append((name, len(contents), member_hash))
    bundle_hash = sha256(bytes(bundle_input))
    bundle_bytes = sum(size for _, size, _ in member_metadata)

    meta_text = (
        '{"artifact_members":['
        + ",".join(
            f'{{"bytes":{size},"name":"{name}","sha256":"{digest.hex()}"}}'
            for name, size, digest in member_metadata
        )
        + '],"datum":{"elevation":"meters_above_reference_radius","reference_radius_m":1737400},'
        + '"metadata_overrides":{},"no_data":{"declared":false},"stable_key":"fixture.dem.v1"}'
    )
    meta = meta_text.encode("utf-8")

    pack_path = "Packs/Fixture_F2_L08_P0000.ltp"
    dataset_strings = [
        "Fixture DEM",
        "OpenAI Test Lab",
        "Fixture Mission",
        "Fixture Instrument",
        "1.0",
        "https://example.invalid/fixture",
        "IAU_2015:30100",
        "CC0-1.0",
    ]
    strs, string_ids = string_table(dataset_strings + [pack_path])

    canonical_nan = struct.pack("<Q", 0x7FF8000000000000)
    dataset_float_fields = f64(100.0) + canonical_nan + f64(5.0) + canonical_nan
    dset = b"".join(
        (
            u32(dataset_id),
            u32(0),
            *(u32(string_ids[value]) for value in dataset_strings),
            dataset_float_fields,
            u64(bundle_bytes),
            bundle_hash,
            u32(0),
            u32(len(meta)),
            u32(1),
            u32(0),
        )
    )
    assert len(dset) == 128

    sample_codes = [100, 101, 103, 99, 101, 104, 98, 102, 106]
    decoded_elev = b"".join(u16(value) for value in sample_codes)
    residual_codes = [100, 1, 2, 0xFFFF, 1, 1, 0xFFFF, 2, 1]
    elev = b"".join(u16(value) for value in residual_codes)

    provenance_header = u16(1) + u16(2) + u16(2) + u16(2) + u8(1) + u8(0) + u16(1) + u32(0)
    provenance_entry0 = u32(dataset_id) + u32(0) + f32(0.75) + f32(100.0)
    secondary_dataset = 0xA1B2C3D4
    provenance_entry1 = u32(secondary_dataset) + u32(0) + f32(0.25) + f32(5.0)
    provenance_map = bytes((0, 0, 1, 1))
    prvn = provenance_header + provenance_entry0 + provenance_entry1 + provenance_map
    qual = bytes((0, 4, 1, 12))
    assert len(prvn) == 52

    encoded_key = tile_key(2, 8, 147, 92)
    content_input = bytearray(domain("LTDB_TILE_CONTENT_V1") + u64(encoded_key) + u16(3))
    content_input += content_channel_frame(1, 2, 1, 3, 3, 1, 1, decoded_elev)
    content_input += content_channel_frame(2, 255, 0, 2, 2, 1, 0, prvn)
    content_input += content_channel_frame(3, 1, 0, 2, 2, 0, 0, qual)
    content_hash = sha256(bytes(content_input))

    semantic_config = (
        '{"algorithm_version":1,"datasets":[{"key":"fixture.dem.v1","priority":100,'
        '"source_uri":"https://example.invalid/fixture"}],"datum":{"reference_radius_m":1737400},'
        '"fusion":{"algorithm":"residual_refinement_v1"},"projection":{"type":"qsc","version":1},'
        '"quantization":{"origin_m":-16384,"step_m":0.5},"tiles":{"apron":1,"cells":256,"max_level":8}}'
    ).encode("utf-8")
    semantic_config_hash = sha256(
        domain("LTDB_SEMANTIC_CONFIG_V1") + u64(len(semantic_config)) + semantic_config
    )
    zero_hash = bytes(32).hex()
    window_hash = sha256(domain("LTDB_SOURCE_WINDOW_V1") + b"fixture-window")
    dependency_document = (
        '{"builder_algorithm_version":1,"datum_version":"lunar_mean_radius_1737400_v1",'
        f'"fusion":{{"algorithm":"residual_refinement_v1","configuration_sha256":"{zero_hash}"}},'
        '"projection":{"id":1,"implementation_version":1},'
        f'"quantization":{{"configuration_sha256":"{zero_hash}","id":1}},'
        f'"semantic_configuration_sha256":"{semantic_config_hash.hex()}",'
        f'"sources":[{{"artifact_bundle_sha256":"{bundle_hash.hex()}","dataset_id":{dataset_id},'
        f'"windows":[{{"dependency_sha256":"{window_hash.hex()}"}}]}}],'
        f'"tile_key":"{encoded_key:016x}","tile_schema_version":1}}'
    ).encode("utf-8")
    dependency_hash = sha256(
        domain("LTDB_TILE_DEP_V1") + u64(len(dependency_document)) + dependency_document
    )

    directory_offset = 128
    directory_bytes = 3 * 40
    data_offset = align8(directory_offset + directory_bytes)
    elev_offset = data_offset
    prvn_offset = align8(elev_offset + len(elev))
    qual_offset = align8(prvn_offset + len(prvn))
    payload_bytes = qual_offset + len(qual)
    data_region_bytes = payload_bytes - data_offset

    elev_record = channel_record(1, 2, 0, 1, 3, 3, 1, elev_offset, elev, len(elev), 0, 1)
    prvn_record = channel_record(2, 255, 0, 0, 2, 2, 1, prvn_offset, prvn, len(prvn), 0, 0)
    qual_record = channel_record(3, 1, 0, 0, 2, 2, 0, qual_offset, qual, len(qual), 0, 0)

    tile_header = pad_to(
        b"".join(
            (
                b"LTIL",
                u16(1),
                u16(128),
                u64(encoded_key),
                u32(0x0003),
                u16(3),
                u16(256),
                u16(257),
                u8(1),
                u8(1),
                f32(53.0),
                f32(1.25),
                f32(-16335.0),
                f32(-16331.0),
                u32(dataset_id),
                u16(2),
                u16(0),
                u32(directory_offset),
                u32(directory_bytes),
                u32(data_offset),
                u32(data_region_bytes),
                dependency_hash[:16],
                content_hash[:16],
            )
        ),
        128,
    )

    tile_payload_buffer = bytearray(tile_header + elev_record + prvn_record + qual_record)
    tile_payload_buffer += bytes(elev_offset - len(tile_payload_buffer)) + elev
    tile_payload_buffer += bytes(prvn_offset - len(tile_payload_buffer)) + prvn
    tile_payload_buffer += bytes(qual_offset - len(tile_payload_buffer)) + qual
    tile_payload = bytes(tile_payload_buffer)
    assert len(tile_payload) == payload_bytes

    logical_channel_bytes = len(elev) + len(prvn) + len(qual)
    tile_index = b"".join(
        (
            u64(encoded_key),
            u32(0),
            u32(0x0003),
            u64(64),
            u32(len(tile_payload)),
            u32(logical_channel_bytes),
            u16(min(sample_codes)),
            u16(max(sample_codes)),
            u32(dataset_id),
            u32(53000),
            u32(1250),
            u8(0x05),
            u8(3),
            u16(0),
            u32(crc32c(tile_payload)),
            content_hash[:16],
            dependency_hash[:8],
        )
    )
    assert len(tile_index) == 80

    pack_file_bytes = 64 + len(tile_payload)
    pack_header_zero = b"".join(
        (
            b"LTPK",
            u16(1),
            u16(0),
            u32(64),
            u32(0x01020304),
            u32(0),
            u32(0),
            u64(1),
            u64(64),
            u64(pack_file_bytes),
            bytes(16),
        )
    )
    pack_hash = sha256(pack_header_zero + tile_payload)
    pack_header = pack_header_zero[:48] + pack_hash[:16]
    pack_file = pack_header + tile_payload

    pack_record = b"".join(
        (
            u32(0),
            u32(0),
            u32(string_ids[pack_path]),
            u16(1),
            u16(0),
            u64(1),
            u64(len(pack_file)),
            u64(encoded_key),
            u64(encoded_key),
            pack_hash,
        )
    )
    assert len(pack_record) == 80

    config = (
        '{"package":{"codec":"zstd","codec_level":3,"format_major":1,"format_minor":0,'
        '"name":"Fixture","pack_naming":"v1","target_pack_bytes":1073741824},'
        '"semantic":'
    ).encode("utf-8") + semantic_config + b"}"
    builder_hash = sha256(domain("LTDB_BUILDER_CONFIG_V1") + u64(len(config)) + config)

    registry_input = bytearray(domain("LTDB_DATASET_REGISTRY_V1") + u32(1))
    registry_input += u32(dataset_id) + u32(0)
    for value in dataset_strings:
        encoded = value.encode("utf-8")
        registry_input += u64(len(encoded)) + encoded
    registry_input += dataset_float_fields
    registry_input += u64(bundle_bytes) + bundle_hash
    registry_input += u64(len(meta)) + meta + u32(1)
    dataset_registry_hash = sha256(bytes(registry_input))
    tile_index_hash = sha256(domain("LTDB_TILE_INDEX_V1") + u64(len(tile_index)) + tile_index)

    chunks = {
        b"DSET": dset,
        b"META": meta,
        b"PACK": pack_record,
        b"STRS": strs,
        b"TIDX": tile_index,
    }
    database_content_input = bytearray(
        domain("LTDB_DATABASE_CONTENT_V1") + u16(1) + u16(0) + builder_hash + u32(len(chunks))
    )
    for tag in sorted(chunks):
        database_content_input += chunk_hash_frame(tag, chunks[tag])
    database_content_input += u32(1) + u32(0) + pack_hash
    database_content_hash = sha256(bytes(database_content_input))
    database_id = sha256(domain("LTDB_DATABASE_ID_V1") + builder_hash + dataset_registry_hash)[:16]

    chunk_offset = align8(256 + len(chunks) * 40)
    chunk_offsets: dict[bytes, int] = {}
    for tag in sorted(chunks):
        chunk_offsets[tag] = chunk_offset
        chunk_offset = align8(chunk_offset + len(chunks[tag]))

    chunk_directory_entries: dict[bytes, bytes] = {}
    for tag in sorted(chunks):
        payload = chunks[tag]
        chunk_directory_entries[tag] = b"".join(
            (
                tag,
                u16(1),
                u16(0x0003),
                u64(chunk_offsets[tag]),
                u64(len(payload)),
                u64(len(payload)),
                u32(crc32c(payload)),
                u32(0),
            )
        )

    ltdb_header = pad_to(
        b"".join(
            (
                b"LTDB",
                u16(1),
                u16(0),
                u32(256),
                u32(0x01020304),
                u32(0),
                u32(len(chunks)),
                u64(256),
                database_id,
                f64(1737400.0),
                f64(-16384.0),
                f64(0.5),
                u16(256),
                u16(257),
                u8(1),
                u8(8),
                u8(1),
                u8(1),
                u64(1),
                u32(1),
                u32(1),
                builder_hash,
                dataset_registry_hash,
                tile_index_hash,
                database_content_hash,
            )
        ),
        256,
    )

    database_buffer = bytearray(ltdb_header)
    for tag in sorted(chunks):
        database_buffer += chunk_directory_entries[tag]
    for tag in sorted(chunks):
        database_buffer += bytes(chunk_offsets[tag] - len(database_buffer))
        database_buffer += chunks[tag]
    database_file = bytes(database_buffer)

    return {
        "artifact_bundle_hash_input_v1": bytes(bundle_input),
        "builder_config_jcs_v1": config,
        "channel_record_elev_v1": elev_record,
        "channel_record_prvn_v1": prvn_record,
        "channel_record_qual_v1": qual_record,
        "chunk_directory_entry_v1": chunk_directory_entries[b"DSET"],
        "database_file_v1": database_file,
        "dataset_record_v1": dset,
        "elev_channel_decoded_v1": decoded_elev,
        "elev_channel_logical_v1": elev,
        "ltdb_header_v1": ltdb_header,
        "meta_chunk_v1": meta,
        "pack_file_v1": pack_file,
        "pack_header_v1": pack_header,
        "pack_record_v1": pack_record,
        "provenance_header_v1": provenance_header,
        "provenance_palette_entry_v1": provenance_entry0,
        "prvn_channel_v1": prvn,
        "qual_channel_v1": qual,
        "string_table_v1": strs,
        "tile_dependency_jcs_v1": dependency_document,
        "tile_header_v1": tile_header,
        "tile_index_record_v1": tile_index,
        "tile_payload_v1": tile_payload,
    }


def expected_files(vectors: dict[str, bytes]) -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    sums: list[str] = []
    for name, data in sorted(vectors.items()):
        files[f"{name}.bin"] = data
        files[f"{name}.hex"] = hex_text(data).encode("ascii")
        sums.append(f"{hashlib.sha256(data).hexdigest()}  {name}.bin")
    files["SHA256SUMS"] = ("\n".join(sums) + "\n").encode("ascii")
    return files


def write_files(output: Path, files: dict[str, bytes]) -> None:
    output.mkdir(parents=True, exist_ok=True)
    for relative, data in files.items():
        (output / relative).write_bytes(data)


def check_files(output: Path, files: dict[str, bytes]) -> bool:
    okay = True
    for relative, expected in files.items():
        path = output / relative
        if not path.exists():
            print(f"missing: {path}", file=sys.stderr)
            okay = False
            continue
        actual = path.read_bytes()
        if actual != expected:
            print(f"mismatch: {path}", file=sys.stderr)
            okay = False
    return okay


def main() -> int:
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true", help="write reviewed hex and generated binaries")
    mode.add_argument("--check", action="store_true", help="verify checked-in files without modifying them")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    vectors = make_vectors()
    files = expected_files(vectors)
    if args.write:
        write_files(args.output, files)
        print(f"wrote {len(vectors)} vectors to {args.output}")
        return 0
    return 0 if check_files(args.output, files) else 1


if __name__ == "__main__":
    raise SystemExit(main())
