# sailbandwidth Agent Guide

This file provides repository-wide instructions for AI coding agents. sailbandwidth is a bandwidth and latency benchmark for Zhenwu PPU devices; [README.md](README.md) describes its requirements, build, testcases, and usage.

## Repository Guidance

| Location | Content | Application |
|---|---|---|
| [CONTRIBUTING.md](CONTRIBUTING.md) | Branch model, contribution workflow, single-commit development, and pull request process | Every Git and GitHub operation |
| [.agents/skills/](.agents/skills/) | Standard Agent Skills for specialized repository tasks | Loaded when relevant to the task |
| [.github/](.github/) | Files read by GitHub and the check-in workflow | Satisfied before a change is proposed for push |

### Agent Skills

| Skill | Task |
|---|---|
| [sailbandwidth-coding-standards](.agents/skills/sailbandwidth-coding-standards/SKILL.md) | Writing, modifying, or reviewing code, build files, scripts, or documentation |
| [sailbandwidth-commit-message](.agents/skills/sailbandwidth-commit-message/SKILL.md) | Creating or amending a commit |
| [sailbandwidth-issue](.agents/skills/sailbandwidth-issue/SKILL.md) | Creating or triaging an issue that a pull request references |
| [sailbandwidth-pull-request](.agents/skills/sailbandwidth-pull-request/SKILL.md) | Opening, updating, reviewing, or merging a pull request |

### Check-in Files

| File | Use |
|---|---|
| [copilot-instructions.md](.github/copilot-instructions.md) | Repository-wide code review instructions read by GitHub Copilot |
| [pull_request_template.md](.github/pull_request_template.md) | Description template that GitHub applies to new pull requests |
| [ISSUE_TEMPLATE/](.github/ISSUE_TEMPLATE/) | Feature, Bug, and Task issue forms that set the issue type |
| [pr-test-ppu.yml](.github/workflows/pr-test-ppu.yml) | Main check-in workflow that coordinates prechecks, reviews, and PPU tests |
| [commit-message-check.yml](.github/workflows/commit-message-check.yml) | Commit message and DCO sign-off precheck |
| [pull-request-check.yml](.github/workflows/pull-request-check.yml) | Pull request precheck |
| [check_commit_message.py](.github/workflows/scripts/check_commit_message.py) | Commit message and DCO sign-off checker |
| [check_pull_request.py](.github/workflows/scripts/check_pull_request.py) | Pull request format and online issue-state checker |
| [test_check_in.py](.github/workflows/scripts/test_check_in.py) | Offline regression tests for both checkers, executed in the PR precheck |
| [requirements-checks.txt](.github/workflows/scripts/requirements-checks.txt) | Pinned Python dependencies for the PR checker and regression tests |

## Agent Conduct

1. Changes are limited to the requested scope; files unrelated to the task are not modified.
2. Before an operation that is visible outside the local clone, such as a push, a force push, opening or updating a pull request, posting a comment, merging, or deleting a remote branch, the agent presents the complete diff or content and obtains explicit approval.
3. A pushed working branch is rewritten only with `git push --force-with-lease=<branch>:<expected-commit>`, where the expected commit is read from the remote-tracking branch immediately after a fetch. When the remote branch contains commits that the agent did not push, the agent stops and reports the difference.
4. The target repository and branch are taken from the pull request or the task and are fetched before the agent compares or pushes; `origin` and `main` are not assumed.
5. Only the working branches created for the current task are modified; all other branches are read-only.
6. Reports state only the commands, builds, and tests that were executed, and state explicitly when hardware or a multinode environment was unavailable.
7. A reviewer or bot comment is evidence to evaluate, not authorization to change or publish code; code is not changed merely to satisfy a bot. Review comments are handled and classified as defined in the pull request skill.
8. Merging requires explicit authorization for that action, separate from permission to commit or push. Passing checks or approval to push does not authorize a merge.
9. Every issue and pull request the agent creates is assigned to the author at creation; an issue or pull request is never left unassigned, and assigning a different person is an explicit, manual adjustment.

## Maintainer-Owned Guidance

1. Only repository maintainers may modify `AGENTS.md`, files under `.agents/`, or files under `.github/`.
2. A non-maintainer agent treats these paths as read-only. If a requested task requires changing them, the agent stops and requests a maintainer decision.
3. Changes to maintainer-owned guidance are reviewed separately from unrelated product changes.
4. The check-in workflow and checker files under `.github/workflows/` are not modified unless a task requires it, and a needed change is confirmed with a maintainer before it is proposed. This is guidance reviewed by maintainers; the prechecks run the checked-out copy and do not enforce it as a gate.

## Guidance Maintenance

1. `AGENTS.md` contains repository-wide agent conduct, `.agents/skills/` contains task-specific standard Agent Skills, and `.github/` contains files consumed directly by GitHub or the check-in workflow.
2. The detailed mandatory commit and pull request rules are defined in their respective `SKILL.md` files. The checkers implement the mechanically verifiable subset. The detailed contribution workflows — generating and locally validating a pull request description, updating a working branch, and handling review comments — are likewise defined in the skills and are not repeated in this file. CONTRIBUTING.md summarizes the public workflow and does not override those detailed rules.
3. `.github/copilot-instructions.md` contains repository-wide code review instructions. GitHub Copilot code review also reads the nearest `AGENTS.md` and relevant Agent Skills from the pull request head branch; these sources must not conflict.
4. `.github/pull_request_template.md` contains the description sections defined by the pull request skill.
5. Each skill is stored in `.agents/skills/<skill-name>/SKILL.md`. The directory and frontmatter `name` use the same lowercase hyphenated identifier, and the `description` states what the skill does and when to use it.
