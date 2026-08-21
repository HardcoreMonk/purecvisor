#!/usr/bin/env python3
                          
                                                                          
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                                        
                                                        

import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_transport_bind import scan_text, TARGET, MAIN_TARGET              

GATE = Path(__file__).resolve().parent.parent / "check_transport_bind.py"


def _sources() -> tuple[str, str]:
    return TARGET.read_text(), MAIN_TARGET.read_text()


def _assert_red(rest: str, main: str) -> None:
    reasons = scan_text(rest, main)
    assert reasons, "unsafe counterfactual이 gate를 통과함"


def test_current_tree_pass():
    result = subprocess.run(
        [sys.executable, str(GATE)], capture_output=True, text=True)
    assert result.returncode == 0, f"{result.stdout}\n{result.stderr}"


def test_current_invariant_recognized():
    rest, main = _sources()
    assert scan_text(rest, main) == []


def test_red_validation_bypass_plus_if0_expected_decoy():
                                         
    rest, main = _sources()
    main = main.replace(
        "pcv_config_validate_transport(&error)",
        "TRUE\n#if 0\n || !pcv_config_validate_transport(&error)\n#endif",
        1,
    )
    _assert_red(rest, main)


def test_red_validation_result_ignored_standalone():
                                                         
    rest, main = _sources()
    expected = (
        "if (!pcv_config_get_https_enabled(&https_enabled, &error) ||\n"
        "        !pcv_config_validate_transport(&error)) {"
    )
    replacement = (
        "pcv_config_validate_transport(&error);\n"
        "    if (!pcv_config_get_https_enabled(&https_enabled, &error)) {"
    )
    assert expected in main
    _assert_red(rest, main.replace(expected, replacement, 1))


def test_red_validation_short_circuit_wrapper():
                                        
    rest, main = _sources()
    expected = (
        "if (!pcv_config_get_https_enabled(&https_enabled, &error) ||\n"
        "        !pcv_config_validate_transport(&error)) {"
    )
    replacement = (
        "if (transport_override ||\n"
        "        (!pcv_config_get_https_enabled(&https_enabled, &error) ||\n"
        "         !pcv_config_validate_transport(&error))) {"
    )
    assert expected in main
    _assert_red(rest, main.replace(expected, replacement, 1))


def test_red_cleanup_nested_decoy_and_early_success():
                                                    
    rest, main = _sources()
    expected = (
        "pcv_config_shutdown();\n"
        "        pcv_log_shutdown();\n"
        "        return EXIT_FAILURE;"
    )
    replacement = (
        "if (FALSE) {\n"
        "            pcv_config_shutdown();\n"
        "            pcv_log_shutdown();\n"
        "            return EXIT_FAILURE;\n"
        "        }\n"
        "        return EXIT_SUCCESS;"
    )
    assert expected in main
    _assert_red(rest, main.replace(expected, replacement, 1))


def test_extra_parentheses_are_benign():
                                        
    rest, main = _sources()
    expected = (
        "if (!pcv_config_get_https_enabled(&https_enabled, &error) ||\n"
        "        !pcv_config_validate_transport(&error)) {"
    )
    replacement = (
        "if (((!pcv_config_get_https_enabled(\n"
        "             &https_enabled, &error))) ||\n"
        "        ((!pcv_config_validate_transport(&error)))) {"
    )
    assert expected in main
    assert scan_text(rest, main.replace(expected, replacement, 1)) == []


def test_red_plan_decoy_initializer_unsafe_actual_return():
                                                        
    rest, main = _sources()
    start = rest.index("PcvRestTransportPlan\npcv_rest_transport_plan")
    end = rest.index("\nvoid\npcv_rest_transport_initialize", start)
    unsafe = """
PcvRestTransportPlan
pcv_rest_transport_plan(PcvRestTlsMode mode,
                        const gchar *configured_plaintext_bind)
{
    gboolean internal = mode == PCV_REST_TLS_INTERNAL;
    const gchar *bind_mode = "all";
    if (FALSE) {
        PcvRestTransportPlan expected = {
            .mode = mode,
            .initialize_tls = internal,
            .load_certificate = internal,
            .create_tls_context = internal,
            .listen_https = internal,
            .plaintext_bind_mode = bind_mode,
            .plaintext_host = "127.0.0.1",
        };
        (void)expected;
    }
    return (PcvRestTransportPlan) {
        .mode = PCV_REST_TLS_INTERNAL,
        .initialize_tls = TRUE,
        .load_certificate = TRUE,
        .create_tls_context = TRUE,
        .listen_https = TRUE,
        .plaintext_bind_mode = "all",
        .plaintext_host = "0.0.0.0",
    };
}
"""
    _assert_red(rest[:start] + unsafe + rest[end:], main)


