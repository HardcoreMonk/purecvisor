#!/usr/bin/env python3
                          
                                                                     
                                                      
                                                                 
                                                      
 
                      
                                                  
                                                  
                                                 
                                                   

      

                                                  
                                               
                                       
                                                      

                                                
                                                         
                                               
   

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
CENTRAL_SPAWN = "src/utils/pcv_spawn.c"
PRIVDROP_SOURCE = "src/utils/pcv_privdrop.c"
SYSTEMD_UNIT = "packaging/deb/purecvisorsd.service"
MODULES_LOAD = "packaging/deb/purecvisor-lio.conf"

DROPPED_CAPS = {
    "CAP_SYS_MODULE",
    "CAP_SYS_RAWIO",
    "CAP_SYS_TIME",
    "CAP_MAC_OVERRIDE",
    "CAP_MAC_ADMIN",
}
REQUIRED_MODULES = [
    "target_core_mod",
    "iscsi_target_mod",
    "target_core_iblock",
    "nf_conntrack_bridge",
]
PROFILE_CONSTANTS = {
    "PCV_CHILD_CAP_BASE",
    "PCV_CHILD_CAP_STORAGE",
    "PCV_CHILD_CAP_SIGNAL",
    "PCV_CHILD_CAP_DHCP",
    "PCV_CHILD_CAP_RUNTIME",
}
FORBIDDEN_SPAWN_RE = re.compile(
    r"\b(?:g_subprocess_newv|g_subprocess_launcher_(?:new|spawnv)|"
    r"g_spawn_[A-Za-z0-9_]+)\s*\("
)


def strip_c_comments_and_literals(text: str) -> str:
                                               
    out: list[str] = []
    state = "code"
    i = 0
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "code":
            if ch == "/" and nxt == "/":
                out.extend((" ", " "))
                state = "line-comment"
                i += 2
                continue
            if ch == "/" and nxt == "*":
                out.extend((" ", " "))
                state = "block-comment"
                i += 2
                continue
            if ch == '"':
                out.append(" ")
                state = "string"
                i += 1
                continue
            if ch == "'":
                out.append(" ")
                state = "char"
                i += 1
                continue
            out.append(ch)
            i += 1
            continue

        if state == "line-comment":
            out.append("\n" if ch == "\n" else " ")
            if ch == "\n":
                state = "code"
            i += 1
            continue

        if state == "block-comment":
            if ch == "*" and nxt == "/":
                out.extend((" ", " "))
                state = "code"
                i += 2
                continue
            out.append("\n" if ch == "\n" else " ")
            i += 1
            continue

                                             
        out.append("\n" if ch == "\n" else " ")
        if ch == "\\" and i + 1 < len(text):
            next_ch = text[i + 1]
            out.append("\n" if next_ch == "\n" else " ")
            i += 2
            continue
        if (state == "string" and ch == '"') or (state == "char" and ch == "'"):
            state = "code"
        i += 1
    return "".join(out)


def split_top_level_args(argument_text: str) -> list[str]:
                                               
    args: list[str] = []
    start = 0
    round_depth = square_depth = brace_depth = 0
    for index, ch in enumerate(argument_text):
        if ch == "(":
            round_depth += 1
        elif ch == ")":
            round_depth -= 1
        elif ch == "[":
            square_depth += 1
        elif ch == "]":
            square_depth -= 1
        elif ch == "{":
            brace_depth += 1
        elif ch == "}":
            brace_depth -= 1
        elif ch == "," and round_depth == square_depth == brace_depth == 0:
            args.append(argument_text[start:index].strip())
            start = index + 1
    args.append(argument_text[start:].strip())
    return args


def find_calls(code: str, function_name: str) -> list[tuple[int, list[str]]]:
                                              
    calls: list[tuple[int, list[str]]] = []
    pattern = re.compile(rf"\b{re.escape(function_name)}\s*\(")
    for match in pattern.finditer(code):
        open_index = code.find("(", match.start())
        depth = 1
        cursor = open_index + 1
        while cursor < len(code) and depth:
            if code[cursor] == "(":
                depth += 1
            elif code[cursor] == ")":
                depth -= 1
            cursor += 1
        if depth:
            line = code.count("\n", 0, match.start()) + 1
            raise ValueError(f"{function_name} 호출의 닫는 괄호가 없음(line {line})")
        line = code.count("\n", 0, match.start()) + 1
        calls.append((line, split_top_level_args(code[open_index + 1 : cursor - 1])))
    return calls


def source_errors(rel_path: str, text: str) -> tuple[list[str], int]:
                                                   
    code = strip_c_comments_and_literals(text)
    errors: list[str] = []
    checked = 0
    if rel_path != CENTRAL_SPAWN:
        for match in FORBIDDEN_SPAWN_RE.finditer(code):
            line = code.count("\n", 0, match.start()) + 1
            called = match.group(0).split("(", 1)[0].strip()
            errors.append(
                f"{rel_path}:{line}: '{called}'가 중앙 pcv_spawn launcher를 우회한다"
            )
            checked += 1

        for name, profile_index in (
            ("pcv_spawn_newv_profile", 2),
            ("pcv_spawn_sync_profile", 1),
        ):
            for line, args in find_calls(code, name):
                checked += 1
                if len(args) <= profile_index:
                    errors.append(f"{rel_path}:{line}: {name} profile 인자가 없음")
                    continue
                profile = args[profile_index]
                if profile not in PROFILE_CONSTANTS:
                    errors.append(
                        f"{rel_path}:{line}: {name} profile은 고정 enum 상수여야 함: "
                        f"'{profile or '<empty>'}'"
                    )
    return errors, checked


