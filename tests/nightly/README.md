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

## Local validation

From the repository root, using Python 3.10 or newer:

```sh
python3 -S -B -m unittest discover -s tests/nightly -v
python3 -S -B tests/nightly/cases.py --dataset smoke --output "$RUN_DIR/selection"
```

The tooling is standard-library-only except optional Excel reporting
(`XlsxWriter==3.2.9` on the report runner). No benchmark execution dependency is
added. The unit tests exercise real local subprocesses with fixture binaries;
they do not validate the Sail SDK, hardware or MPI transport.

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
