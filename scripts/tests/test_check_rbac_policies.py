#!/usr/bin/env python3
                          
                                                                    
                                          
                                                                             
                                                                   
                                                  
 
                      
                                                     
                                 
                                                    

             
                                                            
                                
                                            
                                                          
                                                       
                                            
                                                                                   
                      
                                                                      
                                                                       
                                                                            
                                                                             
   
import contextlib
import io
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / "scripts" / "check_rbac_policies.py"
DISPATCHER = ROOT / "src" / "api" / "dispatcher.c"
                                                                                  
POLICY = ROOT / "src" / "api" / "dispatcher_policy.c"
sys.path.insert(0, str(ROOT / "scripts"))
import check_rbac_policies              
import rpc_extract              


def _rest_family() -> str:
                                                            
    return rpc_extract.rest_family_text()

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
    dfd, dpath = tempfile.mkstemp(suffix="-dispatcher.c")
    try:
        with os.fdopen(fd, "w") as handle:
            handle.write(rest_text)
        with os.fdopen(dfd, "w") as handle:
            handle.write(_real())
        return subprocess.run([sys.executable, str(GATE), dpath, path],
                              capture_output=True, text=True)
    finally:
        os.unlink(path)
        os.unlink(dpath)


def _real() -> str:
                                                        
                                                            
    return POLICY.read_text(errors="replace") + "\n" + DISPATCHER.read_text(errors="replace")


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


def test_removing_role_check_call_fails():
                                                                    
    text = _real()
    call = "_dispatch_role_check(method, params, rpc_id_str, server, connection, audit_username)"
    assert call in text
    result = _run(text.replace(call, "TRUE", 1))
    assert result.returncode == 1, result.stdout + result.stderr
    assert "ADR-0079" in result.stdout


def test_owner_check_before_role_check_fails():
                                     
    text = _real()
    start = text.index("void purecvisor_dispatcher_dispatch(")
    at = text.index("pcv_mainloop_guard_rpc_enter(method);", start)
    early = "if (!_dispatch_owner_check(method, params, rpc_id_str, server, connection, audit_username)) { }\n    "
    result = _run(text[:at] + early + text[at:])
    assert result.returncode == 1, result.stdout + result.stderr
    assert "ADR-0079" in result.stdout


def test_second_owner_call_outside_check_fails():
                                           
    text = _real()
    start = text.index("void purecvisor_dispatcher_dispatch(")
    at = text.index("pcv_mainloop_guard_rpc_enter(method);", start)
    extra = "gboolean allowed = pcv_disp_vm_owner_scoped_method_allowed(method, params, connection, 1, NULL);\n    "
    result = _run(text[:at] + extra + text[at:])
    assert result.returncode == 1, result.stdout + result.stderr
    assert "ADR-0079" in result.stdout


def test_rest_real_tree_passes():
    result = _run_rest(_rest_family())
    assert result.returncode == 0, result.stdout + result.stderr


RAW_STRSTR = '    const gchar *mp = strstr(rpc, "\\"method\\":\\"");\n'


def _drop_envelope_parse(text: str) -> str:
                                                            
    mutated, n = check_rbac_policies.REST_METHOD_CALL_RE.subn("PCV_REST_RPC_METHOD_OK", text, count=1)
    assert n == 1, "envelope 파싱 호출을 찾지 못함"
    return mutated


def _insert_raw_strstr(text: str) -> str:
                                                         
    m = check_rbac_policies.REST_PERMISSION_CALL_RE.search(text)
    assert m, "권한 판정 호출을 찾지 못함"
    line = text.rfind("\n", 0, m.start()) + 1
    return text[:line] + RAW_STRSTR + text[line:]


def _to_rq_form(text: str) -> str:
                                                                     
    text = re.sub(r"pcv_rest_rpc_envelope_method\s*\(\s*rpc\s*,", "pcv_rest_rpc_envelope_method(rq->rpc,", text)
    text = re.sub(r"pcv_rbac_check_permission\s*\(\s*subject\s*,", "pcv_rbac_check_permission(rq->subject,", text)
    assert "pcv_rest_rpc_envelope_method(rq->rpc," in text and "pcv_rbac_check_permission(rq->subject," in text
    return text


def test_rest_gate_without_envelope_parse_fails():
    result = _run_rest(_drop_envelope_parse(_rest_family()))
    assert result.returncode == 1, result.stdout + result.stderr
    assert "REST RBAC" in result.stdout


def test_rest_raw_strstr_method_fails():
    result = _run_rest(_insert_raw_strstr(_rest_family()))
    assert result.returncode == 1, result.stdout + result.stderr
    assert "strstr" in result.stdout


def test_rest_rq_context_form_passes_and_regressions_fail():
                                                                                         
    rq_form = _to_rq_form(_rest_family())
    result = _run_rest(rq_form)
    assert result.returncode == 0, result.stdout + result.stderr
    result = _run_rest(_drop_envelope_parse(rq_form))
    assert result.returncode == 1 and "REST RBAC" in result.stdout, result.stdout + result.stderr
    result = _run_rest(_insert_raw_strstr(rq_form))
    assert result.returncode == 1 and "strstr" in result.stdout, result.stdout + result.stderr


def test_unrelated_comment_passes():
    result = _run(_real() + "\n/* unrelated trailing comment */\n")
    assert result.returncode == 0, result.stdout + result.stderr


@contextlib.contextmanager
def _rest_root(files: dict):
                                                                                
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for rel, text in files.items():
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_text(text)
        saved = check_rbac_policies.REST_ROOT
        check_rbac_policies.REST_ROOT = root
        try:
            yield root
        finally:
            check_rbac_policies.REST_ROOT = saved


def _main_repo_mode() -> tuple:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = check_rbac_policies.main([])
    return rc, out.getvalue() + err.getvalue()


def _family_files() -> dict:
    return {p.relative_to(ROOT).as_posix(): p.read_text() for p in rpc_extract.rest_family()}


def test_repo_mode_raw_strstr_only_in_split_file_fails():
                                                                          
    files = _family_files()
    with _rest_root(files):
        rc, out = _main_repo_mode()
    assert rc == 0, out
    raw = ('static const gchar *\n_raw_method(const gchar *rpc)\n{\n'
           '    return strstr(rpc, "\\"method\\":\\"");\n}\n')
    split = "src/api/rest_request.c"
    with _rest_root({**files, split: files.get(split, "") + raw}):
        rc, out = _main_repo_mode()
    assert rc == 1 and "strstr" in out, out


def test_repo_mode_raw_strstr_reported_with_split_file_and_local_line():
                                                                      
    files = _family_files()
    split = "src/api/rest_request.c"
    orig = files[split]
    assert orig.endswith("\n")
    raw = ('static const gchar *\n_raw_method(const gchar *rpc)\n{\n'
           '    return strstr(rpc, "\\"method\\":\\"");\n}\n')
    expected = f"{split}:{orig.count(chr(10)) + 4}"
    with _rest_root({**files, split: orig + raw}):
        rc, out = _main_repo_mode()
    assert rc == 1 and expected in out, (expected, out)


def test_repo_mode_permission_call_absent_fails():
                                                  
    files = {rel: check_rbac_policies.REST_PERMISSION_CALL_RE.sub("TRUE", text)
             for rel, text in _family_files().items()}
    assert sum(check_rbac_policies.REST_PERMISSION_CALL_RE.search(t) is not None
               for t in _family_files().values()) == 1
    with _rest_root(files):
        rc, out = _main_repo_mode()
    assert rc == 1 and "pcv_rbac_check_permission" in out, out


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
