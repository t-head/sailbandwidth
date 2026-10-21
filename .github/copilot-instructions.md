# sailbandwidth Code Review Instructions

Review every pull request in this repository according to the following rules.

## Review scope

- For the first review, review the pull request diff and the code paths that it affects. For a subsequent push, review only the incremental diff from the last reviewed head SHA to the current head SHA, including metadata changes relevant to that diff.
- Before a subsequent review, identify both head SHAs and read the existing review threads and their classifications. After amend, force-push, or rebase, compare the two final trees rather than treating the entire base-to-head patch as new work.
- Do not repeat a finding already classified as `addressed`, `won't fix`, or `incorrect` unless the incremental change invalidates its recorded resolution. Report a new finding on unchanged code only when the incremental change makes it reachable, worsens it, or depends on it.
- If the previous reviewed revision or review history is unavailable, state that incremental review could not be verified and request that context; do not silently claim an incremental review or restart a full review.
- Read each incrementally changed file in full for context and trace the referenced definitions before a finding is reported; reading context does not expand the finding scope.
- Unrelated existing code is not reported unless the change makes it reachable, worsens it, or depends on it.
- The `main` branch and maintained release branches are protected branches. The review covers the code and metadata proposed in the pull request.

## Review procedure

- Identify the review baseline and current head SHA, and read the actual diff for the scope above. Recheck existing unresolved findings affected by that diff.
- Read the changed files in full and inspect the affected declarations, callers, tests, build files, and documentation.
- Evaluate kernel launch, synchronization, and memory-ordering behavior against the HGGC and PPU implementation rather than assumed CUDA behavior.
- Verify that the commit message and the pull request description accurately describe the diff, including the stated tests and compatibility impact.
- Classify findings as Blocker, Major, or Normal, in that order.
- Each finding includes the severity and a concise title, the exact `file:line` evidence, the concrete failure mode and the affected configuration, the reason the change causes the failure, a bounded correction, and the required validation.
- A runtime failure is reported only when a reachable code path exists. An uncertain issue is classified as Normal, and the author is asked for evidence.
- When no findings exist, the review states so explicitly and lists validation gaps separately.

## Severity model

- Blocker (merge is not permitted) applies only when the change can cause wrong benchmark results, silent data corruption, memory safety violations, hangs, security or license violations, build failure in a supported configuration, or public disclosure of internal information, or when the commit message misdescribes the change.
- Major (correction is required) applies when the change causes a material regression, a compatibility break, a resource leak, a misleading diagnostic, incomplete integration, or insufficient validation of a high-risk path, or when the pull request description misdescribes the change. A Major finding remains unresolved at merge only with a documented maintainer decision.
- Normal (the author provides an explanation) applies to maintainability, clarity, weak evidence, minor inconsistency, or behavior that may be intentional but requires an explicit rationale. Style preferences are not classified as Major.
- When findings are posted as pull request comments, Blocker and Major findings are prefixed with `Required:`, Normal findings with `Question:`, and optional improvements with `Suggestion:`.

## Accepted project conventions (not reported as findings)

- The `.cu` and `.cuh` extensions belong to the PPU hgcc build and do not indicate a CUDA runtime dependency.
- The NVIDIA copyright notices and the statement in the README Copyright section that sailbandwidth is derived from nvbandwidth are required attribution.
- `cu_mask` and related dispatch-mask names refer to PPU Compute Unit and EIT concepts unless code evidence shows otherwise.
- Command-line parsing uses the unmodified header-only argparse library bundled under `argparse/`; its MIT license notice is retained in `Licenses.txt`. Boost is not a build dependency.
- The D2D Read and Write arrows follow the AccessType display convention inherited from nvbandwidth, which is retained for output compatibility. Only regressions, internally inconsistent new labels, or undocumented contract changes are reported.
- Definitions for unsupported multicast and bisect paths are not findings while they remain unregistered or filtered and are not exposed as supported.
- Hidden diagnostic options are not required to be listed among the common README options.
- GitHub Actions branch filters use workflow glob syntax. In the release-branch filter `release/v[0-9]+.[0-9]+.[0-9]+`, `+` means one or more occurrences of the preceding digit range; this pattern intentionally matches names such as `release/v2.2.1`.
- `AGENTS.md` and `.agents/skills/<skill-name>/SKILL.md` use GitHub's standard agent instruction and Agent Skills paths. `copilot-instructions.md` supplies review-wide guidance; the skills supply task-specific guidance.
- The prechecks run the checkers from the pull request working tree. Whether a pull request may modify a workflow or a checker is a maintainer review decision, not a process-enforced gate; do not report the absence of tamper-proofing as a finding. Review a workflow or checker change for correctness like any other diff.
- The DCO sign-off requires a trailing `Signed-off-by: Name <email>` trailer whose name contains no angle brackets and whose email has exactly one `@`. The email is intentionally permissive so that anonymous and no-reply addresses are accepted; it is not matched against the commit author and not validated against strict RFC dot-atom rules. Report only a structurally malformed trailer, not a permissive but valid address.

