#!/usr/bin/env python3
                          
                                                            
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                          
                                       

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_proxy_identity import (              
    FORBIDDEN,
    TARGET,
    WS_TARGET,
    scan_text,
)


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
    assert scan_text(TARGET.read_text()) == []


def test_comment_does_not_trigger_direct_peer_gate():
    source = TARGET.read_text() + f"\n/* {FORBIDDEN}(msg); */\n"
    assert scan_text(source) == []


def test_direct_peer_lookup_is_red():
    source = TARGET.read_text().replace(
        "const gchar *request_client_ip =",
        f"(void){FORBIDDEN}(msg); const gchar *request_client_ip =",
        1,
    )
    assert any(FORBIDDEN in reason for reason in scan_text(source))


def test_auth_failure_callsites_are_independently_red():
    source = TARGET.read_text()
    call = "pcv_rest_auth_audit_address(msg)"
    for nth, scope in (
        (1, "auth invalid API key audit"),
        (2, "auth missing header audit"),
        (3, "auth invalid JWT audit"),
    ):
        _assert_red(_replace_nth(source, call, '"unknown"', nth), scope)


def test_rate_limit_callsite_bypass_is_red_even_with_decoy():
    source = TARGET.read_text()
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
    source = TARGET.read_text().replace(
        "pcv_rest_rate_limit_key_for_message(msg, path, method)",
        """pcv_build_rate_limit_key(request_client_ip, path, method)
#if 0
        pcv_rest_rate_limit_key_for_message(msg, path, method)
#endif""",
        1,
    )
    _assert_red(source, "rate-limit callsite")


def test_unused_macro_decoy_cannot_replace_rate_callsite():
    source = TARGET.read_text().replace(
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
    source = TARGET.read_text().replace(
        "const gchar *remote_str = request_client_ip;",
        'const gchar *remote_str = "unknown";',
        1,
    )
    _assert_red(source, "mutation/access audit")


def test_brute_force_security_ip_bypass_is_red():
    source = TARGET.read_text().replace(
        "pcv_rbac_get_ip_remaining_lockout(client_ip)",
        'pcv_rbac_get_ip_remaining_lockout("unknown")',
        1,
    )
    _assert_red(source, "brute-force/security IP")


def test_login_audit_bypass_is_red():
    source = TARGET.read_text().replace(
        '"fail", 401, 0, client_ip);',
        '"fail", 401, 0, "unknown");',
        1,
    )
    _assert_red(source, "login audit auth.failed")


def test_login_success_audit_bypass_is_red():
    source = TARGET.read_text().replace(
        '"bootstrap recovery", "ok", 0, 0, client_ip);',
        '"bootstrap recovery", "ok", 0, 0, "unknown");',
        1,
    )
    _assert_red(source, "login audit auth.bootstrap.fallback")


def test_password_and_register_audits_are_independently_red():
    source = TARGET.read_text()
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
    source = TARGET.read_text()
    marker = "method, path, remote_str"
    _assert_red(
        _replace_nth(source, marker, 'method, path, "unknown"', 1),
        "mutation/access audit")
    _assert_red(
        _replace_nth(source, marker, 'method, path, "unknown"', 2),
        "mutation/access audit")


def test_external_https_hsts_bypass_is_red():
    source = TARGET.read_text().replace(
        "pcv_rest_client_identity_is_external_https(identity)",
        "FALSE",
        1,
    )
    _assert_red(source, "HSTS/external HTTPS")


def test_ws_current_tree_passes():
    assert scan_text(TARGET.read_text(), WS_TARGET.read_text()) == []


def test_ws_consumer_bypass_is_red_even_with_global_decoy():
    rest = TARGET.read_text()
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
    rest = TARGET.read_text()
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
    rest = TARGET.read_text()
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
