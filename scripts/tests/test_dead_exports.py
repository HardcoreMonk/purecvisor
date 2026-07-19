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
    c = 'gboolean pcv_dead(void){ return 0; }'
    assert find_dead([hdr], [c]) == {"pcv_dead"}

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
