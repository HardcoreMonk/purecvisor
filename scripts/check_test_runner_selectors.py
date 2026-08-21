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
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    runner = args.test_runner
    if not runner.is_absolute():
        runner = (ROOT / runner).resolve()
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
