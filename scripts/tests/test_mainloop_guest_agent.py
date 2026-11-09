#!/usr/bin/env python3


import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_mainloop_guest_agent as gate

ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / "scripts" / "check_mainloop_guest_agent.py"
MONITOR = "src/modules/dispatcher/handler_monitor.c"
WORKER_FILE = "src/modules/dispatcher/handler_vm_guest.c"
WORKER_FN = "_guest_ping_worker"


def _make_root(tmp: Path, edits: dict[str, str] | None = None) -> Path:

    for rel in {f for f, _ in gate.ALLOWLIST} | {MONITOR}:
        dst = tmp / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / rel, dst)
    for rel, text in (edits or {}).items():
        (tmp / rel).write_text(text, encoding="utf-8")
    return tmp


def _run(root: Path | None) -> subprocess.CompletedProcess:
    cmd = [sys.executable, str(GATE)] + (["--root", str(root)] if root else [])
    return subprocess.run(cmd, capture_output=True, text=True)


def _first_function_with_body(text: str) -> str:

    code = gate.mask_code(text)
    spans = gate.function_spans(code)
    assert spans, "함수를 찾지 못함"
    return spans[0][0]


def test_current_tree_passes():

    result = _run(None)
    assert result.returncode == 0, f"{result.stdout}\n{result.stderr}"
    assert gate.check_tree(ROOT) == []


def test_counterfactual_mainloop_agent_call_fails():

    original = (ROOT / MONITOR).read_text(encoding="utf-8")
    fn = _first_function_with_body(original)
    code = gate.mask_code(original)
    start = next(s for name, s, e in gate.function_spans(code) if name == fn)
    brace = code.index("{", start)
    injected = (original[:brace + 1]
                + "\n    (void)VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_AGENT;\n"
                + original[brace + 1:])
    with tempfile.TemporaryDirectory() as tmp:
        root = _make_root(Path(tmp), {MONITOR: injected})
        result = _run(root)
    assert result.returncode == 1, f"{result.stdout}\n{result.stderr}"
    out = result.stdout + result.stderr
    assert "handler_monitor.c" in out and fn in out, out
    assert "VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_AGENT" in out, out


def test_counterfactual_shutdown_in_main_loop_fails():

    original = (ROOT / MONITOR).read_text(encoding="utf-8")
    fn = _first_function_with_body(original)
    code = gate.mask_code(original)
    start = next(s for name, s, e in gate.function_spans(code) if name == fn)
    brace = code.index("{", start)
    injected = original[:brace + 1] + "\n    virDomainShutdown(NULL);\n" + original[brace + 1:]
    with tempfile.TemporaryDirectory() as tmp:
        result = _run(_make_root(Path(tmp), {MONITOR: injected}))
    assert result.returncode == 1 and fn in result.stderr + result.stdout


def test_allowlisted_worker_passes():

    text = (ROOT / WORKER_FILE).read_text(encoding="utf-8")
    same_file = {k: v for k, v in gate.ALLOWLIST.items() if k[0] == WORKER_FILE}
    assert (WORKER_FILE, WORKER_FN) in same_file
    assert gate.check_files({WORKER_FILE: text}, same_file) == []

    without = {k: v for k, v in same_file.items() if k[1] != WORKER_FN}
    reasons = gate.check_files({WORKER_FILE: text}, without)
    assert any(WORKER_FN in r for r in reasons), reasons


def test_comment_and_string_ignored():

    src = (
        "/* virDomainQemuAgentCommand 를 부르지 않는다 */\n"
        "static void f(void)\n{\n"
        "    // VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_AGENT\n"
        '    const char *s = "virDomainShutdown( virDomainGetFSInfo";\n'
        "    /* virDomainFSFreeze(dom) */\n"
        "    (void)s;\n}\n"
    )
    assert gate.check_files({"src/x.c": src}, {}) == []
    code_call = src.replace("    (void)s;", "    virDomainFSFreeze(dom, NULL, 0, 0);\n    (void)s;")
    reasons = gate.check_files({"src/x.c": code_call}, {})
    assert len(reasons) == 1 and "virDomainFSFreeze" in reasons[0] and " f" in reasons[0], reasons


def test_unknown_allowlist_entry_fails():

    text = (ROOT / WORKER_FILE).read_text(encoding="utf-8")

    renamed = text.replace(WORKER_FN, WORKER_FN + "_renamed")
    reasons = gate.check_files({WORKER_FILE: renamed}, {(WORKER_FILE, WORKER_FN): "x"})
    assert any("stale" in r and WORKER_FN in r for r in reasons), reasons

    src = "static void w(void)\n{\n    int x = 0; (void)x;\n}\n"
    reasons = gate.check_files({"src/y.c": src}, {("src/y.c", "w"): "x"})
    assert any("stale" in r and "w" in r for r in reasons), reasons

    reasons = gate.check_files({}, {("src/none.c", "w"): "x"})
    assert any("stale" in r for r in reasons), reasons


def test_unbalanced_braces_fail_closed():

    src = "static void a(void)\n{\n#if X\n    if (1) {\n#else\n    if (2) {\n#endif\n    }\n"
    reasons = gate.check_files({"src/a.c": src}, {})
    assert any("fail-closed" in r for r in reasons), reasons
    cont = "#define M(x) \\\n    do { \\\n    } while (0)\nstatic void c(void)\n{\n    virDomainShutdown(d);\n}\n"
    assert [h[0] for h in gate.find_hits(cont)] == ["c"]


def test_self_test_flag_passes():

    result = subprocess.run([sys.executable, str(GATE), "--self-test"], capture_output=True, text=True)
    assert result.returncode == 0, f"{result.stdout}\n{result.stderr}"


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
    print(f"[test_mainloop_guest_agent] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
