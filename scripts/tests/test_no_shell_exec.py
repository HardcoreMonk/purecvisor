#!/usr/bin/env python3


















import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_no_shell_exec as gate

GATE = Path(__file__).resolve().parent.parent / "check_no_shell_exec.py"
MONITOR = gate.ROOT / "src" / "modules" / "dispatcher" / "handler_monitor.c"


def _run_on(files: dict) -> subprocess.CompletedProcess:
    with tempfile.TemporaryDirectory() as tmp:
        for name, body in files.items():
            (Path(tmp) / name).write_text(body, encoding="utf-8")
        return subprocess.run([sys.executable, str(GATE), tmp], capture_output=True, text=True)


def test_current_tree_passes():

    result = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert result.returncode == 0, f"{result.stdout}\n{result.stderr}"


def test_legacy_arp_popen_is_blocked():

    body = ('#include <stdio.h>\n'
            'void f(void) {\n'
            '    FILE *arp = popen("arp -n 2>/dev/null", "r");\n'
            '    if (arp) pclose(arp);\n'
            '}\n')
    assert gate.find_violations(body) == [(3, "popen")]
    result = _run_on({"legacy.c": body})
    assert result.returncode == 1
    assert "legacy.c:3: popen()" in result.stderr


def test_spaced_and_macro_calls_are_blocked():

    body = ('#define RUN(c) system (c)\n'
            'int g(const char *c) { return system\t(c); }\n')
    assert [name for _, name in gate.find_violations(body)] == ["system", "system"]
    assert _run_on({"spaced.h": body}).returncode == 1


def test_mentions_members_and_prefixed_names_pass():

    body = ('/* system()/popen() 금지 — pcv_spawn_sync() 사용 */\n'
            '// popen("x") 예시\n'
            'static const char *m = "use system() never popen(\\"x\\")";\n'
            "static const char q = '(';\n"
            'struct ops { int (*system)(void); };\n'
            'int h(struct ops *o, struct ops v) { return o->system() + v.system(); }\n'
            'int my_system(void); int popen_count(void);\n'
            'int k(void) { return my_system() + popen_count(); }\n')
    assert gate.find_violations(body) == []
    assert _run_on({"clean.c": body}).returncode == 0


def test_comment_disguise_does_not_hide_real_call():

    body = 'void z(void) { /* safe */ popen("id", "r"); }\n'
    assert gate.find_violations(body) == [(1, "popen")]


def test_reintroduced_popen_in_monitor_copy_fails():

    text = MONITOR.read_text(encoding="utf-8")
    anchor = "                    (void)_monitor_arp_cache_ipv4(raw_mac, vm_ip, sizeof(vm_ip));"
    assert anchor in text, "ARP 폴백 포맷 변경?"
    mutated = text.replace(anchor, anchor + '\n                    pclose(popen("arp -n", "r"));')
    assert gate.find_violations(text) == []
    assert [name for _, name in gate.find_violations(mutated)] == ["popen"]
    assert _run_on({"handler_monitor.c": mutated}).returncode == 1


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"OK   {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"[test_no_shell_exec] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
