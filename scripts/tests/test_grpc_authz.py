#!/usr/bin/env python3
                          
                                                             
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                           
                                                          

                                      
                                                       
                                  
                        
                                
                                                   
   
import os
import re
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_grpc_authz import (              
    scan_text, check_no_token_refusal, strip_code, strip_comments, _match_brace,
    ROOT, TARGET, INJECT_TOKENS,
)

GATE = Path(__file__).resolve().parent.parent / "check_grpc_authz.py"
REST_REQUEST = ROOT / "src" / "api" / "rest_request.c"
GRPC_INJECT_FN = "_grpc_inject_caller_identity"
REST_ATTACH_FN = "pcv_rest_rpc_attach_auth_context"
TENANT_REMOVE_RE = re.compile(r'json_object_remove_member\(\s*params\s*,\s*"_pcv_caller_tenant"\s*\)')
TENANT_SET_RE = re.compile(
    r'json_object_set_(?:string|null)_member\(\s*params\s*,\s*"_pcv_caller_tenant"')

                                                              
_INJECT_HELPER = '''
static char *inject(const char *p, int role) {
    json_object_set_int_member(o, "_pcv_caller_role", role);
    json_object_set_string_member(o, "_pcv_caller_sub", "grpc");
    json_object_set_null_member(o, "_pcv_caller_tenant");
    return 0;
}
'''

PASS_FILE = _INJECT_HELPER + '''
void pcv_grpc_server_start(void) {
    G_grpc_auth_token = dup(cfg);
    if (!G_grpc_auth_token || !*G_grpc_auth_token) {
        LOG("refuse");
        return;
    }
    start_thread();
}
'''

FALLTHROUGH_FILE = _INJECT_HELPER + '''
void pcv_grpc_server_start(void) {
    G_grpc_auth_token = dup(cfg);
    if (!G_grpc_auth_token || !*G_grpc_auth_token) {
        if (bind != loop) { WARN("x"); return; }
        LOG("restricted to 127.0.0.1");
    }
    start_thread();
}
'''

NO_INJECT_FILE = '''
static char *inject(const char *p, int role) {
    return 0;
}
void pcv_grpc_server_start(void) {
    G_grpc_auth_token = dup(cfg);
    if (!G_grpc_auth_token || !*G_grpc_auth_token) {
        LOG("refuse");
        return;
    }
    start_thread();
}
'''


def _run_gate_on(text: str) -> int:
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(text)
        tmp = f.name
    try:
        r = subprocess.run([sys.executable, str(GATE), tmp],
                           capture_output=True, text=True)
        return r.returncode
    finally:
        os.unlink(tmp)


def test_current_tree_pass():
                             
    r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"


def test_current_tokens_and_refusal():
                                     
    text = TARGET.read_text()
    missing, refusal_ok, reason = scan_text(text)
    assert missing == [], f"주입 토큰 누락: {missing}"
    assert refusal_ok, f"무토큰 거부 미인식: {reason}"


def test_refusal_logic_return_ok():
                                    
    ok, _ = check_no_token_refusal(strip_code(PASS_FILE))
    assert ok


def test_refusal_logic_fallthrough_fails():
                                                
    ok, reason = check_no_token_refusal(strip_code(FALLTHROUGH_FILE))
    assert not ok, f"fall-through인데 ok로 판정: {reason}"


def test_gate_pass_on_synthetic():
                                           
    assert _run_gate_on(PASS_FILE) == 0


def test_gate_red_on_fallthrough():
                                          
    assert _run_gate_on(FALLTHROUGH_FILE) == 1


def test_gate_red_on_no_inject():
                                  
    assert _run_gate_on(NO_INJECT_FILE) == 1


def test_real_file_injection_removed_fails():
                                                        
    orig = TARGET.read_text()
    for tok in INJECT_TOKENS:
        assert tok in orig, f"{tok} 를 정본에서 찾지 못함 — 소스 변경?"
    reverted = orig
    for tok in INJECT_TOKENS:
        reverted = reverted.replace(tok, "_removed_key")
    assert _run_gate_on(reverted) == 1, "주입 토큰 제거 사본에서 게이트가 RED가 아님"


