#!/usr/bin/env python3
                          
                                                                                       
                                                                                                               
                                                                                                                            
 
                      
                                                                                                                                                
                                                                              
                                                                             
                                                                     
                                                               
                                                                        
                         
   
                                                                     

                                                         

                                                       

                                                          
                                                    
   
from __future__ import annotations

import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / "ui"
MAKEFILE = ROOT / "Makefile"
BUNDLE = UI / "app.bundle.js"
SW = UI / "sw.js"


def fail(message: str) -> int:
    print(f"FAIL: {message}", file=sys.stderr)
    print("hint: run make ui-bundle", file=sys.stderr)
    return 1


def load_ui_modules() -> list[Path]:
    return load_make_paths("UI_MODULES")


def load_make_paths(variable: str) -> list[Path]:
    text = MAKEFILE.read_text(encoding="utf-8")
    match = re.search(rf"^{re.escape(variable)}\s*=\s*(?P<body>.*?)(?<!\\)\n", text, re.M | re.S)
    if not match:
        raise RuntimeError(f"Makefile {variable} definition not found")
    body = match.group("body").replace("\\\n", " ")
    return [ROOT / token.replace("$(UI_DIR)", "ui") for token in body.split()]


def cache_identity(paths: list[Path]) -> str:
                                                                 
    manifest = bytearray()
    for path in paths:
        if not path.exists():
            raise FileNotFoundError(path)
        digest = hashlib.sha1(path.read_bytes()).hexdigest()
        relative = path.relative_to(ROOT).as_posix()
        manifest.extend(f"{digest}  {relative}\n".encode())
    return hashlib.sha1(manifest).hexdigest()[:8]


def main() -> int:
    mods = load_ui_modules()

                                                               
    listed = {p.resolve() for p in mods}
    for f in sorted((UI / "modules").glob("*.js")):
        if f.resolve() not in listed:
            return fail(f"{f.relative_to(ROOT)} 가 Makefile UI_MODULES 에 없음 (번들 누락)")

    h = hashlib.sha1()
    for p in mods:
        if not p.exists():
            return fail(f"UI_MODULES 에 있으나 파일 없음: {p.relative_to(ROOT)}")
        h.update(p.read_bytes())
    src8 = h.hexdigest()[:8]

    head = BUNDLE.read_bytes()[:300].decode("utf-8", "replace")
    m = re.search(r"PCV_UI_SOURCE_SHA1=['\"]([0-9a-f]{8})['\"]", head)
    if not m:
        return fail(
            "app.bundle.js에 PCV_UI_SOURCE_SHA1 없음 — 민파이 파이프라인"
            "(make ui-bundle, esbuild 필요)으로 재생성 후 커밋"
        )
    if m.group(1) != src8:
        return fail(f"app.bundle.js(src-sha1 {m.group(1)}) 가 소스({src8})와 불일치")

    cache_inputs = load_make_paths("UI_CACHE_INPUTS")
    try:
        cache8 = cache_identity(cache_inputs)
    except FileNotFoundError as exc:
        return fail(f"UI_CACHE_INPUTS 에 있으나 파일 없음: {exc.args[0].relative_to(ROOT)}")
    sw_match = re.search(r"const\s+CACHE_NAME\s*=\s*['\"]pcv-ui-v([0-9a-f]+)['\"]", SW.read_text(encoding="utf-8"))
    if not sw_match:
        return fail("sw.js 에 CACHE_NAME = 'pcv-ui-v<hash>' 패턴 없음")
    if sw_match.group(1) != cache8:
        return fail(
            f"sw.js CACHE_NAME(v{sw_match.group(1)}) 이 "
            f"UI_CACHE_INPUTS({cache8})와 불일치"
        )

    print(
        f"OK: app.bundle.js src-sha1 {src8}, sw.js CACHE_NAME v{cache8} 일치 "
        f"({len(mods)} sources, {len(cache_inputs)} cache inputs)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
