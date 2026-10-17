#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2025-2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# See LICENSE for license information
"""Check a pull request against .agents/skills/sailbandwidth-pull-request/SKILL.md.

The branch, structure, title, and description rules are checked here. The DCO
sign-off and the commit message contents are checked by the commit message
precheck. This script imports shared Git and message-format helpers from
check_commit_message.py. Rules that require judgment are verified during review,
and the review and merge rules are enforced by the human review stage, branch
protection, and the repository settings. Findings reference the rule numbers of
.agents/skills/sailbandwidth-pull-request/SKILL.md.

Usage:
    check_pull_request.py --event <path>    check the pull request of a GitHub event payload
    check_pull_request.py --title <title> --body-file <path> --head-ref <branch> [--base-ref main] [--head HEAD]
"""

import argparse
import json
import os
import re
import subprocess
import sys

from markdown_it import MarkdownIt

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_commit_message as commit_rules  # noqa: E402

BRANCH_RE = re.compile(r"^(?:feat|fix|hotfix|refactor|docs)/[a-z]+(?:-[a-z]+)*$")
ALLOWED_CHANGE_TYPES = {"Feature", "Bug fix", "Refactor"}
COMMENT_RE = re.compile(r"<!--.*?(?:-->|\Z)", re.DOTALL)
CHECKED_RE = re.compile(r"^\[[xX]\](?:[ \t]+(?P<label>[^\n]*)|$)$")
ISSUE_LINE_RE = re.compile(r"^(?:Closes|Refs) #(?P<number>[1-9][0-9]*)$")
REPOSITORY_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9-]*/[A-Za-z0-9_.-]+")
MARKDOWN = MarkdownIt("commonmark")
SECTIONS = ("Summary", "Related issue", "Change type", "Validation", "Risk and compatibility")


def inline_text(token):
    return "".join("\n" if child.type in ("softbreak", "hardbreak") else child.content
                   for child in token.children or []
                   if child.type in ("text", "code_inline", "image", "softbreak", "hardbreak"))


def parse_sections(body):
    tokens = MARKDOWN.parse(body)
    sections = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token.type == "heading_open" and token.tag == "h2" and token.level == 0:
            sections.append((inline_text(tokens[index + 1]).strip(), []))
            index += 3
            continue
        if sections:
            sections[-1][1].append(token)
        index += 1
    return sections


def has_content(tokens):
    for token in tokens:
        if token.type == "inline" and (inline_text(token).strip() or
                                      any(child.type == "image" for child in token.children or [])):
            return True
        if token.type in ("fence", "code_block") and token.content.strip():
            return True
        if token.type == "html_block" and COMMENT_RE.sub("", token.content).strip():
            return True
    return False


def related_issues(tokens, findings):
    numbers = set()
    invalid = False
    for token in tokens:
        if token.type in ("paragraph_open", "paragraph_close") and token.level == 0:
            continue
        if token.type == "html_block" and not COMMENT_RE.sub("", token.content).strip():
            continue
        if token.type != "inline" or token.level != 1:
            invalid = True
            continue
        if any(child.type not in ("text", "softbreak", "hardbreak", "html_inline") or
               (child.type == "html_inline" and COMMENT_RE.sub("", child.content).strip())
               for child in token.children or []):
            invalid = True
            continue
        for line in inline_text(token).splitlines():
            if not line.strip():
                continue
            match = ISSUE_LINE_RE.fullmatch(line.strip())
            if match:
                numbers.add(int(match.group("number")))
            else:
                invalid = True
    if invalid or not numbers:
        findings.append((11, "'## Related issue' must contain plain 'Closes #<issue>' or "
                             "'Refs #<issue>' lines, not code examples or cross-repository references"))
    return sorted(numbers)


def checked_labels(tokens):
    labels = []
    in_bullets = False
    for index, token in enumerate(tokens):
        if token.level == 0 and token.type in ("bullet_list_open", "bullet_list_close"):
            in_bullets = token.type == "bullet_list_open"
        if (in_bullets and token.type == "inline" and token.level == 3 and index >= 2 and
                tokens[index - 1].type == "paragraph_open" and tokens[index - 2].type == "list_item_open"):
            match = CHECKED_RE.fullmatch(token.content.split("\n", 1)[0])
            if match:
                labels.append((match.group("label") or "").strip())
    return labels


