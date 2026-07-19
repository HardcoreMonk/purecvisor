
"""
Verify that generated UI artifacts match ui/modules/*.js + ui/app.js.

Intentionally non-mutating. If it fails, regenerate with:

    make ui-bundle        (또는 래퍼: scripts/bundle-ui.sh)

파이프라인 단일 소스는 Makefile `ui-bundle` 타깃 (2026-07-06 프론트 #2/#3
이후 — 구 bundle-ui.sh 자체 concat/ORDER 경로는 은퇴, 래퍼로 대체).
"""
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
    text = MAKEFILE.read_text(encoding="utf-8")
    match = re.search(r"^UI_MODULES\s*=\s*(?P<body>.*?)(?<!\\)\n", text, re.M | re.S)
    if not match:
        raise RuntimeError("Makefile UI_MODULES definition not found")
    body = match.group("body").replace("\\\n", " ")
    return [ROOT / token.replace("$(UI_DIR)", "ui") for token in body.split()]

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
    m = re.search(r"src-sha1 ([0-9a-f]{8})", head)
    if not m:
        return fail(
            "app.bundle.js 배너에 src-sha1 없음 — 민파이 파이프라인"
            "(make ui-bundle, esbuild 필요)으로 재생성 후 커밋"
        )
    if m.group(1) != src8:
        return fail(f"app.bundle.js(src-sha1 {m.group(1)}) 가 소스({src8})와 불일치")

    bundle8 = hashlib.sha1(BUNDLE.read_bytes()).hexdigest()[:8]
    sw_match = re.search(r"const CACHE_NAME = 'pcv-ui-v([0-9a-f]+)';", SW.read_text(encoding="utf-8"))
    if not sw_match:
        return fail("sw.js 에 CACHE_NAME = 'pcv-ui-v<hash>' 패턴 없음")
    if sw_match.group(1) != bundle8:
        return fail(f"sw.js CACHE_NAME(v{sw_match.group(1)}) 이 app.bundle.js({bundle8})와 불일치")

    print(f"OK: app.bundle.js src-sha1 {src8}, sw.js CACHE_NAME v{bundle8} 일치 ({len(mods)} sources)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
