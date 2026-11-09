#!/usr/bin/env python3
                          
                                                            
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                          
                                       

import contextlib
import io
import re
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_proxy_identity
import rpc_extract
from check_proxy_identity import (              
    FORBIDDEN,
    WS_TARGET,
    scan_text,
)

REPO = Path(__file__).resolve().parents[2]

NAMED_FUNCTIONS = {
    "pcv_rest_auth_audit_address": "identity seam",
    "pcv_rest_rate_limit_key_for_message": "rate-limit seam",
    "_send_json": "HSTS/external HTTPS",
    "_authenticate": "auth invalid API key audit",
    "_on_request": "request security identity",
}

BLOCK_FUNCTIONS = {
    "pcv_rest_prelude": "rate-limit callsite",
    "pcv_rest_auth_token": "brute-force/security IP",
    "pcv_rest_auth_password": "password audit",
    "pcv_rest_auth_register": "register audit",
    "pcv_rest_auth_totp_verify": "totp audit",
}

def _rest() -> str:

    return rpc_extract.rest_family_text()

def _apply_rest_renames(text: str) -> str:

    for old, new in rpc_extract.rest_renames().items():
        text = re.sub(rf"\b{re.escape(old)}\b", new, text)
    return text

def _drop_definition(text: str, name: str) -> str:

    for candidate in {name, rpc_extract.rest_renames().get(name, name)}:
        text = re.sub(rf"^({re.escape(candidate)})(\s*\()", r"\1_gone\2", text, flags=re.M)
    return text


def _replace_nth(source: str, old: str, new: str, nth: int = 1) -> str:
    start = 0
    for _ in range(nth):
        found = source.find(old, start)
        assert found >= 0, f"{nth}번째 mutation 대상 미발견: {old}"
        start = found + len(old)
    found = start - len(old)
    return source[:found] + new + source[found + len(old):]


def _assert_red(source: str, scope: str) -> None:
    reasons = scan_text(source)
    assert any(reason.startswith(scope) for reason in reasons), reasons


def test_current_tree_passes():
    assert scan_text(_rest()) == []


def test_comment_does_not_trigger_direct_peer_gate():
    source = _rest() + f"\n/* {FORBIDDEN}(msg); */\n"
    assert scan_text(source) == []


def test_direct_peer_lookup_is_red():
    source = _rest().replace(
        "const gchar *request_client_ip =",
        f"(void){FORBIDDEN}(msg); const gchar *request_client_ip =",
        1,
    )
    assert any(FORBIDDEN in reason for reason in scan_text(source))


IDENTITY = REPO / "src/api/rest_client_identity.c"

def test_collector_gate_missing_is_red():
    text = _rest()
    gate = "if (!pcv_rest_collector_gate(msg)) return PCV_REST_STEP_CLEANUP;"
    assert text.count(gate) == 5, "수집기 처리부 다섯 곳의 문지기 호출이 아님"
    _assert_red(text.replace(gate, "", 1), "collector gate")

def test_collector_gate_after_body_is_red():
    text = _rest()
    body = "        pcv_rest_defer_rendered(msg, rq->self->rest_ctx, _internal_vms_render, NULL, NULL);\n"
    anchors = re.findall(
        r"(?m)^        if \(!pcv_rest_collector_gate\(msg\)\) return PCV_REST_STEP_CLEANUP;[^\n]*\n" + re.escape(body),
        text,
    )
    assert len(anchors) == 1, anchors
    anchor = anchors[0]
    gate = anchor[: -len(body)]
    _assert_red(text.replace(anchor, body + gate, 1), "collector gate")

def test_new_public_path_above_gate_is_red():
    text = _rest()
    anchor = 'if (g_strcmp0(path, REST_API_PREFIX "/version") == 0'
    assert anchor in text
    injected = 'if (g_strcmp0(path, REST_API_PREFIX "/debug/dump") == 0) { return PCV_REST_STEP_CLEANUP; }\n    '
    _assert_red(text.replace(anchor, injected + anchor, 1), "public path allowlist")

