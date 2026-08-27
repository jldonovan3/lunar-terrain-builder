#!/usr/bin/env python3
"""Portable, data-driven provisioning for external lunar DEM products."""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import struct
import sys
import time
from typing import Dict, Iterable, List, Mapping, Optional, Sequence, Tuple
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urljoin, urlparse
from urllib.request import Request, urlopen


DEFAULT_CONFIG_PATH = Path(__file__).resolve().with_name("provisioning.json")
SUPPORTED_SCHEMA_VERSION = 1
HASH_PATTERN = re.compile(r"^[0-9a-f]+$")
CONTENT_RANGE_PATTERN = re.compile(r"^bytes\s+(\d+)-\d+/(?:\d+|\*)$", re.IGNORECASE)


class ProvisioningError(Exception):
    """A user-actionable provisioning failure."""


class _RetryableDownloadError(Exception):
    pass


@dataclass(frozen=True)
class NetworkSpec:
    retry_count: int
    timeout_seconds: float
    chunk_bytes: int
    user_agent: str


@dataclass(frozen=True)
class MemberSpec:
    bytes: Optional[int]
    md5: Optional[str]
    sha256: Optional[str]
    role: Optional[str]
    url: Optional[str]


@dataclass(frozen=True)
class ChecksumManifestSpec:
    url: str
    path: str
    format: str
    member_path_prefix: str
    sha256: Optional[str]


@dataclass(frozen=True)
class CanonicalBundleSpec:
    domain: str
    sha256: str


@dataclass(frozen=True)
class BundleSpec:
    name: str
    description: str
    members: Tuple[str, ...]
    total_bytes: int
    upstream_checksum_manifest: Optional[str]
    canonical_bundle: Optional[CanonicalBundleSpec]
    sha256_manifest: Optional[str]


@dataclass(frozen=True)
class ProductSpec:
    name: str
    display_name: str
    dataset_key: str
    revision: str
    provenance_uri: str
    root_environment: str
    download_base_url: str
    members: Mapping[str, MemberSpec]
    checksum_manifests: Mapping[str, ChecksumManifestSpec]
    bundles: Mapping[str, BundleSpec]


@dataclass(frozen=True)
class ProfileSpec:
    name: str
    product: str
    download_bundle: str
    verify_bundles: Tuple[str, ...]


@dataclass(frozen=True)
class ProvisioningConfig:
    network: NetworkSpec
    products: Mapping[str, ProductSpec]
    profiles: Mapping[str, ProfileSpec]


@dataclass(frozen=True)
class BundleVerification:
    total_bytes: int
    canonical_sha256: Optional[str]
    sha256_by_member: Mapping[str, str]


def _config_error(location: str, message: str) -> ProvisioningError:
    return ProvisioningError(f"configuration error at {location}: {message}")


def _expect_object(value: object, location: str) -> Mapping[str, object]:
    if not isinstance(value, dict):
        raise _config_error(location, "expected an object")
    return value


def _expect_array(value: object, location: str) -> List[object]:
    if not isinstance(value, list):
        raise _config_error(location, "expected an array")
    return value