def test_red_plan_internal_forced_true():
                                              
    rest, main = _sources()
    expected = "gboolean internal = mode == PCV_REST_TLS_INTERNAL;"
    assert expected in rest
    _assert_red(rest.replace(expected, "gboolean internal = TRUE;", 1), main)


def test_red_plan_nested_safe_bind_decoy_unsafe_top_level():
                                                             
    rest, main = _sources()
    expected = (
        "const gchar *bind_mode =\n"
        '        internal && g_strcmp0(configured_plaintext_bind, "all") == 0\n'
        '            ? "all" : "loopback";'
    )
    replacement = (
        "if (FALSE) {\n"
        "        const gchar *bind_mode =\n"
        '            internal && g_strcmp0(configured_plaintext_bind, "all") == 0\n'
        '                ? "all" : "loopback";\n'
        "        (void)bind_mode;\n"
        "    }\n"
        '    const gchar *bind_mode = "all";'
    )
    assert expected in rest
    _assert_red(rest.replace(expected, replacement, 1), main)


def test_red_validation_bypass_fresh_unsafe_plan_if0_decoy():
                                                           
    rest, main = _sources()
    main = main.replace(
        "pcv_config_validate_transport(&error)",
        "TRUE\n#if 0\n || !pcv_config_validate_transport(&error)\n#endif",
        1,
    )
    original = (
        "PcvRestTransportPlan rest_transport = pcv_rest_transport_plan(\n"
        "        pcv_rest_tls_mode_from_config(https_enabled),\n"
        '        pcv_config_get_string("server", "bind_plaintext", "loopback"));'
    )
    replacement = (
        "PcvRestTransportPlan validated_transport = pcv_rest_transport_plan(\n"
        "        pcv_rest_tls_mode_from_config(https_enabled),\n"
        '        pcv_config_get_string("server", "bind_plaintext", "loopback"));\n'
        "    PcvRestTransportPlan rest_transport = pcv_rest_transport_plan(\n"
        '        PCV_REST_TLS_INTERNAL, "all");'
    )
    assert original in main
    _assert_red(rest, main.replace(original, replacement, 1))


def test_red_unconditional_listen_all_with_if0_loopback_decoy():
                                                             
    rest, main = _sources()
    start = rest.index("static gboolean\n_transport_listen_plaintext")
    end = rest.index("static gboolean\n_transport_listen_https", start)
    unsafe = """
static gboolean
_transport_listen_plaintext(gpointer data, const gchar *endpoint,
                            GError **error)
{
    RestTransportContext *ctx = data;
#if 0
    GInetAddress *lo = g_inet_address_new_loopback(AF_INET);
    GSocketAddress *addr = g_inet_socket_address_new(lo, ctx->server->port);
    return soup_server_listen(ctx->server->soup, addr, 0, error);
#endif
    return soup_server_listen_all(ctx->server->soup, ctx->server->port,
                                  SOUP_SERVER_LISTEN_IPV4_ONLY, error);
}

"""
    _assert_red(rest[:start] + unsafe + rest[end:], main)


def test_red_dead_macro_decoy():
                                      
    rest, main = _sources()
    main = main.replace(
        "pcv_config_validate_transport(&error)",
        "TRUE\n#define DEAD_VALIDATE() pcv_config_validate_transport(&error)",
        1,
    )
    _assert_red(rest, main)


def test_red_plan_substitution_at_constructor():
                                                 
    rest, main = _sources()
    main = main.replace(
        "pcv_rest_server_new(dispatcher, 0, rest_transport)",
        "pcv_rest_server_new(dispatcher, 0,\n"
        "            pcv_rest_transport_plan(PCV_REST_TLS_INTERNAL, \"all\"))",
        1,
    )
    _assert_red(rest, main)


