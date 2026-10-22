#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2025-2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# See LICENSE for license information

import contextlib
import io
import json
import subprocess
import unittest
from unittest import mock

import check_commit_message as commit
import check_pull_request as pr


def description(**changes):
    values = dict(zip(pr.SECTIONS, (
        "Add contribution checks.", "Closes #11", "- [x] Other: chore",
        "Run regression tests.", "No product behavior change.")))
    values.update(changes)
    return "\n\n".join(f"## {heading}\n\n{value}" for heading, value in values.items()) + "\n"


def check_body(body, commit_type="chore"):
    findings = []
    numbers = pr.check_description(body, findings, commit_type)
    return numbers, findings


class MarkdownTests(unittest.TestCase):
    def test_canonical_description(self):
        self.assertEqual(check_body(description()), ([11], []))

    def test_literal_markers_in_code(self):
        samples = (
            "    <!--", "\t<!--", "Literal `<!--` here.",
            "Literal ``<!-- ` example`` here.", "Literal `<!--\nmarker` here.",
            "Literal ``<!--\n` marker`` here.", "```text\n<!--\n## Summary\n```",
            "~~~text\n<!--\n## Summary\n~~~", "    ## Summary\n    <!--",
            "<pre>literal marker</pre>",
        )
        for sample in samples:
            with self.subTest(sample=sample):
                self.assertEqual(check_body(description(Validation=sample)), ([11], []))

    def test_comment_precedence_over_fences(self):
        body = description(Validation="<!--\n```\n## Summary\n```\n-->\nRan checks.")
        self.assertEqual(check_body(body), ([11], []))

    def test_comments_do_not_supply_section_content(self):
        for sample in ("", "<!-- fill this in -->", "<!-- a -->\n<!-- b -->"):
            with self.subTest(sample=sample):
                _, findings = check_body(description(Validation=sample))
                self.assertTrue(any(rule == 11 and "empty" in reason for rule, reason in findings))

    def test_unclosed_block_comment_hides_later_sections(self):
        _, findings = check_body(description(Summary="<!-- unfinished"))
        self.assertTrue(any("no '## Related issue'" in reason for _, reason in findings))

    def test_real_duplicate_heading(self):
        _, findings = check_body(description() + "\n## Summary\n\nDuplicate.\n")
        self.assertTrue(any(rule == 11 and "exactly one" in reason for rule, reason in findings))

    def test_heading_in_nested_content_is_not_a_section(self):
        for sample in ("> ## Summary\n> Quoted example.", "- ## Summary\n  Nested example."):
            with self.subTest(sample=sample):
                self.assertEqual(check_body(description(Validation=sample)), ([11], []))

    def test_section_order(self):
        body = description().replace("## Summary", "## Validation", 1)
        body = body.replace("## Validation\n\nRun", "## Summary\n\nRun", 1)
        _, findings = check_body(body)
        self.assertTrue(any(rule == 11 and "order" in reason for rule, reason in findings))

    def test_nonrendered_checkboxes_never_count(self):
        samples = (
            "```text\n- [x] Other: chore\n```", "~~~\n- [x] Other: chore\n~~~",
            "    - [x] Other: chore", "\t- [x] Other: chore",
            "`- [x] Other: chore`", "<!--\n- [x] Other: chore\n-->",
            "> - [x] Other: chore", "- `[x] Other: chore`",
            "<pre>\n- [x] Other: chore\n</pre>",
        )
        for sample in samples:
            with self.subTest(sample=sample):
                _, findings = check_body(description(**{"Change type": sample}))
                self.assertTrue(any(rule == 12 and "selects 0" in reason for rule, reason in findings))

    def test_rendered_checkbox_and_code_example(self):
        block = "    - [x] Feature\n\n- [x] Other: chore\n- [ ] Bug fix\n\n```\n- [x] Feature\n```"
        self.assertEqual(check_body(description(**{"Change type": block})), ([11], []))

    def test_multiple_checked_types(self):
        _, findings = check_body(description(**{"Change type": "- [x] Other: chore\n- [X] Feature"}))
        self.assertTrue(any(rule == 12 and "selects 2" in reason for rule, reason in findings))

    def test_continued_selection_is_counted(self):
        for suffix in ("\n  Additional context.", "\n\n  Separate explanatory paragraph."):
            with self.subTest(suffix=suffix):
                body = description(**{"Change type": "- [x] Feature" + suffix})
                self.assertEqual(check_body(body, "feat"), ([11], []))

    def test_continued_selection_cannot_hide_multiple_selections(self):
        block = "- [x] Feature\n  Additional context.\n- [x] Other: chore"
        _, findings = check_body(description(**{"Change type": block}))
        self.assertTrue(any(rule == 12 and "selects 2" in reason for rule, reason in findings))

    def test_empty_checked_labels_are_not_ignored(self):
        for item in ("- [x]", "- [x] ", "- [x]\n  Additional context."):
            with self.subTest(item=item):
                _, findings = check_body(description(**{"Change type": item}))
                self.assertTrue(any(rule == 12 and "unsupported" in reason for rule, reason in findings))
                _, findings = check_body(description(**{"Change type": item + "\n- [x] Other: chore"}))
                self.assertTrue(any(rule == 12 and "selects 2" in reason for rule, reason in findings))

    def test_change_type_mapping(self):
        for kind, label in (("feat", "Feature"), ("fix", "Bug fix"), ("refactor", "Refactor"),
                            ("chore", "Other: chore"), ("docs", "Other: docs"),
                            ("test", "Other: test"), ("perf", "Other: perf")):
            with self.subTest(kind=kind):
                self.assertEqual(check_body(description(**{"Change type": f"- [x] {label}"}), kind), ([11], []))
                _, findings = check_body(description(**{"Change type": "- [x] Other: unknown"}), kind)
                self.assertTrue(any(rule == 12 for rule, _ in findings))

    def test_other_requires_space_and_nonempty_label(self):
        for kind in (None, "custom", "chore"):
            for label in ("Other:chore", "Other:", "Other: "):
                with self.subTest(kind=kind, label=label):
                    _, findings = check_body(description(**{"Change type": f"- [x] {label}"}), kind)
                    self.assertTrue(any(rule == 12 for rule, _ in findings))

    def test_issue_references_are_plain_top_level_lines(self):
        invalid = ("N/A", "Closes #0", "Refs another/project#11", "`Closes #11`",
                   "```\nCloses #11\n```", "    Closes #11", "> Closes #11",
                   "- Closes #11", "<!-- Closes #11 -->", "Refs #1\u0662")
        for sample in invalid:
            with self.subTest(sample=sample):
                numbers, findings = check_body(description(**{"Related issue": sample}))
                self.assertEqual(numbers, [])
                self.assertTrue(any(rule == 11 for rule, _ in findings))

    def test_issue_numbers_are_deduplicated(self):
        body = description(**{"Related issue": "<!-- hint -->\nCloses #11\nRefs #12\nRefs #11"})
        self.assertEqual(check_body(body), ([11, 12], []))

    def test_examples_elsewhere_do_not_supply_an_issue(self):
        body = description(**{"Related issue": "Refs #12", "Validation": "```text\nCloses #11\n```"})
        numbers, findings = check_body(body)
        self.assertEqual((numbers, findings), ([12], []))
        lookup = mock.Mock(return_value={"number": 12, "state": "closed"})
        pr.check_open_issues("example/project", numbers, findings, lookup)
        lookup.assert_called_once_with("example/project", 12)
        self.assertTrue(any(rule == 11 for rule, _ in findings))

    def test_sensitive_content_checks_still_apply(self):
        for sample in ("Contact someone@example.com", "See [/home/user/work]", "See C:/private/work"):
            with self.subTest(sample=sample):
                _, findings = check_body(description(Validation=sample))
                self.assertTrue(any(rule == 13 for rule, _ in findings))
        self.assertEqual(check_body(description(Validation="Compare ratio 3:1.")), ([11], []))


