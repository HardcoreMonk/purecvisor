#!/usr/bin/env python3
import contextlib
import io
import json
import shutil
import sys
import tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_json_ingress as gate
from check_json_ingress import evaluate, find_sites, load_trusted

REAL_ROOT = gate.ROOT
REAL_TRUSTED = gate.TRUSTED_FILE


def _fn(body: str, name: str = "f") -> str:
    return f"static void {name}(void)\n{{\n{body}\n}}\n"


RAW = _fn("    json_parser_load_from_data(p, text, sz, NULL);")
ENTRY = {"file": "src/api/rest_request.c", "function": "f", "data": "text", "reason": "내부 출력"}


def test_raw_site_has_function_and_data():
    assert find_sites(RAW) == [("f", "text", 3)]


def test_wrapper_not_counted():
    assert find_sites(_fn("    pcv_rpc_parse_guarded(text, sz, &p, &e);")) == []


def test_line_comment_not_counted():
    assert find_sites(_fn("    // json_parser_load_from_data(p, x, -1, NULL);")) == []


def test_block_comment_single_line_not_counted():
    assert find_sites(_fn("    /* json_parser_load_from_data() */")) == []


def test_block_comment_multiline_not_counted():
    src = _fn("    /*\n     * 1. json_parser_load_from_data()\n     */")
    assert find_sites(src) == []


def test_string_literal_not_counted():
    assert find_sites(_fn('    log("json_parser_load_from_data(p, x)");')) == []


def test_real_call_after_block_comment_counted():
    src = _fn("    /* json_parser_load_from_data() */\n    json_parser_load_from_data(p, text, -1, NULL);")
    assert find_sites(src) == [("f", "text", 4)]


def test_nested_call_data_argument():
    src = _fn("    if (!json_parser_load_from_data(p, g_strdup(a), -1, &err)) return;")
    assert find_sites(src) == [("f", "g_strdup(a)", 3)]


def test_file_scope_call_is_a_site():
    assert find_sites("static int q = (json_parser_load_from_data(p, x, -1, NULL));\n") == \
        [(gate.FILE_SCOPE, "x", 1)]


def test_comment_marker_is_not_a_waiver():
    src = _fn("    /* PCV_PARSE_TRUSTED: 내부 응답 */\n    json_parser_load_from_data(p, text, -1, NULL);")
    reasons = evaluate({"src/api/rest_request.c": src}, {})
    assert len(reasons) == 1 and "f()" in reasons[0], reasons


def test_listed_site_passes():
    trusted = load_trusted([ENTRY], ["src/api/rest_request.c"])
    assert evaluate({"src/api/rest_request.c": RAW}, trusted) == []


def test_unlisted_site_fails_with_location():
    reasons = evaluate({"src/api/rest_request.c": RAW}, {})
    assert len(reasons) == 1, reasons
    assert "src/api/rest_request.c:3" in reasons[0] and "f()" in reasons[0] and "(text)" in reasons[0], reasons


def test_trusted_function_with_other_data_fails():
    trusted = load_trusted([ENTRY], ["src/api/rest_request.c"])
    src = RAW.replace("(p, text,", "(p, request_body,")
    reasons = evaluate({"src/api/rest_request.c": src}, trusted)
    assert any("request_body" in r and "신뢰 목록에 없다" in r for r in reasons), reasons
    assert any(r.startswith("stale") for r in reasons), reasons


def test_second_parse_in_trusted_function_fails():
    trusted = load_trusted([ENTRY], ["src/api/rest_request.c"])
    src = _fn("    json_parser_load_from_data(p, text, -1, NULL);\n"
              "    json_parser_load_from_data(q, text, -1, NULL);")
    reasons = evaluate({"src/api/rest_request.c": src}, trusted)
    assert len(reasons) == 1 and "두 번 이상" in reasons[0], reasons


def test_stale_entry_fails():
    trusted = load_trusted([ENTRY], ["src/api/rest_request.c"])
    reasons = evaluate({"src/api/rest_request.c": RAW.replace("f(void)", "g(void)")}, trusted)
    assert any(r.startswith("stale") and "rest_request.c f(text)" in r for r in reasons), reasons
    assert any("g()" in r for r in reasons), reasons


def test_unbalanced_braces_fail_closed():
    src = "static void a(void)\n{\n#if X\n    if (1) {\n#else\n    if (2) {\n#endif\n    }\n}\n"
    reasons = evaluate({"src/api/rest_request.c": src}, {})
    assert any("fail-closed" in r for r in reasons), reasons


def _schema_error(entries, files=("src/api/rest_request.c",)):
    try:
        load_trusted(entries, list(files))
    except ValueError as exc:
        return str(exc)
    raise AssertionError(f"검증이 통과했다: {entries!r}")


