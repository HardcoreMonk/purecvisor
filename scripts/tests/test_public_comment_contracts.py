#!/usr/bin/env python3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import strip_source_comments as policy


class PublicCommentContracts(unittest.TestCase):
    def test_makefile_transform_applies_recipe_comment_policy(self):
        with tempfile.TemporaryDirectory(prefix="pcv-comment-policy-") as directory:
            path = Path(directory) / "Makefile"
            path.write_text("target:\n\t@# 설명 주석\n\t@echo kept\n")
            before, after = policy.transform(path, policy.classify(path))
            self.assertNotEqual(before, after)
            self.assertNotIn("설명 주석", after)
            self.assertIn("\t@echo kept\n", after)

    def test_make_recipe_comment_modifiers_are_removed(self):
        self.assertTrue(hasattr(policy, "strip_makefile"), "Makefile recipe 주석 처리가 필요하다")
        for prefix in ("@", "-", "+", "@-+", "@ "):
            source = f"target:\n\t{prefix}# 설명 주석\n\t@echo ok\n"
            stripped = policy.strip_makefile(source)
            self.assertNotIn("설명 주석", stripped)
            self.assertIn("\t@echo ok\n", stripped)

    def test_make_recipe_hash_literals_are_preserved(self):
        self.assertTrue(hasattr(policy, "strip_makefile"), "Makefile recipe 주석 처리가 필요하다")
        source = 'target:\n\t@printf "%s\\n" "# quoted" value@#literal\n'
        self.assertEqual(policy.strip_makefile(source), source)

    def test_shell_comment_substitution_is_removed_with_continuation_preserved(self):
        source = 'printf "%s\\n" \\\n  `# 한글 설명 " quoted` \\\n  `# 다음 설명` \\\n  "kept"\n'
        stripped = policy.strip_hash(source)
        self.assertNotIn("한글 설명", stripped)
        self.assertNotIn("다음 설명", stripped)
        self.assertEqual(stripped.count("\\\n"), source.count("\\\n"))
        for candidate in (source, stripped):
            result = subprocess.run(["bash", "-c", candidate], text=True, capture_output=True)
            self.assertEqual((result.returncode, result.stdout, result.stderr), (0, "kept\n", ""))

    def test_shell_real_substitution_and_quoted_comment_text_are_preserved(self):
        for source in (
            'value=`printf kept`\n',
            'printf "%s\\n" \'`# literal`\'\n',
            'printf "%s\\n" "\\`# literal\\`"\n',
            '  `printf kept` \\\n  next\n',
        ):
            self.assertEqual(policy.strip_hash(source), source)

    def test_shell_multiline_quoted_substitution_text_is_preserved(self):
        for quote in ("'", '"'):
            source = 'printf "%s\\n" ' + quote + '\\\n  `# literal` \\\n  kept' + quote + '\n'
            self.assertEqual(policy.strip_hash(source), source)

    def test_shell_quote_opened_before_a_noncontinued_line_is_preserved(self):
        source = "printf '%s\\n' 'start\ncontinued \\\n  `# literal` \\\nend'\n"
        stripped = policy.strip_hash(source)
        self.assertEqual(stripped, source)
        before = subprocess.run(["bash", "-c", source], capture_output=True)
        after = subprocess.run(["bash", "-c", stripped], capture_output=True)
        self.assertEqual((after.returncode, after.stdout, after.stderr),
                         (before.returncode, before.stdout, before.stderr))

    def test_shell_quoted_heredoc_preserves_backtick_literal(self):
        for delimiter in ("EOF", "END-DATA", "END DATA"):
            source = "cat <<'" + delimiter + "'\ncontinued \\\n  `# literal` \\\nend\n" + delimiter + "\n"
            stripped = policy.strip_hash(source)
            self.assertEqual(stripped, source)
            before = subprocess.run(["bash", "-c", source], capture_output=True)
            after = subprocess.run(["bash", "-c", stripped], capture_output=True)
            self.assertEqual((after.returncode, after.stdout, after.stderr),
                             (before.returncode, before.stdout, before.stderr))

    def test_executed_shell_heredoc_strips_only_noop_comments(self):
        body = 'printf "%s\\n" \\\n  `# 실행 주석` \\\n  "kept"\n'
        for command in (
            "bash -s -- fixture <<'REMOTE_EOF'\n",
            'if ! "$SSH_BIN" fixture bash -s -- \\\n  fixture \\\n  fixture <<\'REMOTE_EOF\'\n',
        ):
            source = command + body + "REMOTE_EOF\n"
            if command.startswith("if"):
                source += "then exit 1; fi\n"
            stripped = policy.strip_hash(source)
            self.assertNotIn("실행 주석", stripped)
            harness = 'ssh_fixture() { shift; "$@"; }\nSSH_BIN=ssh_fixture\n'
            for candidate in (source, stripped):
                result = subprocess.run(["bash", "-c", harness + candidate], capture_output=True, text=True)
                self.assertEqual((result.returncode, result.stdout, result.stderr), (0, "kept\n", ""))

    def test_nested_literal_heredoc_closes_before_outer_shell_comments(self):
        source = "bash -s <<'REMOTE_EOF'\ncat <<'END-DATA'\ncontinued \\\n  `# literal` \\\nend\nEND-DATA\nprintf '%s\\n' \\\n  `# outer comment` \\\n  kept\nREMOTE_EOF\n"
        stripped = policy.strip_hash(source)
        self.assertIn("`# literal`", stripped)
        self.assertNotIn("outer comment", stripped)
        before = subprocess.run(["bash", "-c", source], capture_output=True)
        after = subprocess.run(["bash", "-c", stripped], capture_output=True)
        self.assertEqual(before.returncode, 0)
        self.assertEqual((after.returncode, after.stdout, after.stderr),
                         (before.returncode, before.stdout, before.stderr))

    def test_multiple_heredocs_are_conservatively_preserved(self):
        source = "cat <<'ONE' <<'TWO'\ndiscarded\nONE\ncontinued \\\n  `# literal` \\\nend\nTWO\n"
        self.assertEqual(policy.strip_hash(source), source)

    def test_literal_heredoc_does_not_close_at_tab_indented_delimiter(self):
        source = "cat <<'EOF'\n\tEOF\ncontinued \\\n  `# literal` \\\nend\nEOF\n"
        self.assertEqual(policy.strip_hash(source), source)

    def test_make_continued_recipe_preserves_quoted_at_hash_literal(self):
        source = "target:\n\t@printf '%s\\n' 'start \\\n\t@# literal \\\n\tend'\n"
        stripped = policy.strip_makefile(source)
        self.assertEqual(stripped, source)
        before = subprocess.run(["make", "--no-print-directory", "-f", "-"],
                                input=source, text=True, capture_output=True)
        after = subprocess.run(["make", "--no-print-directory", "-f", "-"],
                               input=stripped, text=True, capture_output=True)
        self.assertEqual((after.returncode, after.stdout, after.stderr),
                         (before.returncode, before.stdout, before.stderr))


if __name__ == "__main__":
    unittest.main()
