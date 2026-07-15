import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_dead_exports import strip_code, collect_declared, count_uses, find_dead

def test_strip_removes_comment_and_string():
    s = 'foo(); /* pcv_x() */ // pcv_y()\n bar("pcv_z()");'
    out = strip_code(s)
    assert "pcv_x" not in out and "pcv_y" not in out and "pcv_z" not in out

def test_collect_declared_from_header():
    hdr = 'gboolean pcv_foo(const gchar *j);\n/* pcv_ignored() in comment */'
    assert collect_declared([hdr]) == {"pcv_foo"}

def test_count_uses_counts_definition_and_refs():
    c = 'gboolean pcv_foo(int a){return 1;}\nint m(){ return pcv_foo(2); }'
    assert count_uses("pcv_foo", [strip_code(c)]) == 2

def test_dead_when_only_definition():
    hdr = 'gboolean pcv_dead(void);'
    c = 'gboolean pcv_dead(void){ return 0; }'   # 정의만, 사용 0
    assert find_dead([hdr], [c]) == {"pcv_dead"}

def test_not_dead_via_function_pointer():
    hdr = 'void pcv_handle_x(void);'
    c = ('void pcv_handle_x(void){}\n'
         'void reg(){ g_hash_table_insert(t, "m", pcv_handle_x); }')  # 포인터 등록 = 사용
    assert find_dead([hdr], [c]) == set()

def test_word_boundary_no_substring_match():
    hdr = 'void pcv_foo(void);'
    c = 'void pcv_foo(void){}\nvoid u(){ pcv_foobar(); }'  # pcv_foobar는 pcv_foo 아님
    assert find_dead([hdr], [c]) == {"pcv_foo"}

def test_waiver_excludes():
    hdr = 'void pcv_intentional(void);'
    c = '/* PCV_DEAD_EXPORT_OK: 외부 API */\nvoid pcv_intentional(void){}'
    assert find_dead([hdr], [c]) == set()

def test_char_literal_quote_not_string():
    # '"' 문자 리터럴이 팬텀 문자열을 열어 뒤따르는 실호출을 삼키면 안 됨.
    hdr = 'void pcv_live(void);'
    c = ('void pcv_live(void){}\n'
         'void u(int c){ if (c == \'"\') { pcv_live(); log("x"); } }')
    assert find_dead([hdr], [c]) == set()   # pcv_live는 호출됨 → dead 아님

def test_apostrophe_inside_string_intact():
    # 실 문자열 속 아포스트로피는 문자 리터럴로 오인되지 않고 뒤 코드 보존.
    hdr = 'void pcv_alive(void);'
    c = ('void pcv_alive(void){}\n'
         'void u(){ const char *s = "can\'t"; pcv_alive(); }')
    assert find_dead([hdr], [c]) == set()   # pcv_alive는 호출됨 → dead 아님

def test_url_slashes_in_string_not_comment():
    # 문자열 속 '//'(예: http:// URL)를 라인 주석으로 오제거해 따옴표 짝을
    # 깨뜨리면 안 됨 → 뒤따르는 실호출을 삼키면 오탐.
    hdr = 'void pcv_srv(void);'
    c = ('void pcv_srv(void){}\n'
         'void u(int p){ g_message("REST: http://0.0.0.0:%d/api/", p); pcv_srv(); }')
    assert find_dead([hdr], [c]) == set()   # pcv_srv는 호출됨 → dead 아님

def test_block_comment_open_in_string_not_comment():
    # 문자열 속 '/*'를 블록 주석으로 오인해 뒤 코드를 삼키면 안 됨.
    hdr = 'void pcv_go(void);'
    c = ('void pcv_go(void){}\n'
         'void u(){ const char *s = "/* not a comment"; pcv_go(); }')
    assert find_dead([hdr], [c]) == set()   # pcv_go는 호출됨 → dead 아님

def test_backslash_continued_string_call_preserved():
    # 리뷰어 probe 7b: `\`-이음 문자열이 다음 물리줄에서 닫히고, 같은 줄에
    # 실호출이 이어지는 모양. 줄 단위 상태머신이 문자열 상태를 물리줄
    # 경계에서 리셋하면 닫는 따옴표를 새 문자열의 시작으로 오인해 뒤따르는
    # 실호출을 삼킨다 → 사용처0으로 오탐(FALSE ALARM, 생존 함수가 dead로 잡힘).
    hdr = 'void pcv_probe7b(void);'
    c = ('void pcv_probe7b(void){}\n'
         'void u(){ const char *s = "opens \\\n'
         'closes"); pcv_probe7b(); }')
    assert find_dead([hdr], [c]) == set()   # pcv_probe7b는 호출됨 → dead 아님


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
    print(f"[test_dead_exports] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
