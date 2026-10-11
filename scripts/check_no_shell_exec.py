#!/usr/bin/env python3























from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ROOTS = [ROOT / "src"]
SOURCE_SUFFIXES = {".c", ".h"}

FORBIDDEN_CALL_RE = re.compile(r"(?<![\w.>])(system|popen)\s*\(")


def strip_comments_and_literals(text: str) -> str:

    out: list[str] = []
    i, n = 0, len(text)
    state = "code"
    quote = ""
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if ch == "/" and nxt == "/":
                state = "line"
                out.append("  ")
                i += 2
                continue
            if ch == "/" and nxt == "*":
                state = "block"
                out.append("  ")
                i += 2
                continue
            if ch in ('"', "'"):
                state = "literal"
                quote = ch
                out.append(" ")
                i += 1
                continue
            out.append(ch)
            i += 1
        elif state == "literal":
            if ch == "\\" and i + 1 < n:
                out.append("\n" if text[i + 1] == "\n" else " ")
                out.append(" ")
                i += 2
                continue
            if ch == quote:
                state = "code"
                out.append(" ")
            elif ch == "\n":
                state = "code"
                out.append("\n")
            else:
                out.append(" ")
            i += 1
        elif state == "line":
            if ch == "\n":
                state = "code"
                out.append("\n")
            else:
                out.append(" ")
            i += 1
        else:
            if ch == "*" and nxt == "/":
                state = "code"
                out.append("  ")
                i += 2
                continue
            out.append("\n" if ch == "\n" else " ")
            i += 1
    return "".join(out)


def find_violations(text: str) -> list[tuple[int, str]]:

    code = strip_comments_and_literals(text)
    hits = []
    for match in FORBIDDEN_CALL_RE.finditer(code):
        line = code.count("\n", 0, match.start()) + 1
        hits.append((line, match.group(1)))
    return hits


def collect_sources(targets: list[Path]) -> list[Path]:
    files: list[Path] = []
    for target in targets:
        if target.is_dir():
            files.extend(sorted(p for p in target.rglob("*")
                                if p.is_file() and p.suffix in SOURCE_SUFFIXES))
        elif target.is_file():
            files.append(target)
    return files


def main(argv: list[str]) -> int:
    targets = [Path(a) for a in argv[1:]] if len(argv) > 1 else DEFAULT_ROOTS
    files = collect_sources(targets)
    if not files:
        print("ERROR: 검사 대상 C 소스가 없다", file=sys.stderr)
        return 2
    violations: list[str] = []
    for path in files:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            print(f"ERROR: failed to read {path}: {exc}", file=sys.stderr)
            return 2
        for line, name in find_violations(text):
            try:
                shown = path.relative_to(ROOT)
            except ValueError:
                shown = path
            violations.append(f"{shown}:{line}: {name}()")
    print(f"[check-no-shell-exec] 대상 {len(files)}파일")
    if violations:
        print("[FAIL] system()/popen() 실호출 금지 위반 (ADR-0001·AGENTS §5.5):", file=sys.stderr)
        for item in violations:
            print(f"  - {item}", file=sys.stderr)
        print("       pcv_spawn_sync() argv 배열 또는 커널 파일 직접 읽기로 바꾼다.", file=sys.stderr)
        return 1
    print("[PASS] system()/popen() 실호출 0건")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
