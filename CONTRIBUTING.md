# Contributing to sailbandwidth

This document describes how to contribute to sailbandwidth: the scope of accepted contributions, the development workflow, the coding and commit conventions, and the pull request and review process.

## Before You Start

- **Open an issue first.** GitHub issues are the only channel for sailbandwidth task management and bug tracking, and every pull request must reference at least one open issue with `Closes #<issue>` or `Refs #<issue>`. When no open issue covers the feature you want or the problem you are fixing, create one before opening the pull request, following the [issue guide](.agents/skills/sailbandwidth-issue/SKILL.md). For new testcases, new options, changes to the measurement methodology, and other significant changes, the approach is agreed on in the issue before implementation starts.
- **Check existing issues and pull requests** to avoid duplicate work.
- **License**: Contributions are accepted under the Apache License, Version 2.0. Existing copyright and third-party notices are preserved. See [LICENSE](LICENSE) and [Licenses.txt](Licenses.txt).

## Areas of Contribution

- **Bug fixes**: Reproducible defects with a clear correction.
- **Performance improvements**: Changes supported by before-and-after measurements.
- **Platform support**: Support for additional Zhenwu PPU models, link topologies, and T-Head SAIL SDK versions.
- **Testcases and diagnostics**: New measurement patterns and improvements to data verification and fault diagnosis.
- **Documentation**: Improvements to the README, code comments, and examples.

## Changes Requiring Discussion

The following changes are discussed in an issue before a pull request is opened:

- **Measurement methodology**: Changes to timing, copy size, bandwidth calculation, or data verification. These changes require before-and-after measurements and a description of their effect on the reported results.
- **Output contracts**: Changes to testcase keys, text labels, matrix orientation, JSON fields, option names, or default values. These changes affect downstream parsers and require a compatibility plan.
- **Platform-specific code**: Code that applies only to a specific PPU model, topology, or SDK version. Such code requires capability detection through `filter()` or an established fallback.
- **Workarounds for external issues**: Changes that work around SDK or driver behavior. These changes require a description of the root cause and the affected versions.
- **Incomplete implementations**: Partially supported features remain unregistered or filtered until they are complete.
- **Check-in configuration**: The check-in workflow and checker files under `.github/workflows/` are not changed unless necessary. Such a change requires maintainer confirmation before it is proposed; whether these files may change is a maintainer review decision and is not enforced by the prechecks.
- **Version changes**: The version defined by `project(... VERSION ...)` in `CMakeLists.txt` is upgraded only by maintainers as part of a release. A pull request by a non-maintainer does not change the version; this restriction is review guidance and is not enforced by the prechecks.

## New Testcases and Features

1. **Open an issue** that describes the measurement and its use case.
2. **Agree on the approach** with the maintainers in the issue.
3. **Document the design**: the measured operation, the copy path, the timing method, the expected output, the validation plan, and the expected performance impact.
4. **Implement the change** after the design is accepted.
5. **Open a pull request** that references the issue.

## Development Environment

Install the requirements and build sailbandwidth as described in the Requirements and Build sections of [README.md](README.md).

## Branch Model

| Branch | Purpose |
|---|---|
| `main` | Default development branch |
| `release/v<major>.<minor>.<patch>` | Protected release branch named after its release version, for example `release/v2.2.1` |
| `<type>/<short-description>` | Working branch for a single change; the type is `feat`, `fix`, `hotfix`, `refactor`, or `docs` |

- `main` and the release branches are protected. They receive changes only through pull requests; direct pushes and force pushes are rejected.
- A feature, fix, or other product change that belongs in a release branch should normally be merged into `main` first and then proposed to the release branch. Release-only maintenance, such as a version update or pipeline adjustment, may target the release branch directly.
- Working branch names consist of lowercase words separated by hyphens, for example `fix/invalid-loop-count`.

## Contribution Workflow

The commands in this section use the remote name `upstream` for https://github.com/t-head/sailbandwidth and `origin` for the remote that hosts the working branch. Contributors without write access work in a fork; when the working branch is created in the repository itself, both names refer to the same repository.

1. **Clone the repository**:

   ```bash
   git clone https://github.com/<account>/sailbandwidth.git
   cd sailbandwidth
   git remote add upstream https://github.com/t-head/sailbandwidth.git
   ```