def read_issue(repository, number):
    try:
        result = subprocess.run(
            ["gh", "api", "--hostname", "github.com", f"repos/{repository}/issues/{number}"],
            capture_output=True, text=True, check=False, timeout=30)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RuntimeError(f"cannot query issue #{number}; check GitHub CLI availability and authentication") from error
    if result.returncode:
        raise RuntimeError(f"cannot query issue #{number}; check its existence, authentication, and API availability")
    try:
        issue = json.loads(result.stdout)
    except ValueError as error:
        raise RuntimeError(f"invalid GitHub response for issue #{number}") from error
    if not isinstance(issue, dict) or issue.get("number") != number or issue.get("state") not in ("open", "closed"):
        raise RuntimeError(f"invalid GitHub response for issue #{number}")
    return issue


def check_open_issues(repository, numbers, findings, lookup=None):
    if not REPOSITORY_RE.fullmatch(repository):
        findings.append((11, "issue validation requires a repository of the form OWNER/REPO"))
        return
    if not numbers:
        return
    lookup = lookup or read_issue
    try:
        issues = [lookup(repository, number) for number in numbers]
    except RuntimeError as error:
        findings.append((11, str(error)))
        return
    if not any(issue.get("state") == "open" and "pull_request" not in issue for issue in issues):
        findings.append((11, "'## Related issue' must reference at least one open issue in this repository, "
                             "not a closed issue or a pull request"))


def check_description(body, findings, commit_type):
    if commit_rules.CJK_RE.search(body):
        findings.append((10, "the description must be written in English"))

    sections = parse_sections(body)
    headings = [heading for heading, _ in sections]
    positions = []
    for required in SECTIONS:
        count = headings.count(required)
        if count == 0:
            findings.append((11, f"the description has no '## {required}' section"))
        elif count > 1:
            findings.append((11, f"the description must contain exactly one '## {required}' section; it contains {count}"))
        else:
            positions.append(headings.index(required))
    if positions != sorted(positions):
        findings.append((11, f"the description sections must appear in the order: {', '.join(SECTIONS)}"))

    content = dict(sections)
    for required in SECTIONS:
        if required in content and not has_content(content[required]):
            findings.append((11, f"the '## {required}' section is empty"))
    numbers = related_issues(content["Related issue"], findings) if "Related issue" in content else []
    if "Change type" in content:
        checked = checked_labels(content["Change type"])
        if len(checked) != 1:
            findings.append((12, f"'## Change type' must select exactly one type; it selects {len(checked)}"))
        for label in checked:
            if label in ALLOWED_CHANGE_TYPES:
                continue
            if label.startswith("Other: ") and label[len("Other: "):].strip():
                continue
            findings.append((12, f"'## Change type' selects unsupported type {label!r}"))
        expected = {"feat": "Feature", "fix": "Bug fix", "refactor": "Refactor"}.get(commit_type)
        if len(checked) == 1 and expected and checked[0] != expected:
            findings.append(
                (12, f"'## Change type' selects {checked[0]!r}, but the commit type requires {expected!r}"))
        if len(checked) == 1 and commit_type in commit_rules.TYPES and not expected:
            required = f"Other: {commit_type}"
            if checked[0] != required:
                findings.append(
                    (12, f"'## Change type' must select {required!r} for commit type {commit_type!r}"))

    for line in body.splitlines():
        # The "@" prefilter is an exact equivalence: EMAIL_RE requires a literal
        # "@", and skipping lines without one avoids rescanning the local part
        # at every position on adversarial long lines.
        if "@" in line and commit_rules.EMAIL_RE.search(line):
            findings.append((13, f"the description must not contain email addresses: {line.strip()!r}"))
        if commit_rules.PRIVATE_PATH_RE.search(line):
            findings.append((13, f"the description must not contain private paths: {line.strip()!r}"))
    return numbers


def resolve_base(base_ref, base_sha, override):
    if override:
        return commit_rules.git("rev-parse", "--verify", f"{override}^{{commit}}").strip()
    for remote in ("upstream", "origin"):
        tracking = commit_rules.git(
            "rev-parse", "--verify", "--quiet", f"refs/remotes/{remote}/{base_ref}^{{commit}}", check=False).strip()
        if tracking:
            return tracking
    if base_sha:
        return base_sha
    raise RuntimeError(f"cannot resolve the tip of the base branch {base_ref!r}")