## Lessons from prior reviews

These cases define the intended checker behavior, not exemptions for regressions. Before reporting a similar issue, verify the current implementation and demonstrate the failing input; group related boundary cases in one finding rather than rediscovering them over successive reviews.

- A key-feature list starts with `- ` (hyphen-space); a lone `-` is an empty item. Prose beginning with `--option` or `-1` is not a list and is not checked by requirement 7.
- Issue-reference candidates occupy the complete line. Prose such as `Fix issue 12 by ...` is not a footer reference; standalone `Closes issue 12` and `Closes #abc` are malformed references. A reference belongs in the final contiguous footer, not before later prose. The footer must contain a reference; a sign-off-only footer is rejected as a missing reference.
- Issue numbers are accepted only as ASCII digits; a reference candidate containing a non-ASCII digit such as an Arabic-Indic numeral is still detected and reported as malformed rather than silently treated as prose or converted to the corresponding ASCII issue.
- A typed commit title's colon may be followed by one or more spaces; extra whitespace does not bypass the subject, scope, or non-descriptive-title checks.
- A sign-off alone is not a commit title. DCO identities must come from the contiguous trailing sign-off block; an author's sign-off elsewhere in the body does not satisfy DCO.
- `WIP` is a standalone word or prefix form (`wip:`, `wip-`, `wip_`), not a substring of valid subjects such as `wipe stale cache`.
- Version comparison uses the complete version-like token, including prerelease/build suffixes and extra numeric components. A sentence-ending period is punctuation.
- PR descriptions contain each required heading exactly once. Change types use the supported labels and the exact commit-type mapping, while an unprefixed title does not impose a type mapping.
- Markdown checks operate on parsed structure. Fenced and indented code blocks, multiline code spans, and comments must not supply headings or selected checkboxes; literal comment markers inside code must not hide later sections. Cover these cases together in the checker regression suite.
- Online issue validation uses only plain references in `Related issue`, queries the PR's base repository, and excludes pull request objects. An open issue mentioned elsewhere does not satisfy the requirement; API failures must not be reported as a successful lookup.
- Local fork validation prefers the fetched `upstream` target over `origin`; Actions can use `origin`. Explicit `--base` takes precedence.
- Detailed policy lives in the task-specific skills; CONTRIBUTING.md summarizes the workflow. Do not report consistent summaries as conflicting policy or request duplicate rule files.

## Blocker: Benchmark and data correctness

- Reject any path that can report bandwidth or latency for bytes that were not transferred.
- Validate every size, count, offset, stride, chunk, and loop calculation for overflow, truncation, division by zero, omitted tails, overlap, and out-of-bounds access.
- `bufferSize`, `testSamples`, and `loopCount` reject non-positive input before conversion to unsigned values.
- `nBlocks` and `nThreads` accept only `-1` for automatic mode or positive values. Zero and values below `-1` are rejected before device initialization.
- Bandwidth division requires a positive elapsed time; an invalid time results in an error instead of `NaN` or `Inf`.
- Custom launch geometry uses the actual `blocks * threads` in both the kernel and the adjusted byte calculation. A fixed count of 512 threads is never assumed when a user value is accepted.
- Automatically derived launch geometry remains local to the operation or testcase. It is never written back to global CLI state and never affects later testcases.
- A read testcase executes from the context that performs the remote read. Aggregate SM read paths preserve destination-context execution.
- Changes to copy direction, context preference, buffer ordering, or `BandwidthValue` are checked together; matching labels alone do not prove correct traffic.
- Default data verification is not silently bypassed, narrowed, or moved before the completion of the measured copy operations.

