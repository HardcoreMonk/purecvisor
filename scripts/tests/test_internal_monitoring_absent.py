#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SCRIPT_DIR))

import check_internal_monitoring_absent as gate


def clean_tree() -> dict[str, str]:
    texts: dict[str, str] = {}
    calls: list[str] = []
    for module, symbols in gate.KEEP.items():
        body = "".join(f"JsonArray *\n{symbol}(void)\n{{\n    return NULL;\n}}\n" for symbol in symbols)
        texts[module] = "#include <glib.h>\n" + body
        calls.extend(f"    {symbol}();\n" for symbol in symbols)
    texts["src/modules/dispatcher/handler_consumer.c"] = "void consume(void)\n{\n" + "".join(calls) + "}\n"
    texts["docs/GUIDE.md"] = "# 안내\n"
    return texts


def test_clean_tree_passes() -> None:
    assert gate.check(clean_tree()) == []


def test_every_ring_token_turns_red_in_any_tracked_file() -> None:
    for ring, tokens in gate.RINGS.items():
        for token in tokens:
            for path in ("src/modules/daemons/alert_engine.c", "tests/test_fixture.c",
                         "ui/modules/monitor.js", "docs/GUIDE.md"):
                texts = clean_tree()
                texts[path] = texts.get(path, "") + f"\nint {token}_probe;\n"
                failures = gate.check(texts)
                assert any(path in item and ring in item and token in item for item in failures), (path, token)


def test_gate_file_itself_is_exempt() -> None:
    texts = clean_tree()
    texts[gate.SELF] = " ".join(token for tokens in gate.RINGS.values() for token in tokens)
    assert gate.check(texts) == []


def test_removed_public_getter_turns_red() -> None:
    for module, symbols in gate.KEEP.items():
        for symbol in symbols:
            texts = clean_tree()
            texts[module] = texts[module].replace(f"\n{symbol}(void)", f"\n{symbol}_gone(void)")
            failures = gate.check(texts)
            assert any("정의 없음" in item and symbol in item for item in failures), symbol


def test_unconsumed_public_getter_turns_red() -> None:
    for module, symbols in gate.KEEP.items():
        for symbol in symbols:
            texts = clean_tree()
            consumer = "src/modules/dispatcher/handler_consumer.c"
            texts[consumer] = texts[consumer].replace(f"    {symbol}();\n", "")
            failures = gate.check(texts)
            assert any("소비자 없음" in item and symbol in item for item in failures), symbol


def test_consumer_in_the_same_module_does_not_count() -> None:
    texts = clean_tree()
    module = "src/modules/daemons/ebpf_telemetry.c"
    symbol = gate.KEEP[module][0]
    consumer = "src/modules/dispatcher/handler_consumer.c"
    texts[consumer] = texts[consumer].replace(f"    {symbol}();\n", "")
    texts[module] += f"void self_use(void)\n{{\n    {symbol}();\n}}\n"
    assert any("소비자 없음" in item and symbol in item for item in gate.check(texts))


def test_missing_module_turns_red() -> None:
    for module in gate.KEEP:
        texts = clean_tree()
        del texts[module]
        assert any(module in item and "검사 대상 모듈 없음" in item for item in gate.check(texts)), module


def main() -> int:
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
    print(f"[test_internal_monitoring_absent] {len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
