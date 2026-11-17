#!/usr/bin/env python3
                          
                                                                                                          
                                                                                          
                                                              
                                                                                                         
                                                                   
                                                                                   
                                                                                          
                                                                                      
 
                                                                              
                                                
                              
from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "split"))
import c_blocks              

RAW_LABEL = re.compile(r'[A-Za-z_][A-Za-z0-9_]*=\\"%(?:\.\*)?s')
PRODUCER = re.compile(r"\bpcv_prom_\w+\s*\(|# TYPE |prometheus_exporter\.h")
EXEMPT = (                            
    ("src/modules/security/pcv_suricata.c", "not an alert event (event_type=",
     "EVE 줄 거절 사유 문구(오류 메시지) — Prometheus 라벨이 아니다"),
    ("src/modules/dispatcher/handler_vm_ova.c", "<File ovf:href=",
     "OVF 내보내기 XML 속성 — Prometheus 라벨이 아니다(값 이스케이프는 XML 규칙 몫)"),
    ("src/modules/dispatcher/handler_vm_ova.c", "<VirtualSystem ovf:id=",
     "OVF 내보내기 XML 속성 — Prometheus 라벨이 아니다(값 이스케이프는 XML 규칙 몫)"),
)


def _no_comments(src: str) -> str:
    kinds = c_blocks.classify(src)
    return "".join(ch if k != "m" or ch == "\n" else " " for ch, k in zip(src, kinds))


def sources() -> dict[str, str]:
    return {str(p.relative_to(ROOT)): p.read_text(encoding="utf-8", errors="replace")
            for p in sorted((ROOT / "src").rglob("*.c"))}


def violations(srcs: dict[str, str]) -> tuple[list[str], set[int]]:
                            
    out, used = [], set()
    for rel, raw in srcs.items():
        text = _no_comments(raw)
        if not PRODUCER.search(text):
            continue
        lines = text.split("\n")
        for no, line in enumerate(lines, 1):
            if not RAW_LABEL.search(line):
                continue
            hit = [i for i, (f, frag, _) in enumerate(EXEMPT) if f == rel and frag in line]
            if hit:
                used.update(hit)
                continue
            out.append(f"{rel}:{no}: Prometheus 라벨을 printf 로 직접 조립한다 — pcv_prom_labels() 를 쓴다: {line.strip()[:120]}")
    return out, used


class PromLabelEscapeContract(unittest.TestCase):
    def test_no_raw_label_printf(self):
        srcs = sources()
        bad, used = violations(srcs)
        self.assertEqual(bad, [])
        self.assertEqual(sorted(used), list(range(len(EXEMPT))), "쓰이지 않는 예외가 있다(코드가 바뀌었으면 EXEMPT 를 지운다)")

    def test_counterfactual(self):
        srcs = sources()
        base = len(violations(srcs)[0])
        prod = "src/modules/daemons/vm_diag_view.c"
        bad = dict(srcs, **{prod: srcs[prod] + '\nstatic void x(char *b){ g_snprintf(b, 9, "vm=\\"%s\\"", "a"); }\n'})
        self.assertEqual(len(violations(bad)[0]), base + 1)
        bad = dict(srcs, **{prod: srcs[prod] + '\nstatic void y(char *b){ g_snprintf(b, 9, "k=\\"%.*s\\"", 1, "a"); }\n'})
        self.assertEqual(len(violations(bad)[0]), base + 1)
                                                       
        ok = dict(srcs, **{"src/x_xml.c": 'void f(char *b){ g_snprintf(b, 9, "<a id=\\"%s\\"/>", "x"); }\n',
                            prod: srcs[prod] + '\n/* g_snprintf(b, 9, "vm=\\"%s\\"", n); */\n'
                                               'static void z(char *b){ g_snprintf(b, 9, "le=\\"%.0f\\"", 1.0); }\n'})
        self.assertEqual(len(violations(ok)[0]), base)


if __name__ == "__main__":
    unittest.main(verbosity=1)
