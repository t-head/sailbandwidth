# SPDX-FileCopyrightText: Copyright (c) 2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Offline nightly contract tests; no Sail SDK, MPI installation or PPUs needed."""

import io
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import build_cache
import cases
import performance
import report
import run_cases
import smoke_ci


OP = "device_to_device_memcpy_read_ce"
LATENCY = "device_to_device_latency_sm"


def record(matrix=OP, cell="PPU0/PPU1", number="12.5", unit="+GB/s"):
    return f"&&&& PERF sailbandwidth/{matrix}/{cell} {number} {unit}"


def build_fixture(root, script, link=False):
    root.mkdir()
    archive = root / "package.tar.gz"
    with tarfile.open(archive, "w:gz") as tar:
        for index, name in enumerate(sorted(build_cache.BINARIES)):
            data = script.encode()
            entry = tarfile.TarInfo(name)
            if link and index == 0:
                entry.type = tarfile.SYMTYPE
                entry.linkname = "/bin/sh"
                tar.addfile(entry)
            else:
                entry.size = len(data)
                tar.addfile(entry, io.BytesIO(data))
    expected = {"revision": "a" * 40, "package": "sailbandwidth_hggcrt13_ubuntu2404",
                "sdk": "2.2.1-184ee6", "sha256": build_cache.digest(archive)}
    identity = root / "expected.json"
    cases.write_json(identity, expected)
    cache = root / expected["sha256"]
    return archive, identity, expected, cache


