import contextlib
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parent))
import provision  # noqa: E402


class FakeResponse:
    def __init__(self, status, headers, chunks):
        self._status = status
        self.headers = headers
        self._chunks = list(chunks)

    def __enter__(self):
        return self

    def __exit__(self, exception_type, exception, traceback):
        return False

    def getcode(self):
        return self._status

    def read(self, _size):
        if not self._chunks:
            return b""
        return self._chunks.pop(0)


def network_spec():
    return provision.NetworkSpec(
        retry_count=0,
        timeout_seconds=1.0,
        chunk_bytes=4,
        user_agent="provision-test/1",
    )


def single_member_config(data):
    relative_path = "tiles/example.bin"
    sha256 = hashlib.sha256(data).hexdigest()
    member = provision.MemberSpec(
        bytes=len(data),
        md5=hashlib.md5(data, usedforsecurity=False).hexdigest(),
        sha256=sha256,
        role="elevation",
        url=None,
    )
    canonical = provision.CanonicalBundleSpec(
        domain="LTDB_ARTIFACT_BUNDLE_V1",
        sha256=provision._canonical_digest(
            "LTDB_ARTIFACT_BUNDLE_V1", [(relative_path, len(data), sha256)]
        ),
    )
    bundle = provision.BundleSpec(
        name="artifact",
        description="test artifact",
        members=(relative_path,),
        total_bytes=len(data),
        upstream_checksum_manifest=None,
        canonical_bundle=canonical,
        sha256_manifest=None,
    )
    product = provision.ProductSpec(
        name="test",
        display_name="test product",
        dataset_key="test.product.v1",
        revision="test-revision",
        provenance_uri="https://example.test/product/",
        root_environment="TEST_PRODUCT_ROOT",
        download_base_url="https://example.test/product/",
        members={relative_path: member},
        checksum_manifests={},
        bundles={bundle.name: bundle},
    )
    profile = provision.ProfileSpec(
        name="Artifact",
        product=product.name,
        download_bundle=bundle.name,
        verify_bundles=(bundle.name,),
    )
    config = provision.ProvisioningConfig(
        network=network_spec(),
        products={product.name: product},
        profiles={profile.name: profile},
    )
    return config, profile, relative_path


class ConfigTests(unittest.TestCase):
    def test_committed_config_preserves_locked_sldem_profiles(self):
        config = provision.load_config(Path(__file__).with_name("provisioning.json"))
        product = config.products["sldem2015"]
        artifact = product.bundles["artifact"]
        fullset = product.bundles["fullset"]

        self.assertEqual("nasa.sldem2015.512ppd.v1", product.dataset_key)
        self.assertEqual(3, len(artifact.members))
        self.assertEqual(96, len(fullset.members))
        self.assertEqual(171872390, artifact.total_bytes)
        self.assertEqual(5466757104, fullset.total_bytes)
        self.assertEqual(
            "17810a5b1551a56b865f59c20ae2c78c6aa05112112c557f466e21e19d5b9351",
            artifact.canonical_bundle.sha256,
        )
        self.assertTrue(set(artifact.members).issubset(fullset.members))
        self.assertEqual("Artifact", provision.resolve_profile(config, "artifact").name)
        self.assertEqual("Fullset", provision.resolve_profile(config, "FULLSET").name)

    def test_changed_canonical_member_order_is_rejected(self):
        config_path = Path(__file__).with_name("provisioning.json")
        with config_path.open("r", encoding="utf-8") as stream:
            raw_config = json.load(stream)
        members = raw_config["products"]["sldem2015"]["bundles"]["artifact"]["members"]
        members[0], members[1] = members[1], members[0]

        with tempfile.TemporaryDirectory() as temporary_directory:
            changed_path = Path(temporary_directory) / "changed.json"
            with changed_path.open("w", encoding="utf-8") as stream:
                json.dump(raw_config, stream)
            with self.assertRaisesRegex(provision.ProvisioningError, "canonical_bundle.sha256"):
                provision.load_config(changed_path)

    def test_filesystem_root_is_rejected(self):
        filesystem_root = Path.cwd().anchor
        with self.assertRaisesRegex(provision.ProvisioningError, "filesystem root"):
            provision.resolve_root(filesystem_root)


