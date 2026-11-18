#!/usr/bin/env python3
                          
                                                                                          
                                                                                               
                                                                              
                                                                                       
                                                                            
                                             
                                                            
                                                                           
                                                         
                                                      
import os
import re
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LIB = ROOT / "tests" / "integration" / "lib" / "pcv_isolated_daemon.sh"
INTEG = ROOT / "tests" / "integration"
RUNNER = ROOT / "scripts" / "run_auto_tests.sh"
BPF_SRC = ROOT / "src" / "bpf" / "pcv_lsm.bpf.c"
MOUNTS_SRC = ROOT / "src" / "utils" / "pcv_lsm_mounts.c"
STATE_RE = re.compile(r'^(\s*)STATE="\$\(mktemp -d ')
CHECK_CALL = 'pcv_iso_check_state_fs "$STATE"'

                                                               
FAKE_TABLE = textwrap.dedent("""\
    252:0 /
    0:38 /tmp
    259:1 /var/lib/purecvisor
    0:52 /var/lib/purecvisor/lxc/my\\x20container
    0:61 /srv/other
    8:3 /etc/purecvisorX
""")


def _write_stub(bindir: Path, table: str, target_map: dict, fail_target: bool = False,
                fstype: str = "tmpfs") -> None:
                                                                                             
                                      
    cases = "\n".join(f'        {k!r}) printf "%s\\n" {v!r}; exit 0 ;;' for k, v in target_map.items())
    script = textwrap.dedent("""\
        #!/bin/bash
        # 시험 스텁: pcv_iso_check_state_fs 가 부르는 findmnt 세 형태만 흉내 낸다.
        t=""; list=0; out=""
        while [ $# -gt 0 ]; do
            case "$1" in
                -T) t="$2"; shift 2; continue ;;
                -o) out="$2"; shift 2; continue ;;
                -rn|-nr) list=1 ;;
            esac
            shift
        done
        if [ -n "$t" ] && [ "$out" = FSTYPE ]; then
            [ -n "{fstype}" ] || exit 1
            printf '%s\n' "{fstype}"
            exit 0
        fi
        if [ -n "$t" ]; then
            {fail}
            case "$t" in
        {cases}
            esac
            exit 1
        fi
        if [ $list -eq 1 ]; then cat <<'TABLE'
        {table}TABLE
        exit 0; fi
        exit 2
        """).replace("{cases}", cases).replace("{table}", table).replace(
        "{fail}", "exit 1" if fail_target else ":").replace("{fstype}", fstype)
    p = bindir / "findmnt"
    p.write_text(script)
    p.chmod(0o755)


def _check(state: str, bindir: Path) -> subprocess.CompletedProcess:
    env = dict(os.environ, PATH=f"{bindir}:{os.environ.get('PATH', '/usr/bin:/bin')}")
    cmd = f'. "{LIB}"; pcv_iso_check_state_fs "$1"'
    return subprocess.run(["bash", "-c", cmd, "pcv-iso-check", state], capture_output=True,
                          text=True, env=env, timeout=30)


