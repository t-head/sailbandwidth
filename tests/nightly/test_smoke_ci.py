# SPDX-FileCopyrightText: Copyright (c) 2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Offline checks for the branch-only hardware experiment integration."""

import os
from pathlib import Path
import subprocess
import sys
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
            self.assertTrue(all((row["cluster_timeout_minutes"], row["job_timeout_minutes"]) == (90, 115)
                                for row in rows))
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

    def test_full_requires_exact_coverage_and_budgets_without_splitting_allocations(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "selection"
            cases.generate(cases.ROOT / "families.json", output, "full")
            with self.assertRaisesRegex(ValueError, "full dataset"):
                smoke_ci.smoke_matrix(output)
            rows = smoke_ci.selection_matrix(output, "full")["include"]
            self.assertEqual(len(rows), 3)
            self.assertEqual({(row["platform"], row["nodes"]):
                              (row["case_count"], row["cluster_timeout_minutes"], row["job_timeout_minutes"])
                              for row in rows}, {("m890p-cx8", 1): (48, 300, 325), ("m890p-cx8", 2): (10, 105, 130),
                                                 ("810e-eic", 1): (37, 255, 280)})
            result = subprocess.run([sys.executable, "-S", "-B", str(cases.ROOT / "smoke_ci.py"), "matrix",
                                     str(output), "--dataset", "full", "--mpi-only"], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("only available for smoke diagnostics", result.stderr)
            manifest = cases.load_manifest(output)
            manifest["groups"].pop()
            cases.write_json(output / "groups.json", manifest)
            with self.assertRaisesRegex(ValueError, "Wrong full coverage"):
                smoke_ci.selection_matrix(output, "full")

    def test_vpipe_diagnostic_selects_only_the_default_test_invocation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            output = root / "selection"
            cases.generate(cases.ROOT / "families.json", output, "vpipe")
            row, = smoke_ci.selection_matrix(output, "vpipe")["include"]
            self.assertEqual((row["platform"], row["nodes"], row["cards_per_node"]), ("m890p-cx8", 1, 8))
            self.assertEqual((row["cluster_timeout_minutes"], row["job_timeout_minutes"]), (75, 100))
            _, _, [case] = cases.load_group(output / row["id"])
            self.assertEqual((case.family, case.name), cases.VPIPE_DIAGNOSTIC_CASE)
            self.assertEqual(case.arguments, ["--enableVpipeIdReadOptimize"])
            self.assertEqual(case.timeout, 1200)
            (root / smoke_ci.NAS_PROBES).mkdir()
            smoke_ci.write_nas_challenge(root)
            smoke_ci.record_nas_probe(root, row["id"], 0, dataset="vpipe")
            smoke_ci.verify_nas_probes(root, dataset="vpipe")
            selection = output / row["id"] / "cases.csv"
            selection.write_text(selection.read_text().replace(",1200,", ",300,"))
            with self.assertRaisesRegex(ValueError, "Wrong Vpipe diagnostic"):
                smoke_ci.selection_matrix(output, "vpipe")

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

    def test_full_public_summary_counts_not_run_but_does_not_publish_details(self):
        summary = {"dataset": "full", "platforms": {"m890p-cx8": {"PASSED": 48, "TIMED_OUT": 1, "NOT_RUN": 9},
                                                    "810e-eic": {"PASSED": 36, "MISSING": 1}},
                   "unexpected_results": ["diagnostic path"], "source_revision": "source metadata"}
        with self.assertRaisesRegex(ValueError, "smoke platform totals"):
            smoke_ci.public_summary(summary)
        self.assertEqual(smoke_ci.public_summary(summary, dataset="full"),
                         {"dataset": "full", "platforms": summary["platforms"], "unexpected_result_count": 1})
        summary["platforms"]["m890p-cx8"]["NOT_RUN"] = 8
        with self.assertRaisesRegex(ValueError, "full status totals"):
            smoke_ci.public_summary(summary, dataset="full")


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

    def test_full_probes_still_require_one_response_per_worker(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / smoke_ci.NAS_PROBES).mkdir()
            cases.generate(cases.ROOT / "families.json", root / "selection", "full")
            smoke_ci.write_nas_challenge(root)
            with self.assertRaisesRegex(ValueError, "full dataset"):
                smoke_ci.nas_probe_slots(root)
            slots = smoke_ci.nas_probe_slots(root, dataset="full")
            self.assertEqual(len(slots), 4)
            for group, rank in slots:
                smoke_ci.record_nas_probe(root, group, rank, dataset="full")
            smoke_ci.verify_nas_probes(root, dataset="full")
