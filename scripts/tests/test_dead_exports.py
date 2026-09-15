                          
                                                         
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                       
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_dead_exports import strip_code, collect_declared, count_uses, find_dead
import check_dead_exports as gate


def test_public_waiver_keeps_unlisted_dead_export_blocked():
    assert hasattr(gate, "validate_waivers"), "공개 계약 파일의 근거 있는 예외를 지원해야 한다"
    declared = {"pcv_fixture_hook", "pcv_unwired"}
    entries = {"pcv_fixture_hook": {
        "reason": "회귀 테스트의 실패 주입 진입점",
        "evidence": ["tests/test_fixture.c"],
    }}
    evidence = {"tests/test_fixture.c": "void test_fail(){ pcv_fixture_hook(); }"}
    waived = gate.validate_waivers(entries, declared, declared, evidence)
    assert declared - waived == {"pcv_unwired"}


def test_public_waiver_rejects_missing_reason_or_evidence():
    assert hasattr(gate, "validate_waivers"), "공개 예외 근거 검증이 필요하다"
    for entry in (
        {"reason": "", "evidence": ["tests/test_fixture.c"]},
        {"reason": "테스트 전용", "evidence": []},
        {"reason": "테스트 전용", "evidence": ["tests/missing.c"]},
        {"reason": "테스트 전용", "evidence": ["../private.c"]},
    ):
        try:
            gate.validate_waivers(
                {"pcv_fixture_hook": entry}, {"pcv_fixture_hook"}, {"pcv_fixture_hook"},
                {"tests/test_fixture.c": "pcv_fixture_hook();"}
            )
        except ValueError:
            continue
        assert False, f"근거 없는 예외를 거부해야 한다: {entry}"


def test_public_waiver_rejects_comment_or_string_only_evidence():
    assert hasattr(gate, "validate_waivers"), "공개 예외의 실제 코드 근거 검증이 필요하다"
    entries = {"pcv_fixture_hook": {
        "reason": "회귀 테스트의 실패 주입 진입점",
        "evidence": ["tests/test_fixture.c"],
    }}
    for source in ('/* pcv_fixture_hook(); */', 'log("pcv_fixture_hook");', 'pcv_fixture_hook_other();'):
        try:
            gate.validate_waivers(entries, {"pcv_fixture_hook"}, {"pcv_fixture_hook"},
                                 {"tests/test_fixture.c": source})
        except ValueError:
            continue
        assert False, "주석·문자열·다른 심볼은 예외 근거가 아니다"


def test_public_waiver_rejects_stale_or_undeclared_symbols():
    assert hasattr(gate, "validate_waivers"), "삭제되거나 배선된 예외를 거부해야 한다"
    entries = {"pcv_fixture_hook": {
        "reason": "회귀 테스트의 실패 주입 진입점",
        "evidence": ["tests/test_fixture.c"],
    }}
    for declared, dead in ((set(), {"pcv_fixture_hook"}), ({"pcv_fixture_hook"}, set())):
        try:
            gate.validate_waivers(entries, declared, dead,
                                 {"tests/test_fixture.c": "pcv_fixture_hook();"})
        except ValueError:
            continue
        assert False, "현재 선언·dead 후보와 일치하지 않는 예외를 거부해야 한다"

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
    c = 'gboolean pcv_dead(void){ return 0; }'              
    assert find_dead([hdr], [c]) == {"pcv_dead"}

def test_sec1_class_detected_synthetic():
                                               
    header = "gboolean pcv_session_is_revoked(const gchar *sid);\n"
    wired = (
        "gboolean pcv_session_is_revoked(const gchar *sid){ return check(sid); }\n"
        "void enforce(const gchar *sid){ if (pcv_session_is_revoked(sid)) deny(); }\n"
    )
    unwired = (
        "gboolean pcv_session_is_revoked(const gchar *sid){ return check(sid); }\n"
        "void enforce(const gchar *sid){ allow(); }\n"
    )
    assert "pcv_session_is_revoked" not in find_dead([header], [wired])
    assert "pcv_session_is_revoked" in find_dead([header], [unwired])

def test_not_dead_via_function_pointer():
    hdr = 'void pcv_handle_x(void);'
    c = ('void pcv_handle_x(void){}\n'
         'void reg(){ g_hash_table_insert(t, "m", pcv_handle_x); }')               
    assert find_dead([hdr], [c]) == set()

def test_word_boundary_no_substring_match():
    hdr = 'void pcv_foo(void);'
    c = 'void pcv_foo(void){}\nvoid u(){ pcv_foobar(); }'                          
    assert find_dead([hdr], [c]) == {"pcv_foo"}

def test_waiver_excludes():
    hdr = 'void pcv_intentional(void);'
    c = '/* PCV_DEAD_EXPORT_OK: 외부 API */\nvoid pcv_intentional(void){}'
    assert find_dead([hdr], [c]) == set()

def test_char_literal_quote_not_string():
                                               
    hdr = 'void pcv_live(void);'
    c = ('void pcv_live(void){}\n'
         'void u(int c){ if (c == \'"\') { pcv_live(); log("x"); } }')
    assert find_dead([hdr], [c]) == set()                            

def test_apostrophe_inside_string_intact():
                                              
    hdr = 'void pcv_alive(void);'
    c = ('void pcv_alive(void){}\n'
         'void u(){ const char *s = "can\'t"; pcv_alive(); }')
    assert find_dead([hdr], [c]) == set()                             

def test_url_slashes_in_string_not_comment():
                                                     
                                  
    hdr = 'void pcv_srv(void);'
    c = ('void pcv_srv(void){}\n'
         'void u(int p){ g_message("REST: http://0.0.0.0:%d/api/", p); pcv_srv(); }')
    assert find_dead([hdr], [c]) == set()                           

def test_block_comment_open_in_string_not_comment():
                                            
    hdr = 'void pcv_go(void);'
    c = ('void pcv_go(void){}\n'
         'void u(){ const char *s = "/* not a comment"; pcv_go(); }')
    assert find_dead([hdr], [c]) == set()                          

def test_backslash_continued_string_call_preserved():
                                                   
                                          
                                            
                                                         
    hdr = 'void pcv_probe7b(void);'
    c = ('void pcv_probe7b(void){}\n'
         'void u(){ const char *s = "opens \\\n'
         'closes"); pcv_probe7b(); }')
    assert find_dead([hdr], [c]) == set()                               


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
