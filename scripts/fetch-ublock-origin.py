#!/usr/bin/env python3
"""Stage a pinned, complete uBlock Origin package. Never load or execute it."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import ssl
import stat
import sys
import time
import unicodedata
import urllib.parse
import urllib.request
import zipfile


LOCK_PATH = Path(__file__).resolve().parent.parent / "dependencies/ublock-origin.lock.json"
MAX_ARCHIVE_BYTES = 16 * 1024 * 1024
MAX_EXPANDED_BYTES = 64 * 1024 * 1024
MAX_FILE_BYTES = 16 * 1024 * 1024
MAX_MEMBERS = 4096
CHUNK_BYTES = 64 * 1024
DOWNLOAD_SECONDS = 120
HTTPS_HOSTS = {"github.com", "release-assets.githubusercontent.com", "objects.githubusercontent.com"}


class StageError(Exception):
    """An input failed the dependency staging contract."""


def check_https_url(url):
    parsed = urllib.parse.urlsplit(url)
    if (parsed.scheme != "https" or parsed.hostname not in HTTPS_HOSTS
            or parsed.username is not None or parsed.password is not None
            or parsed.port not in (None, 443)):
        raise StageError("download URL or redirect is not an allowed GitHub HTTPS endpoint")


class HTTPSOnlyRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, new_url):
        check_https_url(new_url)
        return super().redirect_request(request, response, code, message, headers, new_url)


def load_lock(path=LOCK_PATH):
    lock = json.loads(path.read_text(encoding="utf-8"))
    if (lock.get("schema_version") != 1 or lock.get("name") != "uBlock Origin"
            or lock.get("manifest_version") != 2 or lock.get("default_variant") != "firefox"
            or set(lock.get("variants", {})) != {"firefox", "chromium"}
            or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", lock.get("version", ""))):
        raise StageError("unsupported uBlock Origin dependency lock")
    for variant, artifact in lock["variants"].items():
        suffix = "firefox.signed.xpi" if variant == "firefox" else "chromium.zip"
        filename = f"uBlock0_{lock['version']}.{suffix}"
        expected_url = f"https://github.com/gorhill/uBlock/releases/download/{lock['version']}/{filename}"
        if (artifact.get("filename") != filename or artifact.get("url") != expected_url
                or type(artifact.get("size")) is not int
                or not 0 < artifact["size"] <= MAX_ARCHIVE_BYTES
                or artifact.get("archive_root") != ("" if variant == "firefox" else "uBlock0.chromium")
                or not re.fullmatch(r"[0-9a-f]{64}", artifact.get("sha256", ""))
                or not re.fullmatch(r"[0-9a-f]{64}", artifact.get("license_sha256", ""))):
            raise StageError(f"invalid pinned {variant} artifact")
    return lock


def copy_verified(source, output, artifact, deadline=None):
    digest = hashlib.sha256()
    total = 0
    with output.open("xb") as target:
        while True:
            if deadline is not None and time.monotonic() >= deadline:
                raise StageError("download exceeded its time limit")
            data = source.read(min(CHUNK_BYTES, artifact["size"] + 1 - total))
            if not data:
                break
            total += len(data)
            if total > artifact["size"]:
                raise StageError("archive exceeds its pinned byte size")
            digest.update(data)
            target.write(data)
    if total != artifact["size"]:
        raise StageError("archive does not match its pinned byte size")
    if digest.hexdigest() != artifact["sha256"]:
        raise StageError("archive SHA-256 does not match the pinned release")


def download_verified(output, artifact):
    check_https_url(artifact["url"])
    opener = urllib.request.build_opener(
        HTTPSOnlyRedirect(), urllib.request.HTTPSHandler(context=ssl.create_default_context())
    )
    request = urllib.request.Request(
        artifact["url"], headers={"User-Agent": "Mux-uBlock-Origin-dependency-fetcher/1"}
    )
    deadline = time.monotonic() + DOWNLOAD_SECONDS
    with opener.open(request, timeout=30) as response:
        check_https_url(response.geturl())
        copy_verified(response, output, artifact, deadline)


def archive_members(archive, root):
    members = archive.infolist()
    if not members or len(members) > MAX_MEMBERS:
        raise StageError("archive member count is outside the allowed bounds")
    expanded = 0
    names = {}
    result = []
    for member in members:
        name = member.filename
        if (name != member.orig_filename or "\\" in name
                or any(ord(char) < 32 or ord(char) == 127 for char in name)):
            raise StageError("archive contains an unsafe filename")
        parts = name.removesuffix("/").split("/")
        if any(part in ("", ".", "..") for part in parts):
            raise StageError("archive contains an unsafe path")
        if root:
            if parts[0] != root:
                raise StageError("archive member is outside its pinned root")
            parts = parts[1:]
        mode = stat.S_IFMT(member.external_attr >> 16)
        expected_mode = stat.S_IFDIR if member.is_dir() else stat.S_IFREG
        if mode not in (0, expected_mode):
            raise StageError("archive contains a symlink or non-regular member")
        if member.flag_bits & 1:
            raise StageError("encrypted archive members are not supported")
        if member.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
            raise StageError("unsupported archive compression method")
        if not parts:
            if not member.is_dir():
                raise StageError("archive root must be a directory")
            continue
        if member.file_size < 0 or member.file_size > MAX_FILE_BYTES:
            raise StageError("archive member exceeds the expanded byte limit")
        expanded += member.file_size
        if expanded > MAX_EXPANDED_BYTES:
            raise StageError("archive exceeds the total expanded byte limit")
        relative = "/".join(parts)
        key = unicodedata.normalize("NFC", relative).casefold()
        if key in names:
            raise StageError("archive has duplicate or case-aliasing paths")
        names[key] = member.is_dir()
        result.append((member, relative))
    for key in names:
        parts = key.split("/")
        for count in range(1, len(parts)):
            if names.get("/".join(parts[:count])) is False:
                raise StageError("archive has a file/directory path conflict")
    return result


def extract_verified(archive_path, destination, lock, artifact):
    with zipfile.ZipFile(archive_path) as archive:
        members = archive_members(archive, artifact["archive_root"])
        files = {relative: member for member, relative in members if not member.is_dir()}
        missing = set(lock["required_files"]) - files.keys()
        if missing:
            raise StageError("archive is missing required extension files: " + ", ".join(sorted(missing)))
        manifest = json.loads(archive.read(files["manifest.json"]))
        if (manifest.get("name") != lock["name"] or manifest.get("version") != lock["version"]
                or manifest.get("manifest_version") != lock["manifest_version"]
                or manifest.get("background", {}).get("page") != "background.html"
                or sorted(manifest.get("permissions", [])) != sorted(artifact["permissions"])):
            raise StageError("extension identity, background page, or permissions differ from the lock")
        if "extension_id" in artifact:
            gecko = manifest.get("browser_specific_settings", {}).get("gecko", {})
            if gecko.get("id") != artifact["extension_id"]:
                raise StageError("Firefox extension ID differs from the pinned release")
        license_digest = hashlib.sha256(archive.read(files["LICENSE.txt"])).hexdigest()
        if license_digest != artifact["license_sha256"]:
            raise StageError("extension license differs from the pinned release")
        destination.mkdir(mode=0o700)
        for member, relative in members:
            target = destination / relative
            if member.is_dir():
                target.mkdir(parents=True, exist_ok=True, mode=0o700)
                continue
            target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            with archive.open(member) as source, target.open("xb") as output:
                written = 0
                while True:
                    data = source.read(CHUNK_BYTES)
                    if not data:
                        break
                    written += len(data)
                    if written > member.file_size:
                        raise StageError("archive member expanded beyond its declared size")
                    output.write(data)
                if written != member.file_size:
                    raise StageError("archive member is truncated")
            target.chmod(0o600)


def stage(lock, variant, destination, archive_path=None, download=False):
    if (archive_path is None) == (not download):
        raise StageError("choose exactly one of --download and --archive")
    if variant not in lock["variants"]:
        raise StageError("unknown uBlock Origin package variant")
    destination = Path(destination)
    if not destination.is_absolute():
        raise StageError("--destination must be an absolute path")
    artifact = lock["variants"][variant]
    if archive_path is not None:
        archive_path = Path(archive_path)
        if not archive_path.is_file():
            raise StageError("--archive must name an existing regular file")
    destination.parent.mkdir(parents=True, exist_ok=True)
    # mkdir is the exclusive reservation. Never replace or clean up a pre-existing path.
    try:
        destination.mkdir(mode=0o700)
    except FileExistsError as error:
        raise StageError("destination already exists; choose a new staging directory") from error
    try:
        output = destination / artifact["filename"]
        if download:
            download_verified(output, artifact)
        else:
            with archive_path.open("rb") as source:
                if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
                    raise StageError("--archive must name a regular file")
                copy_verified(source, output, artifact)
        extract_verified(output, destination / "extension", lock, artifact)
        (destination / "ublock-origin.lock.json").write_text(
            json.dumps(lock, indent=2) + "\n", encoding="utf-8"
        )
        receipt = {
            "schema_version": 1,
            "status": "staged-not-loaded",
            "name": lock["name"],
            "version": lock["version"],
            "variant": variant,
            "artifact": artifact["filename"],
            "artifact_sha256": artifact["sha256"],
            "source_commit": lock["upstream"]["source_commit"],
            "extension_directory": "extension",
            "license": lock["license"]["spdx"],
            "signature_verification": "not-performed",
            "runtime_qualification": "not-performed"
        }
        # This marker is written only after successful acquisition and full extraction.
        (destination / "STAGED.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    except BaseException:
        shutil.rmtree(destination)
        raise
    return destination


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", choices=("firefox", "chromium"), default="firefox",
                        help="upstream package variant (default: firefox, the full-capability reference)")
    parser.add_argument("--destination", required=True, type=Path,
                        help="new absolute staging directory; never overwritten")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--download", action="store_true", help="explicitly download the pinned GitHub release")
    source.add_argument("--archive", type=Path, help="verify a local archive without using the network")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    try:
        destination = stage(load_lock(), args.variant, args.destination, args.archive, args.download)
    except (StageError, OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile) as error:
        print(f"fetch-ublock-origin: {error}", file=sys.stderr)
        return 1
    print(f"Staged verified uBlock Origin ({args.variant}) at {destination}")
    print("Status: staged-not-loaded. No extension was executed; blocking is not active.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