def test_red_constructor_nested_decoy_then_overwrite():
                                                        
    rest, main = _sources()
    expected = "self->transport = transport;"
    assert expected in rest
    replacement = (
        "if (FALSE) {\n"
        "        self->transport = transport;\n"
        "    }\n"
        "    self->transport = pcv_rest_transport_plan(\n"
        "        PCV_REST_TLS_INTERNAL, \"all\");"
    )
    _assert_red(rest.replace(expected, replacement, 1), main)


def test_red_constructor_field_overwrite():
                                                      
    rest, main = _sources()
    expected = "self->transport = transport;"
    assert expected in rest
    replacement = (
        "self->transport = transport;\n"
        '    self->transport.plaintext_host = "0.0.0.0";'
    )
    _assert_red(rest.replace(expected, replacement, 1), main)


def test_red_server_start_plan_substitution():
                                                                    
    rest, main = _sources()
    rest = rest.replace(
        "&self->transport, &transport_ops, &transport_context,",
        "&unsafe_transport, &transport_ops, &transport_context,",
        1,
    )
    _assert_red(rest, main)


def test_red_server_start_transport_overwrite():
                                          
    rest, main = _sources()
    marker = "gboolean\npcv_rest_server_start(PcvRestServer *self, GError **error)\n{"
    assert marker in rest
    replacement = (
        marker + '\n    self->transport.plaintext_host = "0.0.0.0";')
    _assert_red(rest.replace(marker, replacement, 1), main)


def test_red_plaintext_callback_substituted_with_https():
                                                              
    rest, main = _sources()
    expected = ".listen_plaintext = _transport_listen_plaintext,"
    assert expected in rest
    rest = rest.replace(
        expected, ".listen_plaintext = _transport_listen_https,", 1)
    _assert_red(rest, main)


def test_red_orchestration_hardcoded_plaintext_host():
                                                 
    rest, main = _sources()
    expected = (
        "ops->listen_plaintext(context, transport->plaintext_host, error)")
    assert expected in rest
    _assert_red(rest.replace(
        expected, 'ops->listen_plaintext(context, "0.0.0.0", error)', 1), main)


def test_red_orchestration_nested_decoy_ignored_failure():
                                           
    rest, main = _sources()
    expected = (
        "outcome->plaintext_listening =\n"
        "        ops->listen_plaintext(context, transport->plaintext_host, error);\n"
        "    if (!outcome->plaintext_listening)\n"
        "        return FALSE;"
    )
    replacement = (
        "if (FALSE) {\n"
        "        outcome->plaintext_listening =\n"
        "            ops->listen_plaintext(\n"
        "                context, transport->plaintext_host, error);\n"
        "        if (!outcome->plaintext_listening)\n"
        "            return FALSE;\n"
        "    }\n"
        '    ops->listen_plaintext(context, "0.0.0.0", error);'
    )
    assert expected in rest
    _assert_red(rest.replace(expected, replacement, 1), main)


def test_red_orchestration_fail_if_after_https():
                                                      
    rest, main = _sources()
    failure = (
        "    if (!outcome->plaintext_listening)\n"
        "        return FALSE;\n\n"
    )
    assert failure in rest
    rest = rest.replace(failure, "", 1)
    function_start = rest.index(
        "gboolean\npcv_rest_transport_start")
    function_end = rest.index(
        "\nconst gchar *\npcv_rest_auth_audit_address", function_start)
    orchestration = rest[function_start:function_end]
    marker = "    return TRUE;\n}"
    assert marker in orchestration
    orchestration = orchestration.replace(
        marker,
        "    if (!outcome->plaintext_listening)\n"
        "        return FALSE;\n"
        + marker,
        1,
    )
    _assert_red(
        rest[:function_start] + orchestration + rest[function_end:], main)


if __name__ == "__main__":
    tests = [value for name, value in sorted(globals().items())
             if name.startswith("test_") and callable(value)]
    failed = 0
    for test in tests:
        try:
            test()
            print(f"OK   {test.__name__}")
        except AssertionError as error:
            failed += 1
            print(f"FAIL {test.__name__}: {error}")
    print(f"[test_transport_bind] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