def test_identity_loopback_literal_trust_is_red():
    ident = IDENTITY.read_text()
    call = "pcv_rest_proxy_trusted(_active_trust(), peer_ip, proxy_auth)"
    assert call in ident
    reasons = scan_text(_rest(), identity_text=ident.replace(call, "is_loopback_peer(peer_ip)", 1))
    assert any(r.startswith("proxy trust") for r in reasons), reasons

def test_identity_old_trust_function_is_red():
    ident = IDENTITY.read_text() + "\nstatic gboolean is_trusted_loopback(const gchar *p) { return p != NULL; }\n"
    reasons = scan_text(_rest(), identity_text=ident)
    assert any("is_trusted_loopback" in r for r in reasons), reasons

def test_identity_direct_local_dropped_is_red():
    ident = IDENTITY.read_text()
    call = "identity->direct_local = pcv_rest_is_direct_local("
    assert call in ident
    mutated = ident.replace(call, "identity->direct_local = FALSE && pcv_rest_is_direct_local_gone(", 1)
    reasons = scan_text(_rest(), identity_text=mutated)
    assert any(r.startswith("direct local") for r in reasons), reasons

def test_auth_failure_callsites_are_independently_red():
    source = _rest()
    call = "pcv_rest_auth_audit_address(msg)"

    body = rpc_extract.function_body_in(
        source, rpc_extract.rest_fn_name_in(source, "_authenticate"))
    assert body is not None
    start = source.index(body)
    for nth, scope in (
        (1, "auth invalid API key audit"),
        (2, "auth missing header audit"),
        (3, "auth invalid JWT audit"),
    ):
        mutated_body = _replace_nth(body, call, '"unknown"', nth)
        _assert_red(source[:start] + mutated_body + source[start + len(body):], scope)


def test_rate_limit_callsite_bypass_is_red_even_with_decoy():
    source = _rest()
    source += """
    static void dead_decoy(SoupServerMessage *msg,
                           const gchar *path,
                           const gchar *method) {
        (void)pcv_rest_rate_limit_key_for_message(msg, path, method);
    }
    """
    source = source.replace(
        "pcv_rest_rate_limit_key_for_message(msg, path, method)",
        "pcv_build_rate_limit_key(request_client_ip, path, method)",
        1,
    )
    _assert_red(source, "rate-limit callsite")


def test_inactive_inside_region_decoy_cannot_replace_rate_callsite():
    source = _rest().replace(
        "pcv_rest_rate_limit_key_for_message(msg, path, method)",
        """pcv_build_rate_limit_key(request_client_ip, path, method)
#if 0
        pcv_rest_rate_limit_key_for_message(msg, path, method)
#endif""",
        1,
    )
    _assert_red(source, "rate-limit callsite")


def test_unused_macro_decoy_cannot_replace_rate_callsite():
    source = _rest().replace(
        "pcv_rest_rate_limit_key_for_message(msg, path, method)",
        "pcv_build_rate_limit_key(request_client_ip, path, method)",
        1,
    )
    source += """
#define UNUSED_PROXY_IDENTITY_DECOY(msg, path, method) \\
    pcv_rest_rate_limit_key_for_message(msg, path, method)
"""
    _assert_red(source, "rate-limit callsite")


def test_mutation_access_audit_bypass_is_red():
    source = _rest().replace(
        "const gchar *remote_str = request_client_ip;",
        'const gchar *remote_str = "unknown";',
        1,
    )
    _assert_red(source, "mutation/access audit")


def test_brute_force_security_ip_bypass_is_red():
    source = _rest().replace(
        "pcv_rbac_get_ip_remaining_lockout(client_ip)",
        'pcv_rbac_get_ip_remaining_lockout("unknown")',
        1,
    )
    _assert_red(source, "brute-force/security IP")


