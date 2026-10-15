#!/usr/bin/env python3






import re
import unittest
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "src/api/rest_server.c"
ANCHOR = "\n_on_request(SoupServer"


def strip_c(text: str) -> str:

    out, i, n = [], 0, len(text)
    while i < n:
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
        elif text[i] in "\"'":
            c = text[i]
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(c + " " * (min(j, n) - i - 1) + (c if j < n else ""))
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def on_request_body() -> tuple[str, int, str]:

    raw = SRC.read_text()
    code = strip_c(raw)
    start = code.index(ANCHOR)
    end = code.index("\n}\n", start) + 2
    return code[start:end], code.count("\n", 0, start) + 1, raw[start:end]


class RestSegsBoundsContract(unittest.TestCase):
    def setUp(self):
        self.body, self.base, self.raw = on_request_body()

    def line_of(self, pos: int) -> int:
        return self.base + self.body.count("\n", 0, pos)

    def test_nseg_computed_before_indexing(self):
        m = re.search(r"\bg_strv_length\s*\(\s*segs\s*\)", self.body)
        self.assertIsNotNone(m, "nseg = g_strv_length(segs) 가 없다")
        first_idx = re.search(r"\bsegs\s*\[\s*\d+\s*\]", self.body)
        self.assertIsNotNone(first_idx, "segs[N] 인덱싱이 없다")
        self.assertLess(m.start(), first_idx.start(),
                        f"g_strv_length(segs)(L{self.line_of(m.start())})가 "
                        f"첫 segs[N](L{self.line_of(first_idx.start())})보다 뒤에 있다")

    def test_every_constant_index_is_length_guarded(self):

        lines = self.body.splitlines()
        bad = []
        for i, line in enumerate(lines):
            for m in re.finditer(r"\bsegs\s*\[\s*(\d+)\s*\]", line):
                k = int(m.group(1))
                if re.search(r"\bnseg\s*>\s*%d\b" % k, line):
                    continue
                bad.append((self.base + i, line.strip()))
        self.assertFalse(bad, "nseg로 가드되지 않은 상수 인덱스 segs[N] 접근: "
                         + "; ".join(f"L{n}: {t}" for n, t in bad))

    def test_segs_plus_offset_is_action_guarded(self):


        for m in re.finditer(r"\bsegs\s*\+\s*(\d+)\b", self.body):
            off = int(m.group(1))
            self.assertGreaterEqual(off, 1, "segs+0 같은 접근은 segs와 동일 — 의도 확인 필요")

            ctx = self.raw[max(0, m.start() - 1500):m.start()]
            self.assertIn('"volumes"', ctx,
                          f"segs+{off}(L{self.line_of(m.start())})가 action 가드 분기 밖에 있다")


if __name__ == "__main__":
    unittest.main()
