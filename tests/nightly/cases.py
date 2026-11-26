# SPDX-FileCopyrightText: Copyright (c) 2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Expand reviewed families into a reproducible execution matrix and group CSVs."""

import argparse
import csv
from dataclasses import dataclass
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parent
NAME = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]*\Z")
OPERATION = re.compile(r"[a-z][a-z0-9_]*\Z")
REVISION = re.compile(r"[0-9a-f]{40}\Z")
ENV_KEYS = {"ALIPPU_RESET_CE_MASK", "NCCL_DEBUG", "UMD_PLATFORM_TYPE"}
FAMILIES = {
    "m890p-functional": ("m890p-cx8", 1, 8),
    "m890p-performance": ("m890p-cx8", 1, 8),
    "m890p-mpi": ("m890p-cx8", 2, 8),
    "810e-performance": ("810e-eic", 1, 16),
}
SELECTORS = {
    "m890p-cx8": "board-type=ZW-M890P,nic-type=cx8",
    "810e-eic": "board-type=OAM-810E,nic-type=eic",
}
VPIPE = {"--enableVpipeIdReadOptimize", "--useSingleLinkForIcnRead",
         "--linkId", "--vpipeIdForDie0", "--vpipeIdForDie1"}
FLAGS = VPIPE | {"--displayUR", "--disableVm", "--enableSplitSizeReadOptimize"}
CSV_FIELDS = ["family", "name", "timeout_seconds", "arguments"]


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object)


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def validate_arguments(args):
    """Accept the reviewed benchmark subset, not arbitrary CLI or shell syntax."""
    if not isinstance(args, list) or not args or any(not isinstance(arg, str) for arg in args):
        raise ValueError("Missing benchmark arguments")
    seen = set()
    operation = None
    index = 0
    while index < len(args):
        option = args[index]
        index += 1
        if option in seen:
            raise ValueError(f"Repeated benchmark argument: {option}")
        seen.add(option)
        if option in {"-t", "-i", "--linkId", "--vpipeIdForDie0", "--vpipeIdForDie1"}:
            if index == len(args):
                raise ValueError(f"Missing argument value: {option}")
            value = args[index]
            index += 1
            if option == "-t" and OPERATION.fullmatch(value):
                operation = value
            elif option == "-i" and re.fullmatch(r"[1-9][0-9]*", value):
                pass
            elif option in VPIPE and re.fullmatch(r"[0-9]+", value):
                pass
            else:
                raise ValueError(f"Invalid value for {option}: {value}")
        elif option not in FLAGS:
            raise ValueError(f"Unreviewed benchmark argument: {option}")
    if operation is None and args != ["--enableVpipeIdReadOptimize"]:
        raise ValueError("Only the reviewed Vpipe Case may run the default testcase set")
    return operation


@dataclass(frozen=True)
class Case:
    family: str
    name: str
    timeout: int
    arguments: list

    @property
    def row(self):
        return {"family": self.family, "name": self.name, "timeout_seconds": self.timeout,
                "arguments": json.dumps(self.arguments, separators=(",", ":"))}


def make_case(family, name, timeout, arguments):
    if family not in FAMILIES or not isinstance(name, str) or not NAME.fullmatch(name):
        raise ValueError("Invalid Case identity")
    if type(timeout) is not int or timeout < 1:
        raise ValueError("Case timeout must be a positive number of seconds")
    operation = validate_arguments(arguments)
    if bool(operation and operation.startswith("multinode_")) != (FAMILIES[family][1] > 1):
        raise ValueError(f"{name}: benchmark mode does not match requested resources")
    if operation is None and family != "m890p-functional":
        raise ValueError(f"{name}: only the functional family may run the default set")
    return Case(family, name, timeout, arguments)


def validate_env(env):
    if not isinstance(env, dict) or set(env) != ENV_KEYS or any(
            not isinstance(value, str) or not NAME.fullmatch(value) for value in env.values()):
        raise ValueError("Unreviewed benchmark environment")
    return env


def smoke_cases(cases):
    # Select representative feature paths by arguments, not generated Case IDs.
    predicates = (
        lambda case, op: bool(VPIPE.intersection(case.arguments)),
        lambda case, op: op == "host_to_device_memcpy_ce",
        lambda case, op: op == "device_to_host_memcpy_sm",
        lambda case, op: op == "device_to_device_memcpy_read_ce" and
        "--disableVm" not in case.arguments and not VPIPE.intersection(case.arguments),
        lambda case, op: op == "device_to_device_latency_sm",
        lambda case, op: op == "multinode_device_to_device_memcpy_write_ce",
        lambda case, op: op == "multinode_device_to_device_bidirectional_memcpy_read_sm",
    )
    selected = {}
    for predicate in predicates:
        for case in cases:
            if predicate(case, validate_arguments(case.arguments)):
                selected[(case.family, case.name)] = case
                break
    if not selected:
        raise ValueError("No representative smoke Cases in this environment")
    return list(selected.values())


