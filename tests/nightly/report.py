# SPDX-FileCopyrightText: Copyright (c) 2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Reconcile nightly selection and results; write CSV, summary and platform workbooks."""

import argparse
from collections import Counter, defaultdict
import csv
import json
import math
from pathlib import Path

from build_cache import identity
from cases import VPIPE, load_group, load_manifest, read_json, validate_arguments, write_json
from performance import METRIC_FIELDS, interpret


CASE_COLUMNS = ("group", "family", "name", "status", "detail", "arguments", "duration_seconds")
DATA_COLUMNS = ("group", "family", "name", *METRIC_FIELDS)
RECORDED = {"PASSED", "WAIVED", "FAILED", "INVALID_OUTPUT", "TIMED_OUT",
            "CANCELLED", "EXECUTION_ERROR", "NOT_RUN"}


def verify_case(path, case, group, build):
    result_file = path / "result.json"
    if not result_file.is_file():
        return "MISSING", "No Case result was retained", 0, []
    try:
        result = read_json(result_file)
        if (result["family"] != case.family or result["name"] != case.name or
                result["build"] != build or result["status"] not in RECORDED):
            raise ValueError("Case key, build identity, or status mismatch")
        status = result["status"]
        detail, duration = result["detail"], result["duration_seconds"]
        count = result["metric_count"]
        if (not isinstance(detail, str) or type(duration) not in (int, float) or
                not math.isfinite(duration) or duration < 0 or type(count) is not int or count < 0):
            raise ValueError("Invalid retained duration, diagnostic or measurement count")
        metrics = []
        if status in {"PASSED", "WAIVED", "FAILED", "INVALID_OUTPUT"}:
            if type(result["returncode"]) is not int:
                raise ValueError("Missing benchmark execution record")
            with (path / "raw.log").open(encoding="utf-8", errors="replace") as stream:
                observed, _, metrics = interpret(stream, result["returncode"], validate_arguments(case.arguments),
                                                 group["nodes"] * group["cards_per_node"],
                                                 bool(VPIPE.intersection(case.arguments)))
            if observed != status:
                raise ValueError("Retained status does not match benchmark output")
        if count != len(metrics):
            raise ValueError("Retained measurement count does not match benchmark output")
        return status, detail, duration, metrics
    except (KeyError, TypeError, ValueError, FileNotFoundError) as error:
        return "INVALID_RESULT", str(error), 0, []


def reconcile(selection, results, expected):
    manifest = load_manifest(selection)
    expected_results = set()
    selected_keys = set()
    platforms = defaultdict(lambda: ([], []))
    for entry in manifest["groups"]:
        group_id = entry["id"]
        group, _, cases = load_group(selection / group_id)
        case_rows, metrics_rows = platforms[group["platform"]]
        group_dir = results / group_id
        identity_file = group_dir / "group.json"
        try:
            invalid_group = read_json(identity_file) != {"group": group, "build": expected}
        except (FileNotFoundError, ValueError):
            invalid_group = True
        for case in cases:
            path = group_dir / case.family / case.name
            result_file = path / "result.json"
            key = (case.family, case.name)
            if key in selected_keys:
                raise ValueError("Duplicate selected Case key")
            selected_keys.add(key)
            expected_results.add(result_file)
            if invalid_group and result_file.is_file():
                status, detail, elapsed, metrics = "INVALID_RESULT", "Group or build identity mismatch", 0, []
            else:
                status, detail, elapsed, metrics = verify_case(path, case, group, expected)
            case_rows.append((group_id, case.family, case.name, status, detail,
                              json.dumps(case.arguments), elapsed))
            metrics_rows.extend((group_id, case.family, case.name, *(row[key] for key in METRIC_FIELDS))
                                for row in metrics)
    observed = set(results.glob("*/*/*/result.json"))
    return manifest, platforms, sorted(str(path.relative_to(results)) for path in observed - expected_results)


def write_csv(path, columns, rows):
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(columns)
        writer.writerows(rows)


def write_workbook(path, case_rows, metrics_rows):
    import xlsxwriter

    with xlsxwriter.Workbook(str(path), {"constant_memory": True, "strings_to_formulas": False,
                                         "strings_to_urls": False}) as book:
        for name, columns, rows in (("Cases", CASE_COLUMNS, case_rows), ("Measurements", DATA_COLUMNS, metrics_rows)):
            sheet = book.add_worksheet(name)
            sheet.freeze_panes(1, 0)
            sheet.write_row(0, 0, columns)
            for index, row in enumerate(rows, 1):
                if sheet.write_row(index, 0, row) != 0:
                    raise ValueError("Result exceeds Excel worksheet limits")
            sheet.autofilter(0, 0, len(rows), len(columns) - 1)


def report(selection, results, expected, output, excel=False):
    manifest, platforms, unexpected = reconcile(selection, results, expected)
    output.mkdir(parents=True, exist_ok=False)
    summary = {"source_revision": manifest["source_revision"], "dataset": manifest["dataset"],
               "build": expected, "unexpected_results": unexpected, "platforms": {}}
    for platform, (rows, metrics) in sorted(platforms.items()):
        summary["platforms"][platform] = dict(sorted(Counter(row[3] for row in rows).items()))
        write_csv(output / f"{platform}-cases.csv", CASE_COLUMNS, rows)
        write_csv(output / f"{platform}-metrics.csv", DATA_COLUMNS, metrics)
    write_json(output / "summary.json", summary)
    if excel:
        for platform, (rows, metrics) in sorted(platforms.items()):
            write_workbook(output / f"{platform}.xlsx", rows, metrics)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selection", type=Path, required=True)
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--identity", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--excel", action="store_true", help="Requires XlsxWriter on the reporting runner")
    args = parser.parse_args()
    summary = report(args.selection, args.results, identity(args.identity), args.output, args.excel)
    print(json.dumps(summary, indent=2))
    # A waiver is visible and distinct from a failure, but not complete hardware coverage.
    return int(bool(summary["unexpected_results"]) or any(
        set(statuses) != {"PASSED"} for statuses in summary["platforms"].values()))


if __name__ == "__main__":
    raise SystemExit(main())