## Blocker: Memory, resources, and asynchronous lifetime

- Reject use-after-free, double free, double release, mismatched allocation and deallocation interfaces, and resource leaks that can exhaust resources or corrupt a supported benchmark run.
- Streams, events, contexts, allocations, mappings, registrations, and MPI-owned resources are released on normal and partial-failure paths.
- Host objects and buffers outlive queued CE and SM work and all synchronization points that consume them.
- Stack objects, temporary vectors, and references do not escape into asynchronous execution.
- Peer access and topology reachability are checked; unsupported paths are waived through `filter()` or an established fallback and are not executed optimistically.

## Blocker: Driver table and ABI safety

- Versioned EIT fields are not evaluated before `struct_size` proves that the member is present. A member pointer is passed and dereferenced only after the size check.
- Reject reads of optional function pointers or fields from shorter driver tables.
- Public structures, enum values, exported signatures, JSON fields, and environment variable semantics do not change incompatibly without an explicit compatibility plan.

## Blocker: MPI and hang safety

- All ranks enter collective barriers, waits, and shutdown paths consistently.
- Fatal multinode errors converge through the existing abort mechanism so that no rank waits indefinitely.
- Reject rank-dependent branches that change the number or order of operations without a proven protocol.
- Rank, local device, hostname, and BDF attribution remain correct in diagnostics and JSON output.
- A timeout is not treated as a correctness mechanism.

## Blocker: Testcase integration and output validity

- Every testcase key is unique.
- A new testcase is declared, implemented, registered in the active factory, filtered for unsupported hardware, and selectable through the intended CLI path.
- Unsupported capabilities produce Waived or skipped results through the existing filter mechanism rather than a misleading runtime error.
- JSON output uses the canonical nested path under the sailbandwidth title and the testcase array. Reject malformed or unparsable JSON.
- Text and JSON results describe the same testcase, status, matrix values, sum, and errors.

## Blocker: Build and CLI security

- Reject shell command injection, unsafe `eval`, unintended expansion of command substitutions, backticks, or variables, and argument parsing that corrupts quotes or whitespace.
- The self-contained Bash argument parser preserves the supported Python argparse behavior: unique long-option prefixes, ambiguity rejection, `--opt=value`, short aliases and groups, integer normalization, choices, environment-based defaults, help, and deferred unknown-argument errors.
- Bash 3.2 compatibility is preserved; associative arrays and other unsupported syntax are not introduced.
- A supported build configuration does not lose source files, include paths, libraries, compile definitions, or executable permissions.
- Reject hard-coded credentials, tokens, private keys, internal repositories, private absolute paths, and insecure download bypasses.

## Blocker: License and public-repository safety

- `LICENSE` remains the unmodified Apache-2.0 text.
- Derived source files retain the applicable NVIDIA copyright notice and the modification copyright notice; original files use the existing Apache-2.0 header of the repository.
- The bundled JsonCpp notices and license text remain complete in `Licenses.txt` and in the bundled source.
- Reject internal CI configuration, internal code review links, internal issue identifiers, internal download endpoints, secrets, build artifacts, temporary files, and inaccessible submodule or LFS objects.
- The README retains the Copyright section: the upstream derivation statement, the NVIDIA copyright, the modification copyright, and the references to the Apache-2.0 license and third-party notices.

## Blocker: CI gate integrity

- A new testcase command added to `.github/workflows/scripts/casecmd.list` runs inside the `build-test` job and is covered by `sailbandwidth Tests Gate` automatically; a new top-level workflow job is added to `tests-gate.needs`.
- A required job failure, cancellation, or an unexpected skip on a pull request fails `sailbandwidth Tests Gate`; a green gate beside a needed job that did not succeed is a defect.
- `build-test` runs on every event, so the gate requires it to succeed unconditionally. A job that expands or runs only on `pull_request` events — for example a dynamic test matrix — is expected to be skipped on `push` and `workflow_dispatch`; such a job is required with the `pull_request`-guarded form in the gate, not the unconditional list, and its expected non-PR skip is not reported as a gate defect.
- Changes to CI routing or gate rules are verified against both passing and failing cases: a clean run, a failed job, and the PR-only jobs on pull_request versus push events.

