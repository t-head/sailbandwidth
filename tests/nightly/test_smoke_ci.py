# SPDX-FileCopyrightText: Copyright (c) 2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Offline checks for the one-off real-hardware smoke integration."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

import build_cache
import cases
import smoke_ci


class SmokeCI(unittest.TestCase):
    def test_only_generated_smoke_allocations_are_admitted(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "selection"
            cases.generate(cases.ROOT / "families.json", output, "smoke")
            rows = smoke_ci.smoke_matrix(output)["include"]
            self.assertEqual(len(rows), 3)
            self.assertEqual(sum(row["case_count"] for row in rows), 11)
            manifest = cases.load_manifest(output)
            mpi_group = next(group for group in manifest["groups"] if group["nodes"] == 2)
            mpi_group["rack_policy"] = "spread"
            cases.write_json(output / "groups.json", manifest)
            with self.assertRaisesRegex(ValueError, "rack policy"):
                smoke_ci.smoke_matrix(output)
            manifest["dataset"] = "full"
            cases.write_json(output / "groups.json", manifest)
            with self.assertRaisesRegex(ValueError, "full dataset"):
                smoke_ci.smoke_matrix(output)

    def test_download_must_match_build_commit_image_and_archive(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle = Path(tmp)
            archive = bundle / build_cache.ARCHIVE
            archive.write_bytes(b"fixture archive")
            (bundle / "openmpi.version").write_text("4.1.6-7ubuntu2\n4.1.6-7ubuntu2\n", encoding="ascii")
            revision, image_hash = "a" * 40, "b" * 64
            image, archive_sha = "registry/image@sha256:" + image_hash, build_cache.digest(archive)
            cases.write_json(bundle / build_cache.IDENTITY, {
                "revision": revision, "sdk": "image-" + image_hash,
                "package": smoke_ci.PACKAGE, "sha256": archive_sha,
            })
            smoke_ci.verify_build(bundle, revision, image, archive_sha)
            with self.assertRaisesRegex(ValueError, "producer job"):
                smoke_ci.verify_build(bundle, "c" * 40, image, archive_sha)
            with self.assertRaisesRegex(ValueError, "producer job"):
                smoke_ci.verify_build(bundle, revision, "registry/image@sha256:" + "d" * 64, archive_sha)
            archive.write_bytes(b"damaged archive")
            with self.assertRaisesRegex(ValueError, "producer SHA-256"):
                smoke_ci.verify_build(bundle, revision, image, archive_sha)

    def test_public_summary_excludes_details_and_requires_complete_counts(self):
        summary = {"dataset": "smoke", "platforms": {"m890p-cx8": {"PASSED": 5, "MISSING": 2},
                                                     "810e-eic": {"PASSED": 4}},
                   "unexpected_results": ["diagnostic path"], "source_revision": "source metadata"}
        self.assertEqual(smoke_ci.public_summary(summary), {"dataset": "smoke", "platforms": summary["platforms"],
                                                           "unexpected_result_count": 1})
        summary["platforms"]["m890p-cx8"] = {"path in a status key": 7}
        with self.assertRaisesRegex(ValueError, "status totals"):
            smoke_ci.public_summary(summary)
        summary["platforms"]["m890p-cx8"] = {"PASSED": 5}
        with self.assertRaisesRegex(ValueError, "status totals"):
            smoke_ci.public_summary(summary)


class SmokeNAS(unittest.TestCase):
    def test_run_path_needs_a_mounted_canonical_child(self):
        script = Path(__file__).with_name("smoke_ci.sh")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            mount = root / "mnt"
            base = mount / "parent" / "smoke"
            base.mkdir(parents=True)
            outside = root / "outside"
            outside.mkdir()
            (mount / "alias").symlink_to(outside, target_is_directory=True)
            stub = root / "mountpoint"
            stub.write_text('#!/bin/sh\n[ "$*" = "-q -- $SMOKE_NAS_MOUNT" ] && [ "$MOCK_MOUNTED" = 1 ]\n')
            stub.chmod(0o755)
            env = dict(os.environ, PATH=f"{root}:{os.environ['PATH']}", SMOKE_NAS_MOUNT=str(mount),
                       SMOKE_KEY="123-2", MOCK_MOUNTED="1")

            def run(subdir, mounted="1"):
                return subprocess.run(["bash", str(script), "nas-run"], capture_output=True, text=True,
                                      env=dict(env, SMOKE_NAS_SUBDIR=subdir, MOCK_MOUNTED=mounted))

            accepted = run("parent/smoke")
            self.assertEqual(accepted.returncode, 0, accepted.stderr)
            self.assertEqual(accepted.stdout.strip(), str(base / "sailbandwidth-smoke-123-2"))
            self.assertFalse(Path(accepted.stdout.strip()).exists())
            for subdir in ("../outside", "/parent/smoke", "parent//smoke", "parent/./smoke",
                           "parent/smoke/", "alias", "parent/missing", "parent/\nsmoke"):
                with self.subTest(subdir=subdir):
                    self.assertNotEqual(run(subdir).returncode, 0)
            self.assertIn("not mounted", run("parent/smoke", "0").stderr)

    def test_requires_a_fresh_matching_response_from_every_worker(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            probes = root / smoke_ci.NAS_PROBES
            probes.mkdir()
            cases.generate(cases.ROOT / "families.json", root / "selection", "smoke")
            smoke_ci.write_nas_challenge(root)
            with self.assertRaises(FileExistsError):
                smoke_ci.write_nas_challenge(root)
            slots = sorted(smoke_ci.nas_probe_slots(root))
            self.assertEqual(len(slots), 4)
            for group, rank in slots[:-1]:
                smoke_ci.record_nas_probe(root, group, rank)
            group, rank = slots[-1]
            with self.assertRaisesRegex(ValueError, "missing"):
                smoke_ci.verify_nas_probes(root)
            with self.assertRaisesRegex(ValueError, "Unexpected.*worker rank"):
                smoke_ci.record_nas_probe(root, group, 2)
            smoke_ci.record_nas_probe(root, group, rank)
            smoke_ci.verify_nas_probes(root)
            with self.assertRaises(FileExistsError):
                smoke_ci.record_nas_probe(root, group, rank)
            proof = probes / f"{group}-{rank}"
            proof.write_bytes(b"not the CPU challenge")
            with self.assertRaisesRegex(ValueError, "Wrong NAS challenge response"):
                smoke_ci.verify_nas_probes(root)
            proof.unlink()
            proof.symlink_to(root / smoke_ci.NAS_CHALLENGE)
            with self.assertRaisesRegex(ValueError, "Invalid NAS probe"):
                smoke_ci.verify_nas_probes(root)
