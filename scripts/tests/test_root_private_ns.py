#!/usr/bin/env python3

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WRAPPER = ROOT / "scripts" / "run_root_in_private_ns.sh"
MAKEFILE = ROOT / "Makefile"
COVER = ["/var/lib/purecvisor", "/var/log/purecvisor", "/etc/purecvisor", "/run/purecvisor",
         "/var/log/suricata", "/run/libvirt", "/run/openvswitch", "/var/lib/lxc", "/var/lib/libvirt/images"]
BPFFS = Path("/sys/fs/bpf")

FAKE_SUDO = ('#!/bin/bash\na="$*"; echo "sudo ${a//$\'\\n\'/ }" >>"$PCV_T_LOG"\n'
             'while [[ "${1:-}" == -* ]]; do shift; done\nexec "$@"\n')
FAKE_UNSHARE = ('#!/bin/bash\na="$*"; echo "unshare ${a//$\'\\n\'/ }" >>"$PCV_T_LOG"\n'
                'while [[ "${1:-}" != "--" ]]; do shift; done\nshift\nexec "$@"\n')

FAKE_MOUNT = ('#!/bin/bash\necho "mount $*" >>"$PCV_T_LOG"\n'
              'for last; do :; done\n[[ "$last" == "${PCV_T_MOUNT_FAIL:-}" ]] && exit 32\nexit 0\n')

FAKE_CMD = ('#!/bin/bash\necho "cmd $*" >>"$PCV_T_LOG"\ntouch "$PCV_T_MARKER"\n'
            'echo "ok 1 /fake/leaf"\necho "fake-stderr" >&2\nexit "${PCV_T_RC:-0}"\n')

def code_text(text: str) -> str:
    return "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("#"))

def cover_list(wrapper_text: str) -> list[str]:
    match = re.search(r'COVER_PATHS="\$\{PCV_ROOT_NS_COVER_PATHS-(?P<paths>[^}"]*)\}"', wrapper_text)
    return match["paths"].split() if match else []

def cover_problems(wrapper_text: str) -> list[str]:

    text = code_text(wrapper_text)
    problems = []
    if cover_list(text) != COVER:
        problems.append(f"덮기 목록 {cover_list(text)}")
    for needle in ("unshare --mount --propagation private", 'mount -t tmpfs tmpfs "$p"',
                   "mount -t bpf bpf /sys/fs/bpf", "mkdir -p /run/libvirt/qemu || exit 97"):
        if needle not in text:
            problems.append(f"없음: {needle}")
    if text.count("exit 97") < 3 or 'exec "$@"' not in text or text.rindex("exit 97") > text.index('exec "$@"'):
        problems.append("덮기·bpffs 실패 분기(exit 97)가 실행(exec) 앞에 다 있지 않다")
    return problems

def make_test_recipe(makefile_text: str) -> str:
    match = re.search(r"(?m)^test:[^\n]*\n(?P<recipe>(?:\t[^\n]*\n)+)", makefile_text)
    return match["recipe"] if match else ""

class Box:

    def __init__(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="pcv-root-ns-")
        self.tmp = Path(self._tmp.name)
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        for name, body in (("sudo", FAKE_SUDO), ("unshare", FAKE_UNSHARE), ("mount", FAKE_MOUNT)):
            self.tool(self.bin / name, body)
        self.cover = [self.tmp / "cover-a", self.tmp / "cover-b"]
        for path in self.cover:
            path.mkdir()
        self.log = self.tmp / "calls.log"
        self.marker = self.tmp / "ran"
        self.env = {**os.environ, "PATH": f"{self.bin}:{os.environ['PATH']}", "PCV_T_LOG": str(self.log),
                    "PCV_T_MARKER": str(self.marker),
                    "PCV_ROOT_NS_COVER_PATHS": " ".join([str(self.cover[0]), str(self.tmp / "absent"), str(self.cover[1])])}

        for name in ("sudo", "unshare", "mount"):
            assert shutil.which(name, path=self.env["PATH"]) == str(self.bin / name), name

    def close(self) -> None:
        self._tmp.cleanup()

    @staticmethod
    def tool(path: Path, body: str) -> Path:
        path.write_text(body, encoding="utf-8")
        path.chmod(0o755)
        return path

    def calls(self) -> list[str]:
        return self.log.read_text(encoding="utf-8").splitlines() if self.log.exists() else []

    def mounts(self) -> list[str]:
        return [c for c in self.calls() if c.startswith("mount ")]

