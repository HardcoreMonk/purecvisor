#!/usr/bin/env python3
                          
                                                 
                                              
                                                                 
                                                                  
 
                      
                                                  
   
                                            

                                                          
                                                      

                                         
                                           
                                      
                                           
              

                                                                  
                                               
                                                    
                                                  
   
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / "ui"

PATTERN = re.compile(
    r'\.(innerHTML|outerHTML)\b|\.insertAdjacentHTML\s*\(|\bdocument\.write\s*\('
)


def scan_file(path):
    hits = []
    for i, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if PATTERN.search(line):
            hits.append((i, line.strip()))
    return hits


def main():
    files = sorted(UI.glob("modules/*.js")) + [UI / "app.js"]
    files = [f for f in files if f.exists() and f.name not in ("bundle.js", "app.bundle.js")]

    total = 0
    for f in files:
        hits = scan_file(f)
        if not hits:
            continue
        print(f"\n{f.relative_to(ROOT)}: {len(hits)}")
        for ln, code in hits:
            print(f"  L{ln}: {code[:140]}")
        total += len(hits)

    print(f"\nTOTAL innerHTML/outerHTML/insertAdjacentHTML/document.write sites: {total}")
    print("(ADR-013 DOM-safe 가시성 래칫 — 게이트 아님, exit 0 고정. "
          "신규 diff가 이 숫자를 늘리는지는 코드리뷰에서 직접 확인)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
