---
name: sailbandwidth-commit-message
description: Write, amend, and check sailbandwidth commit messages, covering the mandatory format, the Signed-off-by trailer, recommended writing practices, and examples. Use before every commit or amend in the sailbandwidth repository.
---

# sailbandwidth Commit Message Guide

## Scope

This guide applies to every commit proposed for the `main` branch or a release branch of this repository.

Each pull request contains exactly one commit, and changes made during review are applied by amending that commit. Pull requests are merged with Squash and merge, and the resulting commit uses this commit message. The commit message is therefore the permanent record in the branch history. GitHub appends the pull request number to the title of the resulting commit.

A commit message that violates any mandatory requirement is rejected. The precheck checks the mandatory requirements that can be verified mechanically and reports violations by requirement number; the remaining requirements are verified during review.

## Format

```text
<type>(<scope>): <subject>

[optional concise body: a key feature list or prose paragraphs]

Closes #<issue> or Refs #<issue>
Signed-off-by: Your Name <your@email.com>
```

The type, the scope, and the body are optional. A scope, when present, contains one lowercase word. An agent-generated message should use a key feature list by default; concise prose paragraphs that explain the problem, solution, and relevant limitations are also accepted.

## Mandatory requirements

### Title

1. The title has the form `<subject>`, `<type>: <subject>`, or `<type>(<scope>): <subject>`.
2. The optional scope, when present, is one lowercase word containing only letters and digits.
3. The subject uses the imperative mood and does not end with a period.
4. The subject describes the change in behavior rather than the edited files. Non-descriptive subjects such as `update`, `fix bug`, `changes`, and `wip` are not permitted.

### Body

5. A blank line separates the title from the optional body or footer.
6. The body is optional. When present, it uses either a key feature list or concise prose paragraphs; the two forms are not mixed.
7. In a key feature list, each item occupies one line and starts with `- `, followed by a description that does not end with a period. Prose paragraphs use normal English sentence formatting.
8. After every amendment, the current commit message is reviewed and updated when necessary so that the title and body describe the complete change in its final state.

### Footer

9. A blank line separates the footer from the optional body. Within the footer, the issue reference is immediately followed by the `Signed-off-by` trailers on the next line, with no blank line between them.
10. The footer contains one issue reference of the form `Closes #<issue>` or `Refs #<issue>`, referencing an open issue of this repository.
11. The message ends with at least one trailer of the form `Signed-off-by: Name <email>`. Committing with `git commit -s` using the configured Git identity is the recommended way to add it. The email may be a private or anonymous address and is not required to match the commit author. By adding the trailer, the contributor certifies the contribution under the Developer Certificate of Origin, Version 1.1.

### Content

12. The message is written in English.
13. Every statement in the message is supported by the diff. The message does not claim tests or results that were not obtained.
14. A version number appears only when the version is part of the change and matches the `project(... VERSION ...)` value in `CMakeLists.txt`, prefixed with `v`.
15. The message contains no credentials, tokens, internal links, private paths, or internal issue identifiers. Email addresses appear only in `Signed-off-by` trailers.

Requirement 7 applies only when the body uses a key feature list.

## Recommendations

1. The title starts with one of the types `feat`, `fix`, `refactor`, `docs`, `test`, `chore`, and `perf`, selected according to the type table below. An agent that creates or amends a commit should include the type.
2. The subject and each key feature start with a lowercase letter. An agent that creates or amends a commit should use this form.
3. The complete commit message is as concise as possible and normally contains no more than 72 words. This is a recommendation, not a merge requirement.
4. An agent-generated body uses a key feature list by default and normally contains no more than three items. Concise prose paragraphs may be used when they communicate the problem, solution, and relevant limitations more clearly.
5. Each key feature starts with a verb in the base form, consistent with the imperative subject.
6. Key features are ordered by importance and describe effects rather than implementation steps.
7. A change to a public contract, such as a testcase key, an option, a default value, or a JSON field, is stated in a key feature.
8. The scope is omitted when the change spans several areas.
9. Use `Closes #<issue>` when merging the pull request resolves the issue and `Refs #<issue>` when the issue remains open; the referenced issue is the same open issue the pull request description references.
10. Measured performance data are attached to the associated issue rather than included in the commit message.
11. The project version in `CMakeLists.txt` is upgraded only by maintainers as part of a release. A pull request by a non-maintainer does not change the version; this restriction is review guidance and is not enforced by the prechecks.

## Type and scope

| Change | Type |
|---|---|
| New testcase, option, or capability | `feat` |
| Corrected behavior, result, or diagnostic | `fix` |
| Measured performance improvement | `perf` |
| Restructured code without a change in behavior or results | `refactor` |
| Documentation only | `docs` |
| Tests only | `test` |
| Build files, dependencies, or repository configuration | `chore` |

| Scope | Area |
|---|---|
| `cli` | Command-line options and argument parsing |
| `ce` | Copy engine testcases and copy paths |
| `sm` | SM testcases and kernels |
| `multinode` | Multinode testcases and MPI integration |
| `verify` | Data verification and diagnostics |
| `output` | Text and JSON output |
| `build` | `CMakeLists.txt` and `build.sh` |
| `license` | License files and file headers |

## Writing procedure

1. Review the staged change with `git diff --cached`.
2. Determine the effect of the complete change, and choose the type and the scope.
3. Write the subject in the imperative mood.
4. Choose the body form. An agent normally writes a concise key feature list; use prose paragraphs when they explain the problem, solution, and relevant limitations more clearly.
5. Add the issue reference: `Closes #<issue>` when merging resolves the issue, otherwise `Refs #<issue>`, referencing the open issue behind the change.
6. Commit with `git commit -s`, which appends the `Signed-off-by` trailer for the configured Git identity and records the certification under the Developer Certificate of Origin, Version 1.1.
7. Check the message against the mandatory requirements and the recommendations.

## Amending

1. Apply the change and amend the commit with `git commit --amend -s`.
2. Review the current commit message after every amendment and update the title or body when necessary so that they describe the entire change rather than only the latest revision.
3. Confirm that the message contains exactly one `Signed-off-by` trailer for each identity and that the existing trailers of other contributors are preserved.
4. Confirm that the pull request title remains identical to the commit title.

## Examples

```text
fix(cli): reject invalid launch geometry

Zero launch dimensions previously reached device initialization and produced an invalid configuration.

Reject zero and values below -1 before device initialization. Automatic mode remains available through -1.

Closes #12
Signed-off-by: Your Name <your@email.com>
```

```text
docs: clarify the build guide

- describe the multinode build option
- correct the SDK download link

Refs #34
Signed-off-by: Your Name <your@email.com>
```

The following messages are rejected:

| Message or line | Violated requirements |
|---|---|
| `fix:` (no subject) | 1 |
| `fix: reject invalid input.` | 3 |
| `fix: update` | 4 |
| `fix(copy-engine): reject invalid input` | 2 |
| `- validate input.` | 7 |
| A message without an issue reference | 10 |
| A message without a `Signed-off-by` trailer | 11 |

## Output

The checker outputs `Verdict: PASS` or `Verdict: FAIL`. On failure, `Findings:` lists each mechanically detected violation as `- requirement <number>: <reason>`. Recommendations and requirements that need judgment are reviewed by the author, the agent, and the reviewers; the checker does not emit warnings or generate a replacement message.

Issue numbers, test results, scopes, compatibility statements, and sign-off identities are never invented. The name and email address in the examples are placeholders.