class WrapperStatic(unittest.TestCase):
    def test_wrapper_contract(self) -> None:
        self.assertEqual(cover_problems(WRAPPER.read_text(encoding="utf-8")), [])
        self.assertTrue(os.access(WRAPPER, os.X_OK))

    def test_counterfactual_dropped_cover_path_is_red(self) -> None:
        text = WRAPPER.read_text(encoding="utf-8")
        match = re.search(r'COVER_PATHS="\$\{PCV_ROOT_NS_COVER_PATHS-(?P<paths>[^}"]*)\}"', text)
        self.assertIsNotNone(match)
        for path in ("/var/lib/purecvisor", "/run/libvirt", "/var/lib/libvirt/images"):
            with self.subTest(path):
                kept = [p for p in match["paths"].split() if p != path]
                mutated = text[:match.start("paths")] + " ".join(kept) + text[match.end("paths"):]
                self.assertNotEqual(cover_problems(mutated), [])

    def test_counterfactual_missing_bpffs_is_red(self) -> None:
        text = WRAPPER.read_text(encoding="utf-8")
        mutated = text.replace("mount -t bpf bpf /sys/fs/bpf", ":")
        self.assertNotEqual(cover_problems(mutated), [])

class WrapperBehavior(unittest.TestCase):
    def setUp(self) -> None:
        self.box = Box()
        self.addCleanup(self.box.close)
        self.cmd = Box.tool(self.box.tmp / "cmd", FAKE_CMD)

    def run_wrapper(self, script: Path = WRAPPER, *args: str, **env: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(["bash", str(script), *args], cwd=self.box.tmp, env={**self.box.env, **env},
                              capture_output=True, text=True, timeout=30, stdin=subprocess.DEVNULL)

    def mutated(self, old: str, new: str) -> Path:
        text = WRAPPER.read_text(encoding="utf-8")
        self.assertIn(old, text)
        path = self.box.tmp / "mutated_wrapper.sh"
        path.write_text(text.replace(old, new), encoding="utf-8")
        return path

    def fail_closed_problems(self, script: Path) -> list[str]:

        problems = []
        self.box.marker.unlink(missing_ok=True)
        result = self.run_wrapper(script, str(self.cmd), PCV_T_MOUNT_FAIL=str(self.box.cover[1]))
        if result.returncode != 97:
            problems.append(f"종료 코드 {result.returncode}")
        if self.box.marker.exists():
            problems.append("덮기 실패 뒤에도 명령을 실행했다")
        if f"cover failed: {self.box.cover[1]}" not in result.stderr:
            problems.append(f"실패 문구 없음: {result.stderr!r}")
        return problems

    def test_success_runs_command_inside_namespace_and_passes_status_and_output(self) -> None:
        result = self.run_wrapper(WRAPPER, str(self.cmd), "-v", "--x", PCV_T_RC="7")
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertEqual(result.stdout, "ok 1 /fake/leaf\n")
        self.assertEqual(result.stderr, "fake-stderr\n")
        calls = self.box.calls()
        self.assertTrue(calls[0].startswith("sudo env PCV_NS_COVER="), calls)
        self.assertIn("unshare --mount --propagation private -- bash -c", calls[1])
        expected = [f"mount -t tmpfs tmpfs {self.box.cover[0]}", f"mount -t tmpfs tmpfs {self.box.cover[1]}"]
        if BPFFS.is_dir():
            expected.append("mount -t bpf bpf /sys/fs/bpf")

        self.assertEqual(self.box.mounts(), expected)
        self.assertEqual(calls[-1], "cmd -v --x")

    def test_cover_failure_is_fail_closed(self) -> None:
        self.assertEqual(self.fail_closed_problems(WRAPPER), [])

    def test_bpffs_failure_is_fail_closed(self) -> None:
        if not BPFFS.is_dir():
            self.skipTest("/sys/fs/bpf 없음")
        result = self.run_wrapper(WRAPPER, str(self.cmd), PCV_T_MOUNT_FAIL="/sys/fs/bpf")
        self.assertEqual(result.returncode, 97, result.stderr)
        self.assertFalse(self.box.marker.exists())
        self.assertIn("bpffs mount failed", result.stderr)

    def test_no_command_is_usage_error(self) -> None:
        result = self.run_wrapper(WRAPPER)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(self.box.calls(), [])

    def test_counterfactual_cover_failure_ignored_is_red(self) -> None:
        script = self.mutated('mount -t tmpfs tmpfs "$p" || { echo "[root-ns] cover failed: $p" >&2; exit 97; }',
                              'mount -t tmpfs tmpfs "$p" || true')
        self.assertNotEqual(self.fail_closed_problems(script), [])

class MakeTestRecipe(unittest.TestCase):

    def setUp(self) -> None:
        self.box = Box()
        self.addCleanup(self.box.close)
        self.work = self.box.tmp / "work"
        (self.work / "scripts").mkdir(parents=True)
        shutil.copy2(WRAPPER, self.work / "scripts" / WRAPPER.name)
        Box.tool(self.work / "test_runner", FAKE_CMD)
        Box.tool(self.work / "audit_startup", '#!/bin/bash\necho "audit $*" >>"$PCV_T_LOG"\nexit 0\n')

    def run_recipe(self, recipe: str, **env: str) -> subprocess.CompletedProcess[str]:
        (self.work / "Makefile").write_text(
            "TEST_BIN = test_runner\nAUDIT_STARTUP_TEST_BIN = audit_startup\ntest:\n" + recipe, encoding="utf-8")
        (self.work / "test_results.txt").unlink(missing_ok=True)
        self.box.marker.unlink(missing_ok=True)
        self.box.log.unlink(missing_ok=True)
        menv = {**self.box.env, **env}
        menv.pop("MAKEFLAGS", None)
        menv.pop("MFLAGS", None)
        return subprocess.run(["make", "-s", "--no-print-directory", "test"], cwd=self.work, env=menv,
                              capture_output=True, text=True, timeout=60, stdin=subprocess.DEVNULL)

    def recipe_problems(self, recipe: str) -> list[str]:

        problems = []
        if re.search(r"(?m)(^\t@?|[\s;|&(])sudo\b", recipe):
            problems.append("맨 sudo 실행이 있다")
        ok = self.run_recipe(recipe)
        results = (self.work / "test_results.txt")
        if ok.returncode != 0:
            problems.append(f"성공 경로 종료 코드 {ok.returncode}: {ok.stderr}")
        if not results.exists() or "ok 1 /fake/leaf" not in results.read_text(encoding="utf-8"):
            problems.append("test_results.txt에 시험 출력이 없다")
        elif "fake-stderr" not in results.read_text(encoding="utf-8"):
            problems.append("stderr가 test_results.txt로 가지 않았다")
        if "ok 1 /fake/leaf" not in ok.stdout:
            problems.append("결과를 화면에 내지 않았다")
        if f"mount -t tmpfs tmpfs {self.box.cover[0]}" not in self.box.mounts():
            problems.append("운영 경로 덮기 없이 시험을 실행했다")
        if "audit -v" not in self.box.calls():
            problems.append("감사 시작 시험을 돌리지 않았다")
        failing = self.run_recipe(recipe, PCV_T_RC="5")
        if failing.returncode == 0:
            problems.append("시험 실패를 성공으로 바꿨다")
        if "audit -v" in self.box.calls():
            problems.append("시험 실패 뒤에도 다음 단계로 갔다")
        blocked = self.run_recipe(recipe, PCV_T_MOUNT_FAIL=str(self.box.cover[1]))
        if blocked.returncode == 0:
            problems.append("덮기 실패를 성공으로 바꿨다")
        if self.box.marker.exists():
            problems.append("덮기 실패 뒤에도 시험을 실행했다")
        return problems

    def test_make_test_root_step_goes_through_wrapper(self) -> None:
        recipe = make_test_recipe(MAKEFILE.read_text(encoding="utf-8"))
        self.assertIn("\t@bash scripts/run_root_in_private_ns.sh ./$(TEST_BIN) -v > test_results.txt 2>&1; \\\n", recipe)
        self.assertEqual(self.recipe_problems(recipe), [])

    def test_counterfactual_bare_sudo_recipe_is_red(self) -> None:
        recipe = make_test_recipe(MAKEFILE.read_text(encoding="utf-8"))
        bare = recipe.replace("bash scripts/run_root_in_private_ns.sh ./$(TEST_BIN)", "sudo ./$(TEST_BIN)")
        self.assertNotEqual(bare, recipe)
        problems = self.recipe_problems(bare)
        self.assertIn("맨 sudo 실행이 있다", problems)
        self.assertIn("운영 경로 덮기 없이 시험을 실행했다", problems)
        self.assertIn("덮기 실패 뒤에도 시험을 실행했다", problems)

if __name__ == "__main__":
    unittest.main()
