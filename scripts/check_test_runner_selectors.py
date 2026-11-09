#!/usr/bin/env python3
                          
                                                          
                                                        
                                                               
                                                          
 
                      
                                               
                                                         
                                                

                                                          
                                                        
                                                        

                                                        
                                     

                                                             
                                                            
    
   

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
RUNNER_RE = re.compile(
    r"(?:^|[\s`])(?:sudo\s+)?(?:\./)?(?:bin/)?test_runner(?:\s|$)"
)
PATH_SELECTOR_RE = re.compile(
    r"(?:^|\s)(?P<option>-[pr])\s+"
    r"(?P<selector>/[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*)"
)
LISTED_PATH_RE = re.compile(r"^# (?P<path>/\S+)$", re.MULTILINE)

REGISTER_CALL_RE = re.compile(r"\bg_test_add\w*\s*\(")

FUNCTION_MACRO_RE = re.compile(
    r"^[ \t]*#[ \t]*define[ \t]+(?P<name>[A-Za-z_]\w*)\((?P<params>[^)]*)\)(?P<body>.*)$",
    re.MULTILINE,
)
PREPROCESSOR_LINE_RE = re.compile(r"^[ \t]*#.*$", re.MULTILINE)
IDENT_RE = re.compile(r"[A-Za-z_]\w*")


def canonical_files(root: Path) -> list[Path]:
                                            
    paths = list((root / "tests").glob("test_*.c"))
    paths.extend(
        [
            root / "docs" / "GUIDE.md",
            root / "docs" / "DEVELOPMENT_VERIFICATION_POLICY.md",
            root / "docs" / "ADR_INDEX.md",
        ]
    )
    paths.extend((root / "docs" / "adr").glob("*.md"))
    return sorted(set(paths))


