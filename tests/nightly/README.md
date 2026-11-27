# SailBandwidth nightly test toolkit

`families.json` is a reviewed, generated snapshot of the source test families.
Refresh it from the source definitions rather than editing it by hand. The
snapshot records its source revision and preserves benchmark arguments, including
samples and ICN options. It does not contain Runner internals or upload settings.
Generated execution groups and CSVs are run artifacts, not maintained inputs.

| Allocation | Full | Smoke |
| --- | ---: | ---: |
| M890P CX8, 1 node × 8 PPUs | 48 | 5 |
| M890P CX8, same-rack 2 nodes × 8 PPUs | 10 | 2 |
| 810E, 1 node × 16 PPUs | 37 | 4 |

810E single-node coverage needs a compatibility check on the EIC pool: it is a
board-level candidate adapted from a different source NIC topology, not validated
NIC parity. SNIC, other board types, and cross-rack workloads are out of scope.
The experiment gives only the 16-PPU EIC device-to-device SM latency Case at
least 1200 seconds. Its serial pointer-chase sweep has up to 240 directed pairs;
the 8-PPU run's 56 pairs already take over two minutes. The generated execution
budget is separate from the source snapshot and does not change measured work,
Case arguments, or the budget of any other benchmark.

## Local validation

From the repository root, using Python 3.10 or newer:

```sh
python3 -S -B -m unittest discover -s tests/nightly -v
python3 -S -B tests/nightly/cases.py --dataset smoke --output "$RUN_DIR/selection"
```

The tooling is standard-library-only except optional Excel reporting
(`XlsxWriter==3.2.9` on the report runner). The CPU build runner fetches the
pinned pure-Python wheel, and the report job rechecks its SHA-256 before import.
No pip, virtual environment, or package registry access is needed on the report
runner. No benchmark execution dependency is added. The unit tests exercise real local subprocesses with fixture binaries;
they do not validate the Sail SDK, hardware or MPI transport.

## Full validation budget

The experiment helpers accept `matrix SELECTION --dataset full` for a generated
full selection. The default remains `smoke`; a full selection is rejected
unless explicitly requested, and full runs cannot use the smoke MPI diagnostic
mode. The full selection uses the same three allocations and one shared build.

| Allocation | Cluster timeout | Controller job timeout |
| --- | ---: | ---: |
| M890P CX8, 1 node × 8 PPUs | 300 min | 325 min |
| M890P CX8, same-rack 2 nodes × 8 PPUs | 105 min | 130 min |
| 810E EIC, 1 node × 16 PPUs | 255 min | 280 min |

The cluster timeout covers scheduling, setup and benchmark execution. The full
budget sums effective per-Case timeouts, adds 10 seconds per Case for termination
and 45 minutes for queueing and setup, then rounds up to a 15-minute boundary.
The controller job allows 25 extra minutes. Smoke keeps its 90- and 115-minute
budgets. None of these limits changes the per-Case execution timeout or the
production nightly schedule.

## Workflow integration contract

Keep the existing check-in and release publication jobs separate. Initially
expose only a trusted manual smoke/full dispatch; enable the scheduled handoff
after hardware and result publication checks are green.

1. **Resolve the build.** For scheduled testing, use the explicitly identified
   successful scheduled build and select its Ubuntu 24.04 / HGGCRT 13 package.
   The package already contains single-process and MPI binaries. The producer
   must seal it with its actual build checkout SHA and SDK identity, not the
   workflow trigger SHA. For a manual validation, build those two binaries
   once on CPU for the selected revision. `build_cache.py seal --help` describes
   the inputs. The producer-side `build.json` is not in today's release artifact;
   add it as a separate artifact so the release package contract stays unchanged.
2. **Stage.** Use a run-owned NAS root and one verified archive keyed by its
   SHA-256. Stage the generated selection there once and back it up as an
   artifact. Check the mounted filesystem and permissions before doing cache
   lookups. On a verified miss, download and publish using `build_cache.py`;
   the workflow owns bounded download/checksum retries and selection artifact
   recovery. Bind consumers to the same attempt's selection and to trusted
   producer metadata. Preserve any publisher lock file until no consumer can
   still use the run cache; use a shared UID or a tested shared-write policy.
3. **Allocate.** Use the generated resource matrix, one allocation per group,
   independent failure handling, and a distinct results directory per group.
   Install/source a compatible SDK and MPI on all workers as appropriate. For
   MPI, use cluster-run master mode and the scheduler-supplied hostfile, no
   independent SSH configuration. Pass the actual routed interface (not a
   hardcoded source interface). Apply the cluster's required network/device
   settings, and configure the worker runtime in `setup_command` as well as in
   the master environment. All ranks must resolve the staged binary and their
   working directory at the same absolute path. Use a writable shared scratch
   root for `--work-root` and a fresh results path for each attempt.
4. **Run.** `run_cases.py --group SELECTION/GROUP --cache CACHE --identity BUILD_JSON
   --output RESULTS/GROUP --work-root SCRATCH` invokes one executable per
   selected Case. Supply `--hostfile` and `--mpi-interface` only for MPI groups.
   The executor retains results even for benchmark failures; the reconciler is
   responsible for the overall conclusion. A launcher failure or timeout is
   conservative: later MPI Cases are recorded as not run until remote cleanup
   can be verified. A benchmark-reported failure with a clean launcher exit does
   not stop later Cases.
