#!/usr/bin/env python3
"""Offline dependency-staging tests; optionally check previously downloaded releases."""

import argparse
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest import mock
import urllib.request
import warnings
import zipfile


sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location(
    "mux_fetch_ublock_origin", Path(__file__).with_name("fetch-ublock-origin.py")
)
FETCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FETCH)
UPSTREAM_ARTIFACTS = None


class StagingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="mux-ublock-origin-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.lock = FETCH.load_lock()
        self.no_network = mock.patch.object(
            FETCH.urllib.request, "build_opener", side_effect=AssertionError("unexpected network access")
        )
        self.no_network.start()
        self.addCleanup(self.no_network.stop)

    def fixture(self, variant="firefox", change_manifest=None, extra=(), omit=()):
        lock = copy.deepcopy(self.lock)
        artifact = lock["variants"][variant]
        manifest = {
            "name": lock["name"], "version": lock["version"], "manifest_version": 2,
            "background": {"page": "background.html"}, "permissions": artifact["permissions"][:],
            "browser_specific_settings": {"gecko": {"id": "uBlock0@raymondhill.net"}}
        }
        if change_manifest:
            change_manifest(manifest)
        contents = {name: b"fixture content; not executable\n" for name in lock["required_files"]}
        contents["manifest.json"] = json.dumps(manifest).encode()
        contents["LICENSE.txt"] = b"fixture license\n"
        contents["assets/thirdparties/NOTICE"] = b"third-party notice must survive\n"
        for name in omit:
            contents.pop(name)
        prefix = artifact["archive_root"] + "/" if artifact["archive_root"] else ""
        stream = io.BytesIO()
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                for name, data in contents.items():
                    archive.writestr(prefix + name, data)
                for name, data in extra:
                    archive.writestr(name, data)
        data = stream.getvalue()
        artifact["sha256"] = hashlib.sha256(data).hexdigest()
        artifact["size"] = len(data)
        artifact["license_sha256"] = hashlib.sha256(b"fixture license\n").hexdigest()
        path = self.root / artifact["filename"]
        path.write_bytes(data)
        return lock, path, contents

    def assert_rejected(self, lock, archive, message, variant="firefox"):
        destination = self.root / "staged"
        with self.assertRaisesRegex(FETCH.StageError, message):
            FETCH.stage(lock, variant, destination, archive_path=archive)
        self.assertFalse(destination.exists(), "failed staging must not leave an activation marker or payload")

    def test_lock_pins_both_genuine_releases(self):
        self.assertEqual(self.lock["version"], "1.74.0")
        self.assertEqual(self.lock["default_variant"], "firefox")
        self.assertEqual(self.lock["license"]["spdx"], "GPL-3.0-or-later")
        self.assertEqual(self.lock["variants"]["firefox"]["sha256"],
                         "175756d74468c9ba45863f7fc333d3be670f82d5b066314e915814dd547d1652")
        self.assertEqual(self.lock["variants"]["chromium"]["sha256"],
                         "29a475e82688b304f2a9b2c0577c2655a8aaf75ce363fe02d720d389bbb9f451")
        license_path = FETCH.LOCK_PATH.parent / self.lock["license"]["copy"]
        self.assertEqual(hashlib.sha256(license_path.read_bytes()).hexdigest(),
                         self.lock["variants"][self.lock["license"]["copy_variant"]]["license_sha256"])

    def test_stages_both_layouts_without_changing_payload(self):
        for variant in ("firefox", "chromium"):
            with self.subTest(variant=variant):
                lock, archive, contents = self.fixture(variant)
                destination = self.root / ("staged-" + variant)
                FETCH.stage(lock, variant, destination, archive_path=archive)
                self.assertEqual((destination / archive.name).read_bytes(), archive.read_bytes())
                for name, data in contents.items():
                    target = destination / "extension" / name
                    self.assertEqual(target.read_bytes(), data)
                    self.assertEqual(target.stat().st_mode & 0o777, 0o600)
                receipt = json.loads((destination / "STAGED.json").read_text())
                self.assertEqual(receipt["status"], "staged-not-loaded")
                self.assertEqual(receipt["runtime_qualification"], "not-performed")
                self.assertEqual(receipt["variant"], variant)

    def test_corrupted_archive_is_rejected_before_extraction(self):
        lock, archive, _ = self.fixture()
        data = bytearray(archive.read_bytes())
        data[-1] ^= 1
        archive.write_bytes(data)
        self.assert_rejected(lock, archive, "SHA-256")

    def test_truncated_and_oversized_archives_are_rejected(self):
        for oversized in (False, True):
            with self.subTest(oversized=oversized):
                lock, archive, _ = self.fixture()
                data = archive.read_bytes()
                archive.write_bytes(data + b"x" if oversized else data[:-1])
                self.assert_rejected(lock, archive, "byte size")

    def test_wrong_extension_identity_is_rejected(self):
        for field, value in (("name", "uBlock Origin Lite"), ("version", "0.0.0"), ("manifest_version", 3)):
            with self.subTest(field=field):
                lock, archive, _ = self.fixture(change_manifest=lambda manifest: manifest.update({field: value}))
                self.assert_rejected(lock, archive, "identity")

    def test_changed_permissions_and_firefox_id_are_rejected(self):
        lock, archive, _ = self.fixture(change_manifest=lambda manifest: manifest["permissions"].append("debugger"))
        self.assert_rejected(lock, archive, "permissions")
        lock, archive, _ = self.fixture(change_manifest=lambda manifest: manifest.update(
            {"browser_specific_settings": {"gecko": {"id": "another-extension@example.invalid"}}}
        ))
        self.assert_rejected(lock, archive, "Firefox extension ID")

    def test_missing_payload_and_changed_license_are_rejected(self):
        lock, archive, _ = self.fixture(omit=("js/contentscript.js",))
        self.assert_rejected(lock, archive, "missing required")
        lock, archive, _ = self.fixture()
        lock["variants"]["firefox"]["license_sha256"] = "0" * 64
        self.assert_rejected(lock, archive, "license")

    def test_root_license_digest_excludes_nested_license_files(self):
        # An unzip '*/LICENSE.txt' wildcard concatenates nested licenses, which
        # previously produced a false Chromium pin. Only the exact root counts.
        nested_license = b"separately licensed bundled component\n"
        for variant in ("firefox", "chromium"):
            with self.subTest(variant=variant):
                root = self.lock["variants"][variant]["archive_root"]
                member = (root + "/" if root else "") + "lib/nested/LICENSE.txt"
                lock, archive, contents = self.fixture(variant, extra=((member, nested_license),))
                destination = self.root / ("root-license-" + variant)
                FETCH.stage(lock, variant, destination, archive_path=archive)
                self.assertEqual((destination / "extension/lib/nested/LICENSE.txt").read_bytes(), nested_license)
                concatenated = hashlib.sha256(contents["LICENSE.txt"] + nested_license).hexdigest()
                self.assertNotEqual(lock["variants"][variant]["license_sha256"], concatenated)
                lock["variants"][variant]["license_sha256"] = concatenated
                self.assert_rejected(lock, archive, "license", variant=variant)

    def test_unsafe_archive_paths_are_rejected(self):
        for path in ("../escape", "/absolute", "nested/../../escape", "nested\\escape", "nested//file", "./alias"):
            with self.subTest(path=path):
                lock, archive, _ = self.fixture(extra=((path, b"unsafe"),))
                self.assert_rejected(lock, archive, "unsafe")

    def test_wrong_chromium_root_is_rejected(self):
        lock, archive, _ = self.fixture("chromium", extra=(("other-root/extra", b"unsafe"),))
        self.assert_rejected(lock, archive, "pinned root", variant="chromium")

    def test_duplicate_case_alias_and_file_directory_conflicts_are_rejected(self):
        for path in ("manifest.json", "MANIFEST.json", "manifest.json/child"):
            with self.subTest(path=path):
                lock, archive, _ = self.fixture(extra=((path, b"unsafe"),))
                self.assert_rejected(lock, archive, "duplicate|conflict")

    def test_symlinks_and_special_files_are_rejected(self):
        for mode in (stat.S_IFLNK, stat.S_IFIFO, stat.S_IFCHR):
            with self.subTest(mode=mode):
                member = zipfile.ZipInfo("unsafe-member")
                member.create_system = 3
                member.external_attr = (mode | 0o777) << 16
                lock, archive, _ = self.fixture(extra=((member, b"some-target"),))
                self.assert_rejected(lock, archive, "non-regular")

    def test_archive_resource_limits_are_enforced(self):
        lock, archive, _ = self.fixture()
        for setting, value, message in (
            ("MAX_MEMBERS", 1, "member count"), ("MAX_FILE_BYTES", 1, "expanded byte"),
            ("MAX_EXPANDED_BYTES", 1, "expanded byte")
        ):
            with self.subTest(setting=setting), mock.patch.object(FETCH, setting, value):
                self.assert_rejected(lock, archive, message)

    def test_encrypted_members_are_rejected(self):
        member = zipfile.ZipInfo("encrypted")
        member.flag_bits = 1
        archive = mock.Mock()
        archive.infolist.return_value = [member]
        with self.assertRaisesRegex(FETCH.StageError, "encrypted"):
            FETCH.archive_members(archive, "")

    def test_existing_destination_is_never_modified(self):
        lock, archive, _ = self.fixture()
        destination = self.root / "existing"
        destination.mkdir()
        sentinel = destination / "keep"
        sentinel.write_bytes(b"existing user data")
        with self.assertRaisesRegex(FETCH.StageError, "already exists"):
            FETCH.stage(lock, "firefox", destination, archive_path=archive)
        self.assertEqual(sentinel.read_bytes(), b"existing user data")

    def test_existing_destination_symlink_is_never_followed(self):
        lock, archive, _ = self.fixture()
        real = self.root / "real"
        real.mkdir()
        destination = self.root / "existing-link"
        destination.symlink_to(real, target_is_directory=True)
        with self.assertRaisesRegex(FETCH.StageError, "already exists"):
            FETCH.stage(lock, "firefox", destination, archive_path=archive)
        self.assertTrue(destination.is_symlink())
        self.assertEqual(list(real.iterdir()), [])

    def test_explicit_source_and_absolute_destination_are_required(self):
        lock, archive, _ = self.fixture()
        for kwargs in ({}, {"archive_path": archive, "download": True}):
            with self.subTest(kwargs=kwargs), self.assertRaisesRegex(FETCH.StageError, "exactly one"):
                FETCH.stage(lock, "firefox", self.root / "staged", **kwargs)
        with self.assertRaisesRegex(FETCH.StageError, "absolute"):
            FETCH.stage(lock, "firefox", Path("relative"), archive_path=archive)
        with mock.patch("sys.stderr", new=io.StringIO()), self.assertRaises(SystemExit) as error:
            FETCH.parse_args(["--destination", str(self.root / "staged")])
        self.assertEqual(error.exception.code, 2)

    def test_redirects_cannot_downgrade_https_or_change_to_foreign_hosts(self):
        handler = FETCH.HTTPSOnlyRedirect()
        request = urllib.request.Request(self.lock["variants"]["firefox"]["url"])
        for url in ("http://github.com/file", "https://example.invalid/file", "file:///tmp/file",
                    "https://github.com@evil.invalid/file", "https://github.com:8443/file"):
            with self.subTest(url=url), self.assertRaises(FETCH.StageError):
                handler.redirect_request(request, None, 302, "Found", {}, url)

    def test_explicit_download_uses_pinned_url_and_same_verification(self):
        lock, archive, _ = self.fixture()
        artifact = lock["variants"]["firefox"]
        response = io.BytesIO(archive.read_bytes())
        response.geturl = lambda: "https://release-assets.githubusercontent.com/release-asset"
        opener = mock.Mock()
        opener.open.return_value = response
        with mock.patch.object(FETCH.urllib.request, "build_opener", return_value=opener):
            destination = FETCH.stage(lock, "firefox", self.root / "download", download=True)
        request = opener.open.call_args.args[0]
        self.assertEqual(request.full_url, artifact["url"])
        self.assertEqual(opener.open.call_args.kwargs["timeout"], 30)
        self.assertTrue((destination / "STAGED.json").is_file())

    def test_download_deadline_is_enforced(self):
        lock, archive, _ = self.fixture()
        with self.assertRaisesRegex(FETCH.StageError, "time limit"):
            FETCH.copy_verified(io.BytesIO(archive.read_bytes()), self.root / "expired",
                                lock["variants"]["firefox"], deadline=0)

    def test_previously_downloaded_upstream_packages(self):
        if UPSTREAM_ARTIFACTS is None:
            self.skipTest("pass --upstream-artifacts DIR to qualify the real archives offline")
        for variant, artifact in self.lock["variants"].items():
            with self.subTest(variant=variant):
                archive_path = UPSTREAM_ARTIFACTS / artifact["filename"]
                destination = self.root / ("upstream-" + variant)
                FETCH.stage(self.lock, variant, destination, archive_path=archive_path)
                prefix = artifact["archive_root"] + "/" if artifact["archive_root"] else ""
                checked = 0
                with zipfile.ZipFile(archive_path) as archive:
                    for member in archive.infolist():
                        if member.is_dir():
                            continue
                        relative = member.filename.removeprefix(prefix)
                        self.assertEqual((destination / "extension" / relative).read_bytes(), archive.read(member))
                        checked += 1
                self.assertGreater(checked, 500, "must retain the complete extension, not just filter lists")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream-artifacts", type=Path,
                        help="directory with both pinned archives; never downloads or executes upstream code")
    options, arguments = parser.parse_known_args()
    UPSTREAM_ARTIFACTS = options.upstream_artifacts
    unittest.main(argv=[sys.argv[0]] + arguments)
