#!/usr/bin/env python3
                          
                                                           
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                         
                                                            

                           
                                                     
                                                       
                                                                        
                         
                                 
   
import contextlib
import io
import os
import re
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_secret_logging
import rpc_extract
from check_secret_logging import scan_text

REPO = Path(__file__).resolve().parents[2]

CALL_RE = re.compile(r"\b(?:pcv_rest)?_body_has_secret\(body_str, body_len\)")

DEF_LINE_RE = re.compile(r"^((?:pcv_rest)?_body_has_secret)(\s*\()", re.M)

RENAMED_SNIPPET = (
    'gboolean\npcv_rest_body_has_secret(const gchar *body, gsize len)\n{\n    return FALSE;\n}\n'
    'gboolean is_auth = (g_strstr_len(path, -1, "/auth/token") != nullptr) ||\n'
    '                   pcv_rest_body_has_secret(body_str, body_len);\n')

GATE = Path(__file__).resolve().parent.parent / "check_secret_logging.py"

DEF = 'static gboolean _body_has_secret(const gchar *body, gsize len) { return FALSE; }\n'

NEW_SNIPPET = DEF + '''
gboolean is_auth = (g_strstr_len(path, -1, "/auth/token") != nullptr) ||
                   _body_has_secret(body_str, body_len);
'''

                                  
OLD_SNIPPET = DEF + '''
gboolean is_auth = (g_strstr_len(path, -1, "/auth/token") != nullptr) ||
                   (g_strstr_len(path, -1, "/auth/refresh") != nullptr);
'''

def test_webhook_log_contract_current_tree():
    assert check_secret_logging.check_webhook_contract(REPO) == []

def test_body_mask_covers_cloud_init_user_data():

    text = (Path(REPO) / "src/api/rest_request.c").read_text(encoding="utf-8")
    assert '"user_data"' in text[text.index("secret_keys[]"):text.index("nullptr", text.index("secret_keys[]"))]

def test_webhook_log_raw_url_red():
    snippet = 'PCV_LOG_WARN(ALERT_LOG_DOM, "Webhook retry %d/%d failed for %.100s",\n    attempt, max, url);\n'
    found = check_secret_logging.webhook_log_violations(snippet)
    assert len(found) == 1 and found[0][0] == 1

def test_webhook_log_startup_snapshot_red():
    snippet = ('PCV_LOG_INFO(D, "started webhook=%s", cfg.webhook_url[0] ? cfg.webhook_url : "(none)");\n')
    assert len(check_secret_logging.webhook_log_violations(snippet)) == 1

def test_webhook_log_redacted_and_presence_pass():
    snippet = ('PCV_LOG_WARN(D, "failed for %s", origin);\n'
               'PCV_LOG_INFO(D, "url webhook=%s", c.webhook_url[0] ? "(configured)" : "(none)");\n'
               'PCV_LOG_INFO(D, "literal url word only");\n')
    assert check_secret_logging.webhook_log_violations(snippet) == []

def test_webhook_log_current_engine_reverted_red():
    src = (REPO / "src/modules/daemons/alert_engine.c").read_text()
    needle = 'attempt + 1, max_retries, origin);'
    assert needle in src, "정본에서 재시도 로그 미발견 — 소스 변경?"
    assert check_secret_logging.webhook_log_violations(src.replace(needle, 'attempt + 1, max_retries, url);'))

def test_body_mask_key_required():
    src = (REPO / "src/api/rest_request.c").read_text()
    assert check_secret_logging.body_mask_has_webhook(src)
    assert '"webhook",' in src, "정본에서 마스킹 키 미발견 — 소스 변경?"
    assert not check_secret_logging.body_mask_has_webhook(src.replace('"webhook",', ''))

def test_rest_raw_log_current_tree():
    assert check_secret_logging.check_rest_raw_log_contract(REPO) == []

def test_rest_rpc_log_red():
    snippet = 'x();\nPCV_LOG_INFO(REST_LOG_DOM, "→ RPC %.100s", rq->rpc);\n'
    found = check_secret_logging.rest_raw_log_violations(snippet)
    assert len(found) == 1 and found[0][0] == 2

def test_rest_raw_log_variants_red():
    for snippet in ('g_message("rpc=%s", rpc_with_context);\n',
                    'g_warning("async rpc %s",\n    actx->rpc);\n',
                    'PCV_LOG_WARN(D, "body=%.*s", (int)body_len, body_str);\n'):
        assert len(check_secret_logging.rest_raw_log_violations(snippet)) == 1, snippet