def generate(source, output, dataset):
    catalog = read_json(source)
    revision = catalog.get("source_revision")
    if catalog.get("format_version") != 1 or not isinstance(revision, str) or not REVISION.fullmatch(revision):
        raise ValueError("Unsupported family schema or source revision")
    if dataset not in {"smoke", "full"}:
        raise ValueError("Unknown dataset")
    staged = {}
    families = set()
    for family in catalog["families"]:
        ident = family["id"]
        key = (family["platform"], family["nodes"], family["cards_per_node"])
        if key != FAMILIES.get(ident) or ident in families or not family["cases"]:
            raise ValueError(f"Missing, duplicate, or unsupported family: {ident}")
        families.add(ident)
        env = validate_env(family["env"])
        group = staged.setdefault((*key, tuple(sorted(env.items()))), [])
        group.extend(make_case(ident, name, family["timeout_seconds"], args)
                     for name, args in sorted(family["cases"].items()))
    if families != set(FAMILIES):
        raise ValueError("Incomplete family coverage")

    output.mkdir(parents=True, exist_ok=False)
    manifest = {"format_version": 1, "dataset": dataset, "source_revision": revision, "groups": []}
    for index, ((platform, nodes, cards, env), cases) in enumerate(sorted(staged.items()), 1):
        if dataset == "smoke":
            cases = smoke_cases(cases)
        group_id = f"{platform}-{nodes}n-g{index:02}"
        path = output / group_id
        path.mkdir()
        (path / "benchmark.env").write_text("".join(f"{key}={value}\n" for key, value in env), encoding="utf-8")
        with (path / "cases.csv").open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=CSV_FIELDS)
            writer.writeheader()
            writer.writerows(case.row for case in cases)
        manifest["groups"].append({"id": group_id, "platform": platform, "nodes": nodes,
                                   "cards_per_node": cards, "node_selector": SELECTORS[platform],
                                   "rack_policy": "same" if nodes > 1 else "",
                                   "case_count": len(cases)})
    write_json(output / "groups.json", manifest)
    return manifest


def load_manifest(directory):
    manifest = read_json(directory / "groups.json")
    revision = manifest.get("source_revision")
    ids = [group.get("id") for group in manifest.get("groups", [])]
    if (manifest.get("format_version") != 1 or manifest.get("dataset") not in ("smoke", "full") or
            not isinstance(revision, str) or not REVISION.fullmatch(revision) or
            not ids or any(not isinstance(key, str) or not NAME.fullmatch(key) for key in ids) or
            len(set(ids)) != len(ids)):
        raise ValueError("Invalid execution group manifest")
    return manifest


def load_group(directory):
    manifest = load_manifest(directory.parent)
    matches = [group for group in manifest["groups"] if group["id"] == directory.name]
    if len(matches) != 1:
        raise ValueError("Expected exactly one group in the manifest")
    group, = matches
    with (directory / "cases.csv").open(encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream, strict=True)
        if reader.fieldnames != CSV_FIELDS:
            raise ValueError("Invalid group CSV columns")
        cases = []
        for row in reader:
            if set(row) != set(CSV_FIELDS) or None in row.values():
                raise ValueError("Invalid group CSV row")
            cases.append(make_case(row["family"], row["name"], int(row["timeout_seconds"]),
                                   json.loads(row["arguments"])))
    env = {}
    for line in (directory / "benchmark.env").read_text(encoding="utf-8").splitlines():
        key, sep, value = line.partition("=")
        if not sep or key in env:
            raise ValueError("Malformed group environment")
        env[key] = value
    resources = (group["platform"], group["nodes"], group["cards_per_node"])
    if (not cases or len(cases) != group["case_count"] or
            len({(case.family, case.name) for case in cases}) != len(cases) or
            any(FAMILIES[case.family] != resources for case in cases)):
        raise ValueError("Invalid group Case coverage or resources")
    return group, validate_env(env), cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--families", type=Path, default=ROOT / "families.json")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--dataset", choices=("smoke", "full"), default="smoke")
    args = parser.parse_args()
    result = generate(args.families, args.output, args.dataset)
    print(json.dumps({"include": result["groups"]}))


if __name__ == "__main__":
    main()
