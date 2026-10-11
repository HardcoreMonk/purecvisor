#!/usr/bin/env python3






















import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / "scripts" / "check_rbac_policies.py"
DISPATCHER = ROOT / "src" / "api" / "dispatcher.c"
REST_SERVER = ROOT / "src" / "api" / "rest_server.c"

ROUTE_ANCHOR = 'g_hash_table_insert(g_rpc_routes, "vm.list",'
POLICY_END = "    { NULL, 0 }\n};"


def _run(text: str | None = None) -> subprocess.CompletedProcess:
    if text is None:
        return subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    fd, path = tempfile.mkstemp(suffix="-dispatcher.c")
    try:
        with os.fdopen(fd, "w") as handle:
            handle.write(text)
        return subprocess.run([sys.executable, str(GATE), path],
                              capture_output=True, text=True)
    finally:
        os.unlink(path)


def _run_rest(rest_text: str) -> subprocess.CompletedProcess:
    fd, path = tempfile.mkstemp(suffix="-rest_server.c")
    try:
        with os.fdopen(fd, "w") as handle:
            handle.write(rest_text)
        return subprocess.run([sys.executable, str(GATE), str(DISPATCHER), path],
                              capture_output=True, text=True)
    finally:
        os.unlink(path)


def _real() -> str:
    return DISPATCHER.read_text(errors="replace")


def _drop_policy_row(text: str, method: str) -> str:
    lines = text.splitlines(keepends=True)
    kept = [line for line in lines if not line.lstrip().startswith(f'{{ "{method}"')]
    assert len(kept) < len(lines), f"정책행 {method}를 찾지 못함"
    return "".join(kept)


def _add_route(text: str, method: str) -> str:
    assert ROUTE_ANCHOR in text
    route = f'g_hash_table_insert(g_rpc_routes, "{method}", (gpointer)handle_vm_list_request);\n    '
    return text.replace(ROUTE_ANCHOR, route + ROUTE_ANCHOR, 1)


def _add_policy_row(text: str, method: str, role: int) -> str:
    assert POLICY_END in text
    return text.replace(POLICY_END, f'    {{ "{method}", {role} }},\n' + POLICY_END, 1)


def test_current_tree_pass():
    result = _run()
    assert result.returncode == 0, result.stdout + result.stderr


def test_dropping_viewer_row_fails():
    result = _run(_drop_policy_row(_real(), "vm.list"))
    assert result.returncode == 1, result.stdout + result.stderr
    assert "vm.list" in result.stdout


def test_new_unmapped_route_fails():
    result = _run(_add_route(_real(), "zz.adr0078.read"))
    assert result.returncode == 1, result.stdout + result.stderr
    assert "zz.adr0078.read" in result.stdout


def test_new_route_with_policy_passes():
    text = _add_policy_row(_add_route(_real(), "zz.adr0078.read"), "zz.adr0078.read", 0)
    result = _run(text)
    assert result.returncode == 0, result.stdout + result.stderr


def test_dropping_destructive_row_fails():
    result = _run(_drop_policy_row(_real(), "vm.delete"))
    assert result.returncode == 1, result.stdout + result.stderr
    assert "vm.delete" in result.stdout


def test_removing_alias_wiring_fails():
    text = _real()
    call = "!pcv_dispatcher_vm_target_aliases_valid(method, params, &alias_error)"
    assert call in text
    result = _run(text.replace(call, "FALSE", 1))
    assert result.returncode == 1, result.stdout + result.stderr
    assert "ADR-0079" in result.stdout


def test_alias_reject_without_invalid_params_fails():
    text = _real()
    anchor = "!pcv_dispatcher_vm_target_aliases_valid(method, params, &alias_error)"
    start = text.index(anchor)
    end = text.index("_vm_owner_scoped_method_allowed(", start)
    segment = text[start:end].replace("PURE_RPC_ERR_INVALID_PARAMS", "PURE_RPC_ERR_FORBIDDEN")
    result = _run(text[:start] + segment + text[end:])
    assert result.returncode == 1, result.stdout + result.stderr


def test_rest_real_tree_passes():
    result = _run_rest(REST_SERVER.read_text(errors="replace"))
    assert result.returncode == 0, result.stdout + result.stderr


def test_rest_gate_without_envelope_parse_fails():
    text = REST_SERVER.read_text(errors="replace")
    call = "pcv_rest_rpc_envelope_method(rpc, &rpc_method)"
    assert call in text
    result = _run_rest(text.replace(call, "PCV_REST_RPC_METHOD_OK", 1))
    assert result.returncode == 1, result.stdout + result.stderr
    assert "REST RBAC" in result.stdout


def test_rest_raw_strstr_method_fails():
    text = REST_SERVER.read_text(errors="replace")
    raw = '    const gchar *mp = strstr(rpc, "\\"method\\":\\"");\n'
    anchor = "    gchar *rpc_method = NULL;"
    assert anchor in text
    result = _run_rest(text.replace(anchor, raw + anchor, 1))
    assert result.returncode == 1, result.stdout + result.stderr
    assert "strstr" in result.stdout


def test_unrelated_comment_passes():
    result = _run(_real() + "\n/* unrelated trailing comment */\n")
    assert result.returncode == 0, result.stdout + result.stderr


def main() -> int:
    tests = [value for name, value in sorted(globals().items())
             if name.startswith("test_") and callable(value)]
    for test in tests:
        test()
        print(f"ok {test.__name__}")
    print(f"PASS {len(tests)} tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