class VerificationTests(unittest.TestCase):
    def test_verify_only_uses_existing_member_without_network(self):
        data = b"verified bytes"
        config, profile, relative_path = single_member_config(data)
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory).resolve()
            path = root.joinpath(*relative_path.split("/"))
            path.parent.mkdir(parents=True)
            path.write_bytes(data)
            output = io.StringIO()
            with mock.patch.object(provision, "urlopen") as mocked_urlopen:
                with mock.patch.object(provision, "make_members_read_only"):
                    with contextlib.redirect_stdout(output):
                        provision.run_profile(config, profile, root, verify_only=True)

            mocked_urlopen.assert_not_called()
            self.assertIn("Verified test artifact: 14 bytes", output.getvalue())
            self.assertIn(config.products[profile.product].bundles["artifact"].canonical_bundle.sha256, output.getvalue())

    def test_corrupted_member_is_rejected(self):
        expected_data = b"expected"
        config, profile, relative_path = single_member_config(expected_data)
        product = config.products[profile.product]
        bundle = product.bundles[profile.download_bundle]
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory).resolve()
            path = root.joinpath(*relative_path.split("/"))
            path.parent.mkdir(parents=True)
            path.write_bytes(b"corrupt!")
            with self.assertRaisesRegex(provision.ProvisioningError, "MD5 mismatch"):
                provision.verify_bundle(product, bundle, root, None, {}, 4)

    def test_partial_file_is_rejected_after_verification(self):
        data = b"verified bytes"
        config, profile, relative_path = single_member_config(data)
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory).resolve()
            path = root.joinpath(*relative_path.split("/"))
            path.parent.mkdir(parents=True)
            path.write_bytes(data)
            (root / "unrelated.part").write_bytes(b"partial")
            with self.assertRaisesRegex(provision.ProvisioningError, "incomplete download"):
                provision.run_profile(config, profile, root, verify_only=True)

    def test_pds_manifest_accepts_backslash_paths(self):
        manifest_line = (
            "f009c232030ecd09e21dd67ebafdb02d  "
            "volume\\data\\sldem2015\\tiles\\jp2\\example.jp2\r\n"
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            manifest_path = Path(temporary_directory) / "source.md5"
            manifest_path.write_text(manifest_line, encoding="ascii", newline="")
            checksums = provision._parse_pds_md5_manifest(manifest_path, "tiles/jp2/")
        self.assertEqual(
            "f009c232030ecd09e21dd67ebafdb02d", checksums["tiles/jp2/example.jp2"]
        )


class DownloadTests(unittest.TestCase):
    def test_existing_partial_download_is_resumed_and_atomically_renamed(self):
        response = FakeResponse(
            206,
            {"Content-Range": "bytes 3-5/6", "Content-Length": "3"},
            [b"de", b"f"],
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            target = Path(temporary_directory) / "member.bin"
            partial = Path(f"{target}.part")
            partial.write_bytes(b"abc")
            with mock.patch.object(provision, "urlopen", return_value=response) as mocked_urlopen:
                with contextlib.redirect_stdout(io.StringIO()):
                    provision.download_file("https://example.test/member.bin", target, network_spec())

            request = mocked_urlopen.call_args.args[0]
            self.assertEqual("bytes=3-", request.get_header("Range"))
            self.assertEqual(b"abcdef", target.read_bytes())
            self.assertFalse(partial.exists())

    def test_resume_is_not_silently_restarted_when_server_ignores_range(self):
        response = FakeResponse(200, {"Content-Length": "3"}, [b"def"])
        with tempfile.TemporaryDirectory() as temporary_directory:
            target = Path(temporary_directory) / "member.bin"
            partial = Path(f"{target}.part")
            partial.write_bytes(b"abc")
            with mock.patch.object(provision, "urlopen", return_value=response):
                with contextlib.redirect_stdout(io.StringIO()):
                    with self.assertRaisesRegex(provision.ProvisioningError, "did not honor"):
                        provision.download_file("https://example.test/member.bin", target, network_spec())

            self.assertEqual(b"abc", partial.read_bytes())
            self.assertFalse(target.exists())


if __name__ == "__main__":
    unittest.main()
