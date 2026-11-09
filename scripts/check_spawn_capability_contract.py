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

LXC_SYSTEMD_UNIT = "packaging/systemd/purecvisor-lxc@.service"
SYSTEMD_UNITS = (SYSTEMD_UNIT, LXC_SYSTEMD_UNIT)
MODULES_LOAD = "packaging/deb/purecvisor-lio.conf"
VFIO_MODULES_LOAD = "packaging/deb/purecvisor-vfio.conf"

DROPPED_CAPS = {
    "CAP_SYS_MODULE",
    "CAP_SYS_RAWIO",
    "CAP_SYS_TIME",
    "CAP_MAC_OVERRIDE",
}
REQUIRED_MODULES = [
    "target_core_mod",
    "iscsi_target_mod",
    "target_core_iblock",
    "nf_conntrack_bridge",
]
REQUIRED_VFIO_MODULES = ["vfio_pci"]
PROFILE_CONSTANTS = {
    "PCV_CHILD_CAP_BASE",
    "PCV_CHILD_CAP_STORAGE",
    "PCV_CHILD_CAP_SIGNAL",
    "PCV_CHILD_CAP_DHCP",
    "PCV_CHILD_CAP_RUNTIME",
    "PCV_CHILD_CAP_TRACE",
}
FORBIDDEN_SPAWN_RE = re.compile(
    r"\b(?:g_subprocess_newv|g_subprocess_launcher_(?:new|spawnv)|"
    r"g_spawn_[A-Za-z0-9_]+)\s*\("
)



BOUND_DECL_RE = re.compile(
    r"\bPcvSpawnBound\s+(?:const\s+)?(?:\*\s*)?([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*(=\s*\{)?"
)
BOUND_LITERAL_RE = re.compile(r"\(\s*(?:const\s+)?PcvSpawnBound\s*\)\s*\{")


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


def balanced_brace_body(code: str, open_index: int) -> str:

    depth = 1
    cursor = open_index + 1
    while cursor < len(code) and depth:
        if code[cursor] == "{":
            depth += 1
        elif code[cursor] == "}":
            depth -= 1
        cursor += 1
    if depth:
        line = code.count("\n", 0, open_index) + 1
        raise ValueError(f"PcvSpawnBound 초기화자의 닫는 중괄호가 없음(line {line})")
    return code[open_index + 1 : cursor - 1]


def profile_violation(rel_path: str, profile: str | None) -> str | None:

    if profile is None:
        return "PcvSpawnBound 초기화자에 .profile이 없음(명시 profile 필요)"
    if profile not in PROFILE_CONSTANTS:
        return f"PcvSpawnBound profile은 고정 enum 상수여야 함: '{profile or '<empty>'}'"
    if profile == "PCV_CHILD_CAP_TRACE" and rel_path != "src/modules/daemons/pcv_trace.c":
        return "TRACE 프로필은 pcv_trace 수집 경계에만 허용됨"
    return None


def bound_initializer_profile(body: str) -> str | None:

    entries = [entry for entry in split_top_level_args(body) if entry]
    for entry in entries:
        match = re.fullmatch(r"\.\s*profile\s*=\s*(.*)", entry, re.DOTALL)
        if match:
            return match.group(1).strip()
    if entries and not entries[0].startswith("."):
        return entries[0]
    return None


def bound_profile_errors(rel_path: str, code: str) -> tuple[list[str], int]:

    errors: list[str] = []
    checked = 0
    initializers: list[tuple[int, str, bool]] = []
    names: set[str] = set()
    for match in BOUND_DECL_RE.finditer(code):
        names.add(match.group(1))
        if match.group(3):
            initializers.append((match.end() - 1, match.group(1), bool(match.group(2))))
    for match in BOUND_LITERAL_RE.finditer(code):
        initializers.append((match.end() - 1, "(PcvSpawnBound){}", False))
    for open_index, _name, is_array in initializers:
        line = code.count("\n", 0, open_index) + 1
        body = balanced_brace_body(code, open_index)
        bodies = [body]
        if is_array:


            bodies = []
            for element in split_top_level_args(body):
                element = re.sub(r"^\[[^\]]*\]\s*=\s*", "", element)
                if element.startswith("{"):
                    bodies.append(balanced_brace_body(element, 0))
                elif element:
                    checked += 1
                    errors.append(f"{rel_path}:{line}: PcvSpawnBound 배열 요소가 중괄호 초기화자가 "
                                  f"아니어서 profile을 확인할 수 없음: '{element}'")
            if not bodies:
                checked += 1
                errors.append(f"{rel_path}:{line}: PcvSpawnBound 배열에서 profile을 확인한 요소가 없음")
        for element in bodies:
            checked += 1
            reason = profile_violation(rel_path, bound_initializer_profile(element))
            if reason:
                errors.append(f"{rel_path}:{line}: {reason}")
    for name in sorted(names):
        assign_re = re.compile(
            rf"\b{re.escape(name)}\s*(?:\.|->)\s*profile\s*=(?!=)\s*([^;]*);"
        )
        for match in assign_re.finditer(code):
            checked += 1
            line = code.count("\n", 0, match.start()) + 1
            reason = profile_violation(rel_path, match.group(1).strip())
            if reason:
                errors.append(f"{rel_path}:{line}: {reason}")
    return errors, checked


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
                elif profile == "PCV_CHILD_CAP_TRACE" and rel_path != "src/modules/daemons/pcv_trace.c":
                    errors.append(f"{rel_path}:{line}: TRACE 프로필은 pcv_trace 수집 경계에만 허용됨")
        bound_errors, bound_checked = bound_profile_errors(rel_path, code)
        errors.extend(bound_errors)
        checked += bound_checked
    return errors, checked


