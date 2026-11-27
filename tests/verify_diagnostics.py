#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


FIELDS = (
    "verified buffer", "first bad offset", "first expected", "first actual",
    "first xor", "bit error counts",
)
CASES = (
    "device_to_device_memcpy_read_ce", "device_to_device_memcpy_write_ce",
    "device_to_device_memcpy_read_sm", "device_to_device_memcpy_write_sm",
)
SAMPLE_ARGS = ["-b", "8", "-i", "1", "--loopCount", "2"]


def run(command, directory, fault=None, dump_size=None, extra=()):
    environment = dict(os.environ)
    environment.pop("SAILBW_VERIFY", None)
    environment.pop("SAILBW_DUMP_SIZE", None)
    environment["SAILBW_DUMP_PATH"] = str(directory)
    if fault is not None:
        environment["SAILBW_VERIFY"] = fault
    if dump_size is not None:
        environment["SAILBW_DUMP_SIZE"] = dump_size
    return subprocess.run(
        command + list(extra), env=environment, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, universal_newlines=True, timeout=60,
    )


def json_documents(text):
    decoder = json.JSONDecoder()
    documents = []
    while text.strip():
        text = text.lstrip()
        document, end = decoder.raw_decode(text)
        documents.append(document["sailbandwidth"])
        text = text[end:]
    return documents


def report_fields(text):
    pattern = r"^\s*(?:#\s+)?(" + "|".join(FIELDS) + r")\s*:\s*(.+)$"
    return dict(re.findall(pattern, text, re.MULTILINE))


def check_failure(command, output_format, dump_size):
    with tempfile.TemporaryDirectory(prefix="sailbw-diagnostic-") as directory:
        directory = Path(directory)
        result = run(command, directory, "1,0", dump_size)
        assert result.returncode != 0, "Injected failure unexpectedly passed"
        if output_format == "json":
            documents = json_documents(result.stdout)
            report = next(item["error"] for item in documents
                          if "Data verification FAILED" in item.get("error", ""))
            assert not any(test.get("status") == "Passed" for item in documents
                           for test in item.get("testcases", []))
        else:
            report = result.stdout + result.stderr
        assert "Data verification FAILED" in report
        dumps = list(directory.glob("sailbw_dump_*.txt"))
        assert len(dumps) == 1, "Expected exactly one mismatch dump"
        dump = dumps[0].read_text()
        fields = report_fields(report)
        assert set(fields) == set(FIELDS), "Missing failure diagnostics"
        assert fields == report_fields(dump), "Report and dump disagree"
        assert fields["verified buffer"].startswith("dst: ")
        assert fields["first bad offset"] == "0 (word) / 0 (byte)"
        expected = int(fields["first expected"], 16)
        actual = int(fields["first actual"], 16)
        assert expected != 0 and actual == 0
        assert int(fields["first xor"], 16) == expected ^ actual
        counts = {int(bit): int(count) for bit, count in
                  re.findall(r"b(\d+)=(\d+)", fields["bit error counts"])}
        assert counts and all(0 <= bit < 32 and 0 < count <= 8
                              for bit, count in counts.items())
        if dump_size == "ALL":
            rows = re.findall(r"^\s*\d+\s+0x[0-9a-f]{8}\s+0x[0-9a-f]{8}\s+(0x[0-9a-f]{8})",
                              dump, re.MULTILINE)
            assert len(rows) == 8
            masks = [int(value, 16) for value in rows]
            expected_counts = {bit: sum((mask >> bit) & 1 for mask in masks)
                               for bit in range(32)}
            expected_counts = {bit: count for bit, count in expected_counts.items() if count}
            assert counts == expected_counts, "Bit counts do not match all dump values"
        else:
            assert "1 of 8 bad values" in dump
        return counts


def main():
    parser = argparse.ArgumentParser(description="Run on two idle PPUs selected with HGGC_VISIBLE_DEVICES")
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--mpi-binary", type=Path)
    args = parser.parse_args()
    binary = str(args.binary.resolve())
    with tempfile.TemporaryDirectory(prefix="sailbw-diagnostic-") as directory:
        directory = Path(directory)
        for option in ("--help", "--list"):
            result = run([binary, option], directory)
            assert result.returncode == 0
            if args.baseline:
                baseline = run([str(args.baseline.resolve()), option], directory)
                assert baseline.returncode == 0 and baseline.stdout == result.stdout
        for testcase in CASES:
            command = [binary, "-t", testcase] + SAMPLE_ARGS + ["-F", "json"]
            result = run(command, directory)
            assert result.returncode == 0
            document, = json_documents(result.stdout)
            assert "error" not in document
            assert any(test.get("status") == "Passed" for test in document["testcases"])
            skipped = run(command, directory, "1,0", extra=["--skipVerification"])
            assert skipped.returncode == 0 and "Data verification FAILED" not in skipped.stdout
        assert not list(directory.glob("sailbw_dump_*.txt"))
    print("PASS: help/list, CE/SM read/write success, and skipped verification")
    for testcase in CASES:
        command = [binary, "-t", testcase] + SAMPLE_ARGS
        counts = None
        for output_format in ("text", "json", "perf"):
            for limit in ("1", "ALL"):
                observed = check_failure(command + ["-F", output_format], output_format, limit)
                if counts is not None:
                    assert counts == observed, "Counts depend on dump limit or output format"
                counts = observed
        print("PASS: " + testcase + " failure diagnostics in text/json/perf, dump size 1/ALL")
    if args.mpi_binary:
        command = ["mpirun", "-np", "2", "-x", "HGGC_VISIBLE_DEVICES", "-x", "SAILBW_VERIFY",
                   "-x", "SAILBW_DUMP_PATH", "-x", "SAILBW_DUMP_SIZE",
                   str(args.mpi_binary.resolve()), "-t", "multinode_device_to_device_memcpy_read_ce"]
        command += SAMPLE_ARGS + ["-F", "text"]
        check_failure(command, "text", "ALL")
        print("PASS: same-node two-rank CE failure, exact bit counts, and matching dump")


if __name__ == "__main__":
    main()
