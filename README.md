# sailbandwidth

sailbandwidth is a bandwidth and latency benchmark for Zhenwu PPU devices (Zhenwu 610, Zhenwu 610E,
Zhenwu 810, Zhenwu 810E, Zhenwu M890, etc.). It measures data
movement across host–device and device–device links through optimized Copy Engine (CE) and Streaming
Multiprocessor (SM) testcases, and adds enhanced debugging and fault-diagnosis capabilities, serving
both as a performance baseline and as an interconnect diagnostic.

For more information on sailbandwidth usage, please refer to the
[sailbandwidth documentation](https://developer.t-head.cn/docs_center/doc_detail/index.html?k=bo2ok07hwc).

## Overview

sailbandwidth organizes measurements around the following transfer patterns:

| Pattern | Description |
|---|---|
| H2D / D2H | Host to device and device to host transfers |
| D2D | Device to device transfers between peer PPUs |
| Host-to-all / all-to-host | One host driving, or being driven by, all devices in parallel |
| All-to-one / one-to-all | All peers transferring with one device at once (N:1), or one device with all peers at once (1:N) |
| Latency | Pointer-chase latency for host–device and device–device access |

The H2D, D2H and D2D patterns measure one pair at a time, so each transfer runs without contention.
The all-to-one and one-to-all patterns instead run their copies concurrently and report the
aggregate bandwidth at the device under test, which exposes link contention.

Each bandwidth pattern is available over two copy paths:

- **Copy Engine (CE)**, also called DMA copy — transfers driven by the PPU DMA engine without
  occupying SM compute resources. This is the preferred path for bulk data movement on supported
  PPU systems.
- **Streaming Multiprocessor (SM)**, also called kernel copy — transfers performed by a copy kernel
  running on the PPU SMs. This is also the path used for latency measurements.

Device-to-device transfers may be unidirectional or bidirectional, and read- or write-initiated.

## Requirements

- T-Head SAIL SDK. Download the SDK from the
  [T-Head Developer Center](https://developer.t-head.cn/download/index.html). After installation,
  enter the SDK installation directory and run `source envsetup.sh` to configure the build
  environment.
- CMake 3.20 or above (3.24 or newer recommended).
- Boost `program_options` library (see below).
- Open MPI (multinode builds only).

Boost is linked statically, so its static archive must be installed. On Ubuntu 20.04 / 22.04 / 24.04:

```
apt install libboost-program-options-dev
```

On AliOS 7 / AliOS 8 / AFA3 / AFA4:

```
yum install boost-static
```

The build locates the Boost libraries automatically by searching under `/usr`.

## Build

Build the single-node executable:

```
./build.sh
```

The executable is produced at `build/sailbandwidth`.

### Common build options

The following are commonly used build options. For the complete list, see [`build.sh`](build.sh).

- `--use_mpi` — build the multinode version. MPI is required, and only builds configured with this
  option contain the `multinode` testcases.
- `--ppu_archs <archs>` — target PPU architectures: `ppu_10`, `ppu_15`, or both (the default).
  Restricting this to the architecture of the target machine shortens the build.
- `--rebuild` — remove the `build` directory and configure from scratch. Required when switching
  between single-node and multinode builds, as the previous CMake cache is otherwise reused.
- `--verbose` — print the full compiler command lines, for diagnosing build failures.

## Testcases

Testcase names follow the pattern `<transfer>_<ce|sm>`: the `_ce` suffix selects the copy-engine
path and `_sm` the kernel path. The single exception is `device_local_copy`, which has no suffix
and always uses the copy-engine path. Bidirectional variants add `bidirectional`, and
device-to-device transfers distinguish `read` from `write`. Use `-l` for the full list on your build.

Copy Engine (CE):

| Testcase | Description |
|---|---|
| `host_to_device_memcpy_ce`, `device_to_host_memcpy_ce` | Host ↔ device, unidirectional |
| `host_to_device_bidirectional_memcpy_ce`, `device_to_host_bidirectional_memcpy_ce` | Host ↔ device, bidirectional |
| `device_to_device_memcpy_read_ce`, `device_to_device_memcpy_write_ce` | Device ↔ device, unidirectional read / write |
| `device_to_device_bidirectional_memcpy_read_ce`, `..._write_ce` | Device ↔ device, bidirectional read / write |
| `all_to_host_memcpy_ce`, `host_to_all_memcpy_ce` (+ `bidirectional`) | All devices ↔ host, in parallel |
| `all_to_one_write_ce`, `all_to_one_read_ce` | All peers transfer with one device simultaneously; the aggregate bandwidth at that device is reported |
| `one_to_all_write_ce`, `one_to_all_read_ce` | One device transfers with all peers simultaneously; the aggregate bandwidth at that device is reported |
| `device_local_copy` | Copy between two buffers local to the same device |

Streaming Multiprocessor (SM): the SM counterparts of the CE testcases above, except
`device_local_copy`, plus the following latency testcases:

| Testcase | Description |
|---|---|
| `host_device_latency_sm` | Pointer-chase latency for device access to host memory |
| `device_to_device_latency_sm` | Pointer-chase latency for device ↔ device access |

Latency testcases use a pointer-chase kernel over a fixed 2 MiB buffer; `--bufferSize` is ignored.

## Usage

The following sections describe how to select and run the testcases listed above, and the options
that control their behavior.

```
./build/sailbandwidth [options]
```

Run all testcases, a specific one by name or index, or a group by prefix:

```
./build/sailbandwidth                                     # run all testcases
./build/sailbandwidth -l                                  # list available testcases
./build/sailbandwidth -t device_to_device_memcpy_read_ce  # run one testcase
./build/sailbandwidth -p multinode                        # run all testcases with a prefix
```

### General options

The following are commonly used runtime options. Run `./build/sailbandwidth -h` for all public
options; additional advanced options are defined in
[`src/sailbandwidth.cpp`](src/sailbandwidth.cpp).

| Option | Default | Description |
|---|---|---|
| `-h, --help` | – | Show the help message |
| `-b, --bufferSize <MiB>` | 512 | Copy buffer size in MiB |
| `-l, --list` | – | List available testcases |
| `-t, --testcase <id>` | – | Run specific testcase(s), by name or index |
| `-p, --testcasePrefixes <prefix>` | – | Run testcases matching a prefix |
| `-v, --verbose` | off | Verbose output |
| `-s, --skipVerification` | off | Skip data verification after each copy |
| `-d, --disableAffinity` | off | Disable automatic CPU affinity control |
| `-i, --testSamples <n>` | 3 | Benchmark iterations per testcase |
| `-m, --useMean` | off | Report the mean instead of the median |

Example output (`device_to_device_memcpy_write_ce`, bandwidth in GB/s; the diagonal is `N/A`):

```text
Running device_to_device_memcpy_write_ce.
memcpy CE PPU(row) <- PPU(column) bandwidth (GB/s)
          0         1         2         3         4         5         6         7
0       N/A    384.13    384.14    384.14    384.12    384.12    384.13    384.14
1    384.13       N/A    384.13    384.16    384.14    384.13    384.13    384.15
2    384.16    384.14       N/A    384.17    384.15    384.14    384.16    384.16
3    384.16    384.16    384.16       N/A    384.17    384.15    384.16    384.17
4    384.13    384.13    384.14    384.15       N/A    384.13    384.14    384.14
5    384.13    384.14    384.13    384.15    384.13       N/A    384.13    384.16
6    384.14    384.14    384.13    384.14    384.14    384.13       N/A    384.14
7    384.15    384.15    384.16    384.16    384.16    384.17    384.17       N/A

SUM device_to_device_memcpy_write_ce 21512.06
```

The figures above were measured on a Zhenwu M890 system and are provided for reference only; actual
results depend on the machine configuration, the link topology and the SAIL SDK version. The `SUM`
line is the sum of the matrix elements; because each pair is measured one at a time, it does not
represent the machine's simultaneous aggregate throughput.

### PPU tuning options

The following are the most commonly used options controlling PPU-specific copy behavior:

| Option | Default | Description |
|---|---|---|
| `--nBlocks <n>` | -1 (auto) | Block count for the SM bulk-copy kernel |
| `--nThreads <n>` | -1 (auto) | Thread count for the SM bulk-copy kernel |
| `--displayUR` | off | Print bandwidth utilization-ratio matrices for D2D testcases |
| `--enableSplitSizeReadOptimize` | off | Split CE read commands across ICN links in `all_to_one_read_ce` and `one_to_all_read_ce` |

### Data verification and fault injection

By default, the final buffer contents are verified after each sample's copy iterations complete;
use `--skipVerification` to disable this. On a
verification failure the run prints an aligned report that identifies the affected transfer —
including each endpoint's host, rank and PCI BDF and the first bad offset — and, when both endpoints
are locally readable, distinguishes whether the observed mismatch was already present in the source
or appeared during the transfer. It also writes a dump of the mismatching values and aborts.

The check fills the source buffer with the pattern the destination should end up with, and pre-fills
the destination with a different marker. Afterwards a word still holding the marker is consistent
with never having been written, and a word holding neither was corrupted.

When the reporting process can independently read both the original source and the verified
destination — meaning both are owned locally, have readable mappings, and are distinct buffers — the
report compares the source content with the transfer result (read and write testcases report the same
fault the same way):

| `source` | `transfer` | What happened | Where to look |
|---|---|---|---|
| OK | BAD | The source was intact and the data arrived wrong | The link between the endpoints, or the destination memory |
| OK | NOT-WR | The destination retained its prefill marker | Consistent with an unwritten destination; inspect the transfer path and destination memory |
| BAD | OK | The source memory already held the wrong value and it was moved faithfully | The source endpoint; the link is not implicated by this evidence |
| BAD | BAD | The source is wrong and the destination differs from it as well | Both endpoints |

When the original source cannot be read independently — for example, because it is also the verified
buffer in a split-warp copy, has no local mapping, or is owned by another process — the report drops
the source/transfer split and shows only the top-level comparison of the received value (`actual`)
against the expected value. To attribute a multinode failure, re-run the same pair on one machine
with a single-process device-to-device testcase.

`flipped bits ever` is the union of the wrong bits over all bad values, `always` their intersection,
so they tell you whether the failures share a bit position — not how often that bit fails. Read them
together with `bad values`: a permanently stuck data line would corrupt about half of all words, so a
handful out of millions means marginal rather than stuck. With one or two bad values `ever == always`
carries no information. These are diagnostic hints, not proof; the inference depends on the data
pattern and the fault model.

The following environment variables assist link fault diagnosis:

| Variable | Description |
|---|---|
| `SAILBW_VERIFY=<srcId>,<dstId>` | Deliberately corrupt the matching link to exercise the failure path. Ids are device ordinals (single-node) or MPI ranks (multinode; forward with `mpirun -x`). |
| `SAILBW_DUMP_PATH=<dir>` | Directory for the human-readable dump of mismatching values. The dump is written on every verification failure whether or not this is set; it defaults to the current directory. Each rank writes to its own node, so point this at a shared directory to collect all dumps. |
| `SAILBW_DUMP_SIZE=<N\|ALL>` | Number of bad values to dump (default 100; `ALL` dumps every mismatch). |

```
SAILBW_DUMP_PATH=/tmp SAILBW_DUMP_SIZE=ALL SAILBW_VERIFY=0,1 \
    ./build/sailbandwidth -t device_to_device_memcpy_read_sm -v
```

## Multinode benchmarks

Build the multinode version:

```
./build.sh --use_mpi
```

Multinode runs require MPI and a configured PTG-IMEX service with the corresponding channels created.
Multinode testcases carry the `multinode` prefix and cover device-to-device transfers (read/write,
unidirectional/bidirectional, over both CE and SM) as well as all-to-one and all-from-one
aggregation over SM.

Run on a single node for local testing:

```
mpirun -n 4 ./build/sailbandwidth -p multinode
```

Run across a cluster, one process per device:

```
mpirun -np 8 -npernode 4 --hostfile <path/to/host.cfg> ./build/sailbandwidth -p multinode
```

Example output from two Zhenwu M890 nodes with eight PPUs per node (16 MPI processes):

```text
Running multinode_device_to_device_memcpy_write_ce.
memcpy CE PPU(row) <- PPU(column) bandwidth (GB/s)
          0         1         2         3         4         5         6         7         8         9        10        11        12        13        14        15
0       N/A    384.14    384.16    384.13    384.14    384.14    384.12    384.13    384.14    384.14    384.13    384.14    384.15    384.13    384.15    384.13
1    384.12       N/A    384.15    384.15    384.14    384.13    384.12    384.14    384.14    384.14    384.13    384.14    384.13    384.14    384.13    384.16
2    384.12    384.15       N/A    384.12    384.15    384.13    384.12    384.14    384.14    384.12    384.12    384.14    384.14    384.13    384.15    384.13
3    384.15    384.17    384.15       N/A    384.17    384.16    384.15    384.17    384.17    384.15    384.15    384.16    384.16    384.17    384.15    384.19
4    384.11    384.13    384.15    384.13       N/A    384.14    384.14    384.16    384.14    384.13    384.14    384.13    384.14    384.13    384.13    384.14
5    384.11    384.15    384.14    384.14    384.15       N/A    384.15    384.16    384.14    384.14    384.14    384.15    384.14    384.14    384.13    384.15
6    384.14    384.17    384.16    384.16    384.18    384.17       N/A    384.18    384.15    384.16    384.16    384.17    384.16    384.15    384.16    384.17
7    384.15    384.17    384.16    384.15    384.18    384.17    384.17       N/A    384.16    384.16    384.16    384.17    384.16    384.14    384.16    384.17
8    384.12    384.14    384.13    384.12    384.14    384.13    384.12    384.12       N/A    384.13    384.14    384.15    384.12    384.13    384.13    384.13
9    384.13    384.14    384.14    384.13    384.14    384.13    384.12    384.14    384.14       N/A    384.13    384.15    384.14    384.13    384.13    384.15
10   384.14    384.16    384.15    384.16    384.16    384.16    384.16    384.16    384.17    384.15       N/A    384.17    384.15    384.15    384.16    384.15
11   384.14    384.16    384.16    384.16    384.17    384.16    384.15    384.16    384.16    384.18    384.16       N/A    384.15    384.16    384.16    384.17
12   384.12    384.15    384.17    384.13    384.14    384.14    384.13    384.14    384.15    384.14    384.13    384.15       N/A    384.13    384.14    384.14
13   384.15    384.18    384.17    384.16    384.17    384.16    384.15    384.17    384.16    384.15    384.16    384.15    384.15       N/A    384.16    384.19
14   384.15    384.16    384.18    384.15    384.16    384.16    384.15    384.16    384.16    384.17    384.16    384.17    384.18    384.15       N/A    384.16
15   384.15    384.17    384.15    384.17    384.17    384.16    384.15    384.16    384.16    384.16    384.16    384.17    384.16    384.16    384.16       N/A

SUM multinode_device_to_device_memcpy_write_ce 92195.71
```

The matrix row and column indices are MPI process ranks; each process controls one PPU. As in the
single-node example, `SUM` is the sum of the matrix elements, not simultaneous aggregate throughput.

## Copyright

sailbandwidth is derived from NVIDIA nvbandwidth.

Copyright © 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

Modifications Copyright © 2025-2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.

sailbandwidth is licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) and
[Licenses.txt](Licenses.txt) for license and third-party notice details.
