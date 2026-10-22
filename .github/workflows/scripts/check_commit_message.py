#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2025-2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# See LICENSE for license information
"""Check a commit message against .agents/skills/sailbandwidth-commit-message/SKILL.md.

The mandatory requirements that can be verified mechanically are checked here.
The requirements that need judgment, such as the imperative mood, the accuracy
of the message against the diff, and the choice and completeness of the body,
are verified during review. Findings reference the requirement numbers of
.agents/skills/sailbandwidth-commit-message/SKILL.md.

Usage:
    check_commit_message.py --commit <revision>   check a commit of the repository
    check_commit_message.py --file <path>         check a message file, for example in a commit-msg hook

Each invocation checks both the message format and the DCO sign-off.
"""

import argparse
import os
import re
import subprocess
import sys

# Recommended commit types. The type is optional, and this checker does not require it.
TYPES = ("feat", "fix", "refactor", "docs", "test", "chore", "perf")
NON_DESCRIPTIVE_SUBJECTS = {"update", "updates", "change", "changes", "fix bug", "fix bugs", "wip"}
WIP_PREFIX_RE = re.compile(r"^wip(?:\b|[:_-])", re.IGNORECASE)

TITLE_RE = re.compile(r"^(?P<type>[^\s(:]+)(?:\((?P<scope>[^)]*)\))?:\s+(?P<subject>\S.*)$")
EMPTY_SUBJECT_RE = re.compile(r"^[^\s(:]+(?:\([^)]*\))?:\s*$")
# A typed prefix that is not followed by separator whitespace, such as
# "fix:update" or "fix(scope):update". Excluded: a "scheme://" URI prefix and
# prefixes that do not start with a letter, such as "3:1"; both remain valid
# unprefixed subjects.
MALFORMED_TYPE_RE = re.compile(r"^[A-Za-z][^\s():]*(?:\([^)]*\))?:(?!//)\S")
SCOPE_RE = re.compile(r"^[a-z][a-z0-9]*$")
ISSUE_RE = re.compile(r"^(?:Closes|Refs) #[0-9]+$")
ISSUE_KEYWORD_RE = re.compile(
    r"^(?:Closes|Close|Closed|Fixes|Fix|Fixed|Resolves|Resolve|Resolved|Refs|Ref)\b", re.IGNORECASE)
# A complete-line issue-reference candidate: a keyword followed by a hash token
# ("#123" or a bare "#"), a cross-repository token ("owner/repo#123"), an
# "issue <number>" form, or a bare number ("123"). "keyword issue <non-numeric>"
# is intentionally not a candidate because it is indistinguishable from
# imperative prose such as "Fix issue handling"; a human reviews that form.
# A matched candidate still must satisfy requirement 10.
ISSUE_CANDIDATE_RE = re.compile(
    r"^(?:Closes|Close|Closed|Fixes|Fix|Fixed|Resolves|Resolve|Resolved|Refs|Ref)\s+"
    r"(?:#\S*|[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+#\S*|issue\s+#?\d+|\d+)\s*$", re.IGNORECASE)
SIGNOFF_RE = re.compile(r"^Signed-off-by: (?P<name>[^<>]*[^<>\s]) <(?P<email>[^<>\s@]+@[^<>\s@]+)>$")
EMAIL_RE = re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}")
PRIVATE_PATH_RE = re.compile(r"(?:^|[^A-Za-z0-9])(?:/Users/|/home/|/root/|[A-Za-z]:[\\/])")
CJK_RE = re.compile("[\u3000-\u303f\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uac00-\ud7af\uff00-\uffef]")
VERSION_RE = re.compile(
    r"(?<![A-Za-z0-9_])v\d+\.\d+\.\d+(?:[.+-][0-9A-Za-z.+-]*[0-9A-Za-z])?(?![A-Za-z0-9_+-]|\.\d)")
PROJECT_VERSION_RE = re.compile(
    r"^[ \t]*project\s*\(\s*sailbandwidth\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)(?=\s|\))",
    re.MULTILINE | re.IGNORECASE)
SCISSORS = "# ------------------------ >8 ------------------------"

_DEFAULT_REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
REPO_ROOT = os.path.abspath(os.environ.get("SAILBANDWIDTH_REPO_ROOT", _DEFAULT_REPO_ROOT))
VERSION_SOURCE = "CMakeLists.txt"


def git(*args, check=True):
    """Run a Git command in the repository and return its standard output."""
    result = subprocess.run(["git", "-C", REPO_ROOT] + list(args),
                            capture_output=True, text=True, check=False)
    if check and result.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def git_succeeds(*args):
    """Run a Git command in the repository and report whether it exited with status 0."""
    result = subprocess.run(["git", "-C", REPO_ROOT] + list(args),
                            capture_output=True, text=True, check=False)
    return result.returncode == 0