def test_login_audit_bypass_is_red():

    source = _rest().replace(
        '"login attempt",\n                      "fail", 401, 0, client_ip);',
        '"login attempt",\n                      "fail", 401, 0, "unknown");',
        1,
    )
    _assert_red(source, "login audit auth.failed")


def test_login_run_missing_is_red():

    source = re.sub(r"^pcv_rest_login_run(\s*\()", r"pcv_rest_login_run_gone\1", _rest(), flags=re.M)
    _assert_red(source, "brute-force/security IP")

def test_login_start_drops_client_ip_is_red():

    source = _rest().replace("password, client_ip);\n        if (body)", 'password, "unknown");\n        if (body)', 1)
    assert source != _rest()
    _assert_red(source, "brute-force/security IP")

def test_login_run_ip_recheck_bypass_is_red():

    run = "gint ip_lockout = pcv_rbac_get_ip_remaining_lockout(client_ip);\n    if (ip_lockout > 0) {\n        _set_error"
    source = _rest()
    assert run in source
    _assert_red(source.replace(run, run.replace("(client_ip)", '("unknown")'), 1), "brute-force/security IP")

def test_login_run_other_address_is_red():

    source = _rest().replace("client_ip = in->client_ip;", 'client_ip = "unknown";', 1)
    assert source != _rest()
    _assert_red(source, "brute-force/security IP")

def test_login_bootstrap_refusal_audit_bypass_is_red():
    source = _rest().replace(
        '"bootstrap recovery refused", "fail", 401, 0, client_ip);',
        '"bootstrap recovery refused", "fail", 401, 0, "unknown");',
        1,
    )
    _assert_red(source, "login audit auth.bootstrap.fallback")


def test_password_and_register_audits_are_independently_red():
    source = _rest()
    assignment = "const gchar *rip = request_client_ip;"
    _assert_red(
        _replace_nth(source, assignment, 'const gchar *rip = "unknown";', 1),
        "password audit",
    )
    _assert_red(
        _replace_nth(source, assignment, 'const gchar *rip = "unknown";', 2),
        "register audit",
    )
    for old, scope in (
        ('"fail", code, 0, rip);', "password audit"),
        ('"ok", 0, 0, rip);', "password audit"),
        ('"fail", 409, 0, rip);', "register audit"),
    ):
        _assert_red(source.replace(old, old.replace("rip", '"unknown"'), 1),
                    scope)
    register_success = _replace_nth(
        source, '"ok", 0, 0, rip);',
        '"ok", 0, 0, "unknown");', 2)
    _assert_red(register_success, "register audit")


def test_each_mutation_access_log_callsite_is_red():
    source = _rest()
    marker = "method, path, remote_str"
    _assert_red(
        _replace_nth(source, marker, 'method, path, "unknown"', 1),
        "mutation/access audit")
    _assert_red(
        _replace_nth(source, marker, 'method, path, "unknown"', 2),
        "mutation/access audit")


def test_external_https_hsts_bypass_is_red():
    source = _rest().replace(
        "pcv_rest_client_identity_is_external_https(identity)",
        "FALSE",
        1,
    )
    _assert_red(source, "HSTS/external HTTPS")


def test_ws_current_tree_passes():
    assert scan_text(_rest(), WS_TARGET.read_text()) == []


def test_ws_consumer_bypass_is_red_even_with_global_decoy():
    rest = _rest()
    ws = WS_TARGET.read_text().replace(
        "pcv_rest_client_identity_get_client_ip(identity)",
        '"127.0.0.1"',
        1,
    )
    ws += """
    static void dead_ws_identity_decoy(SoupServerMessage *msg) {
        (void)pcv_rest_client_identity_get(msg);
    }
    """
    reasons = scan_text(rest, ws)
    assert any(reason.startswith("WebSocket per-IP consumer")
               for reason in reasons), reasons