class SelectionTests(unittest.TestCase):
    def test_exact_groups_and_case_identity(self):
        for dataset, expected in (("full", [37, 48, 10]), ("smoke", [4, 5, 2])):
            with self.subTest(dataset=dataset), tempfile.TemporaryDirectory() as temp:
                root = Path(temp) / "selection"
                manifest = cases.generate(cases.ROOT / "families.json", root, dataset)
                self.assertEqual([group["case_count"] for group in manifest["groups"]], expected)
                identities = set()
                longer_budgets = []
                for group in manifest["groups"]:
                    _, env, selected = cases.load_group(root / group["id"])
                    self.assertEqual(env["ALIPPU_RESET_CE_MASK"], "1")
                    self.assertEqual(group["rack_policy"], "same" if group["nodes"] == 2 else "")
                    for case in selected:
                        if case.timeout != 300:
                            longer_budgets.append((case.family, case.name, case.timeout))
                        self.assertNotIn((case.family, case.name), identities)
                        identities.add((case.family, case.name))
                self.assertEqual(len(identities), sum(expected))
                expected_budgets = [("810e-performance", "test_op_device_to_device_latency_sm_i_1", 1200)]
                if dataset == "full":
                    expected_budgets.append((*cases.VPIPE_CASE, 1200))
                self.assertEqual(longer_budgets, expected_budgets)

    def test_eic_execution_budget_does_not_reduce_source_timeout(self):
        case = cases.make_case("810e-performance", "latency", 1500, ["-t", LATENCY])
        self.assertIs(cases.execution_budget(case), case)

    def test_vpipe_execution_budget_preserves_larger_source_timeout_and_other_invocations(self):
        args = ["--enableVpipeIdReadOptimize"]
        for case in (cases.make_case(*cases.VPIPE_CASE, 1500, args),
                     cases.make_case(*cases.VPIPE_CASE, 300, ["-t", OP, *args]),
                     cases.make_case(cases.VPIPE_CASE[0], "another_default_invocation", 300, args)):
            with self.subTest(case=case):
                self.assertIs(cases.execution_budget(case), case)

    def test_reject_foreign_arguments_and_incorrect_mode(self):
        for args in (["-t", OP, "--skipVerification"], ["-t", "echo;shutdown"], ["-t"],
                     ["-t", OP, "-i", "0"], ["-t", OP, "-t", OP], ["-t", f"multinode_{OP}"]):
            with self.subTest(args=args), self.assertRaises(ValueError):
                cases.make_case("m890p-performance", "case", 300, args)

    def test_duplicate_json_keys_are_not_silently_dropped(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            path = root / "families.json"
            path.write_text('{"families": [], "families": []}', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Duplicate JSON key"):
                cases.generate(path, root / "output", "full")
            self.assertFalse((root / "output").exists())


class ParserTests(unittest.TestCase):
    def parse(self, lines, operation=OP, exit_code=0, vpipe=False, timed_out=False):
        return performance.interpret(lines, exit_code, operation, 8, vpipe, timed_out)

    def test_accept_zero_bandwidth_and_positive_latency(self):
        status, _, values = self.parse([record(number="0"), performance.PASS])
        self.assertEqual(status, "PASSED")
        self.assertEqual(values, [{"matrix": OP, "row": 0, "column": 1, "unit": "GB/s", "value": 0.0}])
        self.assertEqual(self.parse([record(LATENCY, number="2e1", unit="-ns"), performance.PASS], LATENCY)[0],
                         "PASSED")

    def test_multiple_matrices_with_shared_endpoints(self):
        lines = [record(f"{OP}{suffix}") for suffix in ("_read1", "_read2", "_total")]
        self.assertEqual(len(self.parse([*lines, performance.PASS])[2]), 3)

    def test_reject_invalid_numeric_unit_index_and_matrix(self):
        for line in (record(number="-1"), record(number="nan"), record(number="inf"), record(unit="-ns"),
                     record(cell="PPU0/PPU8"), record("wrong_case"), record(unit="GB/s")):
            with self.subTest(line=line):
                self.assertEqual(self.parse([line, performance.PASS])[0], "INVALID_OUTPUT")
        self.assertEqual(self.parse([record(LATENCY, number="0", unit="-ns"), performance.PASS], LATENCY)[0],
                         "INVALID_OUTPUT")

    def test_zero_exit_does_not_override_failure_or_waiver(self):
        self.assertEqual(self.parse([performance.FAIL])[0], "FAILED")
        self.assertEqual(self.parse([performance.PASS, "ERROR: rank failed"])[0], "FAILED")
        self.assertEqual(self.parse([f"Waived {OP}: unsupported device", performance.PASS])[0], "WAIVED")
        self.assertEqual(self.parse([performance.VPIPE_UNSUPPORTED, record(), performance.PASS], vpipe=True)[0],
                         "WAIVED")
        self.assertEqual(self.parse([performance.PASS])[0], "INVALID_OUTPUT")
        self.assertEqual(self.parse([record(), record(), performance.PASS])[0], "INVALID_OUTPUT")
        self.assertEqual(self.parse([record(), performance.PASS], exit_code=-9)[0], "FAILED")
        self.assertEqual(self.parse([record(), performance.PASS], timed_out=True)[0], "TIMED_OUT")


class BuildTests(unittest.TestCase):
    def test_hash_invalid_content_can_be_republished_and_extract_both_modes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            archive, _, expected, cache = build_fixture(root / "inputs", "benchmark")
            self.assertEqual(build_cache.seal(archive, expected["revision"], expected["package"], expected["sdk"]),
                             expected)
            self.assertEqual(build_cache.publish(archive, cache, expected), "published")
            self.assertEqual(build_cache.publish(archive, cache, expected), "hit")
            (cache / build_cache.ARCHIVE).write_bytes(b"corrupt")
            with self.assertRaisesRegex(ValueError, "digest mismatch"):
                build_cache.validate(cache, expected)
            self.assertEqual(build_cache.publish(archive, cache, expected), "published")
            build_cache.extract(cache, root / "unpack", expected)
            for binary in build_cache.BINARIES:
                path = root / "unpack" / binary
                self.assertEqual(path.read_text(encoding="utf-8"), "benchmark")
                self.assertTrue(os.access(path, os.X_OK))

    def test_linked_binary_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            archive, _, expected, cache = build_fixture(root / "inputs", "benchmark", link=True)
            build_cache.publish(archive, cache, expected)
            with self.assertRaisesRegex(ValueError, "Invalid benchmark package member"):
                build_cache.extract(cache, root / "unpack", expected)
            self.assertFalse((root / "unpack").exists())

    def test_publication_lock_serializes_processes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            archive, manifest, expected, cache = build_fixture(root / "inputs", "benchmark")
            argv = [sys.executable, "-S", "-B", str(cases.ROOT / "build_cache.py"), "publish",
                    "--archive", str(archive), "--identity", str(manifest), "--cache", str(cache)]
            with subprocess.Popen(argv, stdout=subprocess.PIPE, text=True) as first, \
                    subprocess.Popen(argv, stdout=subprocess.PIPE, text=True) as second:
                outputs = [process.communicate(timeout=10)[0].strip() for process in (first, second)]
                self.assertEqual((first.returncode, second.returncode), (0, 0))
                self.assertCountEqual(outputs, ["published", "hit"])
            build_cache.validate(cache, expected)

    def test_permission_error_is_not_an_invalid_cache_to_replace(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            archive, _, expected, cache = build_fixture(root / "inputs", "benchmark")
            with patch.object(build_cache, "validate", side_effect=PermissionError("unreadable")):
                with self.assertRaises(PermissionError):
                    build_cache.publish(archive, cache, expected)
            self.assertFalse(cache.exists())


class ExecutionTests(unittest.TestCase):
    def test_launcher_uses_allocated_hostfile_and_environment(self):
        with tempfile.TemporaryDirectory() as temp:
            hostfile = Path(temp) / "hosts"
            hostfile.write_text("worker-0 slots=8\nworker-1 slots=8\n", encoding="utf-8")
            env = {"PATH": "/usr/bin", "ALIPPU_RESET_CE_MASK": "1"}
            group = {"nodes": 2, "cards_per_node": 8}
            command = run_cases.mpi_launch(group, hostfile, "eth0", env)
            self.assertEqual(command[:8], ["mpirun", "--allow-run-as-root", "-np", "16", "-npernode", "8",
                                          "--hostfile", str(hostfile.resolve())])
            self.assertEqual(env["OMPI_MCA_plm_rsh_agent"], "cluster-ssh")
            self.assertEqual(env["HWLOC_COMPONENTS"], "-gl")
            self.assertEqual(env["OMPI_MCA_orte_launch_agent"], "env HWLOC_COMPONENTS=-gl orted")
            self.assertEqual(env["OMPI_MCA_btl_tcp_if_include"], "eth0")
            hostfile.write_text("worker-0 slots=8\nworker-0 slots=8\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "unique"):
                run_cases.mpi_launch(group, hostfile, "eth0", env)

    def test_single_node_leaves_mpi_environment_unchanged(self):
        env = {"PATH": "/usr/bin"}
        group = {"nodes": 1, "cards_per_node": 8}
        self.assertEqual(run_cases.mpi_launch(group, None, None, env), [])
        self.assertEqual(env, {"PATH": "/usr/bin"})

    def test_real_subprocess_timeout(self):
        case = cases.make_case("m890p-performance", "timeout", 1, ["-t", OP])
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "case"
            result = run_cases.execute_case(case, {"nodes": 1, "cards_per_node": 8}, os.environ.copy(),
                                            [sys.executable, "-c", "import time; time.sleep(30)"], root, {})
            self.assertEqual(result["status"], "TIMED_OUT")
            self.assertTrue((root / "result.json").is_file())

    def test_real_subprocess_full_generation_execution_and_excel(self):
        self.check_execution_and_excel("full", {"810e-eic": {"PASSED": 37}, "m890p-cx8": {"PASSED": 58}})

    def test_real_subprocess_smoke_generation_execution_and_excel(self):
        self.check_execution_and_excel("smoke", {"810e-eic": {"PASSED": 4}, "m890p-cx8": {"PASSED": 7}})

    def test_real_subprocess_vpipe_progress_is_separate_from_perf(self):
        self.check_execution_and_excel("vpipe", {"m890p-cx8": {"PASSED": 1}})

    def check_execution_and_excel(self, dataset, expected_counts):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest = cases.generate(cases.ROOT / "families.json", root / "selection", dataset)
            command = f'''#!{sys.executable}
import os
from pathlib import Path
import sys
args = sys.argv[1:]
assert args[-2:] == ['-F', 'perf']
trace = os.getenv('{run_cases.PROGRESS_LOG_ENV}')
if trace:
    assert args == ['--enableVpipeIdReadOptimize', '-F', 'perf']
    Path(trace).write_text('START fixture 0 ms\\nFINISH fixture 1 ms\\n')
name = args[args.index('-t') + 1] if '-t' in args else '{OP}'
unit = '-ns' if 'latency' in name else '+GB/s'
print(f'&&&& PERF sailbandwidth/{{name}}/PPU0/PPU1 12.5 {{unit}}')
print('&&&& sailbandwidth test PASSED')
'''
            archive, identity, expected, cache = build_fixture(root / "inputs", command)
            build_cache.publish(archive, cache, expected)
            # A local launcher fixture exercises the argument and result wiring,
            # but cannot validate MPI, device support or remote cleanup.
            launcher = root / "mpirun"
            launcher.write_text(f'''#!{sys.executable}
import os, sys
args = sys.argv[1:]
assert args[:6] == ['--allow-run-as-root', '-np', '16', '-npernode', '8', '--hostfile']
idx = next(i for i, arg in enumerate(args) if arg.endswith('/multi_process/sailbandwidth'))
os.execv(args[idx], args[idx:])
''', encoding="utf-8")
            launcher.chmod(0o755)
            hostfile = root / "hosts"
            hostfile.write_text("worker-0 slots=8\nworker-1 slots=8\n", encoding="utf-8")
            with patch.dict(os.environ, {"PATH": str(root) + os.pathsep + os.environ["PATH"],
                                         run_cases.PROGRESS_LOG_ENV: str(root / "inherited.log")}):
                for group in manifest["groups"]:
                    mpi = group["nodes"] > 1
                    run_cases.run(root / "selection" / group["id"], cache, identity,
                                  root / "results" / group["id"], root, hostfile if mpi else None,
                                  "eth0" if mpi else None)
            self.assertFalse((root / "inherited.log").exists())
            traces = list((root / "results").glob("*/*/*/testcase-progress.log"))
            if dataset == "vpipe":
                self.assertEqual(len(traces), 1)
                self.assertIn("FINISH fixture", traces[0].read_text(encoding="utf-8"))
                self.assertNotIn("START fixture", traces[0].with_name("raw.log").read_text(encoding="utf-8"))
            else:
                self.assertEqual(traces, [])
            counts = report.report(root / "selection", root / "results", expected, root / "csv")
            self.assertEqual(counts["platforms"], expected_counts)
            self.assertEqual(smoke_ci.public_summary(counts, dataset=dataset)["platforms"], expected_counts)
            # Exercise optional Excel output when the reporting dependency is available.
            try:
                import xlsxwriter  # noqa: F401
            except ImportError:
                pass
            else:
                report.report(root / "selection", root / "results", expected, root / "excel", excel=True)
                for platform in counts["platforms"]:
                    with zipfile.ZipFile(root / "excel" / f"{platform}.xlsx") as book:
                        self.assertIn("xl/worksheets/sheet2.xml", book.namelist())
                        self.assertIn(b"12.5", book.read("xl/worksheets/sheet2.xml"))
            log = next((root / "results").glob("*/*/*/raw.log"))
            log.write_text(performance.PASS + "\n", encoding="utf-8")
            unexpected = root / "results" / "unknown" / "family" / "case" / "result.json"
            unexpected.parent.mkdir(parents=True)
            cases.write_json(unexpected, {})
            rejected = report.report(root / "selection", root / "results", expected, root / "rejected")
            self.assertEqual(len(rejected["unexpected_results"]), 1)
            self.assertEqual(sum(row.get("INVALID_RESULT", 0) for row in rejected["platforms"].values()), 1)
            self.assertFalse(list(root.glob("sailbandwidth-*")))

    def test_missing_groups_are_reported_not_accepted(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            cases.generate(cases.ROOT / "families.json", root / "selection", "smoke")
            result = report.report(root / "selection", root / "results", {}, root / "reports")
            self.assertEqual(result["platforms"], {"810e-eic": {"MISSING": 4}, "m890p-cx8": {"MISSING": 7}})


if __name__ == "__main__":
    unittest.main()