2. **Create a working branch** from the latest `main`:

   ```bash
   git fetch upstream
   git switch -c fix/invalid-loop-count upstream/main
   ```

3. **Implement and validate the change** according to the [coding standards](.agents/skills/sailbandwidth-coding-standards/SKILL.md), including the validation required for the type of change.

4. **Stage and review the change** by path:

   ```bash
   git add src/sailbandwidth.cpp
   git diff --cached --check
   git diff --cached
   ```

   The staged change contains no unrelated files, build outputs, verification dumps (`sailbw_dump_*.txt`), credentials, or internal addresses.

5. **Commit the change** as a single signed-off commit whose message follows the [commit message guide](.agents/skills/sailbandwidth-commit-message/SKILL.md):

   ```bash
   git commit -s
   ```

6. **Push the working branch** and open a pull request against `main`:

   ```bash
   git push -u origin fix/invalid-loop-count
   ```

## Single-Commit Development

Each pull request contains exactly one commit. Its commit message, with the pull request number appended to the title, becomes the permanent record in the target branch history. The commands below use `main` as the target branch; for a pull request that targets a release branch, the release branch replaces `main`.

- Changes requested during review are applied by amending the commit:

  ```bash
  git commit --amend -s
  git push --force-with-lease origin fix/invalid-loop-count
  ```

- When `main` advances, rebasing the working branch onto it is recommended; being behind the target is not itself a reason to reject a pull request under this policy:

  ```bash
  git fetch upstream
  git rebase upstream/main
  git push --force-with-lease origin fix/invalid-loop-count
  ```

- Every rewrite of a pushed working branch uses `--force-with-lease`; `git push --force` is not permitted.
- Merge commits are not used in working branches, including merge commits created when a branch is updated on GitHub.
- Before each push, the following command prints `1`:

  ```bash
  git rev-list --count upstream/main..HEAD
  ```

- After each amendment, the commit message and the pull request description are revised to describe the complete change.

## Resolving Conflicts

- Conflicts are resolved in the working branch during the rebase, never in a protected branch.
- Changes made by other contributors are kept or modified only after confirmation with their authors.
- After the conflicts are resolved, the validation of the affected configurations is repeated.

## Coding Standards

All changes follow the [sailbandwidth coding standards](.agents/skills/sailbandwidth-coding-standards/SKILL.md), which define the requirements for repository and build integration, licensing and provenance, style and documentation, correctness and error handling, benchmark integrity, and interfaces and output, together with the validation required for each type of change.

## Commit Messages

Commit messages follow the [commit message guide](.agents/skills/sailbandwidth-commit-message/SKILL.md). A single precheck verifies both the commit message format and the DCO sign-off; a commit is accepted only when it passes.

### Signed Commits

All commits must be signed off to certify the contribution under the Developer Certificate of Origin, Version 1.1:

```bash
git commit -s
```

This adds: `Signed-off-by: Your Name <your@email.com>`

The precheck rejects every commit whose message does not end with a valid `Signed-off-by: Name <email>` trailer. The email may be a private or anonymous address and is not required to match the commit author; committing with `-s` is the recommended way to add the trailer.

By adding this trailer, the contributor certifies agreement with the Developer Certificate of Origin, Version 1.1:

```text
Developer Certificate of Origin
Version 1.1

Copyright (C) 2004, 2006 The Linux Foundation and its contributors.

Everyone is permitted to copy and distribute verbatim copies of this
license document, but changing it is not allowed.


Developer's Certificate of Origin 1.1

By making a contribution to this project, I certify that:

(a) The contribution was created in whole or in part by me and I
    have the right to submit it under the open source license
    indicated in the file; or

(b) The contribution is based upon previous work that, to the best
    of my knowledge, is covered under an appropriate open source
    license and I have the right under that license to submit that
    work with modifications, whether created in whole or in part
    by me, under the same open source license (unless I am
    permitted to submit under a different license), as indicated
    in the file; or

(c) The contribution was provided directly to me by some other
    person who certified (a), (b) or (c) and I have not modified
    it.

(d) I understand and agree that this project and the contribution
    are public and that a record of the contribution (including all
    personal information I submit with it, including my sign-off) is
    maintained indefinitely and may be redistributed consistent with
    this project or the open source license(s) involved.
```

## Pull Requests