def _function_body(text: str, name: str):

    code = strip_code(text)
    m = re.search(r'\b' + re.escape(name) + r'\s*\([^;{]*\)\s*\{', code)
    if not m:
        return None
    close = _match_brace(code, m.end() - 1)
    if close < 0:
        return None
    return strip_comments(text)[m.end() - 1:close + 1]

def grpc_inject_strips_then_sets_tenant(text: str) -> tuple:

    body = _function_body(text, GRPC_INJECT_FN)
    if body is None:
        return False, f"{GRPC_INJECT_FN} 정의를 찾지 못함"
    rm = TENANT_REMOVE_RE.search(body)
    if not rm:
        return False, "클라이언트 _pcv_caller_tenant 제거(json_object_remove_member) 없음 — 위조 키가 남는다"
    sets = [m.start() for m in TENANT_SET_RE.finditer(body)]
    if not sets:
        return False, "_pcv_caller_tenant 설정(문자열 또는 JSON null) 없음 — RBAC 사용자 grpc의 테넌트를 물려받는다"
    if rm.start() > min(sets):
        return False, "_pcv_caller_tenant 설정이 제거보다 앞선다 — 서버 값이 지워지거나 클라이언트 값이 남는다"
    if '\\"_pcv_caller_tenant\\":null' not in body:
        return False, "파싱 실패 폴백 문자열에 \"_pcv_caller_tenant\":null 없음"
    return True, "제거 → 설정 순서, 폴백 null"

def rest_attach_strips_tenant(text: str) -> tuple:

    body = _function_body(text, REST_ATTACH_FN)
    if body is None:
        return False, f"{REST_ATTACH_FN} 정의를 찾지 못함"
    if not TENANT_REMOVE_RE.search(body):
        return False, "REST 본문의 _pcv_caller_tenant 제거 없음 — 요청자가 테넌트를 위조할 수 있다"
    return True, "제거 있음"

def _drop_remove_line(text: str) -> str:

    out, n = re.subn(r'^[^\n]*json_object_remove_member\(\s*params\s*,\s*"_pcv_caller_tenant"\s*\)[^\n]*\n',
                     '', text, count=1, flags=re.M)
    assert n == 1, "정본에서 _pcv_caller_tenant 제거 줄을 찾지 못함 — 소스 변경?"
    return out

def test_grpc_inject_strips_and_sets_tenant():

    grpc = TARGET.read_text()
    rest = REST_REQUEST.read_text()
    ok, why = grpc_inject_strips_then_sets_tenant(grpc)
    assert ok, why
    ok, why = rest_attach_strips_tenant(rest)
    assert ok, why
    assert not grpc_inject_strips_then_sets_tenant(_drop_remove_line(grpc))[0], "gRPC 제거 줄을 빼도 통과"
    assert not rest_attach_strips_tenant(_drop_remove_line(rest))[0], "REST 제거 줄을 빼도 통과"

def test_grpc_tenant_order_counterfactual():

    good = (f"static char *{GRPC_INJECT_FN}(const char *j, int r, const char *t) {{\n"
            '    json_object_remove_member(params, "_pcv_caller_tenant");\n'
            '    json_object_set_null_member(params, "_pcv_caller_tenant");\n'
            '    out = g_strdup_printf("{\\"_pcv_caller_tenant\\":null}");\n'
            "}\n")
    assert grpc_inject_strips_then_sets_tenant(good)[0]
    swapped = good.replace('    json_object_remove_member(params, "_pcv_caller_tenant");\n', '', 1).replace(
        "    out =", '    json_object_remove_member(params, "_pcv_caller_tenant");\n    out =', 1)
    assert not grpc_inject_strips_then_sets_tenant(swapped)[0]
    no_set = good.replace('    json_object_set_null_member(params, "_pcv_caller_tenant");\n', '', 1)
    assert not grpc_inject_strips_then_sets_tenant(no_set)[0]
    no_fallback = good.replace(':null', ':0', 1)
    assert not grpc_inject_strips_then_sets_tenant(no_fallback)[0]

if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items())
             if k.startswith("test_") and callable(v)]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"OK   {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"[test_grpc_authz] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
