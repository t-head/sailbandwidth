---
name: sailbandwidth-pull-request
description: Rules and guidance for sailbandwidth pull requests, covering branches, structure, title, description, and review and merge conditions. Use before opening, updating, reviewing, or merging a pull request in the sailbandwidth repository.
---

# sailbandwidth Pull Request Rules

These rules apply to every pull request that targets the `main` branch or a release branch of this repository. Requirements stated with *must* are mandatory; guidance stated with *should* is recommended. The precheck checks the mandatory requirements that can be verified mechanically and reports violations by rule number; the remaining requirements and recommendations are verified during review or enforced by the repository settings.

The pull request title and description are intended for reviewers and are not recorded in the branch history. The commit message of the pull request is the permanent record.

## Rules

### Branches

1. The working branch should be based on the latest target branch. Agents rebase onto the latest target before opening or updating a pull request, but being behind the target is a recommendation and not a merge gate.
2. The working branch name must have the form `<type>/<short-description>`, where the type is `feat`, `fix`, `hotfix`, `refactor`, or `docs`, and the description consists of lowercase alphabetic words separated by hyphens.
3. A feature, fix, or other product change that also belongs in a release branch should normally be merged into `main` first and then proposed to the release branch. A release-only maintenance change, such as a version update or pipeline adjustment, may target the release branch directly. This ordering is recommended agent guidance and is not a merge requirement.
4. The compare branch must be the working branch. A protected branch must never be used as the compare branch.

### Structure

5. The pull request must address one coherent change.
6. The pull request must contain exactly one commit. Its message is checked independently against the mandatory requirements of the [commit message guide](../sailbandwidth-commit-message/SKILL.md).
7. Updates must be applied by amending the commit and pushing it with `git push --force-with-lease`. `git push --force` is not permitted.
8. The working branch should be updated by rebasing it onto the latest target branch. Merge commits are not permitted in the working branch.

### Title

9. The title must be identical to the title of the pull request commit.

### Description

10. The description must be written in English.
11. The description must contain the following sections, in this order; the [pull request template](../../../.github/pull_request_template.md) provides them:

| Section | Required content |
|---|---|
| `Summary` | What changed and why |
| `Related issue` | At least one open issue in this repository: `Closes #<issue>` when merging resolves it, or `Refs #<issue>` when it remains open. Issues are the only channel for task management and bug tracking; when no open issue covers the change, create one first following the [issue guide](../sailbandwidth-issue/SKILL.md). The repository pull request precheck verifies these references against GitHub at check time and rejects pull request objects; references in other sections or code examples do not satisfy this requirement |
| `Change type` | Feature, bug fix, refactoring, or another specified type |
| `Validation` | Test method, coverage, commands, and results, with output samples or screenshots for user-visible output changes |
| `Risk and compatibility` | Breaking changes and any required data migration or configuration change |

12. In the top-level bullet checklist, exactly one supported change type must be selected on the item's first line: `Feature`, `Bug fix`, `Refactor`, or `Other: <type>` with a nonempty type. Continuation text may explain the selection but does not remove it from the count. Only tests that were executed may be reported. When the commit title starts with `feat`, `fix`, or `refactor`, the selected change type must be Feature, Bug fix, or Refactor, respectively; when it starts with `docs`, `test`, `chore`, or `perf`, the selected change type must be `Other: docs`, `Other: test`, `Other: chore`, or `Other: perf`, respectively.
13. The description and its attachments must contain no credentials, tokens, internal links, private paths, or personal email addresses.
14. After each update, the description must be revised so that it describes the complete change.

### Review and merge

15. At least one member other than the author must approve the current pull request head. Before merging, maintainers verify that the approval and completed Copilot review apply to the current head and that review discussions have been resolved; a successful gate based on an older review does not satisfy this requirement.
16. Every Copilot review comment must be explicitly classified as `addressed`, `won't fix`, or `incorrect`, with the corresponding change or rationale recorded before its conversation is resolved.
17. All required CI checks must pass, and required checks must not be bypassed.
18. The pull request must be merged with Squash and merge.
19. At merge time, the pull request must contain exactly one commit, and the merge commit message must be identical to the commit message except for the pull request number appended to the title.

## Result

- `Verdict: PASS` when every mechanically checked mandatory requirement is met; otherwise `Verdict: FAIL`. Recommendations do not affect the verdict.
- `Findings:` lists each mechanically detected mandatory violation by its rule number, followed by the reason.