- Pull requests meet the [pull request rules](.agents/skills/sailbandwidth-pull-request/SKILL.md), which define the branches, the title, the description sections, and the merge conditions. After [the pull request template](.github/pull_request_template.md) is present on the default branch, GitHub automatically adds its sections to new pull requests; contributors must replace the placeholders and must not leave the default commit text as the complete description.
- A pull request is opened as a draft until its validation is complete and is then marked ready for review.
- Changes in a pull request must not introduce new compiler warnings.
- A pull request that affects performance, including a change with the commit type `perf`, must include a before-and-after comparison of the results of all testcases on at least one Zhenwu M890 system. The comparison is attached to the associated issue as a table. When Zhenwu 810 or Zhenwu 810E systems are available, the corresponding results from those systems are attached as well.
- A pull request remains focused on one change. When a change contains independently reviewable parts, it should be split into independently verifiable pull requests; no fixed line-count limit applies.
- Changes to measurement paths should be approved by two reviewers.
- Every Copilot review comment is handled before merge and is explicitly classified as `addressed`, `won't fix`, or `incorrect`. An `addressed` response identifies the corresponding change; a `won't fix` or `incorrect` response provides the rationale. The conversation is resolved only after the classification and response are recorded.

## Local Check-in Validation

Use a Python 3.10 or newer virtual environment. The pinned dependencies below are only for the PR checker and its regression tests, not for building or running the benchmark.

```bash
python3 -m pip install -r .github/workflows/scripts/requirements-checks.txt
python3 -B -m unittest discover -s .github/workflows/scripts -p 'test_check_in.py' -v
python3 -B .github/workflows/scripts/check_commit_message.py --commit HEAD
```

Save the proposed description as `pr-body.txt`, replace the placeholders below, and use the PR's base repository for `OWNER/REPO`, not a contributor's fork. Online validation requires the GitHub CLI authenticated with read access to that repository's issues.

```bash
python3 -B .github/workflows/scripts/check_pull_request.py \
  --title '<PR title>' --body-file pr-body.txt \
  --head-ref '<working branch>' --base-ref main --head HEAD --repo OWNER/REPO
```

The PR checker parses CommonMark structure so that code examples do not become headings, issue references, or selected change types. With `--event`, CI always verifies the issue numbers from `Related issue` against the base repository at check time. With `--repo`, local validation does the same. Without `--repo`, the local command reports format checks only, not verification of issue state. Failed API requests fail online validation; an open pull request is not accepted as an issue.

## Code Review Process

1. **Automated checks**: Independent prechecks verify the commit message and its DCO sign-off against the [commit message guide](.agents/skills/sailbandwidth-commit-message/SKILL.md), and the [pull request rules](.agents/skills/sailbandwidth-pull-request/SKILL.md). Copilot applies the repository-wide [AI review instructions](.github/copilot-instructions.md), the nearest [agent instructions](AGENTS.md), and relevant Agent Skills.
2. **Maintainer review**: Maintainers review correctness, measurement validity, and compatibility. Before merging, they confirm that the approval and completed Copilot review apply to the current head and that review discussions are resolved. An older approval, a historical bot review, or a successful gate alone is not sufficient.
3. **Iteration**: Feedback is handled with the required classification, and code changes are applied by amending the commit. When a review-wait job times out or is cancelled, the existing workflow is rerun after the review is ready; the commit is not amended merely to restart it. A skipped PPU job is not evidence of a successful hardware test.
4. **Merge**: After approval and passing checks, a maintainer merges the pull request with Squash and merge.

## After Merge

```bash
git switch main
git pull --ff-only upstream main
git branch -D fix/invalid-loop-count
git push origin --delete fix/invalid-loop-count
```

Squash and merge creates a new commit, so Git may report that the working branch is not fully merged. After the merge is confirmed on GitHub, the local branch is deleted with `git branch -D`.

## Agent-Assisted Development

AI coding agents follow [AGENTS.md](AGENTS.md) in addition to this document. An agent that opens or updates a pull request writes the complete description through the available GitHub tool or API and runs the local pull request checker; it does not require a maintainer to copy a generated description when an authenticated tool is available.

## Communication

- **GitHub Issues**: The only channel for task management and bug tracking, including bug reports and feature requests. Every pull request references an open issue.
- **Pull Requests**: Code contributions and review discussions.
