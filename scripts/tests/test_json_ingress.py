                          
                                                                
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                              
import contextlib
import io
import sys
import tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_json_ingress
from check_json_ingress import count_violations

def test_raw_is_violation():
    assert count_violations('json_parser_load_from_data(p, text, sz, NULL);') == 1

def test_wrapper_not_counted():
    assert count_violations('pcv_rpc_parse_guarded(text, sz, &p, &e);') == 0

def test_waiver_same_line():
    assert count_violations('json_parser_load_from_data(p, resp, -1, NULL); /* PCV_PARSE_TRUSTED: 내부응답 */') == 0

def test_waiver_prev_line():
    src = '/* PCV_PARSE_TRUSTED: RPC 응답 */\njson_parser_load_from_data(p, resp, -1, NULL);'
    assert count_violations(src) == 0

def test_line_comment_not_counted():
    assert count_violations('// json_parser_load_from_data(p, x, -1, NULL);') == 0

def test_block_comment_single_line_not_counted():
    assert count_violations('/* json_parser_load_from_data() 설명 */') == 0

def test_block_comment_multiline_not_counted():
    src = ('/**\n'
           ' *   1. JSON 파싱: json_parser_load_from_data()로 요청 변환\n'
           ' */\n')
    assert count_violations(src) == 0

def test_real_call_after_block_comment_counted():
    src = ('/* json_parser_load_from_data() 설명 */\n'
           'json_parser_load_from_data(p, text, -1, NULL);')
    assert count_violations(src) == 1

def test_waiver_prev_line_survives_block_strip():
                                                           
    src = ('/* PCV_PARSE_TRUSTED: 내부응답 */\n'
           'json_parser_load_from_data(p, resp, -1, NULL);')
    assert count_violations(src) == 0


@contextlib.contextmanager
def _root(files):

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for rel, text in files.items():
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_text(text)
        saved = check_json_ingress.ROOT, check_json_ingress.BASELINE_FILE
        check_json_ingress.ROOT = root
        check_json_ingress.BASELINE_FILE = root / "scripts" / "json_ingress_baseline.txt"
        try:
            yield root
        finally:
            check_json_ingress.ROOT, check_json_ingress.BASELINE_FILE = saved

def _main_rc():
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = check_json_ingress.main()
    return rc, out.getvalue() + err.getvalue()

RAW = 'void f(void) {\n    json_parser_load_from_data(p, text, sz, NULL);\n}\n'
WAIVED = 'void f(void) {\n    /* PCV_PARSE_TRUSTED: 내부 */\n    json_parser_load_from_data(p, text, sz, NULL);\n}\n'

def test_split_file_raw_parse_is_counted():

    with _root({"src/api/rest_server.c": "int x;\n", "src/api/rest_request.c": RAW}):
        assert "src/api/rest_request.c" in check_json_ingress.ingress_files()
        rc, out = _main_rc()
    assert rc == 1 and "src/api/rest_request.c: 1" in out, out

def test_split_file_waived_parse_passes():
    with _root({"src/api/rest_server.c": "int x;\n", "src/api/rest_request.c": WAIVED}):
        rc, out = _main_rc()
    assert rc == 0, out

def test_missing_rest_route_file_is_error():

    with _root({"src/api/rest_request.c": WAIVED}):
        try:
            check_json_ingress.ingress_files()
        except FileNotFoundError:
            return
    raise AssertionError("rest_server.c 없는 루트에서 예외가 없다")

if __name__ == "__main__":
    tests = [
        test_split_file_raw_parse_is_counted,
        test_split_file_waived_parse_passes,
        test_missing_rest_route_file_is_error,
        test_raw_is_violation,
        test_wrapper_not_counted,
        test_waiver_same_line,
        test_waiver_prev_line,
        test_line_comment_not_counted,
        test_block_comment_single_line_not_counted,
        test_block_comment_multiline_not_counted,
        test_real_call_after_block_comment_counted,
        test_waiver_prev_line_survives_block_strip,
    ]
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