def _expect_string(value: object, location: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise _config_error(location, "expected a non-empty string")
    return value


def _expect_int(value: object, location: str, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise _config_error(location, f"expected an integer greater than or equal to {minimum}")
    return value


def _reject_unknown_keys(value: Mapping[str, object], allowed: Iterable[str], location: str) -> None:
    unknown = sorted(set(value) - set(allowed))
    if unknown:
        raise _config_error(location, f"unknown field: {unknown[0]}")


def _require_fields(value: Mapping[str, object], required: Iterable[str], location: str) -> None:
    missing = sorted(set(required) - set(value))
    if missing:
        raise _config_error(location, f"missing field: {missing[0]}")


def _expect_hash(value: object, algorithm: str, location: str) -> str:
    digest = _expect_string(value, location).lower()
    expected_length = {"md5": 32, "sha256": 64}[algorithm]
    if len(digest) != expected_length or HASH_PATTERN.fullmatch(digest) is None:
        raise _config_error(location, f"expected a {expected_length}-digit {algorithm.upper()} digest")
    return digest


def _expect_url(value: object, location: str) -> str:
    url = _expect_string(value, location)
    parsed = urlparse(url)
    if parsed.scheme not in ("http", "https") or not parsed.netloc:
        raise _config_error(location, "expected an absolute HTTP or HTTPS URL")
    return url


def _expect_relative_path(value: object, location: str) -> str:
    path = _expect_string(value, location)
    if "\\" in path:
        raise _config_error(location, "use forward slashes in configured paths")
    pure_path = PurePosixPath(path)
    if pure_path.is_absolute() or path != pure_path.as_posix() or any(part in ("", ".", "..") for part in pure_path.parts):
        raise _config_error(location, "expected a normalized relative path without '.' or '..'")
    return path


def _load_member(value: object, location: str) -> MemberSpec:
    member = _expect_object(value, location)
    _reject_unknown_keys(member, ("bytes", "md5", "sha256", "role", "url"), location)
    expected_bytes = None
    if "bytes" in member:
        expected_bytes = _expect_int(member["bytes"], f"{location}.bytes")
    md5 = _expect_hash(member["md5"], "md5", f"{location}.md5") if "md5" in member else None
    sha256 = (
        _expect_hash(member["sha256"], "sha256", f"{location}.sha256")
        if "sha256" in member
        else None
    )
    role = _expect_string(member["role"], f"{location}.role") if "role" in member else None
    url = _expect_url(member["url"], f"{location}.url") if "url" in member else None
    return MemberSpec(bytes=expected_bytes, md5=md5, sha256=sha256, role=role, url=url)


def _load_checksum_manifest(value: object, location: str) -> ChecksumManifestSpec:
    manifest = _expect_object(value, location)
    allowed = ("url", "path", "format", "member_path_prefix", "sha256")
    required = ("url", "path", "format", "member_path_prefix")
    _reject_unknown_keys(manifest, allowed, location)
    _require_fields(manifest, required, location)
    manifest_format = _expect_string(manifest["format"], f"{location}.format")
    if manifest_format != "pds_md5":
        raise _config_error(f"{location}.format", "only 'pds_md5' is supported by schema version 1")
    raw_prefix = _expect_string(manifest["member_path_prefix"], f"{location}.member_path_prefix")
    if not raw_prefix.endswith("/"):
        raise _config_error(f"{location}.member_path_prefix", "expected a trailing slash")
    prefix = _expect_relative_path(raw_prefix[:-1], f"{location}.member_path_prefix") + "/"
    sha256 = (
        _expect_hash(manifest["sha256"], "sha256", f"{location}.sha256")
        if "sha256" in manifest
        else None
    )
    return ChecksumManifestSpec(
        url=_expect_url(manifest["url"], f"{location}.url"),
        path=_expect_relative_path(manifest["path"], f"{location}.path"),
        format=manifest_format,
        member_path_prefix=prefix,
        sha256=sha256,
    )


def _load_bundle(name: str, value: object, location: str) -> BundleSpec:
    bundle = _expect_object(value, location)
    allowed = (
        "description",
        "members",
        "total_bytes",
        "upstream_checksum_manifest",
        "canonical_bundle",
        "sha256_manifest",
    )
    required = ("description", "members", "total_bytes")
    _reject_unknown_keys(bundle, allowed, location)
    _require_fields(bundle, required, location)

    raw_members = _expect_array(bundle["members"], f"{location}.members")
    if not raw_members:
        raise _config_error(f"{location}.members", "expected at least one member")
    members = tuple(
        _expect_relative_path(member, f"{location}.members[{index}]")
        for index, member in enumerate(raw_members)
    )
    if len(members) != len(set(members)):
        raise _config_error(f"{location}.members", "member paths must be unique")

    upstream_manifest = None
    if "upstream_checksum_manifest" in bundle:
        upstream_manifest = _expect_string(
            bundle["upstream_checksum_manifest"], f"{location}.upstream_checksum_manifest"
        )

    canonical_bundle = None
    if "canonical_bundle" in bundle:
        canonical = _expect_object(bundle["canonical_bundle"], f"{location}.canonical_bundle")
        _reject_unknown_keys(canonical, ("domain", "sha256"), f"{location}.canonical_bundle")
        _require_fields(canonical, ("domain", "sha256"), f"{location}.canonical_bundle")
        domain = _expect_string(canonical["domain"], f"{location}.canonical_bundle.domain")
        try:
            domain.encode("ascii")
        except UnicodeEncodeError as error:
            raise _config_error(f"{location}.canonical_bundle.domain", "expected ASCII text") from error
        canonical_bundle = CanonicalBundleSpec(
            domain=domain,
            sha256=_expect_hash(canonical["sha256"], "sha256", f"{location}.canonical_bundle.sha256"),
        )

    sha256_manifest = None
    if "sha256_manifest" in bundle:
        sha256_manifest = _expect_relative_path(bundle["sha256_manifest"], f"{location}.sha256_manifest")

    return BundleSpec(
        name=name,
        description=_expect_string(bundle["description"], f"{location}.description"),
        members=members,
        total_bytes=_expect_int(bundle["total_bytes"], f"{location}.total_bytes"),
        upstream_checksum_manifest=upstream_manifest,
        canonical_bundle=canonical_bundle,
        sha256_manifest=sha256_manifest,
    )


def _canonical_digest(domain: str, records: Sequence[Tuple[str, int, str]]) -> str:
    if len(records) > 0xFFFFFFFF:
        raise ProvisioningError("canonical bundle has too many members")
    digest = hashlib.sha256()
    digest.update(domain.encode("ascii"))
    digest.update(b"\0")
    digest.update(struct.pack("<I", len(records)))
    for relative_path, member_bytes, member_sha256 in records:
        encoded_path = relative_path.encode("utf-8")
        if len(encoded_path) > 0xFFFFFFFF:
            raise ProvisioningError(f"artifact member path is too long: {relative_path}")
        if member_bytes > 0xFFFFFFFFFFFFFFFF:
            raise ProvisioningError(f"artifact member is too large: {relative_path}")
        digest.update(struct.pack("<I", len(encoded_path)))
        digest.update(encoded_path)
        digest.update(struct.pack("<Q", member_bytes))
        digest.update(bytes.fromhex(member_sha256))
    return digest.hexdigest()


def _load_product(name: str, value: object, location: str) -> ProductSpec:
    product = _expect_object(value, location)
    allowed = (
        "display_name",
        "dataset_key",
        "revision",
        "provenance_uri",
        "root_environment",
        "download_base_url",
        "members",
        "checksum_manifests",
        "bundles",
    )
    required = allowed
    _reject_unknown_keys(product, allowed, location)
    _require_fields(product, required, location)

    raw_members = _expect_object(product["members"], f"{location}.members")
    members: Dict[str, MemberSpec] = {}
    for raw_path, raw_member in raw_members.items():
        path = _expect_relative_path(raw_path, f"{location}.members key")
        members[path] = _load_member(raw_member, f"{location}.members[{path!r}]")

    raw_manifests = _expect_object(product["checksum_manifests"], f"{location}.checksum_manifests")
    checksum_manifests: Dict[str, ChecksumManifestSpec] = {}
    for manifest_name, raw_manifest in raw_manifests.items():
        checked_name = _expect_string(manifest_name, f"{location}.checksum_manifests key")
        checksum_manifests[checked_name] = _load_checksum_manifest(
            raw_manifest, f"{location}.checksum_manifests[{checked_name!r}]"
        )

    raw_bundles = _expect_object(product["bundles"], f"{location}.bundles")
    if not raw_bundles:
        raise _config_error(f"{location}.bundles", "expected at least one bundle")
    bundles: Dict[str, BundleSpec] = {}
    all_bundle_members = set()
    for bundle_name, raw_bundle in raw_bundles.items():
        checked_name = _expect_string(bundle_name, f"{location}.bundles key")
        bundle = _load_bundle(value=raw_bundle, name=checked_name, location=f"{location}.bundles[{checked_name!r}]")
        if bundle.upstream_checksum_manifest not in (None, *checksum_manifests.keys()):
            raise _config_error(
                f"{location}.bundles[{checked_name!r}].upstream_checksum_manifest",
                f"unknown checksum manifest {bundle.upstream_checksum_manifest!r}",
            )
        bundles[checked_name] = bundle
        all_bundle_members.update(bundle.members)

    unused_members = sorted(set(members) - all_bundle_members)
    if unused_members:
        raise _config_error(f"{location}.members", f"metadata exists for unused member {unused_members[0]!r}")
    for bundle in bundles.values():
        if bundle.upstream_checksum_manifest is None and bundle.canonical_bundle is None:
            unpinned = [
                member_path
                for member_path in bundle.members
                if member_path not in members
                or (members[member_path].md5 is None and members[member_path].sha256 is None)
            ]
            if unpinned:
                raise _config_error(
                    f"{location}.bundles[{bundle.name!r}]",
                    f"member {unpinned[0]!r} has no checksum source",
                )
        if bundle.canonical_bundle is not None:
            records: List[Tuple[str, int, str]] = []
            for member_path in bundle.members:
                metadata = members.get(member_path)
                if metadata is None or metadata.bytes is None or metadata.sha256 is None:
                    raise _config_error(
                        f"{location}.bundles[{bundle.name!r}].canonical_bundle",
                        f"member {member_path!r} requires configured bytes and SHA-256",
                    )
                records.append((member_path, metadata.bytes, metadata.sha256))
            configured_digest = _canonical_digest(bundle.canonical_bundle.domain, records)
            if configured_digest != bundle.canonical_bundle.sha256:
                raise _config_error(
                    f"{location}.bundles[{bundle.name!r}].canonical_bundle.sha256",
                    f"expected {configured_digest} for the configured ordered members",
                )

    return ProductSpec(
        name=name,
        display_name=_expect_string(product["display_name"], f"{location}.display_name"),
        dataset_key=_expect_string(product["dataset_key"], f"{location}.dataset_key"),
        revision=_expect_string(product["revision"], f"{location}.revision"),
        provenance_uri=_expect_url(product["provenance_uri"], f"{location}.provenance_uri"),
        root_environment=_expect_string(product["root_environment"], f"{location}.root_environment"),
        download_base_url=_expect_url(product["download_base_url"], f"{location}.download_base_url"),
        members=members,
        checksum_manifests=checksum_manifests,
        bundles=bundles,
    )


def _load_profile(name: str, value: object, location: str) -> ProfileSpec:
    profile = _expect_object(value, location)
    allowed = ("product", "download_bundle", "verify_bundles")
    _reject_unknown_keys(profile, allowed, location)
    _require_fields(profile, allowed, location)
    raw_verify_bundles = _expect_array(profile["verify_bundles"], f"{location}.verify_bundles")
    if not raw_verify_bundles:
        raise _config_error(f"{location}.verify_bundles", "expected at least one bundle")
    verify_bundles = tuple(
        _expect_string(bundle, f"{location}.verify_bundles[{index}]")
        for index, bundle in enumerate(raw_verify_bundles)
    )
    if len(verify_bundles) != len(set(verify_bundles)):
        raise _config_error(f"{location}.verify_bundles", "bundle names must be unique")
    return ProfileSpec(
        name=name,
        product=_expect_string(profile["product"], f"{location}.product"),
        download_bundle=_expect_string(profile["download_bundle"], f"{location}.download_bundle"),
        verify_bundles=verify_bundles,
    )


def load_config(path: Path) -> ProvisioningConfig:
    try:
        with path.open("r", encoding="utf-8") as stream:
            raw_config = json.load(stream)
    except FileNotFoundError as error:
        raise ProvisioningError(f"configuration file not found: {path}") from error
    except json.JSONDecodeError as error:
        raise ProvisioningError(
            f"invalid JSON in {path} at line {error.lineno}, column {error.colno}: {error.msg}"
        ) from error
    except OSError as error:
        raise ProvisioningError(f"cannot read configuration file {path}: {error}") from error

    config = _expect_object(raw_config, "root")
    allowed = ("schema_version", "network", "products", "profiles")
    _reject_unknown_keys(config, allowed, "root")
    _require_fields(config, allowed, "root")
    version = _expect_int(config["schema_version"], "schema_version", minimum=1)
    if version != SUPPORTED_SCHEMA_VERSION:
        raise _config_error(
            "schema_version", f"expected {SUPPORTED_SCHEMA_VERSION}, got {version}"
        )

    raw_network = _expect_object(config["network"], "network")
    network_fields = ("retry_count", "timeout_seconds", "chunk_bytes", "user_agent")
    _reject_unknown_keys(raw_network, network_fields, "network")
    _require_fields(raw_network, network_fields, "network")
    retry_count = _expect_int(raw_network["retry_count"], "network.retry_count")
    if retry_count > 20:
        raise _config_error("network.retry_count", "must not exceed 20")
    timeout_value = raw_network["timeout_seconds"]
    if isinstance(timeout_value, bool) or not isinstance(timeout_value, (int, float)) or timeout_value <= 0:
        raise _config_error("network.timeout_seconds", "expected a positive number")
    chunk_bytes = _expect_int(raw_network["chunk_bytes"], "network.chunk_bytes", minimum=1)
    if chunk_bytes > 64 * 1024 * 1024:
        raise _config_error("network.chunk_bytes", "must not exceed 67108864")
    network = NetworkSpec(
        retry_count=retry_count,
        timeout_seconds=float(timeout_value),
        chunk_bytes=chunk_bytes,
        user_agent=_expect_string(raw_network["user_agent"], "network.user_agent"),
    )

    raw_products = _expect_object(config["products"], "products")
    if not raw_products:
        raise _config_error("products", "expected at least one product")
    products: Dict[str, ProductSpec] = {}
    for product_name, raw_product in raw_products.items():
        checked_name = _expect_string(product_name, "products key")
        products[checked_name] = _load_product(checked_name, raw_product, f"products[{checked_name!r}]")

    raw_profiles = _expect_object(config["profiles"], "profiles")
    if not raw_profiles:
        raise _config_error("profiles", "expected at least one profile")
    profiles: Dict[str, ProfileSpec] = {}
    folded_profile_names: Dict[str, str] = {}
    for profile_name, raw_profile in raw_profiles.items():
        checked_name = _expect_string(profile_name, "profiles key")
        folded_name = checked_name.casefold()
        if folded_name in folded_profile_names:
            raise _config_error(
                "profiles", f"profile names {folded_profile_names[folded_name]!r} and {checked_name!r} differ only by case"
            )
        folded_profile_names[folded_name] = checked_name
        profiles[checked_name] = _load_profile(checked_name, raw_profile, f"profiles[{checked_name!r}]")

    for profile in profiles.values():
        if profile.product not in products:
            raise _config_error(
                f"profiles[{profile.name!r}].product", f"unknown product {profile.product!r}"
            )
        product = products[profile.product]
        if profile.download_bundle not in product.bundles:
            raise _config_error(
                f"profiles[{profile.name!r}].download_bundle",
                f"unknown bundle {profile.download_bundle!r}",
            )
        download_members = set(product.bundles[profile.download_bundle].members)
        for bundle_name in profile.verify_bundles:
            if bundle_name not in product.bundles:
                raise _config_error(
                    f"profiles[{profile.name!r}].verify_bundles", f"unknown bundle {bundle_name!r}"
                )
            missing_download_member = next(
                (member for member in product.bundles[bundle_name].members if member not in download_members),
                None,
            )
            if missing_download_member is not None:
                raise _config_error(
                    f"profiles[{profile.name!r}]",
                    f"verified member {missing_download_member!r} is absent from download bundle",
                )

    return ProvisioningConfig(network=network, products=products, profiles=profiles)


def resolve_profile(config: ProvisioningConfig, requested_name: str) -> ProfileSpec:
    folded_name = requested_name.casefold()
    for name, profile in config.profiles.items():
        if name.casefold() == folded_name:
            return profile
    available = ", ".join(config.profiles)
    raise ProvisioningError(f"unknown profile {requested_name!r}; configured profiles: {available}")


def resolve_root(raw_root: str) -> Path:
    if not raw_root.strip():
        raise ProvisioningError("the external product root must not be empty")
    try:
        root = Path(raw_root).expanduser().resolve(strict=False)
    except (OSError, RuntimeError) as error:
        raise ProvisioningError(f"cannot resolve external product root {raw_root!r}: {error}") from error
    if root.parent == root:
        raise ProvisioningError("the external product root must not be a filesystem root")
    return root


def _rooted_path(root: Path, relative_path: str) -> Path:
    candidate = root.joinpath(*PurePosixPath(relative_path).parts).resolve(strict=False)
    try:
        candidate.relative_to(root)
    except ValueError as error:
        raise ProvisioningError(f"configured path escapes the external product root: {relative_path}") from error
    return candidate


def _download_url(base_url: str, relative_path: str) -> str:
    encoded_path = "/".join(quote(part, safe="") for part in PurePosixPath(relative_path).parts)
    return urljoin(base_url.rstrip("/") + "/", encoded_path)


def _retry_delay(attempt: int) -> int:
    return min(2**attempt, 10)


def _copy_response(response: object, stream: object, chunk_bytes: int) -> int:
    copied = 0
    while True:
        chunk = response.read(chunk_bytes)
        if not chunk:
            break
        stream.write(chunk)
        copied += len(chunk)
    content_length_text = response.headers.get("Content-Length")
    if content_length_text is not None:
        try:
            content_length = int(content_length_text)
        except ValueError as error:
            raise _RetryableDownloadError(f"invalid Content-Length {content_length_text!r}") from error
        if copied != content_length:
            raise _RetryableDownloadError(
                f"short response: expected {content_length} bytes, received {copied}"
            )
    return copied


def download_file(url: str, target: Path, network: NetworkSpec) -> None:
    if target.is_file():
        return
    if target.exists():
        raise ProvisioningError(f"download target exists but is not a regular file: {target}")

    target.parent.mkdir(parents=True, exist_ok=True)
    partial = Path(f"{target}.part")
    if partial.exists() and not partial.is_file():
        raise ProvisioningError(f"partial download path is not a regular file: {partial}")

    last_error: Optional[BaseException] = None
    for attempt in range(network.retry_count + 1):
        offset = partial.stat().st_size if partial.exists() else 0
        action = "Resuming" if offset else "Downloading"
        print(f"{action} {url} -> {target}")
        headers = {"User-Agent": network.user_agent}
        if offset:
            headers["Range"] = f"bytes={offset}-"
        request = Request(url, headers=headers)
        try:
            with urlopen(request, timeout=network.timeout_seconds) as response:
                status = response.getcode()
                if offset:
                    if status != 206:
                        raise ProvisioningError(
                            f"server did not honor the resume request for {url}; retained {partial}"
                        )
                    content_range = response.headers.get("Content-Range", "")
                    match = CONTENT_RANGE_PATTERN.fullmatch(content_range.strip())
                    if match is None or int(match.group(1)) != offset:
                        raise ProvisioningError(
                            f"server returned an invalid Content-Range for {url}; retained {partial}"
                        )
                elif status != 200:
                    raise ProvisioningError(f"unexpected HTTP status {status} for {url}")

                mode = "ab" if offset else "wb"
                with partial.open(mode) as stream:
                    _copy_response(response, stream, network.chunk_bytes)
                    stream.flush()
                    os.fsync(stream.fileno())
            os.replace(partial, target)
            return
        except ProvisioningError:
            raise
        except HTTPError as error:
            last_error = error
            if error.code not in (408, 429) and not 500 <= error.code <= 599:
                break
        except (URLError, TimeoutError, ConnectionError, http.client.IncompleteRead, _RetryableDownloadError) as error:
            last_error = error

        if attempt < network.retry_count:
            time.sleep(_retry_delay(attempt))

    detail = f": {last_error}" if last_error is not None else ""
    raise ProvisioningError(
        f"download failed for {url}{detail}; re-run to resume {partial}"
    )


def _file_digest(
    path: Path,
    algorithm: str,
    cache: Dict[Tuple[Path, str], str],
    chunk_bytes: int,
) -> str:
    key = (path, algorithm)
    cached = cache.get(key)
    if cached is not None:
        return cached
    digest = hashlib.new(algorithm, usedforsecurity=False)
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(chunk_bytes), b""):
            digest.update(chunk)
    result = digest.hexdigest()
    cache[key] = result
    return result


def _parse_pds_md5_manifest(path: Path, member_path_prefix: str) -> Mapping[str, str]:
    checksums: Dict[str, str] = {}
    prefix_folded = member_path_prefix.casefold()
    try:
        with path.open("r", encoding="ascii") as stream:
            for line_number, line in enumerate(stream, start=1):
                match = re.match(r"^([0-9A-Fa-f]{32})\s+(.+?)\s*$", line.rstrip("\r\n"))
                if match is None:
                    continue
                source_path = match.group(2).lstrip("*").replace("\\", "/")
                prefix_index = source_path.casefold().rfind(prefix_folded)
                if prefix_index < 0:
                    continue
                relative_path = source_path[prefix_index:]
                try:
                    relative_path = _expect_relative_path(
                        relative_path, f"checksum manifest {path}, line {line_number}"
                    )
                except ProvisioningError:
                    continue
                key = relative_path.casefold()
                digest = match.group(1).lower()
                previous = checksums.get(key)
                if previous is not None and previous != digest:
                    raise ProvisioningError(
                        f"conflicting MD5 entries for {relative_path} in checksum manifest {path}"
                    )
                checksums[key] = digest
    except UnicodeDecodeError as error:
        raise ProvisioningError(f"checksum manifest is not ASCII text: {path}") from error
    if not checksums:
        raise ProvisioningError(
            f"no entries with prefix {member_path_prefix!r} found in checksum manifest {path}"
        )
    return checksums


def load_upstream_checksums(
    product: ProductSpec,
    manifest_name: str,
    root: Path,
    network: NetworkSpec,
    verify_only: bool,
    digest_cache: Dict[Tuple[Path, str], str],
) -> Mapping[str, str]:
    spec = product.checksum_manifests[manifest_name]
    path = _rooted_path(root, spec.path)
    if not path.is_file():
        if verify_only:
            raise ProvisioningError(
                f"missing checksum manifest in --verify-only mode: {path}"
            )
        download_file(spec.url, path, network)
    if spec.sha256 is not None:
        actual = _file_digest(path, "sha256", digest_cache, network.chunk_bytes)
        if actual != spec.sha256:
            raise ProvisioningError(
                f"SHA-256 mismatch for checksum manifest {path}: expected {spec.sha256}, got {actual}. "
                "Delete only this file and re-run."
            )
    if spec.format == "pds_md5":
        return _parse_pds_md5_manifest(path, spec.member_path_prefix)
    raise ProvisioningError(f"unsupported checksum manifest format: {spec.format}")


def _canonical_bundle_sha256(
    bundle: BundleSpec,
    root: Path,
    sha256_by_member: Mapping[str, str],
) -> str:
    if bundle.canonical_bundle is None:
        raise AssertionError("canonical bundle configuration is required")
    records: List[Tuple[str, int, str]] = []
    for relative_path in bundle.members:
        size = _rooted_path(root, relative_path).stat().st_size
        records.append((relative_path, size, sha256_by_member[relative_path]))
    return _canonical_digest(bundle.canonical_bundle.domain, records)


def verify_bundle(
    product: ProductSpec,
    bundle: BundleSpec,
    root: Path,
    upstream_checksums: Optional[Mapping[str, str]],
    digest_cache: Dict[Tuple[Path, str], str],
    chunk_bytes: int,
) -> BundleVerification:
    total_bytes = 0
    sha256_by_member: Dict[str, str] = {}
    need_all_sha256 = bundle.canonical_bundle is not None or bundle.sha256_manifest is not None

    for relative_path in bundle.members:
        path = _rooted_path(root, relative_path)
        if not path.is_file():
            raise ProvisioningError(f"missing artifact member: {path}")
        actual_bytes = path.stat().st_size
        metadata = product.members.get(relative_path)
        if metadata is not None and metadata.bytes is not None and actual_bytes != metadata.bytes:
            raise ProvisioningError(
                f"byte-count mismatch for {relative_path}: expected {metadata.bytes}, got {actual_bytes}. "
                "Make the file writable if needed, delete only this file, and re-run."
            )

        expected_md5 = metadata.md5 if metadata is not None else None
        manifest_md5 = upstream_checksums.get(relative_path.casefold()) if upstream_checksums else None
        if upstream_checksums is not None and manifest_md5 is None:
            raise ProvisioningError(
                f"no official MD5 found for {relative_path} in the configured checksum manifest"
            )
        if expected_md5 is not None or manifest_md5 is not None:
            actual_md5 = _file_digest(path, "md5", digest_cache, chunk_bytes)
            for source, expected in (("configured", expected_md5), ("official", manifest_md5)):
                if expected is not None and actual_md5 != expected:
                    raise ProvisioningError(
                        f"{source} MD5 mismatch for {relative_path}. "
                        "Make the file writable if needed, delete only this file, and re-run."
                    )

        expected_sha256 = metadata.sha256 if metadata is not None else None
        if need_all_sha256 or expected_sha256 is not None:
            actual_sha256 = _file_digest(path, "sha256", digest_cache, chunk_bytes)
            sha256_by_member[relative_path] = actual_sha256
            if expected_sha256 is not None and actual_sha256 != expected_sha256:
                raise ProvisioningError(
                    f"SHA-256 mismatch for {relative_path}. "
                    "Make the file writable if needed, delete only this file, and re-run."
                )
        total_bytes += actual_bytes

    if total_bytes != bundle.total_bytes:
        raise ProvisioningError(
            f"bundle byte-count mismatch for {bundle.name}: expected {bundle.total_bytes}, got {total_bytes}"
        )

    canonical_sha256 = None
    if bundle.canonical_bundle is not None:
        canonical_sha256 = _canonical_bundle_sha256(bundle, root, sha256_by_member)
        if canonical_sha256 != bundle.canonical_bundle.sha256:
            raise ProvisioningError(
                f"canonical bundle SHA-256 mismatch for {bundle.name}: "
                f"expected {bundle.canonical_bundle.sha256}, got {canonical_sha256}"
            )

    return BundleVerification(
        total_bytes=total_bytes,
        canonical_sha256=canonical_sha256,
        sha256_by_member=sha256_by_member,
    )


def write_sha256_manifest(root: Path, relative_path: str, digests: Mapping[str, str]) -> Path:
    path = _rooted_path(root, relative_path)
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = Path(f"{path}.part")
    with partial.open("w", encoding="ascii", newline="\n") as stream:
        for member_path in sorted(digests):
            stream.write(f"{digests[member_path]}  {member_path}\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(partial, path)
    return path


def assert_no_partial_files(root: Path) -> None:
    partials = sorted(path for path in root.rglob("*.part") if path.is_file())
    if partials:
        raise ProvisioningError(
            f"incomplete download remains at {partials[0]}; re-run the same profile to resume it"
        )


def make_members_read_only(root: Path, members: Sequence[str]) -> None:
    write_bits = stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH
    for relative_path in members:
        path = _rooted_path(root, relative_path)
        path.chmod(path.stat().st_mode & ~write_bits)


def run_profile(
    config: ProvisioningConfig,
    profile: ProfileSpec,
    root: Path,
    verify_only: bool,
) -> None:
    product = config.products[profile.product]
    download_bundle = product.bundles[profile.download_bundle]
    if verify_only:
        if not root.is_dir():
            raise ProvisioningError(f"external product root does not exist in --verify-only mode: {root}")
    else:
        root.mkdir(parents=True, exist_ok=True)
        for relative_path in download_bundle.members:
            target = _rooted_path(root, relative_path)
            metadata = product.members.get(relative_path)
            source_url = metadata.url if metadata is not None and metadata.url is not None else None
            if source_url is None:
                source_url = _download_url(product.download_base_url, relative_path)
            download_file(source_url, target, config.network)

    digest_cache: Dict[Tuple[Path, str], str] = {}
    upstream_by_manifest: Dict[str, Mapping[str, str]] = {}
    results: Dict[str, BundleVerification] = {}
    for bundle_name in profile.verify_bundles:
        bundle = product.bundles[bundle_name]
        upstream = None
        if bundle.upstream_checksum_manifest is not None:
            manifest_name = bundle.upstream_checksum_manifest
            if manifest_name not in upstream_by_manifest:
                upstream_by_manifest[manifest_name] = load_upstream_checksums(
                    product,
                    manifest_name,
                    root,
                    config.network,
                    verify_only,
                    digest_cache,
                )
            upstream = upstream_by_manifest[manifest_name]
        results[bundle_name] = verify_bundle(
            product, bundle, root, upstream, digest_cache, config.network.chunk_bytes
        )

    result = results[download_bundle.name]
    sha256_manifest_path = None
    if download_bundle.sha256_manifest is not None:
        sha256_manifest_path = write_sha256_manifest(
            root, download_bundle.sha256_manifest, result.sha256_by_member
        )

    assert_no_partial_files(root)
    make_members_read_only(root, download_bundle.members)

    print(f"Verified {download_bundle.description}: {result.total_bytes} bytes")
    if result.canonical_sha256 is not None:
        print(f"Verified artifact bundle SHA-256: {result.canonical_sha256}")
    if sha256_manifest_path is not None:
        print(f"Wrote SHA-256 manifest: {sha256_manifest_path}")
    print(f"{product.root_environment}={root}")


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Download and verify a configured lunar DEM provisioning profile."
    )
    parser.add_argument("profile", metavar="PROFILE", help="configured profile name (for example, Artifact or Fullset)")
    parser.add_argument(
        "--root",
        metavar="PATH",
        help="external product root; defaults to the selected product's configured environment variable",
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=DEFAULT_CONFIG_PATH,
        metavar="PATH",
        help=f"product/profile JSON (default: {DEFAULT_CONFIG_PATH.name} beside this script)",
    )
    parser.add_argument(
        "--verify-only",
        action="store_true",
        help="verify existing files without making any network requests",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = _argument_parser()
    arguments = parser.parse_args(argv)
    try:
        config = load_config(arguments.config.resolve(strict=False))
        profile = resolve_profile(config, arguments.profile)
        product = config.products[profile.product]
        root_value = arguments.root
        if root_value is None:
            root_value = os.environ.get(product.root_environment)
        if root_value is None or not str(root_value).strip():
            raise ProvisioningError(
                f"--root or {product.root_environment} must name the external {product.display_name} root"
            )
        root = resolve_root(str(root_value))
        run_profile(config, profile, root, arguments.verify_only)
        return 0
    except ProvisioningError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("error: interrupted; re-run the same profile to resume any .part download", file=sys.stderr)
        return 130
    except OSError as error:
        print(f"error: filesystem operation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