class IssueStateTests(unittest.TestCase):
    def test_open_issue_is_accepted(self):
        findings = []
        pr.check_open_issues("example/project", [11], findings,
                             lambda repo, number: {"number": number, "state": "open"})
        self.assertEqual(findings, [])

    def test_closed_issue_and_open_pr_are_rejected(self):
        for issue in ({"state": "closed"}, {"state": "open", "pull_request": {}}):
            with self.subTest(issue=issue):
                findings = []
                pr.check_open_issues("example/project", [11], findings, lambda repo, number: issue)
                self.assertTrue(any(rule == 11 for rule, _ in findings))

    def test_one_open_issue_satisfies_multiple_references(self):
        findings = []
        pr.check_open_issues("example/project", [11, 12], findings,
                             lambda repo, number: {"state": "open" if number == 11 else "closed"})
        self.assertEqual(findings, [])

    def test_lookup_failure_fails_closed(self):
        findings = []
        pr.check_open_issues("example/project", [11], findings,
                             mock.Mock(side_effect=RuntimeError("API unavailable")))
        self.assertEqual(findings, [(11, "API unavailable")])

    def test_invalid_repository_does_not_call_api(self):
        lookup = mock.Mock()
        findings = []
        pr.check_open_issues("../../project", [11], findings, lookup)
        lookup.assert_not_called()
        self.assertTrue(findings)

    @mock.patch.object(pr.subprocess, "run")
    def test_read_issue_uses_fixed_host_and_argument_array(self, run):
        issue = {"number": 11, "state": "open"}
        run.return_value = mock.Mock(returncode=0, stdout=json.dumps(issue))
        self.assertEqual(pr.read_issue("example/project", 11), issue)
        self.assertEqual(run.call_args.args[0], ["gh", "api", "--hostname", "github.com",
                                                "repos/example/project/issues/11"])
        self.assertEqual(run.call_args.kwargs["timeout"], 30)

    @mock.patch.object(pr.subprocess, "run")
    def test_read_issue_rejects_errors_and_invalid_responses(self, run):
        for response in ("not-json", "[]", "{}", '{"number": 12, "state": "open"}'):
            with self.subTest(response=response):
                run.return_value = mock.Mock(returncode=0, stdout=response)
                with self.assertRaises(RuntimeError):
                    pr.read_issue("example/project", 11)
        run.return_value = mock.Mock(returncode=1, stdout="", stderr="sensitive error detail")
        with self.assertRaisesRegex(RuntimeError, "cannot query issue"):
            pr.read_issue("example/project", 11)
        for error in (OSError("missing gh"), subprocess.TimeoutExpired("gh", 30)):
            run.side_effect = error
            with self.assertRaises(RuntimeError):
                pr.read_issue("example/project", 11)

    @mock.patch.object(pr, "check_pull_request", return_value=[])
    def test_event_always_enables_online_validation_for_base_repository(self, check):
        event = {"repository": {"full_name": "example/project"}, "pull_request": {
            "number": 9, "title": "chore: add checks", "body": description(),
            "head": {"ref": "docs/add-checks", "sha": "abc", "repo": {"full_name": "fork/project"}},
            "base": {"ref": "main", "sha": "def"}}}
        with mock.patch("builtins.open", mock.mock_open(read_data=json.dumps(event))), \
                mock.patch("sys.argv", ["check_pull_request.py", "--event", "event.json"]), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(pr.main(), 0)
        self.assertEqual(check.call_args.args[0]["repository"], "example/project")

    def test_full_checker_calls_online_lookup(self):
        data = {"title": "chore: add checks", "body": description(), "head_ref": "docs/add-checks",
                "base_ref": "main", "head_sha": "head", "repository": "example/project"}
        with mock.patch.object(pr, "resolve_base", return_value="base"), \
                mock.patch.object(pr.commit_rules, "git", side_effect=["head", "head\n", "chore: add checks\n", ""]), \
                mock.patch.object(pr, "read_issue", return_value={"state": "closed"}) as lookup:
            findings = pr.check_pull_request(data)
        lookup.assert_called_once_with("example/project", 11)
        self.assertTrue(any(rule == 11 for rule, _ in findings))


