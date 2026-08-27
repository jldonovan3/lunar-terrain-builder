#!/usr/bin/env python3
"""Check that M8 Builder TOML source identities match provisioning.json.

The parser is intentionally limited to the identity fields used by the checked-in
M8 configurations. It is not a general TOML implementation and keeps the
provisioning path compatible with Python 3.9 and the standard library.
"""

import argparse
import json
from pathlib import Path
import re
import sys
from typing import Dict, Iterable, List, Mapping, Sequence, Tuple


class LockConsistencyError(RuntimeError):
    pass


SCALAR_PATTERN = r"(?m)^{name}\s*=\s*(.+?)\s*$"
MEMBER_PATTERN = re.compile(r"\{([^{}]+)\}")


def _scalar(block: str, name: str) -> object:
    match = re.search(SCALAR_PATTERN.format(name=re.escape(name)), block)
    if match is None:
        raise LockConsistencyError(f"missing Builder field {name!r}")
    text = match.group(1)
    try:
        return json.loads(text)
    except json.JSONDecodeError as error:
        if re.fullmatch(r"[0-9]+", text):
            return int(text)
        raise LockConsistencyError(f"unsupported Builder scalar {name!r}: {text}") from error


def _array_body(block: str, name: str) -> str:
    match = re.search(rf"(?m)^{re.escape(name)}\s*=\s*\[", block)
    if match is None:
        raise LockConsistencyError(f"missing Builder array {name!r}")
    start = match.end() - 1
    depth = 0
    in_string = False
    escaped = False
    for index in range(start, len(block)):
        character = block[index]
        if in_string:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                in_string = False
            continue
        if character == '"':
            in_string = True
        elif character == "[":
            depth += 1
        elif character == "]":
            depth -= 1
            if depth == 0:
                return block[start + 1:index]
    raise LockConsistencyError(f"unterminated Builder array {name!r}")


def _artifact_members(block: str) -> List[Tuple[str, int, str]]:
    result: List[Tuple[str, int, str]] = []
    for member in MEMBER_PATTERN.findall(_array_body(block, "artifact_members")):
        fields: Dict[str, object] = {}
        for name in ("name", "bytes", "sha256"):
            match = re.search(rf"(?:^|,)\s*{name}\s*=\s*(\"(?:\\.|[^\"])*\"|[0-9]+)", member)
            if match is None:
                raise LockConsistencyError(f"artifact member is missing {name!r}")
            fields[name] = json.loads(match.group(1))
        result.append((str(fields["name"]), int(fields["bytes"]), str(fields["sha256"])))
    if not result:
        raise LockConsistencyError("Builder artifact_members must not be empty")
    return result


def _raster_blocks(text: str) -> Iterable[str]:
    starts = [match.start() for match in re.finditer(r"(?m)^\[\[raster\]\]\s*$", text)]
    for index, start in enumerate(starts):
        yield text[start: starts[index + 1] if index + 1 < len(starts) else len(text)]


def _matching_bundle(
    product: Mapping[str, object],
    members: Sequence[Tuple[str, int, str]],
    bundle_bytes: int,
    bundle_sha256: str,
) -> str:
    member_names = [member[0] for member in members]
    bundles = product["bundles"]
    assert isinstance(bundles, dict)
    for name, raw_bundle in bundles.items():
        bundle = raw_bundle
        assert isinstance(bundle, dict)
        canonical = bundle.get("canonical_bundle")
        if (
            bundle["members"] == member_names
            and bundle["total_bytes"] == bundle_bytes
            and isinstance(canonical, dict)
            and canonical["sha256"] == bundle_sha256
        ):
            return name
    raise LockConsistencyError("Builder artifact members and bundle identity match no provisioning bundle")


def verify(lock_path: Path, config_paths: Sequence[Path]) -> int:
    with lock_path.open("r", encoding="utf-8") as stream:
        lock = json.load(stream)
    products = lock["products"]
    by_key = {product["dataset_key"]: product for product in products.values()}
    source_count = 0
    for config_path in config_paths:
        text = config_path.read_text(encoding="utf-8")
        if re.search(r"(?m)^source_root\s*=", text):
            raise LockConsistencyError(f"{config_path}: checked-in M8 configs may not contain source_root")
        for block in _raster_blocks(text):
            source_count += 1
            stable_key = str(_scalar(block, "stable_key"))
            product = by_key.get(stable_key)
            if product is None:
                raise LockConsistencyError(f"{config_path}: unknown provisioning dataset key {stable_key!r}")
            if _scalar(block, "source_uri") != product["provenance_uri"]:
                raise LockConsistencyError(f"{config_path}: provenance URI drift for {stable_key}")
            if _scalar(block, "source_root_environment") != product["root_environment"]:
                raise LockConsistencyError(f"{config_path}: root environment drift for {stable_key}")
            members = _artifact_members(block)
            product_members = product["members"]
            for member_name, member_bytes, member_sha256 in members:
                locked = product_members.get(member_name)
                if locked is None:
                    raise LockConsistencyError(f"{config_path}: unlocked artifact member {member_name!r}")
                if locked.get("bytes") != member_bytes or locked.get("sha256") != member_sha256:
                    raise LockConsistencyError(f"{config_path}: member identity drift for {member_name!r}")
            _matching_bundle(
                product,
                members,
                int(_scalar(block, "artifact_bundle_bytes")),
                str(_scalar(block, "artifact_bundle_sha256")),
            )
    return source_count


def main(argv: Sequence[str] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, required=True)
    parser.add_argument("configs", nargs="+", type=Path)
    arguments = parser.parse_args(argv)
    try:
        count = verify(arguments.lock.resolve(), [path.resolve() for path in arguments.configs])
    except (OSError, KeyError, TypeError, ValueError, LockConsistencyError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"Verified {count} Builder source declarations against {arguments.lock}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
