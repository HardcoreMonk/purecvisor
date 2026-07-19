import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_error_codes import strip_code, find_raw_literals_in_text, find_pcv_err_in_text, ENUM_DEF_FILE

def test_raw_literal_injection_detected():

    c = 'pcv_audit_log(NULL, "vm.resize_disk", target, "fail", -32000, 0, "local");'
    assert find_raw_literals_in_text("fake.c", c) == ["fake.c:1"]

def test_raw_literal_multiline_reports_correct_line():

    c = 'int a = 1;\nint b = PURE_RPC_ERR_TIMEOUT;\nint c = -32001;\n'
    assert find_raw_literals_in_text("fake.c", c) == ["fake.c:3"]

def test_pcv_err_reintroduction_detected():

    c = '#define PCV_ERR_FOO -32000\nint x = PCV_ERR_FOO;'
    hits = find_pcv_err_in_text("fake.c", c)
    assert hits == ["fake.c:PCV_ERR_FOO", "fake.c:PCV_ERR_FOO"]

def test_string_literal_excluded():

    c = 'gboolean ok = (strstr(result, "-32601") != NULL);'
    assert find_raw_literals_in_text("fake.c", c) == []

def test_line_comment_excluded():

    c = '// -32601 : Method Not Found\nint y = PURE_RPC_ERR_METHOD_NOT_FOUND;'
    assert find_raw_literals_in_text("fake.c", c) == []

def test_block_comment_excluded():

    c1 = '/* -32601 in block comment */\nint y = PURE_RPC_ERR_METHOD_NOT_FOUND;'
    c2 = '/* 에러 코드\n *   -32601 : Method Not Found\n */\nint y = 1;'
    assert find_raw_literals_in_text("fake.c", c1) == []
    assert find_raw_literals_in_text("fake.c", c2) == []

def test_pcv_vm_err_domain_excluded():

    c = 'g_set_error(error, PCV_VM_ERROR, PCV_VM_ERR_NOT_FOUND, "no vm");'
    assert find_pcv_err_in_text("fake.c", c) == []

def test_enum_def_file_excluded_for_pcv_err():

    c = '#define PCV_ERR_LEGACY -32000  /* 정의부라 제외되어야 함 */'
    assert find_pcv_err_in_text(ENUM_DEF_FILE, c) == []

def test_clean_tree_fixture_passes():

    c = ('gint code = PURE_RPC_ERR_TIMEOUT; /* -32003 참고 */\n'
         'if (strstr(msg, "-32601")) { report(); }\n')
    assert find_raw_literals_in_text("fake.c", c) == []
    assert find_pcv_err_in_text("fake.c", c) == []

def test_char_literal_quote_not_string():

    c = 'if (c == \'"\') { int e = -32000; }'
    assert find_raw_literals_in_text("fake.c", c) == ["fake.c:1"]

def test_backslash_continued_string_preserves_line_count():

    c = 'const char *s = "opens \\\ncloses";\nint bad = -32002;\n'
    assert strip_code(c).count('\n') == c.count('\n')
    assert find_raw_literals_in_text("fake.c", c) == ["fake.c:3"]

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
    print(f"[test_error_codes] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
