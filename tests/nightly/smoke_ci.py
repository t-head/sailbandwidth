# SPDX-FileCopyrightText: Copyright (c) 2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Run the branch-only hardware smoke workflow's contract checks."""

import argparse
import json
import os
from pathlib import Path
import re
import secrets

from build_cache import ARCHIVE, IDENTITY, digest, identity
from cases import SELECTORS, load_group, load_manifest, read_json
from report import RECORDED


SMOKE_GROUPS = {("810e-eic", 1, 16): 4, ("m890p-cx8", 1, 8): 5, ("m890p-cx8", 2, 8): 2}
PACKAGE = "ubuntu24-hggcrt13"
NAS_CHALLENGE = "nas-challenge"
NAS_PROBES = "nas-probes"


def verify_build(bundle, revision, image, archive_sha, identity_only=False):
    parts = image.rsplit("@sha256:", 1)
    if len(parts) != 2 or not re.fullmatch(r"[0-9a-f]{64}", parts[1]):
        raise ValueError("Expected a digest-pinned build/runtime image")
    expected = {"revision": revision, "sdk": "image-" + parts[1], "package": PACKAGE, "sha256": archive_sha}
    if identity(bundle / IDENTITY) != expected:
        raise ValueError("Build identity does not match the producer job")
    if not identity_only:
        if digest(bundle / ARCHIVE) != archive_sha:
            raise ValueError("Downloaded build archive does not match the producer SHA-256")
        versions = (bundle / "openmpi.version").read_text(encoding="ascii").splitlines()
        if len(versions) != 2 or not all(re.fullmatch(r"4\.[0-9][0-9A-Za-z.~:+-]*", item) for item in versions):
            raise ValueError("Expected two Open MPI 4 package versions")


def smoke_matrix(selection):
    manifest = load_manifest(selection)
    if manifest["dataset"] != "smoke":
        raise ValueError("The experiment must not run a full dataset")
    resources = {}
    for item in manifest["groups"]:
        group, _, cases = load_group(selection / item["id"])
        platform, nodes, cards = spec = group["platform"], group["nodes"], group["cards_per_node"]
        if (spec in resources or group["node_selector"] != SELECTORS[platform] or
                group["rack_policy"] != ("same" if nodes == 2 else "")):
            raise ValueError("Unexpected smoke allocation or rack policy")
        resources[spec] = len(cases)
    if resources != SMOKE_GROUPS:
        raise ValueError(f"Wrong smoke coverage: {resources}")
    return {"include": manifest["groups"]}


def nas_probe_slots(root):
    return {(group["id"], rank) for group in smoke_matrix(root / "selection")["include"]
            for rank in range(group["nodes"])}


def write_nas_challenge(root):
    with (root / NAS_CHALLENGE).open("xb") as handle:
        handle.write(secrets.token_hex(16).encode("ascii"))


def read_nas_challenge(root):
    with (root / NAS_CHALLENGE).open("rb") as handle:
        token = handle.read(33)
    if not re.fullmatch(rb"[0-9a-f]{32}", token):
        raise ValueError("Invalid NAS challenge")
    return token


def record_nas_probe(root, group, rank):
    if (group, rank) not in nas_probe_slots(root):
        raise ValueError("Unexpected smoke NAS probe group or worker rank")
    token = read_nas_challenge(root)
    # Write once; both workers of the MPI group must leave distinct proofs.
    with (root / NAS_PROBES / f"{group}-{rank}").open("xb") as handle:
        handle.write(token)


def verify_nas_probes(root, only_group=None):
    token = read_nas_challenge(root)
    expected = {f"{group}-{rank}" for group, rank in nas_probe_slots(root)
                if only_group is None or group == only_group}
    if not expected:
        raise ValueError("No expected NAS probe slots for the selected group")
    actual = {entry.name: entry for entry in (root / NAS_PROBES).iterdir()}
    if actual.keys() != expected:
        raise ValueError(f"Wrong NAS probes: missing={sorted(expected - actual.keys())}, "
                         f"unexpected={sorted(actual.keys() - expected)}")
    for name, entry in sorted(actual.items()):
        if entry.is_symlink() or not entry.is_file():
            raise ValueError(f"Invalid NAS probe for {name}")
        with entry.open("rb") as handle:
            if handle.read(33) != token:
                raise ValueError(f"Wrong NAS challenge response for {name}")


def public_summary(summary):
    """Publish only platform counters, not raw logs, case details or paths."""
    platforms = summary["platforms"]
    expected = {platform: sum(count for (key, _, _), count in SMOKE_GROUPS.items() if key == platform)
                for platform in SELECTORS}
    if summary["dataset"] != "smoke" or set(platforms) != set(expected):
        raise ValueError("Expected complete smoke platform totals")
    allowed_statuses = RECORDED | {"MISSING", "INVALID_RESULT"}
    for platform, counters in platforms.items():
        if (not counters.keys() <= allowed_statuses or
                any(type(count) is not int or count <= 0 for count in counters.values()) or
                sum(counters.values()) != expected[platform]):
            raise ValueError("Invalid smoke status totals")
    return {"dataset": "smoke", "platforms": platforms,
            "unexpected_result_count": len(summary["unexpected_results"])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("validate-build", "matrix", "public-summary",
                                           "write-nas-challenge", "record-nas-probe", "verify-nas-probes"))
    parser.add_argument("path", type=Path)
    parser.add_argument("--identity-only", action="store_true")
    parser.add_argument("--mpi-only", action="store_true")
    parser.add_argument("--group")
    parser.add_argument("--rank", type=int)
    args = parser.parse_args()
    if args.action == "validate-build":
        verify_build(args.path, os.environ["GITHUB_SHA"], os.environ["SMOKE_IMAGE"],
                     os.environ["SMOKE_BUILD_SHA"], args.identity_only)
    elif args.action == "matrix":
        matrix = smoke_matrix(args.path)
        if args.mpi_only:
            matrix["include"] = [group for group in matrix["include"] if group["nodes"] == 2]
        print("matrix=" + json.dumps(matrix, separators=(",", ":")))
    elif args.action == "public-summary":
        print(json.dumps(public_summary(read_json(args.path)), indent=2))
    else:
        try:
            if args.action == "write-nas-challenge":
                write_nas_challenge(args.path)
            elif args.action == "record-nas-probe":
                record_nas_probe(args.path, args.group, args.rank)
            else:
                verify_nas_probes(args.path)
        except ValueError as exc:
            raise SystemExit(str(exc)) from None
        except OSError as exc:
            raise SystemExit(f"Smoke NAS {args.action} failed: {exc.strerror}") from None


if __name__ == "__main__":
    main()