def bounding_unit_errors(rel_path: str, unit: str) -> list[str]:

    bounding_lines = [
        line.strip().split("=", 1)[1].split()
        for line in unit.splitlines()
        if line.strip().startswith("CapabilityBoundingSet=")
    ]
    if len(bounding_lines) != 1 or not bounding_lines[0] or not bounding_lines[0][0].startswith("~"):
        return [f"{rel_path}: negative CapabilityBoundingSet 한 줄이 필요함"]
    tokens = bounding_lines[0]
    unit_drops = {tokens[0][1:], *tokens[1:]}
    if unit_drops != DROPPED_CAPS:
        return [f"{rel_path}: bounding 제외={sorted(unit_drops)}, 기대={sorted(DROPPED_CAPS)}"]
    return []


def configuration_errors(root: Path) -> tuple[list[str], int]:

    errors: list[str] = []
    checked = 3 + len(SYSTEMD_UNITS)
    try:
        privdrop = (root / PRIVDROP_SOURCE).read_text(encoding="utf-8")
        units = {rel: (root / rel).read_text(encoding="utf-8") for rel in SYSTEMD_UNITS}
        modules_text = (root / MODULES_LOAD).read_text(encoding="utf-8")
        vfio_modules_text = (root / VFIO_MODULES_LOAD).read_text(encoding="utf-8")
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

    for rel, unit in units.items():
        errors.extend(bounding_unit_errors(rel, unit))

    modules = [
        line.strip()
        for line in modules_text.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if modules != REQUIRED_MODULES:
        errors.append(
            f"{MODULES_LOAD}: modules={modules}, 기대={REQUIRED_MODULES}"
        )
    vfio_modules = [
        line.strip()
        for line in vfio_modules_text.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if vfio_modules != REQUIRED_VFIO_MODULES:
        errors.append(
            f"{VFIO_MODULES_LOAD}: modules={vfio_modules}, "
            f"기대={REQUIRED_VFIO_MODULES}"
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

    trace = "pcv_spawn_newv_profile(argv, flags, PCV_CHILD_CAP_TRACE, error);"
    errors, checked = source_errors("src/modules/daemons/pcv_trace.c", trace)
    if errors or checked != 1:
        print("self-test Trace 수집 고정 프로필을 거부함", file=sys.stderr)
        return 1
    errors, _ = source_errors("src/modules/example.c", trace)
    if not any("수집 경계" in error for error in errors):
        print("self-test Trace 프로필의 무관 caller 확산을 놓침", file=sys.stderr)
        return 1

    bound_ok = (
        "PcvSpawnBound b = { .profile = PCV_CHILD_CAP_RUNTIME, .timeout_sec = 5 };\n"
        "pcv_spawn_sync_bounded(argv, &(PcvSpawnBound){ .timeout_sec = 3,\n"
        "  .profile = PCV_CHILD_CAP_BASE }, NULL, NULL, &st, error);\n"
        "static gboolean f(const PcvSpawnBound *bound) { return bound != NULL; }\n"
        "PcvSpawnBound arr[] = { [0] = { .profile = PCV_CHILD_CAP_BASE },\n"
        "  { .profile = PCV_CHILD_CAP_RUNTIME } };\n"
    )
    errors, checked = source_errors("src/modules/bound.c", bound_ok)
    if errors or checked != 4:
        print(f"self-test PcvSpawnBound 고정 profile fixture 실패: errors={errors}, checked={checked}",
              file=sys.stderr)
        return 1
    for label, fixture in (
        ("초기화자 동적 profile", "PcvSpawnBound b = { .timeout_sec = 5, .profile = user_profile };"),
        ("복합 리터럴 동적 profile", "f(&(PcvSpawnBound){ .profile = p, .timeout_sec = 1 });"),
        ("멤버 대입 동적 profile", "PcvSpawnBound b = { .profile = PCV_CHILD_CAP_BASE };\nb.profile = p;"),
        ("profile 누락", "PcvSpawnBound b = { .timeout_sec = 5 };"),
        ("배열 지정자 요소 동적 profile", "PcvSpawnBound a[] = { [0] = { .profile = p } };"),
    ):
        errors, _ = source_errors("src/modules/bound_bad.c", fixture)
        if not errors:
            print(f"self-test PcvSpawnBound {label} 반사실을 놓침", file=sys.stderr)
            return 1

    good = "[Service]\nCapabilityBoundingSet=~CAP_SYS_MODULE CAP_SYS_RAWIO CAP_SYS_TIME CAP_MAC_OVERRIDE\n"
    if bounding_unit_errors(LXC_SYSTEMD_UNIT, good):
        print("self-test 컨테이너 유닛 정상 bounding을 거부함", file=sys.stderr)
        return 1
    for label, fixture in (
        ("CAP_SYS_TIME 누락", good.replace(" CAP_SYS_TIME", "")),
        ("양의 bounding", good.replace("=~", "=")),
        ("bounding 줄 없음", "[Service]\n"),
    ):
        if not bounding_unit_errors(LXC_SYSTEMD_UNIT, fixture):
            print(f"self-test 컨테이너 유닛 {label} 반사실을 놓침", file=sys.stderr)
            return 1
    print("[check-spawn-capabilities:self-test] PASS (13/13)")
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
        f"modules={len(REQUIRED_MODULES) + len(REQUIRED_VFIO_MODULES)})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
