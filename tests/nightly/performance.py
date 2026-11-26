# SPDX-FileCopyrightText: Copyright (c) 2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Interpret SailBandwidth's -F perf output, not human-readable matrices."""

import math
import re


PASS = "&&&& sailbandwidth test PASSED"
FAIL = "&&&& sailbandwidth test FAILED"
METRIC_PREFIX = "&&&& PERF sailbandwidth/"
METRIC = re.compile(r"&&&& PERF sailbandwidth/([A-Za-z0-9_.-]+)/PPU(\d+)/PPU(\d+) (\S+) ([+-])((?:GB/s)|ns)\Z")
WAIVER = re.compile(r"(?:Waived|Skipping) ([A-Za-z0-9_.-]+): (.*)\Z")
ERROR = re.compile(r"(?:ERROR:|(?:Error|ERROR|Not Found) [A-Za-z0-9_.-]+:)")
VPIPE_UNSUPPORTED = "sailbandwidth was built without Vpipe ID support"
METRIC_FIELDS = ("matrix", "row", "column", "unit", "value")


def interpret(lines, returncode, operation, devices, requires_vpipe, timed_out=False):
    """Return status, diagnostic and validated matrix entries from raw output."""
    if timed_out:
        return "TIMED_OUT", "Case exceeded its time limit", []
    if returncode != 0:
        return "FAILED", f"Benchmark exited with status {returncode}", []
    passed = 0
    waivers = []
    metrics = []
    seen = set()
    ignored_vpipe = False
    for line in lines:
        line = line.strip()
        if line == FAIL or ERROR.match(line):
            return "FAILED", line, []
        if line == PASS:
            passed += 1
        elif VPIPE_UNSUPPORTED in line:
            ignored_vpipe = True
        elif (match := WAIVER.fullmatch(line)):
            if operation and match[1] != operation:
                return "INVALID_OUTPUT", f"Unexpected waived testcase: {match[1]}", []
            waivers.append(f"{match[1]}: {match[2]}")
        elif line.startswith(METRIC_PREFIX):
            match = METRIC.fullmatch(line)
            if not match:
                return "INVALID_OUTPUT", "Malformed performance record", []
            matrix, row, column, number, direction, unit = match.groups()
            if operation and matrix not in {operation, *(operation + suffix for suffix in
                    ("_read1", "_read2", "_write1", "_write2", "_total"))}:
                return "INVALID_OUTPUT", f"Unexpected matrix: {matrix}", []
            try:
                row, column, value = int(row), int(column), float(number)
            except ValueError:
                return "INVALID_OUTPUT", "Malformed matrix index or value", []
            if (row >= devices or column >= devices or not math.isfinite(value) or value < 0 or
                    (operation and ("latency" in operation) != (unit == "ns")) or
                    (unit == "ns" and (value == 0 or direction != "-")) or
                    (unit == "GB/s" and direction != "+")):
                return "INVALID_OUTPUT", "Out-of-range matrix index, value or unit", []
            key = (matrix, row, column, unit)
            if key in seen:
                return "INVALID_OUTPUT", "Duplicate performance cell", []
            seen.add(key)
            metrics.append(dict(zip(METRIC_FIELDS, (*key, value))))
    if passed != 1:
        return "INVALID_OUTPUT", "Expected exactly one SailBandwidth completion marker", []
    if requires_vpipe and ignored_vpipe:
        return "WAIVED", "Vpipe options were ignored by the benchmark build", []
    if waivers:
        if operation and metrics:
            return "INVALID_OUTPUT", "One testcase emitted both measurements and a waiver", []
        return "WAIVED", "; ".join(waivers), metrics
    if not metrics:
        return "INVALID_OUTPUT", "No measured performance cells", []
    return "PASSED", "", metrics