def listed_paths(test_runner: Path) -> set[str]:
                                                      
    try:
        result = subprocess.run(
            [str(test_runner), "-l"],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError as exc:
        raise RuntimeError(f"test runner 실행 실패: {exc}") from exc
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(
            f"test runner 목록 조회 실패(exit {result.returncode}): {detail}"
        )
    paths = set(LISTED_PATH_RE.findall(result.stdout))
    if not paths:
        raise RuntimeError("test runner가 등록 경로를 하나도 출력하지 않았다")
    return paths


def selector_errors(
    rel_path: str,
    text: str,
    registered: set[str],
) -> tuple[list[str], int]:
                                                          
    errors: list[str] = []
    checked = 0
    for line_number, line in enumerate(text.splitlines(), start=1):
        if not RUNNER_RE.search(line):
            continue
                                                         
                                                     
        for match in PATH_SELECTOR_RE.finditer(line):
            selector = match.group("selector")
            option = match.group("option")
            if "..." in selector:
                continue
            checked += 1
            descendants = any(
                path.startswith(f"{selector}/") for path in registered
            )
            exact = selector in registered
            if option == "-p" and descendants:
                errors.append(
                    f"{rel_path}:{line_number}: '{selector}'는 suite prefix다; "
                    "전체 실행에는 -p 대신 -r을 사용해야 한다"
                )
            elif not exact and not descendants:
                errors.append(
                    f"{rel_path}:{line_number}: 등록되지 않은 {option} selector "
                    f"'{selector}'는 0건 실행 위험이 있다"
                )
    return errors, checked


def check_tree(root: Path, registered: set[str]) -> tuple[list[str], int]:
                                         
    errors: list[str] = []
    checked = 0
    for path in canonical_files(root):
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as exc:
            raise RuntimeError(f"정본 파일 읽기 실패({path}): {exc}") from exc
        if path.parent == root / "tests":
                                                          
                                                             
            header_lines: list[str] = []
            for line in text.splitlines():
                if line.lstrip().startswith("#include"):
                    break
                header_lines.append(line)
            text = "\n".join(header_lines)
        rel_path = path.relative_to(root).as_posix()
        file_errors, file_checked = selector_errors(rel_path, text, registered)
        errors.extend(file_errors)
        checked += file_checked
    return errors, checked


def strip_c_comments(text: str) -> str:

    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch in "\"'":
            j = i + 1
            while j < n and text[j] != ch and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif ch == "/" and nxt == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("".join("\n" if c == "\n" else " " for c in text[i:end]))
            i = end
        elif ch == "/" and nxt == "/":
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
        else:
            out.append(ch)
            i += 1
    return "".join(out)

def _mask_string_contents(text: str) -> str:

    out = list(text)
    i = 0
    n = len(text)
    while i < n:
        if text[i] in "\"'":
            quote = text[i]
            j = i + 1
            while j < n and text[j] != quote and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            for k in range(i + 1, min(j, n)):
                if out[k] != "\n":
                    out[k] = " "
            i = j + 1
        else:
            i += 1
    return "".join(out)

def _split_call_args(text: str, start: int) -> tuple[list[str], int] | None:

    args: list[str] = []
    depth = 0
    i = start
    piece = start
    n = len(text)
    while i < n:
        ch = text[i]
        if ch in "\"'":
            j = i + 1
            while j < n and text[j] != ch:
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            if depth == 0:
                args.append(text[piece:i])
                return args, i + 1
            depth -= 1
        elif ch == "," and depth == 0:
            args.append(text[piece:i])
            piece = i + 1
        i += 1
    return None

def _arg_tokens(arg: str) -> list[tuple[str, str]] | None:

    tokens: list[tuple[str, str]] = []
    i = 0
    n = len(arg)
    while i < n:
        ch = arg[i]
        if ch.isspace():
            i += 1
        elif ch == '"':
            j = i + 1
            while j < n and arg[j] != '"':
                j += 2 if arg[j] == "\\" else 1
            if j >= n:
                return None
            tokens.append(("str", arg[i + 1:j]))
            i = j + 1
        else:
            match = IDENT_RE.match(arg, i)
            if not match:
                return None
            tokens.append(("ident", match.group(0)))
            i = match.end()
    return tokens or None

def registered_test_paths(text: str) -> tuple[set[str], int]:

    code = strip_c_comments(text).replace("\\\n", " ")
    macros: dict[str, tuple[list[str], list[tuple[str, str]]]] = {}
    unresolved = 0
    for match in FUNCTION_MACRO_RE.finditer(code):
        body = match.group("body")
        call = REGISTER_CALL_RE.search(body)
        if not call:
            continue
        params = [p.strip() for p in match.group("params").split(",") if p.strip()]
        parsed = _split_call_args(body, call.end())
        first = _arg_tokens(parsed[0][0]) if parsed and parsed[0] else None
        if first is None:
            unresolved += 1
            continue
        macros[match.group("name")] = (params, first)
    body_code = PREPROCESSOR_LINE_RE.sub("", code)

    masked = _mask_string_contents(body_code)

    paths: set[str] = set()
    for call in REGISTER_CALL_RE.finditer(masked):
        parsed = _split_call_args(body_code, call.end())
        first = _arg_tokens(parsed[0][0]) if parsed and parsed[0] else None
        if first is None or any(kind != "str" for kind, _ in first):
            unresolved += 1
            continue
        paths.add("".join(value for _, value in first))
    for name, (params, template) in macros.items():
        for call in re.finditer(rf"\b{re.escape(name)}\s*\(", masked):
            parsed = _split_call_args(body_code, call.end())
            if parsed is None:
                unresolved += 1
                continue
            args = parsed[0]
            pieces: list[str] = []
            for kind, value in template:
                if kind == "str":
                    pieces.append(value)
                    continue
                if value not in params or params.index(value) >= len(args):
                    pieces = []
                    break
                arg = _arg_tokens(args[params.index(value)])
                if arg is None or any(k != "str" for k, _ in arg):
                    pieces = []
                    break
                pieces.extend(v for _, v in arg)
            if pieces:
                paths.add("".join(pieces))
            else:
                unresolved += 1
    return {p for p in paths if p.startswith("/")}, unresolved

def paths_for_files(files: list[Path], registered: set[str]) -> tuple[list[str], str]:

    extracted: set[str] = set()
    unresolved = 0
    for path in files:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            raise RuntimeError(f"시험 파일 읽기 실패({path}): {exc}") from exc
        file_paths, file_unresolved = registered_test_paths(text)
        extracted |= file_paths
        unresolved += file_unresolved
    selected = sorted(extracted & registered)
    summary = (
        f"[paths-for] 파일 {len(files)}개, 추출 경로 {len(extracted)}개, "
        f"러너 목록 일치 {len(selected)}개, 목록 밖 제외 {len(extracted) - len(selected)}개, "
        f"동적 등록 미추출 {unresolved}건"
    )
    return selected, summary

def self_test() -> int:
                                                   
    registered = {
        "/suite/one",
        "/suite/two",
        "/leaf",
        "/mixed",
        "/mixed/child",
    }
    sample = "\n".join(
        [
            "./test_runner -p /suite",
            "./test_runner -p /leaf",
            "./test_runner -p /missing",
            "./test_runner -r /suite",
            "./test_runner -r /missing-recursive",
            "./test_runner -p /trace/...",
            "./test_runner -p /mixed",
            "./test_runner -p /leaf -p /suite",
        ]
    )
    errors, checked = selector_errors("fixture.md", sample, registered)
    expected_fragments = ["'/suite'는 suite", "'/missing'는 0건", "'/mixed'는 suite"]
    if checked != 8 or len(errors) != 5:
        print(
            f"FAIL: selector self-test count mismatch "
            f"(checked={checked}, errors={len(errors)})"
        )
        return 1
    joined = "\n".join(errors)
    if not all(fragment in joined for fragment in expected_fragments):
        print(f"FAIL: selector self-test classification mismatch\n{joined}")
        return 1
    print("PASS: test runner selector self-test (suite/leaf/unknown/ellipsis)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--test-runner",
        type=Path,
        default=ROOT / "test_runner",
        help="`-l` inventory를 제공할 test_runner 경로",
    )
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument(
        "--paths-for",
        nargs="+",
        type=Path,
        metavar="TEST_C",
        help="판정 대신 이 시험 파일들이 등록한 leaf 경로 중 러너 목록에 있는 것만 출력한다",
    )
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    runner = args.test_runner
    if not runner.is_absolute():
        runner = (ROOT / runner).resolve()
    if args.paths_for:
        try:
            selected, summary = paths_for_files(
                [p if p.is_absolute() else ROOT / p for p in args.paths_for],
                listed_paths(runner),
            )
        except RuntimeError as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 2
        for path in selected:
            print(path)
        print(summary, file=sys.stderr)
        return 0
    try:
        registered = listed_paths(runner)
        errors, checked = check_tree(ROOT, registered)
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    if errors:
        for error in errors:
            print(f"FAIL: {error}")
        print(
            f"FAIL: test runner selector 계약 위반 {len(errors)}건 "
            f"(검사한 -p 명령 {checked}개)"
        )
        return 1
    print(
        f"PASS: test runner selector 계약 "
        f"(등록 경로 {len(registered)}개, 검사한 -p/-r selector {checked}개)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
