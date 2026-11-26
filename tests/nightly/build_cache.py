# SPDX-FileCopyrightText: Copyright (c) 2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Publish and restore a run-owned release archive after verifying build identity."""

import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import os
from pathlib import Path
import re
import shutil
import stat
import tarfile
import tempfile
import time

from cases import NAME, REVISION, read_json, write_json


ARCHIVE = "build.tar.gz"
IDENTITY = "build.json"
BINARIES = {f"comm_tools/{mode}/sailbandwidth" for mode in ("single_process", "multi_process")}
MPI_PROBE = "comm_tools/multi_process/mpi_probe"
BUILD_KEYS = {"revision", "package", "sdk", "sha256"}


def digest(path):
    with path.open("rb") as stream:
        result = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
        return result.hexdigest()


def checked_identity(data):
    if (not isinstance(data, dict) or set(data) != BUILD_KEYS or
            any(not isinstance(value, str) for value in data.values()) or
            not REVISION.fullmatch(data["revision"]) or not NAME.fullmatch(data["package"]) or
            not NAME.fullmatch(data["sdk"]) or not re.fullmatch(r"[0-9a-f]{64}", data["sha256"])):
        raise ValueError("Invalid build identity")
    return data


def identity(path):
    return checked_identity(read_json(path))


def seal(archive, revision, package, sdk):
    """Create producer metadata from the checked-out build revision and archive."""
    with tarfile.open(archive, mode="r:gz") as tar:
        package_members(tar)
    return checked_identity({"revision": revision, "package": package, "sdk": sdk, "sha256": digest(archive)})


def validate(directory, expected):
    # Permission and I/O errors propagate; only absent or invalid content is a miss.
    if not stat.S_ISDIR(directory.lstat().st_mode):
        raise ValueError("Invalid cache directory")
    archive = directory / ARCHIVE
    manifest = directory / IDENTITY
    for path in (archive, manifest):
        if not stat.S_ISREG(path.lstat().st_mode):
            raise ValueError(f"Not a regular build file: {path.name}")
    if identity(manifest) != expected or digest(archive) != expected["sha256"]:
        raise ValueError("Build identity or archive digest mismatch")
    return archive


@contextmanager
def publication_lock(parent):
    descriptor = os.open(parent / ".build.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o666)
    try:
        deadline = time.monotonic() + 120
        while True:
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise TimeoutError("Timed out waiting for build cache publication") from None
                time.sleep(1)
        yield
    finally:
        os.close(descriptor)


def publish(archive, directory, expected):
    """Publish to an existing run-owned cache root; never remove adjacent entries."""
    if directory.name != expected["sha256"] or directory.parent.is_symlink() or not directory.parent.is_dir():
        raise ValueError("Build cache requires an existing run-owned parent and digest key")
    with publication_lock(directory.parent):
        try:
            validate(directory, expected)
            return "hit"
        except (FileNotFoundError, ValueError):
            pass
        with tempfile.TemporaryDirectory(prefix=".build-stage-", dir=directory.parent) as staging:
            stage = Path(staging) / "entry"
            stage.mkdir(mode=0o755)
            shutil.copyfile(archive, stage / ARCHIVE)
            write_json(stage / IDENTITY, expected)
            for path in stage.iterdir():
                path.chmod(0o644)
            validate(stage, expected)
            if directory.is_symlink():
                raise ValueError("Refusing to replace a symlinked cache")
            if directory.exists():
                # Quarantine only this invalid digest entry. The next attempt
                # can restore it if a reader observed the short rename window.
                directory.rename(Path(staging) / "invalid")
            stage.rename(directory)
    return "published"


def package_members(package):
    members = {}
    for member in package:
        key = member.name.removeprefix("./")
        if key in BINARIES or key == MPI_PROBE:
            if not member.isfile() or key in members or not (0 < member.size <= 1024**3):
                raise ValueError(f"Invalid benchmark package member: {key}")
            members[key] = member
    if not BINARIES <= members.keys():
        raise ValueError("The release must contain both single-node and MPI binaries")
    return members


def extract(directory, output, expected):
    """Extract only the benchmark binaries and optional diagnostic probe, never archive links."""
    archive = validate(directory, expected)
    with tarfile.open(archive, mode="r:gz") as package:
        members = package_members(package)
        output.mkdir(parents=True, exist_ok=False)
        for key, member in members.items():
            path = output / key
            path.parent.mkdir(parents=True, exist_ok=True)
            with package.extractfile(member) as src, path.open("xb") as dst:
                shutil.copyfileobj(src, dst)
            path.chmod(0o755)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("seal", "validate", "publish", "extract"))
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--identity", type=Path, required=True)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--revision")
    parser.add_argument("--package")
    parser.add_argument("--sdk")
    args = parser.parse_args()
    if args.action == "seal":
        if not all((args.archive, args.revision, args.package, args.sdk)):
            parser.error("seal requires --archive, --revision, --package and --sdk")
        write_json(args.identity, seal(args.archive, args.revision, args.package, args.sdk))
        return
    if not args.cache:
        parser.error("validate, publish and extract require --cache")
    expected = identity(args.identity)
    if args.action == "validate":
        print(validate(args.cache, expected))
    elif args.action == "publish" and args.archive:
        print(publish(args.archive, args.cache, expected))
    elif args.action == "extract" and args.output:
        extract(args.cache, args.output, expected)
    else:
        parser.error("publish requires --archive; extract requires --output")


if __name__ == "__main__":
    main()