def test_ws_inactive_identity_decoy_is_red():
    rest = _rest()
    ws = WS_TARGET.read_text().replace(
        "const PcvRestClientIdentity *identity =",
        """#if 0
        const PcvRestClientIdentity *identity =
            pcv_rest_client_identity_get(msg);
#endif
        const PcvRestClientIdentity *identity =""",
        1,
    )
    reasons = scan_text(rest, ws)
    assert any(reason.startswith("WebSocket per-IP consumer")
               for reason in reasons), reasons


def test_ws_macro_identity_decoy_is_red():
    rest = _rest()
    ws = WS_TARGET.read_text().replace(
        "pcv_rest_client_identity_get(msg)",
        '"identity-decoy"',
        1,
    )
    ws += """
#define WS_IDENTITY_DECOY(msg) pcv_rest_client_identity_get(msg)
"""
    reasons = scan_text(rest, ws)
    assert any(reason.startswith("WebSocket per-IP consumer")
               for reason in reasons), reasons

def test_renamed_family_passes():

    renamed = _apply_rest_renames(_rest())
    assert "pcv_rest_send_json(" in renamed and not re.search(r"^_send_json\(", renamed, re.M)
    assert scan_text(renamed) == []

def test_each_named_function_missing_is_red():

    for text in (_rest(), _apply_rest_renames(_rest())):
        for name, scope in NAMED_FUNCTIONS.items():
            mutated = _drop_definition(text, name)
            assert mutated != text, f"정의를 못 찾아 변조 불가: {name}"
            _assert_red(mutated, scope)

def _manifest_rows() -> list[dict]:
    import split_target
    rel = split_target.load("rest_server").blocks
    rows, header = [], None
    for line in (REPO / rel).read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        cols = line.split("\t")
        if header is None:
            header = cols
        else:
            rows.append(dict(zip(header, cols)))
    return rows

def _split_shape(text: str, functions=tuple(BLOCK_FUNCTIONS)) -> str:

    rows = _manifest_rows()
    out = text
    for fn in functions:
        if rpc_extract.function_body_in(out, fn) is not None:
            continue
        body = rpc_extract.function_body_in(out, "_on_request")
        assert body is not None
        base = out.index(body)
        lines = body.splitlines(keepends=True)
        offs = [0]
        for ln in lines:
            offs.append(offs[-1] + len(ln))
        idx = next(i for i, r in enumerate(rows) if r["function"] == fn)

        def start_of(row):
            for i, ln in enumerate(lines):
                if ln.rstrip("\n") == row["anchor"]:
                    return i - int(row["lead"])
            if row["function"] != "-":
                for i, ln in enumerate(lines):
                    if re.search(rf"\b{re.escape(row['function'])}\s*\(\s*rq\s*\)", ln):
                        return i
            return None

        lo = start_of(rows[idx])
        assert lo is not None, f"{fn}: anchor를 _on_request에서 못 찾음"
        hi = next(h for h in (start_of(r) for r in rows[idx + 1:]) if h is not None)
        cut = "".join(lines[lo:hi]).replace("goto cleanup;", "return PCV_REST_STEP_CLEANUP;")
        func = ("\n/* ==== src/api/rest_mock_split.c ==== */\nPcvRestStep\n" + fn + "(PcvRestRequest *rq)\n{\n"
                "    SoupServerMessage *msg = rq->msg;\n    const gchar *path = rq->path;\n"
                "    const gchar *method = rq->method;\n    const gchar *request_client_ip = rq->request_client_ip;\n"
                + cut + "    return PCV_REST_STEP_NEXT;\n}\n")
        new_body = "".join(lines[:lo]) + f"    PCV_REST_RUN({fn}(rq));\n" + "".join(lines[hi:])
        out = out[:base] + new_body + out[base + len(body):] + func
    if not re.search(check_proxy_identity.RQ_IP_COPY_RE, out):
        snap = re.search(check_proxy_identity.SNAPSHOT_RE + r"\s*;", out)
        assert snap is not None
        out = out[:snap.end()] + "\n    rq->request_client_ip = request_client_ip;" + out[snap.end():]
    return out

