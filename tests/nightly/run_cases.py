# SPDX-FileCopyrightText: Copyright (c) 2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Run one allocated SailBandwidth execution group and preserve its evidence."""

import argparse
from contextlib import contextmanager
import csv
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import tempfile
import time

from build_cache import extract, identity
from cases import ENV_KEYS, VPIPE, load_group, validate_arguments, write_json
from performance import METRIC_FIELDS, interpret


def mpi_launch(group, hostfile, interface, env):
    nodes, cards = group["nodes"], group["cards_per_node"]
    if nodes == 1:
        if hostfile or interface:
            raise ValueError("Single-node groups do not use MPI launcher inputs")
        return []
    if not hostfile or not interface or not re.fullmatch(r"[A-Za-z0-9_.-]+", interface):
        raise ValueError("MPI requires a cluster hostfile and routed interface")
    hosts = []
    for line in hostfile.read_text(encoding="utf-8").splitlines():
        if line.strip():
            hosts.append(line.split()[0])
    if len(hosts) != nodes or len(set(hosts)) != nodes:
        raise ValueError("Cluster hostfile does not contain one unique entry per allocated node")
    # Rank environment forwarding happens after the remote daemon starts.
    env.update({"HWLOC_COMPONENTS": "-gl", "OMPI_MCA_plm_rsh_agent": "cluster-ssh",
                "OMPI_MCA_orte_launch_agent": "env HWLOC_COMPONENTS=-gl orted",
                "OMPI_MCA_orte_keep_fqdn_hostnames": "1", "OMPI_MCA_oob_tcp_if_include": interface,
                "OMPI_MCA_btl_tcp_if_include": interface})
    keys = ("PATH", "LD_LIBRARY_PATH", "HWLOC_COMPONENTS", *sorted(ENV_KEYS))
    forward = [arg for key in keys if key in env for arg in ("-x", key)]
    return ["mpirun", "--allow-run-as-root", "-np", str(nodes * cards), "-npernode", str(cards),
            "--hostfile", str(hostfile.resolve()), *forward]


def terminate(process):
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(process.pid, sig)
        except ProcessLookupError:
            break
        if sig == signal.SIGTERM:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
    process.wait(timeout=5)


@contextmanager
def interrupt_on_term():
    def cancel(_sig, _frame):
        raise KeyboardInterrupt

    previous = signal.signal(signal.SIGTERM, cancel)
    try:
        yield
    finally:
        signal.signal(signal.SIGTERM, previous)


def execute_case(case, group, env, argv, output, build):
    output.mkdir(parents=True)
    work = output / "work"
    work.mkdir()
    (output / "command.txt").write_text(shlex.join(argv) + "\n", encoding="utf-8")
    start = time.monotonic()
    returncode, metrics = None, []
    raw = output / "raw.log"
    timed_out = False
    cancelled = False
    try:
        with raw.open("wb") as stream:
            process = subprocess.Popen(argv, env=env, cwd=work, stdout=stream,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            try:
                returncode = process.wait(timeout=case.timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                terminate(process)
            except KeyboardInterrupt:
                cancelled = True
                terminate(process)
        if cancelled:
            status, detail = "CANCELLED", "Runner was interrupted"
        else:
            with raw.open(encoding="utf-8", errors="replace") as stream:
                status, detail, metrics = interpret(
                    stream, returncode, validate_arguments(case.arguments),
                    group["nodes"] * group["cards_per_node"], bool(VPIPE.intersection(case.arguments)), timed_out)
    except (OSError, subprocess.TimeoutExpired) as error:
        status, detail = "EXECUTION_ERROR", str(error)
    if metrics:
        with (output / "metrics.csv").open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, METRIC_FIELDS)
            writer.writeheader()
            writer.writerows(metrics)
    result = {"family": case.family, "name": case.name, "status": status, "detail": detail,
              "returncode": returncode, "duration_seconds": round(time.monotonic() - start, 3),
              "build": build, "metric_count": len(metrics)}
    write_json(output / "result.json", result)
    if cancelled:
        raise KeyboardInterrupt
    return result


def run_group(group, cases, env, launcher, binary, output, build):
    blocked = False
    with interrupt_on_term():
        for case in cases:
            result_dir = output / case.family / case.name
            if blocked:
                result_dir.mkdir(parents=True)
                result = {"family": case.family, "name": case.name, "status": "NOT_RUN",
                          "detail": "MPI cleanup was not confirmed after a launcher failure",
                          "returncode": None, "duration_seconds": 0, "build": build, "metric_count": 0}
                write_json(result_dir / "result.json", result)
            else:
                result = execute_case(case, group, env, [*launcher, str(binary), *case.arguments, "-F", "perf"],
                                      result_dir, build)
                # Killing a local launcher cannot prove the remote ranks stopped.
                # Until remote recovery is validated, only continue after a clean local launch.
                blocked = bool(launcher and (result["status"] in {"TIMED_OUT", "EXECUTION_ERROR"} or
                                             (result["returncode"] is not None and result["returncode"] != 0)))
            print(f'{case.family}/{case.name}: {result["status"]}', flush=True)


def run(directory, cache, identity_path, output, work_root, hostfile=None, interface=None):
    group, shared_env, cases = load_group(directory)
    build = identity(identity_path)
    env = {**os.environ, **shared_env}
    launcher = mpi_launch(group, hostfile, interface, env)
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    mode = "multi_process" if launcher else "single_process"
    write_json(output / "group.json", {"group": group, "build": build})
    # For MPI this root must be mounted at the same absolute path on every
    # worker. Only results, not the extracted executable, are retained.
    with tempfile.TemporaryDirectory(prefix="sailbandwidth-", dir=work_root.resolve()) as scratch:
        stage = Path(scratch) / "build"
        extract(cache, stage, build)
        binary = stage / "comm_tools" / mode / "sailbandwidth"
        run_group(group, cases, env, launcher, binary, output, build)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--group", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--identity", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--hostfile", type=Path)
    parser.add_argument("--mpi-interface")
    args = parser.parse_args()
    run(args.group, args.cache, args.identity, args.output, args.work_root, args.hostfile, args.mpi_interface)


if __name__ == "__main__":
    main()