def check_pull_request(pr, base_override=None):
    findings = []
    title = (pr.get("title") or "").strip()
    body = (pr.get("body") or "").replace("\r\n", "\n")
    head_ref, base_ref = pr["head_ref"], pr["base_ref"]
    head = commit_rules.git("rev-parse", "--verify", f"{pr['head_sha']}^{{commit}}").strip()
    base = resolve_base(base_ref, pr.get("base_sha"), base_override)

    if not BRANCH_RE.match(head_ref):
        findings.append((2, f"the working branch name {head_ref!r} does not have the form <type>/<short-description>"))
    if head_ref in ("main", base_ref):
        findings.append((4, f"the compare branch {head_ref!r} must be a working branch"))

    commits = commit_rules.git("rev-list", "--reverse", f"{base}..{head}").split()
    if len(commits) != 1:
        findings.append((6, f"the pull request must contain exactly one commit; it contains {len(commits)}"))
    else:
        commit = commits[0]
        message = commit_rules.git("log", "-1", "--format=%B", commit)
        subject = message.splitlines()[0].strip() if message.strip() else ""
        if title != subject:
            findings.append((9, f"the title {title!r} is not identical to the commit title {subject!r}"))
    if commit_rules.git("rev-list", "--merges", f"{base}..{head}").split():
        findings.append((8, "the working branch contains merge commits; rebase it onto the base branch"))

    title_match = commit_rules.TITLE_RE.match(title)
    commit_type = title_match.group("type") if title_match else None
    numbers = check_description(body, findings, commit_type)
    if pr.get("repository"):
        check_open_issues(pr["repository"], numbers, findings)
    return sorted(set(findings))


def report(findings, label):
    in_actions = os.environ.get("GITHUB_ACTIONS") == "true"
    print(label)
    print(f"Verdict: {'FAIL' if findings else 'PASS'}")
    if findings:
        print("Findings:")
        for rule, description in findings:
            print(f"- rule {rule}: {description}")
            if in_actions:
                print(f"::error title=Pull request rule {rule}::{description}")
    return 1 if findings else 0


def main():
    parser = argparse.ArgumentParser(
        description="Check a pull request against .agents/skills/sailbandwidth-pull-request/SKILL.md.")
    parser.add_argument("--event", help="path of a GitHub pull_request event payload")
    parser.add_argument("--title", help="pull request title")
    parser.add_argument("--body-file", help="path of a file that contains the pull request description")
    parser.add_argument("--head-ref", help="name of the working branch")
    parser.add_argument("--repo", help="OWNER/REPO for online issue-state validation with the GitHub CLI")
    parser.add_argument("--base-ref", default="main", help="name of the base branch (default: main)")
    parser.add_argument("--head", default="HEAD", help="revision of the working branch head (default: HEAD)")
    parser.add_argument(
        "--base", help="revision of the base branch tip (default: upstream/<base-ref>, then origin/<base-ref>)")
    args = parser.parse_args()

    if args.event:
        with open(args.event, encoding="utf-8") as handle:
            event = json.load(handle)
        data = event["pull_request"]
        pr = {"title": data.get("title"), "body": data.get("body"), "head_ref": data["head"]["ref"],
              "head_sha": data["head"]["sha"], "base_ref": data["base"]["ref"], "base_sha": data["base"]["sha"],
              "repository": event["repository"]["full_name"]}
        label = f"Pull request #{data.get('number')}"
    else:
        if not (args.title and args.body_file and args.head_ref):
            parser.error("--title, --body-file, and --head-ref are required without --event")
        with open(args.body_file, encoding="utf-8") as handle:
            body = handle.read()
        pr = {"title": args.title, "body": body, "head_ref": args.head_ref, "head_sha": args.head,
              "base_ref": args.base_ref, "base_sha": None, "repository": args.repo}
        label = f"Pull request from {args.head_ref} to {args.base_ref}"

    label += " (including online issue validation)" if pr.get("repository") else " (local format checks only)"
    return report(check_pull_request(pr, args.base), label)


if __name__ == "__main__":
    sys.exit(main())