class CommitMessageTests(unittest.TestCase):
    signoff = "Signed-off-by: Contributor <contributor@users.noreply.github.com>"
    reference = "Refs #11"

    def message(self, subject="chore: add checks", body="", footer=None):
        return subject + "\n\n" + (body + "\n\n" if body else "") \
            + (footer or self.reference + "\n" + self.signoff) + "\n"

    def test_accepted_messages(self):
        for body in ("", "- check metadata", "Explain the behavior.\n\nDescribe the change.",
                     "Connect to 10.0.0.1.", "Compare ratio 3:1."):
            with self.subTest(body=body):
                self.assertEqual(commit.check_message(self.message(body=body)), [])
        self.assertEqual(commit.check_message(self.message(footer="Closes #11\n" + self.signoff)), [])

    def test_title_and_body_regressions(self):
        for subject, body, rule in (("fix:", "", 1), ("fix(copy-engine): reject input", "", 2),
                                    ("fix: reject input.", "", 3), ("fix: update", "", 4),
                                    ("chore: add checks", "- check metadata.", 7),
                                    ("chore: add checks", "- check metadata\nMixed prose.", 6)):
            with self.subTest(subject=subject, body=body):
                self.assertIn(rule, {n for n, _ in commit.check_message(self.message(subject, body))})

    def test_dco_and_footer_regressions(self):
        invalid = (("Closes #11\n\n" + self.signoff, 9),
                   ("Closes 11\n" + self.signoff, 10),
                   ("Refs #11\nSigned-off-by: Contributor <a@@example.com>", 11))
        for footer, rule in invalid:
            with self.subTest(footer=footer):
                self.assertIn(rule, {n for n, _ in commit.check_message(self.message(footer=footer))})
        findings = {n for n, _ in commit.check_message("chore: add checks\n")}
        self.assertIn(10, findings)
        self.assertIn(11, findings)

    def test_cross_repository_issue_reference_is_rejected(self):
        # A cross-repository reference is a candidate: in the footer it fails
        # requirement 10, and in a body paragraph it fails requirement 9 as well.
        footer_findings = {n for n, _ in commit.check_message(
            self.message(footer="Closes other/project#11\n" + self.signoff))}
        self.assertIn(10, footer_findings)
        body_findings = {n for n, _ in commit.check_message(
            self.message(body="Closes other/project#11"))}
        self.assertIn(9, body_findings)
        self.assertIn(10, body_findings)

    def test_typed_title_whitespace_is_normalized(self):
        # Extra spaces after the colon must not bypass the subject and scope checks.
        self.assertIn(4, {n for n, _ in commit.check_message(self.message(subject="fix:  wip"))})
        self.assertIn(2, {n for n, _ in commit.check_message(self.message(subject="fix(copy-engine):  reject input"))})
        # A valid double-space typed title is still parsed and accepted.
        self.assertEqual(commit.check_message(self.message(subject="fix:  reject invalid input")), [])

    def test_typed_title_missing_separator_whitespace_is_rejected(self):
        # A typed prefix without separator whitespace must not fall back to an
        # unprefixed subject; unprefixed titles still pass.
        for subject in ("fix:update", "fix(copy-engine):reject invalid input"):
            with self.subTest(subject=subject):
                self.assertIn(1, {n for n, _ in commit.check_message(self.message(subject=subject))})
        self.assertEqual(commit.check_message(self.message(subject="update the build")), [])
        self.assertEqual(commit.check_message(self.message(subject="http://example.com documents the endpoint")), [])

    def test_missing_issue_reference_is_rejected(self):
        # A sign-off-only footer does not satisfy the mandatory reference.
        self.assertIn(10, {n for n, _ in commit.check_message(self.message(footer=self.signoff))})
        # A reference separated from the sign-offs by prose stays in the body:
        # it fails the placement requirement and leaves the footer without a
        # reference. (A reference directly above the sign-offs is absorbed
        # into the footer and reported by requirement 9 alone.)
        findings = {n for n, _ in commit.check_message(
            self.message(body="Refs #11\nMore prose.", footer=self.signoff))}
        self.assertIn(9, findings)
        self.assertIn(10, findings)

    def test_non_latin_signoff_name_is_accepted(self):
        # A sign-off carries the contributor's Git identity, which may be
        # non-Latin; requirement 12 applies to the message prose, not trailers.
        signoff = "Signed-off-by: 张三 <contributor@users.noreply.github.com>"
        self.assertEqual(commit.check_message(self.message(footer=self.reference + "\n" + signoff)), [])
        self.assertIn(12, {n for n, _ in commit.check_message(self.message(body="修复构建问题"))})

    def test_wip_prefix_checked_on_full_title(self):
        # A WIP marker in the type position (before the colon) is rejected too.
        for title in ("wip: add checks", "WIP:  add checks", "wip-add checks",
                      "wip_add checks", "WIP add checks", "wip"):
            with self.subTest(title=title):
                self.assertIn(4, {n for n, _ in commit.check_message(self.message(subject=title))})
        for title in ("wipe stale cache", "fix: wipe stale cache"):
            with self.subTest(title=title):
                self.assertEqual(commit.check_message(self.message(subject=title)), [])

    def test_unicode_digit_issue_reference_is_rejected(self):
        # An Arabic-Indic digit must not be parsed as the ASCII issue number 12.
        findings = commit.check_message(self.message(footer="Refs #1\u0662\n" + self.signoff))
        self.assertTrue(any(n == 10 for n, _ in findings))
        # Unicode-digit reference attempts in the body are still detected as
        # misplaced or malformed candidates, not silently treated as prose.
        for body in ("Closes issue 1\u0662", "Refs \u0662"):
            findings = commit.check_message(self.message(body=body))
            self.assertTrue(any(n in (9, 10) for n, _ in findings), body)
        # Ordinary prose that merely starts with a keyword is still accepted.
        self.assertEqual(
            commit.check_message(self.message(body="Fix issue 12 by rejecting invalid input")), [])

    def test_version_suffixes_are_compared_completely(self):
        # Hyphenated prerelease/build identifiers and extra components are matched
        # in full so a mismatch against the project version is not missed; the exact project version
        # value and a sentence-ending period are accepted.
        version = "v2.2.1"
        for token in ("v2.2.1-rc1", "v2.2.1-rc-1", "v2.2.1-rc--1",
                      "v2.2.1+build--1", "v2.2.1.1"):
            with self.subTest(token=token):
                findings = commit.check_message(self.message(body=f"target {token}"), version)
                self.assertIn(14, {n for n, _ in findings})
        self.assertEqual(commit.check_message(self.message(body="target v2.2.1"), version), [])
        self.assertEqual(commit.check_message(self.message(body="target v2.2.1."), version), [])

    def test_version_scan_has_no_catastrophic_backtracking(self):
        # A malformed token with a long ambiguous run must be scanned in bounded
        # time; the version suffix pattern has no nested quantifier.
        import time
        evil = "target v2.2.1" + "-a" * 36 + "_"
        started = time.perf_counter()
        commit.check_message(self.message(body=evil), "v2.2.1")
        self.assertLess(time.perf_counter() - started, 2.0)


if __name__ == "__main__":
    unittest.main()
