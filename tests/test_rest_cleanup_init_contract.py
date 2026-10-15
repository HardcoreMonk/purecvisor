#!/usr/bin/env python3







import re
import unittest
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "src/api/rest_server.c"
ANCHOR = "\n_on_request(SoupServer"

FREE_CALL = re.compile(r"\b(?:g_free|g_strfreev|json_object_unref|pcv_trace_context_free)\s*\(\s*([A-Za-z_]\w*)\s*\)")

EXPECTED_MIN = {"segs", "body", "subject", "rpc", "vm_delete_name", "req_id", "trace_ctx"}

DECL = re.compile(r"^    (?:const\s+)?[A-Za-z_]\w*\s*(?:\*\s*)*[A-Za-z_]\w*\s*(?:=|,|;)")


def strip_c(text: str) -> str:

    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(c + " " * (min(j, n) - i - 1) + (c if j < n else ""))
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def on_request_body() -> tuple[str, int]:

    code = strip_c(SRC.read_text())
    start = code.index(ANCHOR)
    return code[start:code.index("\n}\n", start) + 2], code.count("\n", 0, start) + 1


def labels(body: str) -> tuple[int, int]:
    clean = re.search(r"^cleanup:", body, re.M)
    no_free = re.search(r"^cleanup_no_free:", body, re.M)
    if not clean or not no_free:
        raise AssertionError("_on_request에 cleanup:/cleanup_no_free: 레이블이 없다")
    return clean.start(), no_free.start()


def freed_names(body: str) -> set[str]:
    clean, _ = labels(body)
    return set(FREE_CALL.findall(body[clean:]))


def function_scope_decls(body: str) -> dict[str, int]:

    found: dict[str, int] = {}
    offset = 0
    for line in body.splitlines(keepends=True):
        if DECL.match(line) and not line.lstrip().startswith(("return", "goto")):
            stmt = line.strip().rstrip(";")
            head = re.match(r"(?:const\s+)?[A-Za-z_]\w*", stmt)
            rest = stmt[head.end():] if head else ""
            depth, part, parts = 0, [], []
            for ch in rest:
                depth += ch in "([{"
                depth -= ch in ")]}"
                if ch == "," and depth == 0:
                    parts.append("".join(part))
                    part = []
                else:
                    part.append(ch)
            parts.append("".join(part))
            for declarator in parts:
                m = re.match(r"\s*(?:\*\s*)*([A-Za-z_]\w*)", declarator)
                if m:
                    found.setdefault(m.group(1), offset)
        offset += len(line)
    return found


class RestCleanupInitContract(unittest.TestCase):
    def setUp(self):
        self.body, self.base_line = on_request_body()
        self.freed = freed_names(self.body)

    def line_of(self, pos: int) -> int:
        return self.base_line + self.body.count("\n", 0, pos)

    def test_cleanup_frees_expected_pointers(self):
        missing = EXPECTED_MIN - self.freed
        self.assertFalse(missing, f"cleanup 해제 목록 파싱 결과에 {sorted(missing)} 없음 — 계약 파서 점검")

    def test_freed_pointers_initialized_before_first_goto(self):
        clean, _ = labels(self.body)
        first_goto = re.search(r"\bgoto\s+cleanup(?:_no_free)?\s*;", self.body)
        self.assertIsNotNone(first_goto, "_on_request에 goto cleanup이 없다")
        decls = function_scope_decls(self.body[:clean])
        late = {}
        for name in sorted(self.freed):
            with self.subTest(name=name):
                self.assertIn(name, decls, f"{name}: 함수 지역 선언을 찾지 못함")
                if decls[name] > first_goto.start():
                    late[name] = self.line_of(decls[name])
        self.assertFalse(late, f"cleanup이 해제하는 포인터가 첫 goto cleanup({self.line_of(first_goto.start())}행) 뒤에서야"
                         f" 선언된다(선언 행: {late}) — 그 goto가 초기화를 건너뛰어 쓰레기 포인터를 해제한다")

    def test_no_bare_return_after_request_id_allocated(self):
        clean, _ = labels(self.body)
        alloc = re.search(r"\breq_id\s*=\s*pcv_generate_request_id\s*\(", self.body)
        self.assertIsNotNone(alloc, "req_id 할당 지점을 찾지 못함")

        region = self.body[alloc.start():clean]
        lines = [self.line_of(alloc.start() + m.start()) for m in re.finditer(r"\breturn\s*;", region)]
        self.assertFalse(lines, f"req_id 할당 뒤 cleanup:을 건너뛰는 return;이 {len(lines)}개 있다"
                         f"(행: {lines}) — req_id·trace_ctx가 새고 TLS 요청 ID가 남는다. goto cleanup으로 빠질 것")


if __name__ == "__main__":
    unittest.main()
