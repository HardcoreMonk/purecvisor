import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
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
    # block-comment strip이 라인 인접성(직전 줄 waiver)을 보존하는지 회귀 검증
    src = ('/* PCV_PARSE_TRUSTED: 내부응답 */\n'
           'json_parser_load_from_data(p, resp, -1, NULL);')
    assert count_violations(src) == 0


if __name__ == "__main__":
    tests = [
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
