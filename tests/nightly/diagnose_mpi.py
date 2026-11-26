# SPDX-FileCopyrightText: Copyright (c) 2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Triage one smoke MPI allocation without changing normal Case budgets."""

import argparse
from itertools import chain
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time

from build_cache import MPI_PROBE, extract, identity
from cases import load_group, validate_arguments
from performance import interpret
from run_cases import interrupt_on_term, mpi_launch, terminate
from smoke_ci import smoke_matrix, verify_nas_probes


OPERATION = "multinode_device_to_device_memcpy_write_ce"
PROGRESS = "SAILBANDWIDTH_MPI_PROGRESS"
HOST = r"[A-Za-z0-9][A-Za-z0-9_.-]*"
RANK_LINE = re.compile(rf"probe rank=([01]) host=({HOST})")
UUID = r"GPU-[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}"


class DiagnosticError(Exception):
    """A stage failed; no further MPI commands are safe in this allocation."""


def setup_devices(logs, group):
    devices = {}
    for worker in range(2):
        text = (logs / f"{group}-{worker}.log").read_text(encoding="utf-8")
        hosts = re.findall(rf"^Diagnostic host: ({HOST})$", text, re.MULTILINE)
        uuids = re.findall(rf"^PPU 0: .*\(UUID: ({UUID})\)$", text, re.MULTILINE)
        if len(hosts) != 1 or len(uuids) != 1 or hosts[0] in devices or uuids[0] in devices.values():
            raise DiagnosticError("Worker setup did not identify two distinct hosts and PPUs")
        devices[hosts[0]] = uuids[0]
    return devices


def run_step(name, command, env, output, work, timeout):
    stdout = output / f"{name}.stdout.log"
    stderr = output / f"{name}.stderr.log"
    started = time.monotonic()
    print(f"{name}: starting ({timeout}s budget)", flush=True)
    try:
        with stdout.open("xb") as out, stderr.open("xb") as err:
            process = subprocess.Popen(command, env=env, cwd=work, stdout=out, stderr=err,
                                       start_new_session=True)
            try:
                rc = process.wait(timeout=timeout)
            except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
                terminate(process)
                status = "timed out" if isinstance(error, subprocess.TimeoutExpired) else "was interrupted"
                raise DiagnosticError(f"{name} {status}; remote rank exit was not verified") from None
    except (OSError, subprocess.TimeoutExpired):
        raise DiagnosticError(f"{name} could not run or terminate cleanly") from None
    if rc != 0:
        raise DiagnosticError(f"{name} exited {rc}; see the retained logs")
    print(f"{name}: exited in {time.monotonic() - started:.1f}s", flush=True)
    return stdout, stderr


def verify_hostname(lines, devices):
    hosts = [line.strip() for line in lines if line.strip()]
    if len(hosts) != 2 or set(hosts) != devices.keys():
        raise DiagnosticError("MPI did not launch one hostname command on each worker")


def verify_probe(lines, devices):
    ranks = {}
    for line in lines:
        match = RANK_LINE.fullmatch(line.rstrip("\n"))
        if not match or int(match[1]) in ranks:
            raise DiagnosticError("Invalid or duplicate MPI probe rank")
        ranks[int(match[1])] = match[2]
    if set(ranks) != {0, 1} or set(ranks.values()) != devices.keys():
        raise DiagnosticError("MPI barrier did not complete on both workers")
    return [devices[ranks[rank]] for rank in (0, 1)]


def verify_benchmark(logs, ranks):
    with logs[0].open(encoding="utf-8", errors="replace") as out, \
            logs[1].open(encoding="utf-8", errors="replace") as err:
        status, _, metrics = interpret(chain(out, err), 0, OPERATION, ranks, False)
    expected = {(source, peer) for source in range(ranks) for peer in range(ranks) if source != peer}
    observed = {(m["row"], m["column"]) for m in metrics if m["matrix"] == OPERATION}
    if status != "PASSED" or observed != expected:
        raise DiagnosticError(f"Benchmark did not retain all {len(expected)} directed pairs ({status})")
    print(f"Verified {len(observed)} directed pairs", flush=True)


def diagnose(root, group_id, build_sha, hostfile, interface):
    selection = root / "selection"
    mpi_groups = [item["id"] for item in smoke_matrix(selection)["include"] if item["nodes"] == 2]
    if mpi_groups != [group_id]:
        raise DiagnosticError("Diagnostics require the reviewed 890P two-worker smoke group")
    group, shared_env, cases = load_group(selection / group_id)
    original, = [case for case in cases if validate_arguments(case.arguments) == OPERATION]
    build = identity(root / "build.json")
    if build["sha256"] != build_sha:
        raise DiagnosticError("Expected build archive does not match the staged identity")
    verify_nas_probes(root, only_group=group_id)
    devices = setup_devices(root / "logs", group_id)
    output = root / "logs" / f"{group_id}-diagnostics"
    output.mkdir()
    env = {**os.environ, **shared_env, PROGRESS: "1"}
    small_launch = mpi_launch({**group, "cards_per_node": 1}, hostfile, interface, env)
    full_launch = mpi_launch(group, hostfile, interface, env)
    with tempfile.TemporaryDirectory(prefix="mpi-diagnostic-", dir=root / "scratch") as work:
        work = Path(work)
        stage = work / "build"
        extract(root / "cache" / build_sha, stage, build)
        probe = stage / MPI_PROBE
        if not probe.is_file():
            raise DiagnosticError("The verified archive does not contain the MPI probe")
        binary = probe.with_name("sailbandwidth")
        with interrupt_on_term():
            hostname, _ = run_step("01-hostname", [*small_launch, "hostname"], env, output, work, 30)
            verify_hostname(hostname.read_text().splitlines(), devices)
            probe_log, _ = run_step("02-mpi-barrier", [*small_launch, str(probe)], env, output, work, 60)
            uuids = verify_probe(probe_log.read_text().splitlines(), devices)
            verify_benchmark(run_step("03-cross-node-ce", [*small_launch, "-x", PROGRESS, str(binary),
                "-t", OPERATION, "-b", "16", "--loopCount", "1", "-i", "1", "--pair", *uuids, "-F", "perf"],
                env, output, work, 90), 2)
            verify_benchmark(run_step("04-original-16-rank", [*full_launch, "-x", PROGRESS, str(binary),
                *original.arguments, "-F", "perf"], env, output, work, 600), 16)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--group", required=True)
    parser.add_argument("--build-sha", required=True)
    parser.add_argument("--hostfile", type=Path, required=True)
    parser.add_argument("--mpi-interface", required=True)
    args = parser.parse_args()
    try:
        diagnose(args.root, args.group, args.build_sha, args.hostfile, args.mpi_interface)
    except DiagnosticError as error:
        raise SystemExit(f"MPI diagnostics stopped: {error}") from None
    except (OSError, ValueError) as error:
        # Worker paths and setup records must not appear in public workflow logs.
        raise SystemExit(f"MPI diagnostics stopped: cannot validate or read diagnostic inputs ({type(error).__name__})") from None
    print("All MPI diagnostic stages passed", flush=True)


if __name__ == "__main__":
    main()