def configuration_errors(root: Path) -> tuple[list[str], int]:
                                                                     
    errors: list[str] = []
    checked = 3
    try:
        privdrop = (root / PRIVDROP_SOURCE).read_text(encoding="utf-8")
        unit = (root / SYSTEMD_UNIT).read_text(encoding="utf-8")
        modules_text = (root / MODULES_LOAD).read_text(encoding="utf-8")
    except OSError as exc:
        raise RuntimeError(f"capability 설정 파일 읽기 실패: {exc}") from exc

    match = re.search(
        r"PCV_SPAWN_CEILING_MASK\s*=\s*(.*?);",
        strip_c_comments_and_literals(privdrop),
        re.DOTALL,
    )
    if not match:
        errors.append(f"{PRIVDROP_SOURCE}: PCV_SPAWN_CEILING_MASK initializer를 찾지 못함")
    else:
        source_drops = set(re.findall(r"~\s*PCV_CAP_BIT\s*\(\s*(CAP_[A-Z0-9_]+)\s*\)", match.group(1)))
        if source_drops != DROPPED_CAPS:
            errors.append(
                f"{PRIVDROP_SOURCE}: ceiling 제외={sorted(source_drops)}, "
                f"기대={sorted(DROPPED_CAPS)}"
            )

    bounding_lines = [
        line.strip().split("=", 1)[1].split()
        for line in unit.splitlines()
        if line.strip().startswith("CapabilityBoundingSet=")
    ]
    if len(bounding_lines) != 1 or not bounding_lines[0] or not bounding_lines[0][0].startswith("~"):
        errors.append(f"{SYSTEMD_UNIT}: negative CapabilityBoundingSet 한 줄이 필요함")
    else:
        tokens = bounding_lines[0]
        unit_drops = {tokens[0][1:], *tokens[1:]}
        if unit_drops != DROPPED_CAPS:
            errors.append(
                f"{SYSTEMD_UNIT}: bounding 제외={sorted(unit_drops)}, "
                f"기대={sorted(DROPPED_CAPS)}"
            )

    modules = [
        line.strip()
        for line in modules_text.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if modules != REQUIRED_MODULES:
        errors.append(
            f"{MODULES_LOAD}: modules={modules}, 기대={REQUIRED_MODULES}"
        )
    return errors, checked


def check_tree(root: Path) -> tuple[list[str], int]:
                                                     
    errors: list[str] = []
    checked = 0
    for path in sorted((root / "src").rglob("*.c")):
        rel_path = path.relative_to(root).as_posix()
        try:
            file_errors, file_checked = source_errors(
                rel_path, path.read_text(encoding="utf-8")
            )
        except (OSError, ValueError) as exc:
            raise RuntimeError(f"{rel_path} 검사 실패: {exc}") from exc
        errors.extend(file_errors)
        checked += file_checked
    config_errors, config_checked = configuration_errors(root)
    errors.extend(config_errors)
    checked += config_checked
    return errors, checked


def self_test() -> int:
                                                
    safe = """
/* g_subprocess_newv(argv, error); */
const char *example = "g_spawn_sync(";
pcv_spawn_sync_profile(argv, PCV_CHILD_CAP_STORAGE, NULL, NULL, error);
"""
    errors, checked = source_errors("src/modules/example.c", safe)
    if errors or checked != 1:
        print(f"self-test safe fixture 실패: errors={errors}, checked={checked}", file=sys.stderr)
        return 1

    bypass = "g_subprocess_newv(argv, error);"
    errors, _ = source_errors("src/modules/bypass.c", bypass)
    if not any("우회" in error for error in errors):
        print("self-test direct spawn 반사실을 놓침", file=sys.stderr)
        return 1

    dynamic = "pcv_spawn_newv_profile(argv, flags, user_profile, error);"
    errors, _ = source_errors("src/modules/dynamic.c", dynamic)
    if not any("고정 enum" in error for error in errors):
        print("self-test dynamic profile 반사실을 놓침", file=sys.stderr)
        return 1

    multiline = "pcv_spawn_newv_profile(\n argv,\n flags,\n PCV_CHILD_CAP_RUNTIME,\n error);"
    errors, checked = source_errors("src/modules/multiline.c", multiline)
    if errors or checked != 1:
        print(f"self-test multiline fixture 실패: errors={errors}, checked={checked}", file=sys.stderr)
        return 1

    print("[check-spawn-capabilities:self-test] PASS (4/4)")
    return 0


def main() -> int:
                                                               
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    try:
        errors, checked = check_tree(ROOT)
    except RuntimeError as exc:
        print(f"[check-spawn-capabilities] ERROR: {exc}", file=sys.stderr)
        return 2
    if errors:
        for error in errors:
            print(f"[check-spawn-capabilities] FAIL: {error}", file=sys.stderr)
        return 1
    print(
        "[check-spawn-capabilities] PASS "
        f"({checked} spawn/profile/config 계약, drop={len(DROPPED_CAPS)}, "
        f"modules={len(REQUIRED_MODULES)})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
