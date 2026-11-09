#!/usr/bin/env python3
                                                                                               
                                                                                  
                                                                                       
                                                       
                                                                                    
                                                                                          
                                                                       
                                                                  
                                                                
                                         
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import rpc_extract              

LEN_RE = re.compile(r"\bg_strv_length\s*\(\s*segs\s*\)")
INDEX_RE = re.compile(r"\bsegs\s*\[\s*(\d+)\s*\]")
PLUS_RE = re.compile(r"\bsegs\s*\+\s*(\d+)\b")


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


def check(text: str, functions=None) -> dict[str, list[str]]:
                                                           
    out: dict[str, list[str]] = {"length_first": [], "constant_index": [], "plus_offset": []}
    code = strip_c(text)
    spans = rpc_extract.rest_request_spans(code, functions)
    if spans is None:
        for k in out:
            out[k].append("요청 범위(_on_request 또는 구간 함수 정의)를 찾지 못함")
        return out
    body = "".join(code[a:b] for a, b in spans)                  
    raw = "".join(text[a:b] for a, b in spans)                           

    def loc(pos: int) -> str:
        return rpc_extract.rest_text_location(text, rpc_extract.rest_scope_origin(spans, pos))

    m = LEN_RE.search(body)
    first_idx = INDEX_RE.search(body)
    if m is None:
        out["length_first"].append("nseg = g_strv_length(segs) 가 없다")
    if first_idx is None:
        out["length_first"].append("segs[N] 인덱싱이 없다(요청 범위를 못 읽음?)")
    if m is not None and first_idx is not None and m.start() > first_idx.start():
        out["length_first"].append(f"g_strv_length(segs)({loc(m.start())})가 첫 segs[N]({loc(first_idx.start())})보다 뒤에 있다")

                                                               
    pos = 0
    for line in body.splitlines(keepends=True):
        for im in INDEX_RE.finditer(line):
            k = int(im.group(1))
            if not re.search(r"\bnseg\s*>\s*%d\b" % k, line):
                out["constant_index"].append(f"{loc(pos + im.start())}: {line.strip()}")
        pos += len(line)

                                                                          
                                                                         
    plus = list(PLUS_RE.finditer(body))
    if not plus:
        out["plus_offset"].append("segs + N 사용을 요청 범위에서 0건 찾았다 — 매핑 사슬(구간 함수)을 못 읽으면 "
                                  "가드 검사가 공허 통과한다")
    for pm in plus:
        off = int(pm.group(1))
        if off < 1:
            out["plus_offset"].append(f"{loc(pm.start())}: segs+0 같은 접근은 segs와 동일 — 의도 확인 필요")
                                                                
        if '"volumes"' not in raw[max(0, pm.start() - 1500):pm.start()]:
            out["plus_offset"].append(f"segs+{off}({loc(pm.start())})가 action 가드 분기 밖에 있다")
    return out


class RestSegsBoundsContract(unittest.TestCase):
                                   

    @classmethod
    def setUpClass(cls):
        cls.result = check(rpc_extract.rest_family_text(ROOT))

    def test_nseg_computed_before_indexing(self):
        self.assertFalse(self.result["length_first"], self.result["length_first"])

    def test_every_constant_index_is_length_guarded(self):
        self.assertFalse(self.result["constant_index"], "nseg로 가드되지 않은 상수 인덱스 segs[N] 접근: "
                         + "; ".join(self.result["constant_index"]))

    def test_segs_plus_offset_is_action_guarded(self):
        self.assertFalse(self.result["plus_offset"], self.result["plus_offset"])


                                                                                     
SPLIT = """\
static void
_on_request(SoupServer *server, SoupServerMessage *msg, const gchar *path, GHashTable *query, gpointer user_data)
{
    PcvRestRequest rq_storage = {0};
    PcvRestRequest *rq = &rq_storage;
    gchar **segs = nullptr;
    segs = pcv_rest_split_path(path);
    rq->segs = segs;
    const guint nseg = g_strv_length(segs);
    const gchar *resource = nseg > 0 ? segs[0] : "";
    const gchar *action   = nseg > 2 ? segs[2] : "";
    PcvRestStep st = pcv_rest_map_compute(rq);
    if (st == PCV_REST_STEP_CLEANUP) goto cleanup;
cleanup:
    g_strfreev(rq->segs);
}

/* ==== src/api/rest_map_compute.c ==== */
PcvRestStep
pcv_rest_map_compute(PcvRestRequest *rq)
{
    gchar **segs = rq->segs;
    const gchar *action = rq->action;
    if (g_strcmp0(action, "volumes") == 0) {
        rq->rpc = pcv_rest_build_rpc_params("container.volume.detach", segs + 3);
    }
    return PCV_REST_STEP_UNMATCHED;
}
"""


def _once(text: str, old: str, new: str) -> str:
    assert text.count(old) == 1, f"합성 텍스트 앵커가 하나가 아님: {old!r}"
    return text.replace(old, new, 1)


class RestSegsBoundsCounterfactual(unittest.TestCase):
    def assertRed(self, text: str, key: str, needle: str):
        got = check(text)
        self.assertTrue(got[key] and any(needle in r for r in got[key]), got)

    def test_split_shape_passes(self):
        got = check(SPLIT)
        self.assertFalse(any(got.values()), got)

    def test_unguarded_index_in_block_function_is_red(self):
        bad = _once(SPLIT, "    gchar **segs = rq->segs;\n",
                    "    gchar **segs = rq->segs;\n    const gchar *tail = segs[3];\n")
        self.assertRed(bad, "constant_index", "segs[3]")

    def test_plus_offset_outside_action_branch_is_red(self):
        bad = _once(SPLIT, "    gchar **segs = rq->segs;\n",
                    "    gchar **segs = rq->segs;\n    detach_all(segs + 3);\n")
        self.assertRed(bad, "plus_offset", "action 가드 분기 밖")

    def test_no_plus_offset_anywhere_is_red(self):
        self.assertRed(_once(SPLIT, "segs + 3", "NULL"), "plus_offset", "0건")

    def test_block_definition_missing_is_red(self):
                                                                         
        gone = _once(SPLIT, "pcv_rest_map_compute(PcvRestRequest *rq)\n{", "pcv_rest_map_gone(PcvRestRequest *rq)\n{")
        self.assertRed(gone, "plus_offset", "구간 함수 정의")

    def test_length_after_first_index_is_red(self):
        bad = _once(SPLIT, "    const guint nseg = g_strv_length(segs);\n", "")
        bad = _once(bad, "    PcvRestStep st =", "    const guint nseg = g_strv_length(segs);\n    PcvRestStep st =")
        self.assertRed(bad, "length_first", "보다 뒤에")


if __name__ == "__main__":
    unittest.main()
