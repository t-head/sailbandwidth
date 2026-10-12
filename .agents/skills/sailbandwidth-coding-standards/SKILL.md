---
name: sailbandwidth-coding-standards
description: Applies the stable coding, compatibility, benchmark-integrity, licensing, and validation requirements of sailbandwidth. Use before writing, modifying, or reviewing source code, build files, scripts, or documentation in this repository.
---

# sailbandwidth Coding Standards

## Scope

These standards apply to every change in this repository. Requirements stated with *must* are mandatory; guidance stated with *should* is recommended.

Follow the style and abstractions of the surrounding sailbandwidth code. When the implementation is derived from nvbandwidth, preserve upstream-compatible structure and behavior unless the change intentionally introduces and documents a divergence. Do not reformat or refactor unrelated code.

## Repository and Build Integration

- Keep implementation, declarations, build definitions, tests, and documentation consistent with one another.
- Add new source files and dependencies to the established CMake build. Do not introduce a dependency when the existing standard library or project infrastructure is sufficient.
- Preserve the supported single-node and multinode build modes. Platform-specific behavior must be capability-gated and must not break other supported configurations.
- Build and shell changes must preserve the currently supported tool and shell environments, quote external input, and avoid unsafe command evaluation.
- Bundled third-party code must be updated only as a complete, attributable upstream unit; avoid local behavioral changes in bundled sources.

## Licensing and Provenance

- New source, build, and script files must use the established Apache-2.0 header appropriate to their provenance.
- Files derived from nvbandwidth must retain the applicable upstream copyright and license notices and the repository's modification notice.
- `LICENSE` must remain the unmodified Apache-2.0 license. Required third-party notices in `Licenses.txt` and bundled sources must remain complete.
- Public files must not contain credentials, tokens, internal links, private addresses, private paths, or inaccessible dependencies.

## Style and Documentation

- Match the formatting, naming, file organization, and language conventions of the surrounding code.
- Use clear names that distinguish units, counts, indexes, ranks, devices, and ownership where those distinctions affect correctness.
- Comments explain intent, invariants, ownership, synchronization, and compatibility constraints rather than restating the code.
- Source comments and public documentation use professional English and are updated with the behavior they describe.
- Do not combine functional changes with unrelated formatting or cleanup.

## Correctness and Error Handling

- Use the established error-checking and reporting interfaces for runtime, driver, system, and multinode operations. Check every relevant return value.
- Preserve the original error and enough device, host, and rank context to diagnose a failure.
- Release resources on normal and partial-failure paths. Objects and buffers must outlive asynchronous operations that use them.
- Validate user input, sizes, counts, offsets, arithmetic, and optional interfaces before use. Reject invalid input before it can affect device initialization or measurement.
- Unsupported hardware or capabilities must be skipped through the established capability-filtering mechanism rather than failing after a testcase starts.

## Benchmark Integrity

- A result must describe the operation that was actually executed and the bytes that were actually transferred.
- Timing boundaries, synchronization, transfer direction, execution context, and bandwidth or latency calculations must remain internally consistent.
- Do not add allocation, initialization, verification, diagnostics, output formatting, or unrelated synchronization to a measured interval unless the metric explicitly includes it.
- Keep automatically derived or testcase-specific state local so that one testcase cannot change the behavior of a later testcase.
- Data verification must remain enabled by default and must complete after the measured work.
- Changes to a measurement path require before-and-after evidence appropriate to the affected hardware and execution modes.

## Interfaces and Output

- Treat CLI options, testcase identifiers, default values, text output, JSON fields, and environment variables as compatibility-sensitive interfaces.
- An intentional interface change must include validation, documentation, and a compatibility statement.
- Text and structured output must describe the same testcase, status, values, and errors.
- New testcases and options must be discoverable through the established registration and help mechanisms and documented in `README.md`.
- Multinode paths must preserve a consistent collective order and failure path across ranks so that no rank waits indefinitely.

## Validation

Validation must be proportional to the change:

| Change | Required validation |
|---|---|
| Source or build logic | Build every affected supported configuration with no new warnings |
| Testcase or data path | Run the affected testcase with data verification and relevant boundary cases |
| CLI or output contract | Check valid and invalid input, help or listing output, and text/JSON consistency |
| Performance-sensitive path | Compare before and after results for the isolated testcase and the relevant full sequence |
| Multinode behavior | Build and run the affected multinode path when the environment is available |
| Documentation or licensing | Check commands, links, headers, notices, formatting, and public-repository safety |

Never report a build, test, platform, or performance result that was not obtained. State unavailable hardware or multinode validation explicitly in the pull request description.