def is_signoff_line(line):
    return line.startswith("Signed-off-by:")


def split_paragraphs(lines):
    paragraphs, current = [], []
    for line in lines:
        if line.strip():
            current.append(line)
        elif current:
            paragraphs.append(current)
            current = []
    if current:
        paragraphs.append(current)
    return paragraphs


def check_title(title, findings):
    match = TITLE_RE.match(title)
    if match is None and EMPTY_SUBJECT_RE.match(title):
        findings.append((1, f"the typed title has no subject: {title!r}"))
        return
    if match is None and MALFORMED_TYPE_RE.match(title):
        findings.append((1, f"the type prefix must be followed by ': ' with separator whitespace: {title!r}"))
        return
    scope = match.group("scope") if match else None
    subject = (match.group("subject") if match else title).rstrip()
    if scope is not None and not SCOPE_RE.match(scope):
        findings.append((2, f"the scope {scope!r} must be one lowercase word containing only letters and digits"))
    if subject.endswith("."):
        findings.append((3, f"the subject must not end with a period: {subject!r}"))
    if subject.lower() in NON_DESCRIPTIVE_SUBJECTS or WIP_PREFIX_RE.match(subject) \
            or WIP_PREFIX_RE.match(title):
        findings.append((4, f"the title is not descriptive or is marked WIP: {title!r}"))


def check_body(paragraphs, findings):
    """Check an optional key feature list or prose body."""
    lines = [line for paragraph in paragraphs for line in paragraph]
    if not lines:
        return
    for line in lines:
        if ISSUE_CANDIDATE_RE.match(line):
            findings.append((9, f"the issue reference must be in the trailing footer block: {line!r}"))
            if not ISSUE_RE.match(line):
                findings.append((10, f"the issue reference must be 'Closes #<issue>' or 'Refs #<issue>': {line!r}"))
    uses_list = any(line == "-" or line.startswith("- ") for line in lines)
    if not uses_list:
        return
    for line in lines:
        if not line.startswith("- ") or not line[2:].strip():
            findings.append((6, f"a key feature body must contain only nonempty lines that start with '- ': {line!r}"))
            continue
        text = line[2:]
        if text.rstrip().endswith("."):
            findings.append((7, f"the key feature must not end with a period: {line!r}"))


def check_dco(lines):
    """Check the trailing DCO sign-off block and return requirement 11 findings.

    The sign-off certifies the contribution under DCO 1.1. The email may be a
    private or anonymous address and is not required to match the commit author;
    only the presence of a well-formed trailing trailer block is enforced.
    """
    findings = []
    trailer_start = len(lines)
    while trailer_start and lines[trailer_start - 1].startswith("Signed-off-by:"):
        trailer_start -= 1

    for line in lines[:trailer_start]:
        if line.startswith("Signed-off-by:"):
            findings.append((11, f"Signed-off-by trailers must form one contiguous block at the end: {line!r}"))

    signoffs = 0
    if trailer_start == 0:
        findings.append((11, "the Signed-off-by trailer block must follow a commit title"))
    for line in lines[trailer_start:]:
        if not SIGNOFF_RE.match(line):
            findings.append((11, f"the trailer must have the form 'Signed-off-by: Name <email>': {line!r}"))
            continue
        signoffs += 1

    if not signoffs:
        findings.append((11, "the message does not end with a valid Signed-off-by trailer"))
    return sorted(set(findings))


def check_footer(lines, findings):
    seen_signoff = False
    seen_reference = False
    for line in lines:
        if line.startswith("Signed-off-by:"):
            seen_signoff = True
        elif ISSUE_KEYWORD_RE.match(line):
            seen_reference = True
            if not ISSUE_RE.match(line):
                findings.append((10, f"the issue reference must be 'Closes #<issue>' or 'Refs #<issue>': {line!r}"))
            if seen_signoff:
                findings.append((9, f"the issue reference must precede the Signed-off-by trailers: {line!r}"))
        else:
            findings.append((9, f"the footer contains only the issue reference and Signed-off-by trailers: {line!r}"))
    if not seen_reference:
        findings.append((10, "the footer must contain the issue reference 'Closes #<issue>' or 'Refs #<issue>'"))