## Major: Performance validity and regression risk

- Report new synchronization, allocation, context switching, copies, logging, or driver queries in the measured hot path.
- Require before-and-after evidence for changes to CE or SM kernels, cache or route policy, context preference, launch geometry, timing events, copy size, loop structure, or aggregate bandwidth calculation.
- Compare a testcase run alone with the full testcase sequence when shared or global state may affect results.
- Distinguish a true kernel performance change from the correction of a previously invalid measurement, and require both isolated and sequence results.
- Require single-node and multinode evidence when shared code affects both modes.
- Verification, memset, diagnostics, and output formatting remain outside the timed interval unless the metric explicitly includes them.

## Major: Result semantics and compatibility

- Changes to text labels, matrix orientation, testcase keys, the JSON `bandwidth_description`, or field names can break downstream parsers and require a compatibility statement.
- Pairwise, bidirectional, aggregate, and latency formulas match the bytes and directions that are actually measured.
- `Total` follows the existing testcase contract; averaging or double counting is not introduced by inference.
- Diagnostics do not overstate root causes. `NOT-WR` proves only that the destination retained the prefill marker; it does not prove packet loss.
- Source and transfer attribution is valid only when the source and the verified destination are independently readable, locally owned or mapped, and distinct buffers. Otherwise only expected and actual values are reported.

## Major: Resource and error quality

- Report unbounded or per-iteration resource leaks even when they do not immediately corrupt results.
- New HG, HGGC, HGML, and MPI calls use the established assertion and error wrappers or explicitly handle recoverable failures.
- Text and JSON error paths preserve the original error and sufficient host, rank, and device context.

## Major: Build, documentation, and integration

- New source files are added to CMake; new options have defaults, range checks, help text, and documentation appropriate to their visibility.
- Build changes cover the normal, debug, stripped, MPI, and feature-gated configurations that the diff affects.
- README commands, testcase names, defaults, model names, links, JSON statements, and multinode instructions match the current code.
- Performance examples state their measurement context and do not imply simultaneous aggregate throughput when the cells were measured sequentially.
- Version strings come from `src/include/version.h` and are not duplicated in other files.

## Normal: Maintainability and consistency

- Request a rationale when ownership, context, direction, or synchronization invariants are not documented.
- Report stale or misleading comments, spelling errors, unclear byte and element naming, inconsistent terminology, and unexplained magic values.
- Request evidence for omitted tests; tests unrelated to the change are not required.
- Recommend splitting a pull request that mixes unrelated feature, refactoring, documentation, and formatting work or exceeds a reviewable size.
- Check formatting, include guards, executable bits, line endings, trailing whitespace, and consistency with the surrounding style.
- Public code and documentation use professional English.

## Validation matrix

- Validation is proportional to the diff.
- CLI and argument parsing: syntax check, comparison between Bash 3.2 and Bash 4.x, valid and invalid boundary cases, and help output.
- C++ and device code: the affected build configurations with no new warnings.
- CE and SM performance: A/B comparison of the isolated testcase and the full sequence, including custom geometry where relevant.
- Data verification: successful copy, intentional mismatch, dump format, attribution preconditions, and text and JSON behavior.
- MPI: at least two nodes, or a justified statement that the environment is unavailable; failure propagation is covered when it changes.
- Output: JSON parsing and comparison of testcase names, status, matrix, and sum with the text output.
- Documentation and license: link and command validation, SPDX and notice scan, and internal-information scan.
- Check-in scripts: run the offline checker regression suite, including Markdown structure, issue-state API error paths, commit format, and DCO; checking the current PR alone does not test the checkers.
- Runtime validation that was not executed is never claimed.

## Output format

- Findings are sorted by severity and impact in the sections Blocker, Major, and Normal, followed by the sections Validation gaps and Verdict. Empty severity sections are omitted.
- Each finding is written as `- [<severity>] <title> — path:line`, followed by the entries Impact, Evidence, Fix, and Validate.
- The Validation gaps section lists missing evidence and unavailable environments.
- The verdict is No-Go, Go after fixes, or Go. A Blocker always results in No-Go. A Major finding normally results in Go after fixes. A Normal finding requires an author response but does not block the merge by itself.