5. **Reconcile and report.** Run `report.py --selection SELECTION --results RESULTS
   --identity BUILD_JSON --output REPORTS --excel` even after a test failure.
   Final status requires the expected Case keys and the same build identity.
   A waiver is not a pass. Matrix cells retain their test, row, column, unit and
   value; no throughput or variability threshold is used. Keep NAS raw logs
   unchanged. Review logs, diagnostics and platform metadata before publishing
   to public artifacts; do not upload the run directory wholesale. Use a
   90-day retention policy for approved artifacts where allowed. Reconcile
   missing groups explicitly rather than assuming the summary always runs when
   the workflow is forcibly cancelled.

Keep this workflow's conclusion independent of PR-required checks. Before
scheduling, validate the three allocations, SDK/driver compatibility, Vpipe
support, MPI launcher and data path, cross-worker NAS access, retry on a real
cache miss, and actual per-platform Excel measurements.

## Branch-only hardware smoke experiment

The temporary workflow tests exactly 11 cases across the three allocations above.
It builds both executables on a CPU runner using the same image digest as the
workers, uploads the build once, and stages one verified copy on NAS. The MPI
workers must match the Open MPI 4 package versions of the build. `cluster-run`
provides the hostfile and passwordless launcher; no separate SSH setup is needed.
Both MPI workers install the configured HPN SDK (2.1.3 by default). The
shared MPI launcher starts remote daemons with `HWLOC_COMPONENTS=-gl`;
forwarding the setting to ranks alone would not cover daemon startup.
Neither setting changes CPU builds or single-node benchmark launches.

Before starting the experiment, configure the repository variables
`SAILBANDWIDTH_NAS_CONTROLLER_MOUNT`, `SAILBANDWIDTH_NAS_WORKER_MOUNT`, and
`SAILBANDWIDTH_NAS_SUBDIR`. Both mounts must refer to the same storage, and the
named subdirectory must already exist. Set it to a dedicated path relative to
both mount points. Slash-separated paths are supported, but empty components,
absolute paths, `.`/`..`, and symlinks are not allowed. The CPU staging runner
must be able to create a run directory there. Do not make the parent directory
world-writable to work around runner identity or NAS permissions.

Staging writes a per-run challenge, and each allocated PPU worker reads it and
writes its response before any benchmark starts. The CPU report job verifies
all responses, including both workers in the MPI group. This checks shared
read/write access without allocating a separate PPU job. The result path is
scoped by run ID and attempt; rerun all jobs rather than only the failed jobs.

Two Excel reports, the per-case data and the raw logs stay in the run-owned NAS
folder. Only status totals (plus the separate build artifact) are uploaded to
GitHub. Missing or waived cases do not count as passing; a cancelled workflow may
not produce a report. The experiment has no schedule and does not replace check-in.

### One-allocation MPI diagnosis on the experiment branch

Set `SMOKE_MPI_DIAGNOSTIC: '1'` in the experiment workflow to select only the
reviewed 890P same-rack MPI group. Neither single-node allocation nor the usual
11-Case reconciliation runs in this mode. Use `'0'` for normal smoke. The build
includes a small MPI probe in the same SHA-256-verified archive as the benchmark
only in diagnostic mode.

Before the MPI probe, `setup_command` runs the same HPN installer used by
regular two-node smoke. Its default URL selects the same HPN SDK 2.1.3
build as the successful PCCL control. `HPN_SDK_URL` can override it in the worker
setup environment; an explicitly empty value keeps the image's SDK. A nonempty
URL must point to a public HTTPS `.bin` on `zone-ci.oss-cn-hangzhou.aliyuncs.com`
and cannot include credentials, a query, or a fragment. Redirects are rejected.
Downloads have bounded retries, and a failed download or install stops setup.
Installer output stays in the run-owned worker setup log on NAS. The CPU build
and single-node smoke do not install HPN. This experiment still uses the same
Ubuntu Open MPI package on the CPU build and the workers.

After both workers' NAS responses are verified, the master runs these stages
using the scheduler hostfile and the smoke launcher:

| Stage | Budget |
| --- | ---: |
| Direct `cluster-ssh` hostname and runtime lookup, once per allocated worker | 30 s per worker |
| One `hostname` process per worker through `mpirun` | 60 s |
| One rank per worker: `MPI_Init`, barrier and finalize | 60 s |
| CE write, one PPU per worker, 16 MiB, one copy iteration, both directions | 90 s |
| Original 16-rank CE write with unchanged benchmark arguments, all 240 pairs | 600 s |

The first failure stops the sequence. A local timeout does not prove remote
ranks exited; the allocation is not reused after a failed stage. No normal Case
timeout, release workflow, or check-in workflow is changed.

Keep stage stdout and stderr under the run-owned NAS `logs` directory. Worker
setup logs record the default-route interface and MPI resolution both before and
after prepending `/usr/bin`. The direct SSH probe records the remote session's
hwloc setting, MPI executable paths and version. Open MPI 4 starts remote daemons
as `env HWLOC_COMPONENTS=-gl orted` in both smoke modes, so the GL plugin is
excluded before daemon initialization even if the SSH session did not inherit
the master environment. The MPI package, scheduler configuration, and hostname
compression are unchanged. The master records hostfile names and DNS lookups on
NAS; `mpirun hostname` captures launcher and control-plane details on stderr. A scoped
`SAILBANDWIDTH_MPI_PROGRESS=1` on the diagnostic benchmark ranks flushes MPI
readiness and pair counts to stderr; it adds no messages to `-F perf` and does
not log inside the measured copy. Do not use diagnostic measurements for
performance comparisons. The diagnostic job only passes after the expected
measurements include both directed small-pair cells and all 240 full-run cells.