def check_content(lines, version, findings):
    for line in lines:
        is_signoff = line.startswith("Signed-off-by:")
        # The sign-off name carries the contributor's Git identity and may be
        # non-Latin; requirement 12 applies to the message prose, not trailers.
        if not is_signoff and CJK_RE.search(line):
            findings.append((12, f"the message must be written in English: {line!r}"))
        # The "@" prefilter is an exact equivalence: EMAIL_RE requires a literal
        # "@", and skipping lines without one avoids rescanning the local part
        # at every position on adversarial long lines.
        if not is_signoff and "@" in line and EMAIL_RE.search(line):
            findings.append((15, f"email addresses appear only in Signed-off-by trailers: {line!r}"))
        if PRIVATE_PATH_RE.search(line):
            findings.append((15, f"the message must not contain private paths: {line!r}"))
        for found in VERSION_RE.findall(line):
            if version and found != version:
                findings.append((14, f"the version {found} does not match {VERSION_SOURCE} ({version})"))


def check_message(message, version=None):
    """Return a list of (requirement, description) findings for a commit message."""
    findings = []
    lines = [line.rstrip() for line in message.splitlines()]
    while lines and not lines[-1]:
        lines.pop()
    while lines and not lines[0]:
        lines.pop(0)
    if not lines:
        return [(1, "the commit message is empty")]
    if is_signoff_line(lines[0]) or ISSUE_CANDIDATE_RE.match(lines[0]):
        findings.append((1, f"the commit title is missing; the first line is a footer entry: {lines[0]!r}"))

    check_title(lines[0], findings)
    body = lines[1:]
    if body and body[0].strip():
        findings.append((5, "a blank line must separate the title from the body or footer"))

    paragraphs = split_paragraphs(body)
    body_paragraphs = list(paragraphs)
    footer = []
    if paragraphs:
        last = paragraphs[-1]
        signoff_index = next((index for index, line in enumerate(last) if is_signoff_line(line)), None)
        if signoff_index is not None:
            footer_start = signoff_index
            if footer_start and ISSUE_CANDIDATE_RE.match(last[footer_start - 1]):
                footer_start -= 1
            footer = last[footer_start:]
            body_paragraphs = paragraphs[:-1]
            if not footer_start and body_paragraphs and len(body_paragraphs[-1]) == 1 \
                    and ISSUE_CANDIDATE_RE.match(body_paragraphs[-1][0]):
                findings.append((9, "the issue reference and Signed-off-by trailers must be one contiguous footer block"))
                footer = body_paragraphs[-1] + footer
                body_paragraphs = body_paragraphs[:-1]
            if footer_start:
                body_paragraphs.append(last[:footer_start])
                findings.append((9, "a blank line must separate the footer from the body"))

    check_body(body_paragraphs, findings)
    check_footer(footer, findings)
    findings.extend(check_dco(lines))
    check_content(lines, version, findings)
    return sorted(set(findings))


def strip_comments(message):
    """Remove the comment lines that Git adds to a message file before cleanup."""
    kept = []
    for line in message.splitlines():
        if line.startswith(SCISSORS):
            break
        if not line.startswith("#"):
            kept.append(line)
    return "\n".join(kept)


def read_version(revision=None):
    if revision:
        text = git("show", f"{revision}:{VERSION_SOURCE}", check=False)
    else:
        path = os.path.join(REPO_ROOT, VERSION_SOURCE)
        text = ""
        if os.path.exists(path):
            with open(path, encoding="utf-8") as handle:
                text = handle.read()
    text = re.sub(r"#\[(=*)\[.*?\]\1\]", "", text, flags=re.DOTALL)
    text = re.sub(r"#[^\n]*", "", text)
    match = PROJECT_VERSION_RE.search(text)
    return "v" + match.group(1) if match else None


def report(findings, label):
    """Print the verdict and the findings by requirement number and return the exit code."""
    in_actions = os.environ.get("GITHUB_ACTIONS") == "true"
    print(f"{label}")
    print(f"Verdict: {'FAIL' if findings else 'PASS'}")
    if findings:
        print("Findings:")
        for rule, description in findings:
            print(f"- requirement {rule}: {description}")
            if in_actions:
                print(f"::error title=Commit message requirement {rule}::{description}")
    return 1 if findings else 0


def main():
    parser = argparse.ArgumentParser(
        description="Check a commit message against .agents/skills/sailbandwidth-commit-message/SKILL.md.")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--commit", help="revision of the commit to check, for example HEAD")
    source.add_argument("--file", help="path of a commit message file")
    args = parser.parse_args()

    if args.commit:
        message = git("log", "-1", "--format=%B", args.commit)
        version = read_version(args.commit)
        label = f"Commit {git('rev-parse', '--short', args.commit).strip()}"
    else:
        with open(args.file, encoding="utf-8") as handle:
            message = strip_comments(handle.read())
        version = read_version()
        label = f"Commit message file {args.file}"

    findings = check_message(message, version)
    if version is None:
        findings.append((14, f"cannot read the project VERSION from {VERSION_SOURCE}"))
    return report(findings, label)


if __name__ == "__main__":
    sys.exit(main())