def test_schema_rejects_bad_entries():
    assert "배열" in _schema_error({"x": ENTRY})
    assert "필드" in _schema_error([{k: v for k, v in ENTRY.items() if k != "reason"}])
    assert "필드" in _schema_error([dict(ENTRY, line=3)])
    assert "비어" in _schema_error([dict(ENTRY, reason="  ")])
    assert "경계 파일이 아니다" in _schema_error([dict(ENTRY, file="src/utils/x.c")])
    assert "중복" in _schema_error([ENTRY, dict(ENTRY, reason="다른 근거")])


@contextlib.contextmanager
def _root(files, trusted):
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for rel, text in files.items():
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_text(text)
        trusted_file = root / "contracts" / "json_ingress_trusted.json"
        if trusted is not None:
            trusted_file.parent.mkdir(parents=True, exist_ok=True)
            trusted_file.write_text(json.dumps(trusted, ensure_ascii=False))
        gate.ROOT, gate.TRUSTED_FILE = root, trusted_file
        try:
            yield root
        finally:
            gate.ROOT, gate.TRUSTED_FILE = REAL_ROOT, REAL_TRUSTED


def _main_rc():
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = gate.main()
    return rc, out.getvalue() + err.getvalue()


def test_split_file_raw_parse_is_flagged():
    with _root({"src/api/rest_server.c": "int x;\n", "src/api/rest_request.c": RAW}, []):
        assert "src/api/rest_request.c" in gate.ingress_files()
        rc, out = _main_rc()
    assert rc == 1 and "src/api/rest_request.c:3 f()" in out, out


def test_split_file_listed_parse_passes():
    with _root({"src/api/rest_server.c": "int x;\n", "src/api/rest_request.c": RAW}, [ENTRY]):
        rc, out = _main_rc()
    assert rc == 0 and "raw 파싱 1 / 신뢰 목록 1" in out, out


def test_missing_trusted_file_fails():
    with _root({"src/api/rest_server.c": "int x;\n"}, None):
        rc, out = _main_rc()
    assert rc == 1 and "신뢰 목록 계약" in out, out


def test_missing_rest_route_file_is_error():
    with _root({"src/api/rest_request.c": RAW}, []):
        try:
            gate.ingress_files()
        except FileNotFoundError:
            return
    raise AssertionError("rest_server.c 없는 루트에서 예외가 없다")


def _real_tree(edits=None):
    files = {rel: (REAL_ROOT / rel).read_text(errors="replace")
             for rel in gate.ingress_files() if (REAL_ROOT / rel).exists()}
    for rel, (old, new) in (edits or {}).items():
        assert files[rel].count(old) == 1, (rel, old)
        files[rel] = files[rel].replace(old, new)
    return files, json.loads(REAL_TRUSTED.read_text())


def _total_raw(files):
    return sum(len(find_sites(t)) for t in files.values())


def test_current_tree_passes():
    rc, out = _main_rc()
    assert rc == 0, out
    files, trusted = _real_tree()
    assert _total_raw(files) == len(trusted), (_total_raw(files), len(trusted))


def test_counterfactual_trusted_site_made_unguarded_fails():
    files, trusted = _real_tree({"src/api/rest_response.c": (
        "json_parser_load_from_data(p, rpc_resp, -1, &err)",
        "json_parser_load_from_data(p, client_body, -1, &err)")})
    with _root(files, trusted):
        rc, out = _main_rc()
    assert rc == 1, out
    assert "pcv_rest_send_rpc_result() 의 raw json_parser_load_from_data(client_body)" in out, out
    assert "stale 신뢰 항목: src/api/rest_response.c pcv_rest_send_rpc_result(rpc_resp)" in out, out


def test_counterfactual_count_preserving_swap_fails():
    injected = ("\nstatic gboolean\n_injected_frame_parse(JsonParser *p, const gchar *frame)\n{\n"
                "    return json_parser_load_from_data(p, frame, -1, NULL);\n}\n")
    files, trusted = _real_tree({"src/modules/dispatcher/handler_host_query.c": (
        "json_parser_load_from_data(p, fjson, -1, NULL)", "pcv_injected_guarded(p, fjson)")})
    files["src/api/ws_server.c"] += injected
    assert _total_raw(files) == len(trusted)
    with _root(files, trusted):
        rc, out = _main_rc()
    assert rc == 1, out
    assert "src/api/ws_server.c:" in out and "_injected_frame_parse()" in out, out
    assert "stale 신뢰 항목: src/modules/dispatcher/handler_host_query.c handle_capacity_forecast(fjson)" in out, out


def test_counterfactual_new_raw_parse_outside_list_fails():
    files, trusted = _real_tree()
    files["src/api/rest_request.c"] += _fn(
        "    json_parser_load_from_data(p, body, -1, NULL);", "_injected_body_parse")
    with _root(files, trusted):
        rc, out = _main_rc()
    assert rc == 1 and "_injected_body_parse()" in out and "(body)" in out, out


if __name__ == "__main__":
    tests = [v for k, v in globals().items() if k.startswith("test_") and callable(v)]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"PASS {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    if failed:
        print(f"{failed}/{len(tests)} FAILED")
        sys.exit(1)
    print(f"OK ({len(tests)}/{len(tests)} passed)")
    sys.exit(0)