def test_rest_method_only_log_passes():
    snippet = ('PCV_LOG_INFO(REST_LOG_DOM, "→ RPC %.100s", rpc_method);\n'
               'PCV_LOG_INFO(D, "literal rq->rpc word %d", body_len);\n'
               '/* PCV_LOG_INFO(D, "%s", rq->rpc); */\n')
    assert check_secret_logging.rest_raw_log_violations(snippet) == []

def test_rest_raw_log_current_source_reverted_red():
    src = (REPO / "src/api/rest_server.c").read_text()
    needle = '"→ RPC %.100s", rpc_method);'
    assert needle in src, "정본에서 → RPC 로그 미발견 — 소스 변경?"
    assert check_secret_logging.rest_raw_log_violations(src.replace(needle, '"→ RPC %.100s", rq->rpc);'))


def test_new_snippet_used():
                                                      
    defined, used = scan_text(NEW_SNIPPET)
    assert defined and used


def test_old_snippet_not_used():
                                                          
    defined, used = scan_text(OLD_SNIPPET)
    assert defined and not used, "경로-only is_auth가 여전히 used로 잡힘"


def test_missing_definition():
                                     
    snippet = ('gboolean is_auth = (x) || _body_has_secret(body_str, body_len);')
    defined, used = scan_text(snippet)
    assert not defined and used


def test_gate_passes_on_current_tree():
                                     
    r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"


def test_reverted_source_fails():
                                                                   
                                    

                                                                
                                                                    
                                                      
    orig = rpc_extract.rest_family_text()
    assert CALL_RE.search(orig), "is_auth의 _body_has_secret 호출을 찾지 못함 — 소스 포맷 변경?"
    reverted = CALL_RE.sub("FALSE", orig)
    assert reverted != orig, "is_auth OR 항 패치가 적용되지 않음 — 소스 포맷 변경?"
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(reverted)
        tmp = f.name
    try:
        r = subprocess.run([sys.executable, str(GATE), tmp], capture_output=True, text=True)
        assert r.returncode == 1, f"되돌린 사본에서 게이트가 RED가 아님:\n{r.stdout}\n{r.stderr}"
    finally:
        os.unlink(tmp)

def test_renamed_nonstatic_helper_recognized():

    defined, used = scan_text(RENAMED_SNIPPET)
    assert defined and used

def test_call_only_is_not_definition():

    proto = 'gboolean pcv_rest_body_has_secret(const gchar *body, gsize len);\n'
    call = 'gboolean is_auth = (x) || pcv_rest_body_has_secret(body_str, body_len);\n'
    defined, used = scan_text(proto + call)
    assert not defined and used

def _run_gate_on(text: str) -> subprocess.CompletedProcess:
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(text)
        tmp = f.name
    try:
        return subprocess.run([sys.executable, str(GATE), tmp], capture_output=True, text=True)
    finally:
        os.unlink(tmp)

def test_family_definition_absent_fails():

    orig = rpc_extract.rest_family_text()
    gone = DEF_LINE_RE.sub(r"\1_gone\2", orig)
    assert gone != orig, "헬퍼 정의를 찾지 못함 — 소스 포맷 변경?"
    r = _run_gate_on(gone)
    assert r.returncode == 1 and "정의가 없음" in r.stderr, f"{r.stdout}\n{r.stderr}"

@contextlib.contextmanager
def _family_root(files: dict):

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for rel, text in files.items():
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_text(text)
        saved = check_secret_logging.ROOT
        check_secret_logging.ROOT = root
        try:
            yield root
        finally:
            check_secret_logging.ROOT = saved

def _main_rc() -> tuple:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = check_secret_logging.main([])
    return rc, out.getvalue() + err.getvalue()

def test_default_mode_reads_split_file():

    family = {p.relative_to(REPO).as_posix(): p.read_text() for p in rpc_extract.rest_family()}
    base = {rel: DEF_LINE_RE.sub(r"\1_gone\2", CALL_RE.sub("pcv_rest_body_has_secret(body_str, body_len)", t))
            for rel, t in family.items()}
    assert sum(len(CALL_RE.findall(t)) for t in base.values()) == 1, "is_auth 호출 사이트가 하나가 아님"
    split = 'gboolean\npcv_rest_body_has_secret(const gchar *body, gsize len)\n{\n    return FALSE;\n}\n'
    request_c = "src/api/rest_request.c"
    with _family_root({**base, request_c: base.get(request_c, "") + split}):
        rc, out = _main_rc()
    assert rc == 0, f"분할 파일의 새 이름 정의를 못 읽음:\n{out}"
    with _family_root(base):
        rc, out = _main_rc()
    assert rc == 1 and "정의가 없음" in out, f"정의 0건인데 RED가 아님:\n{out}"


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
    print(f"[test_secret_logging] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