class StateFsHelperTest(unittest.TestCase):
                                                                           

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.bindir = Path(self.tmp.name) / "bin"
        self.bindir.mkdir()
        self.state = Path(self.tmp.name) / "pcv-state"
        self.state.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def _run(self, state_dev, fail_target=False, fstype="tmpfs"):
        _write_stub(self.bindir, FAKE_TABLE, {str(self.state): state_dev}, fail_target, fstype)
        return _check(str(self.state), self.bindir)

    def test_tmpfs_state_passes(self):
        r = self._run("0:38")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stderr, "")

    def test_root_fs_state_fails(self):
        r = self._run("252:0")                                            
        self.assertEqual(r.returncode, 1)
        self.assertIn("FAIL:", r.stderr)
        self.assertIn("252:0", r.stderr)

    def test_prefix_bind_fs_state_fails(self):
        r = self._run("259:1")                                                 
        self.assertEqual(r.returncode, 1)

    def test_nested_mount_with_escaped_space_fails(self):
        r = self._run("0:52")                                                       
        self.assertEqual(r.returncode, 1)

    def test_string_prefix_mount_fails(self):
        r = self._run("8:3")                                                   
        self.assertEqual(r.returncode, 1)

    def test_unrelated_mount_passes(self):
        r = self._run("0:61")                            
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_non_tmpfs_unrelated_mount_fails(self):
                                                                         
        for fstype in ("ext4", "zfs", "overlay"):
            with self.subTest(fstype=fstype):
                r = self._run("0:61", fstype=fstype)
                self.assertEqual(r.returncode, 1)
                self.assertIn("FAIL:", r.stderr)
                self.assertIn("tmpfs", r.stderr)
                self.assertIn(fstype, r.stderr)

    def test_undeterminable_fstype_fails_closed(self):
        r = self._run("0:61", fstype="")
        self.assertEqual(r.returncode, 1)
        self.assertIn("FAIL:", r.stderr)

    def test_same_device_tmpfs_still_fails(self):
                                                         
        r = self._run("259:1", fstype="tmpfs")
        self.assertEqual(r.returncode, 1)
        self.assertIn("같은 파일시스템", r.stderr)

    def test_undeterminable_device_fails_closed(self):
        r = self._run("0:38", fail_target=True)
        self.assertEqual(r.returncode, 1)
        self.assertIn("FAIL:", r.stderr)

    def test_missing_state_dir_fails(self):
        _write_stub(self.bindir, FAKE_TABLE, {})
        r = _check(str(Path(self.tmp.name) / "absent"), self.bindir)
        self.assertEqual(r.returncode, 1)
        self.assertIn("FAIL:", r.stderr)


def _harness_scripts():
    return sorted(p for p in INTEG.glob("*.sh") if "pcv_isolated_daemon.sh" in p.read_text())


class StateFsCallSiteContractTest(unittest.TestCase):
                                                                 

    def test_every_harness_script_checks_state_right_after_mktemp(self):
        scripts = _harness_scripts()
        self.assertGreaterEqual(len(scripts), 17)
        missing = []
        for p in scripts:
            lines = p.read_text().splitlines()
            hits = [i for i, line in enumerate(lines) if STATE_RE.match(line)]
            if not hits:
                missing.append(f"{p.name}: STATE=\"$(mktemp -d …)\" 없음")
                continue
            for i in hits:
                nxt = next((line for line in lines[i + 1:] if line.strip()), "")
                if CHECK_CALL not in nxt:
                    missing.append(f"{p.name}:{i + 2}: 다음 줄에 {CHECK_CALL} 없음")
        self.assertEqual(missing, [])

    def test_runner_checks_before_isolated_tests(self):
        text = RUNNER.read_text()
        body = text[text.index("run_isolated_test() {"):]
        body = body[:body.index("\n}\n")]
        self.assertIn('pcv_iso_check_state_fs "${TMPDIR:-/tmp}"', body)
        self.assertIn("ISOLATED_DAEMON_OK=2", body)


class WatchPrefixSyncTest(unittest.TestCase):
                                         

    def test_three_copies_match(self):
        bpf = re.findall(r'PCV_CRIT\("([^"]+)"\)', BPF_SRC.read_text())
        c_text = MOUNTS_SRC.read_text()
        c_block = c_text[c_text.index("k_watch_prefixes[] = {"):]
        c_block = c_block[:c_block.index("};")]
        daemon = re.findall(r'"([^"]+)"', c_block)
        lib = LIB.read_text()
        sh_block = lib[lib.index("PCV_ISO_WATCH_PREFIXES=("):]
        sh_block = sh_block[:sh_block.index("\n)")]
        harness = re.findall(r"^\s+(/\S+)", sh_block, re.M)
        self.assertEqual(len(bpf), 6)
        self.assertEqual(bpf, daemon)
        self.assertEqual(bpf, harness)


if __name__ == "__main__":
    unittest.main()
