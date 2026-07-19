import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_safety_controls import collect_markers, MARKER_RE

def test_marker_extract():
    src = 'void f(){ /* PCV_SAFETY_CONTROL: alert-silence */ silence(); }'
    assert collect_markers([src]) == {"alert-silence"}

def test_marker_kebab_only():
    assert collect_markers(['/* PCV_SAFETY_CONTROL: session-revoke */']) == {"session-revoke"}
    assert collect_markers(['/* PCV_SAFETY_CONTROL: BadId */']) == set()

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
    print(f"[test_safety_controls] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