def test_split_shape_passes():

    split = _split_shape(_rest())
    for fn in BLOCK_FUNCTIONS:
        assert rpc_extract.function_body_in(split, fn) is not None, fn
        assert re.search(rf"PCV_REST_RUN\({fn}\(rq\)\)", split), fn
    assert scan_text(split) == []

def test_split_shape_block_definition_missing_is_red():

    split = _split_shape(_rest())
    for fn, scope in BLOCK_FUNCTIONS.items():
        mutated = re.sub(rf"^{re.escape(fn)}(\s*\()", rf"{fn}_gone\1", split, flags=re.M)
        assert mutated != split, fn
        _assert_red(mutated, scope)

def test_split_shape_snapshot_removed_is_red():

    split = _split_shape(_rest())
    mutated, n = re.subn(check_proxy_identity.SNAPSHOT_RE, 'request_client_ip = "unknown"', split, count=1)
    assert n == 1
    _assert_red(mutated, "request security identity")

def test_split_shape_rq_copy_missing_or_late_is_red():

    split = _split_shape(_rest())
    copy = re.search(check_proxy_identity.RQ_IP_COPY_RE, split)
    assert copy is not None
    dropped = split[:copy.start()] + split[copy.end():]
    _assert_red(dropped, "request security identity")
    first_run = re.search(r"PCV_REST_RUN\(pcv_rest_\w+\(rq\)\);\n", dropped)
    late = dropped[:first_run.end()] + "    rq->request_client_ip = request_client_ip;\n" + dropped[first_run.end():]
    _assert_red(late, "request security identity")

def test_split_shape_block_consumer_bypass_is_red():

    split = _split_shape(_rest())
    for old, scope in (
        ("const gchar *remote_str = request_client_ip;", "mutation/access audit"),
        ("pcv_rbac_get_ip_remaining_lockout(client_ip)", "brute-force/security IP"),
    ):
        _assert_red(split.replace(old, old.replace("request_client_ip", '"unknown"').replace(
            "(client_ip)", '("unknown")'), 1), scope)

@contextlib.contextmanager
def _family_root(files: dict):

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for rel, text in files.items():
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_text(text)
        saved = check_proxy_identity.ROOT
        check_proxy_identity.ROOT = root
        try:
            yield root
        finally:
            check_proxy_identity.ROOT = saved

def _main_rc() -> tuple:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = check_proxy_identity.main([])
    return rc, out.getvalue() + err.getvalue()

def test_default_mode_reads_split_file_with_new_name():

    family = _rest()
    body = rpc_extract.function_body_in(family, rpc_extract.rest_fn_name_in(family, "_send_json"))
    assert body is not None
    moved = ("void\npcv_rest_send_json(SoupServerMessage *msg, guint status, const gchar *body)\n"
             + body + "\n")
    base = {p.relative_to(REPO).as_posix(): _drop_definition(p.read_text(), "_send_json")
            for p in rpc_extract.rest_family()}
    with_split = {**base, "src/api/rest_response.c": base.get("src/api/rest_response.c", "") + moved}
    with _family_root(with_split):
        rc, out = _main_rc()
    assert rc == 0, f"분할 파일의 새 이름 정의를 못 읽음:\n{out}"
    with _family_root(base):
        rc, out = _main_rc()
    assert rc == 1 and "HSTS/external HTTPS" in out, f"정의 0건인데 RED가 아님:\n{out}"


if __name__ == "__main__":
    tests = [
        value
        for name, value in sorted(globals().items())
        if name.startswith("test_") and callable(value)
    ]
    failures = 0
    for test in tests:
        try:
            test()
            print(f"OK   {test.__name__}")
        except AssertionError as error:
            failures += 1
            print(f"FAIL {test.__name__}: {error}")
    print(f"[test_proxy_identity] {len(tests) - failures}/{len(tests)} passed")
    raise SystemExit(1 if failures else 0)
