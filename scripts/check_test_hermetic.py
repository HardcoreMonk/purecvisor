#!/usr/bin/env python3













































from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


PRODUCTION_ROOTS = (
    "/var/lib/purecvisor",
    "/var/log/purecvisor",
    "/etc/purecvisor",
    "/run/purecvisor",
    "/var/run/purecvisor",
    "/var/log/suricata",
    "/var/lib/suricata",
    "/etc/suricata",
    "/sys/fs/bpf",
)
PRODUCTION_URIS = ("qemu:///system",)


MUTATING_VERBS = {
    "systemctl": {
        "start", "stop", "restart", "try-restart", "reload", "reload-or-restart", "kill",
        "enable", "disable", "mask", "unmask", "isolate", "daemon-reload", "reset-failed",
        "set-property", "edit", "link", "preset", "revert",
    },
    "virsh": {
        "define", "undefine", "destroy", "start", "create", "shutdown", "reboot", "reset",
        "managedsave", "snapshot-create", "snapshot-create-as", "snapshot-delete",
        "attach-device", "detach-device", "attach-interface", "detach-interface",
        "setmem", "setvcpus", "net-define", "net-undefine", "net-start", "net-destroy",
        "pool-define", "pool-destroy", "vol-create", "vol-delete",
    },
    "ovs-vsctl": {
        "add-br", "del-br", "add-port", "del-port", "add-bond", "set", "clear", "remove",
        "destroy", "create", "emer-reset",
    },
}


SENSITIVE_CALLS = {
    "pcv_trace_start": "운영 trace 루트(PCV_TRACE_OUT_ROOT) 쓰기와 retention 적용",
    "pcv_trace_purge_all": "호스트 pid namespace의 retis `pkill -f`와 운영 trace 루트 삭제",
    "pcv_suricata_health_start": "실 suricata.service 재기동(systemd)",
    "pcv_bpf_rehydrate": "운영 bpffs pin·bpf_state.json·커널 LSM attach 변경",
    "network_dhcp_reserve_overlay_ip": "운영 PCV_NETWORK_RUNDIR에 dnsmasq conf/pid 기록",
    "network_dhcp_release_overlay_ip": "운영 PCV_NETWORK_RUNDIR pid 파일 기반 프로세스 종료",
}


SEAM_CALLS = {
    "pcv_qos_tenant_sla_set": (
        "pcv_qos_set_tenant_sla_path_for_test",
        "운영 /var/lib/purecvisor/qos_tenants.json 덮어쓰기",
    ),
    "pcv_overlay_list": ("pcv_overlay_set_test_context", "운영 overlay 메타 디렉터리 읽기"),
    "pcv_overlay_info": ("pcv_overlay_set_test_context", "운영 overlay 메타 디렉터리 읽기"),
}



LINK_SEAMS = (
    ("src/utils/pcv_log.o", "tests/pcv_log_host_paths.o",
     "src/utils/pcv_log.c", "PCV_TEST_OVERRIDE_AUDIT_LOG_PATH"),
    ("src/modules/network/security_group.o", "tests/security_group_host_paths.o",
     "src/modules/network/security_group.c", "PCV_TEST_OVERRIDE_SG_DB_PATH"),
)
SEAM_FLAGS = ("-DPCV_TEST_HOST_PATH_SEAM_MAP", "-include tests/test_host_isolation.h")

GUARD_RE = re.compile(
    r'\bpcv_test_live_opt_in\s*\(\s*"PCV_[A-Z0-9_]+_LIVE"\s*\)'
    r'|\bg_getenv\s*\(\s*"(?:PCV_[A-Z0-9_]+_LIVE|PCV_RUN_DESTRUCTIVE_[A-Z0-9_]+)"\s*\)'
)
REGISTER_RE = re.compile(r"\bg_test_add(?:_func|_data_func|_data_func_full|_vtable)?\s*\(")
IDENT_RE = re.compile(r"\b[A-Za-z_]\w*\b")
DEFINE_RE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)(?!\()\s+(.*)$")
FUNC_HEADER_RE = re.compile(
    r"([A-Za-z_]\w*)\s*\((?:[^()]|\([^()]*\))*\)\s*(?:[A-Z_][A-Z0-9_]*(?:\([^()]*\))?\s*)*$",
    re.S,
)
KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "do", "else"}


_FIXTURE_XML = "libvirt domain XML/argv 비교용 문자열 fixture — 경로를 열거나 실행하지 않는다"
ALLOWLIST: dict[tuple[str, str, str], str] = {
    ("tests/test_backup_basic.c", "test_backup_incremental_result_json", "/var/lib/purecvisor"):
        "JSON 결과 객체의 file 필드 문자열 fixture — 파일시스템 접근 없음",
    ("tests/test_bpf_manifest.c", "test_seal_rejects_load_and_rehydrate", "pcv_bpf_rehydrate"):
        "g_test_trap_subprocess 자식에서 seal 래치가 manifest·libbpf 접촉 전에 거부하는 경로만 본다(임시 디렉터리)",
    ("tests/test_config.c", "test_defaults", "PCV_DEFAULT_DB_PATH"):
        "config getter 기본값과 상수 문자열을 비교만 한다 — DB를 열지 않는다",
    ("tests/test_config.c", "test_defaults", "PCV_DEFAULT_LIBVIRT_URI"):
        "config getter 기본값과 상수 문자열을 비교만 한다 — libvirt에 연결하지 않는다",
    ("tests/test_config.c", "test_defaults", "PCV_DEFAULT_SOCKET_PATH"):
        "config getter 기본값과 상수 문자열을 비교만 한다 — 소켓을 열지 않는다",
    ("tests/test_config.c", "test_env_override_admin_user", "PCV_DEFAULT_SOCKET_PATH"):
        "env override가 다른 키를 바꾸지 않음을 상수 비교로 확인한다 — 소켓 접근 없음",
    ("tests/test_config.c", "test_env_override_socket", "PCV_DEFAULT_LIBVIRT_URI"):
        "env override가 다른 키를 바꾸지 않음을 상수 비교로 확인한다 — libvirt 연결 없음",
    ("tests/test_dpdk.c", "test_dpdk_active_legacy_endpoint_is_closed_and_exact", "/var/run/purecvisor"):
        "가짜 OVS runner 출력의 legacy vhost 경로 문자열 — 실 OVS·파일 접근 없음",
    ("tests/test_dpdk.c", "test_dpdk_owned_vhost_endpoint_converges_and_rechecks", "/var/run/purecvisor"):
        "가짜 OVS runner fixture의 legacy vhost 경로 문자열 — 실 OVS·파일 접근 없음",
    ("tests/test_ova_import_xml.c", "test_ova_import_qcow2_file", "/var/lib/purecvisor"): _FIXTURE_XML,
    ("tests/test_plugin.c", "test_plugin_path_valid", "/etc/purecvisor"):
        "플러그인 경로 검증 함수의 입력 문자열 — 디렉터리를 열지 않는다",
    ("tests/test_qos.c", "test_tenant_sla_rejects_min_gt_max", "pcv_qos_tenant_sla_set"):
        "min>max 입력 검증이 온메모리·디스크 저장 전에 FALSE를 반환하는 경로만 본다",
    ("tests/test_qos.c", "test_tenant_sla_rejects_zero_max", "pcv_qos_tenant_sla_set"):
        "max=0 입력 검증이 온메모리·디스크 저장 전에 FALSE를 반환하는 경로만 본다",
    ("tests/test_security_event.c", "test_security_event_roundtrip", "/etc/purecvisor"):
        "보안 이벤트 target 필드 직렬화 문자열 — 파일 접근 없음",
    ("tests/test_trace.c", "test_build_argv_includes_f_flag_when_filter_present", "/var/run/purecvisor"):
        "retis argv 빌더 입력 출력 디렉터리 문자열 — 실행·디렉터리 생성 없음",
    ("tests/test_trace.c", "test_build_argv_includes_outdir", "/var/run/purecvisor"):
        "retis argv 빌더 입력·비교 문자열 — 실행·디렉터리 생성 없음",
    ("tests/test_trace.c", "test_build_argv_omits_f_flag_when_no_filter", "/var/run/purecvisor"):
        "retis argv 빌더 입력 출력 디렉터리 문자열 — 실행·디렉터리 생성 없음",
    ("tests/test_trace.c", "test_build_argv_shape", "/var/run/purecvisor"):
        "retis argv 빌더 입력·비교 문자열 — 실행·디렉터리 생성 없음",
    ("tests/test_trace.c", "test_start_selfcheck_fail_leaves_guard_free", "pcv_trace_start"):
        "retis가 설치돼 있으면 SKIP하고, 미설치면 selfcheck가 산출물 디렉터리 생성 전에 실패하는 경로만 본다",
    ("tests/test_vm_manager.c", "test_dpdk_metadata_read_legacy_vhost_is_invalid_libvirt", "/var/run/purecvisor"):
        _FIXTURE_XML + "(test:///default 드라이버)",
    ("tests/test_vm_manager.c", "test_dpdk_vhost_inactive_migration_libvirt_reread", "/var/run/purecvisor"):
        _FIXTURE_XML + "(test:///default 드라이버)",
    ("tests/test_vm_manager.c", "test_dpdk_vhost_legacy_migration_is_exact_and_idempotent", "/var/run/purecvisor"):
        _FIXTURE_XML,
    ("tests/test_vm_manager.c", "test_dpdk_vhost_source_classifies_canonical_and_legacy", "/var/run/purecvisor"):
        _FIXTURE_XML,
    ("tests/test_vm_manager.c", "test_dpdk_vhost_source_rejects_foreign_or_ambiguous", "/var/run/purecvisor"):
        _FIXTURE_XML,
    ("tests/test_vpc_model.c", "test_vpc_dnsmasq_conf_arg_is_single_equals_option", "/run/purecvisor"):
        "dnsmasq argv 조립 결과의 --conf-file 문자열 비교 — 실행·파일 접근 없음",
}


@dataclass
class Literal:
    start: int
    end: int
    value: str
    line: int


@dataclass
class Function:
    name: str
    start: int
    end: int
    refs: set[str] = field(default_factory=set)
    guarded: bool = False
    seams: set[str] = field(default_factory=set)


@dataclass
class Violation:
    path: str
    func: str
    kind: str
    token: str
    line: int

    def key(self) -> tuple[str, str, str]:
        return (self.path, self.func, self.token)


def _line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def mask_source(text: str) -> tuple[str, str, list[Literal]]:





    code = list(text)
    masked = list(text)
    literals: list[Literal] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                code[k] = masked[k] = " "
            i = j
        elif c == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if text[k] != "\n":
                    code[k] = masked[k] = " "
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                if text[j] == "\\":
                    j += 1
                elif text[j] == "\n":
                    break
                j += 1
            end = min(j, n - 1)
            if c == '"':
                literals.append(Literal(i, end, text[i + 1:end], _line_of(text, i)))
            for k in range(i + 1, end):
                masked[k] = "_"
            i = end + 1
        else:
            i += 1
    return "".join(code), "".join(masked), literals


def _blank_preprocessor(masked: str) -> str:

    out = list(masked)
    lines = masked.split("\n")
    offset = 0
    cont = False
    for line in lines:
        if cont or line.lstrip().startswith("#"):
            for k in range(offset, offset + len(line)):
                out[k] = " "
            cont = line.rstrip().endswith("\\")
        offset += len(line) + 1
    return "".join(out)


def find_functions(masked: str) -> list[Function]:

    text = _blank_preprocessor(masked)
    funcs: list[Function] = []
    depth = 0
    boundary = 0
    body_start = -1
    name = None
    for i, c in enumerate(text):
        if c == "{":
            if depth == 0:
                header = text[boundary:i].strip()
                m = FUNC_HEADER_RE.search(header)
                if m and "=" not in header.split("(", 1)[0] and m.group(1) not in KEYWORDS:
                    name = m.group(1)
                    body_start = i
                else:
                    name = None
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                if name:
                    funcs.append(Function(name, body_start, i))
                name = None
                boundary = i + 1
            if depth < 0:
                raise ValueError("unbalanced braces")
        elif c == ";" and depth == 0:
            boundary = i + 1
    return funcs


def parse_defines(code: str, known: set[str]) -> set[str]:

    defines: dict[str, str] = {}
    for line in code.split("\n"):
        m = DEFINE_RE.match(line)
        if m:
            defines[m.group(1)] = m.group(2)
    found = set(known)
    changed = True
    while changed:
        changed = False
        for name, value in defines.items():
            if name in found:
                continue
            literal_hit = any(root in value for root in PRODUCTION_ROOTS + PRODUCTION_URIS)
            macro_hit = any(ident in found for ident in IDENT_RE.findall(value))
            if literal_hit or macro_hit:
                found.add(name)
                changed = True
    return found


def production_macros(src_dir: Path) -> set[str]:

    names: set[str] = set()
    headers = sorted(src_dir.rglob("*.h")) if src_dir.is_dir() else []
    texts = []
    for header in headers:
        code, _, _ = mask_source(header.read_text(encoding="utf-8", errors="replace"))
        texts.append(code)
    combined = "\n".join(texts)
    return parse_defines(combined, names)


def _literal_is_production(value: str) -> str | None:

    for uri in PRODUCTION_URIS:
        if uri in value:
            return uri
    hits = []
    for root in PRODUCTION_ROOTS:

        for m in re.finditer(re.escape(root) + r"(?=/|$|[\s'\"=,;:%)])", value):
            hits.append((m.start(), -len(root), root))
    return min(hits)[2] if hits else None


def _command_violations(masked: str, literals: list[Literal], lo: int, hi: int) -> list[tuple[str, int]]:

    hits: list[tuple[str, int]] = []
    body_literals = [lit for lit in literals if lo <= lit.start <= hi]

    for lit in body_literals:
        words = lit.value.split()
        if len(words) >= 2 and words[0] in MUTATING_VERBS:
            verbs = MUTATING_VERBS[words[0]]
            if any(word in verbs for word in words[1:]):
                hits.append((f"{words[0]} {next(w for w in words[1:] if w in verbs)}", lit.start))

    by_start = {lit.start: lit for lit in body_literals}
    for m in re.finditer(r"\{\s*(?=\")", masked[lo:hi + 1]):
        first_pos = lo + m.end()
        first = by_start.get(first_pos)
        if not first or first.value not in MUTATING_VERBS:
            continue
        close = masked.find("}", first_pos, hi + 1)
        close = hi if close < 0 else close
        verbs = MUTATING_VERBS[first.value]
        for lit in body_literals:
            if first.end < lit.start < close and lit.value in verbs:
                hits.append((f"{first.value} {lit.value}", first.start))
                break
    return hits


def analyze_file(path: Path, rel: str, prod_macros: set[str]) -> list[Violation]:

    text = path.read_text(encoding="utf-8", errors="replace")
    code, masked, literals = mask_source(text)
    funcs = find_functions(masked)
    names = {f.name for f in funcs}
    local_macros = parse_defines(code, prod_macros)


    for f in funcs:
        body_masked = masked[f.start:f.end + 1]
        body_code = code[f.start:f.end + 1]
        f.refs = {ident for ident in IDENT_RE.findall(body_masked) if ident in names and ident != f.name}
        f.guarded = bool(GUARD_RE.search(body_code))
        f.seams = {seam for seam, _ in SEAM_CALLS.values() if re.search(r"\b" + seam + r"\s*\(", body_masked)}


    leaves = {"main"} & names
    for m in REGISTER_RE.finditer(masked):
        depth, j = 1, m.end()
        while j < len(masked) and depth:
            if masked[j] == "(":
                depth += 1
            elif masked[j] == ")":
                depth -= 1
            j += 1
        leaves |= {ident for ident in IDENT_RE.findall(masked[m.end():j]) if ident in names}

    referencers: dict[str, set[str]] = {f.name: set() for f in funcs}
    for f in funcs:
        for ref in f.refs:
            referencers[ref].add(f.name)

    def safe_set(direct: set[str]) -> set[str]:
        providers = set(direct)
        changed = True
        while changed:
            changed = False
            for f in funcs:
                if f.name not in providers and f.refs & providers:
                    providers.add(f.name)
                    changed = True
        safe = set(providers)
        changed = True
        while changed:
            changed = False
            for f in funcs:
                if f.name in safe or f.name in leaves:
                    continue
                refs = referencers[f.name]
                if refs and refs <= safe:
                    safe.add(f.name)
                    changed = True
        return safe

    guard_direct = {f.name for f in funcs if f.guarded}
    safe_guard = safe_set(guard_direct)
    safe_seam = {
        seam: safe_set(guard_direct | {f.name for f in funcs if seam in f.seams})
        for seam in {s for s, _ in SEAM_CALLS.values()}
    }

    violations: list[Violation] = []

    def owner(offset: int) -> str:
        for f in funcs:
            if f.start <= offset <= f.end:
                return f.name
        return "<file-scope>"

    def add(offset: int, kind: str, token: str, protected: set[str]) -> None:
        func = owner(offset)
        if func in protected:
            return
        violations.append(Violation(rel, func, kind, token, _line_of(text, offset)))

    define_lines = {i for i, line in enumerate(code.split("\n"), 1) if DEFINE_RE.match(line)}


    for lit in literals:
        if lit.line in define_lines:
            continue
        root = _literal_is_production(lit.value)
        if root:
            add(lit.start, "production-path", root, safe_guard)

    blank = _blank_preprocessor(masked)

    if local_macros:
        macro_re = re.compile(r"\b(" + "|".join(sorted(map(re.escape, local_macros))) + r")\b")
        for m in macro_re.finditer(blank):
            add(m.start(), "production-macro", m.group(1), safe_guard)


    for f in funcs:
        for token, offset in _command_violations(masked, literals, f.start, f.end):
            if f.name not in safe_guard:
                violations.append(Violation(rel, f.name, "host-command", token, _line_of(text, offset)))


    for name in SENSITIVE_CALLS:
        for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", blank):
            add(m.start(), "sensitive-call", name, safe_guard)
    for name, (seam, _) in SEAM_CALLS.items():
        for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", blank):
            add(m.start(), "seam-required", name, safe_seam[seam])

    return violations


def _make_var(makefile: str, name: str) -> list[str]:

    m = re.search(r"(?m)^" + re.escape(name) + r"\s*=\s*(.*)$", makefile)
    return m.group(1).split() if m else []


def link_seam_problems(makefile: str, sources: dict[str, str]) -> list[str]:




    problems: list[str] = []
    logical = makefile.replace("\\\n", " ")
    replaced = _make_var(logical, "TEST_HOST_PATH_SEAM_REPLACED")
    seam_objs = _make_var(logical, "TEST_HOST_PATH_SEAM_OBJS")
    flags = " ".join(_make_var(logical, "TEST_HOST_PATH_SEAM_FLAGS"))
    test_objs = [line for line in logical.splitlines() if re.match(r"TEST_OBJS\s*\+?=", line)]
    if not any("filter-out" in line and "$(TEST_HOST_PATH_SEAM_REPLACED)" in line for line in test_objs):
        problems.append("TEST_OBJS가 $(TEST_HOST_PATH_SEAM_REPLACED) 제품 object를 filter-out하지 않는다")
    if not any(re.match(r"TEST_OBJS\s*\+=\s*\$\(TEST_HOST_PATH_SEAM_OBJS\)\s*$", line) for line in test_objs):
        problems.append("TEST_OBJS가 $(TEST_HOST_PATH_SEAM_OBJS) seam object를 링크하지 않는다")
    for flag in SEAM_FLAGS:
        if flag not in flags:
            problems.append(f"TEST_HOST_PATH_SEAM_FLAGS에 {flag} 없음")
    for product_obj, seam_obj, source, override in LINK_SEAMS:
        if product_obj not in replaced:
            problems.append(f"{product_obj}가 TEST_HOST_PATH_SEAM_REPLACED에 없다(운영 경로 object가 링크됨)")
        if seam_obj not in seam_objs:
            problems.append(f"{seam_obj}가 TEST_HOST_PATH_SEAM_OBJS에 없다")
        rule = re.search(r"(?m)^" + re.escape(seam_obj) + r"\s*:\s*" + re.escape(source)
                         + r"[^\n]*\n((?:\t[^\n]*\n)+)", makefile)
        if not rule or "$(TEST_HOST_PATH_SEAM_FLAGS)" not in rule.group(1):
            problems.append(f"{seam_obj} 규칙이 {source}를 $(TEST_HOST_PATH_SEAM_FLAGS)로 컴파일하지 않는다")
        text = sources.get(source, "")
        if not re.search(r"(?m)^\s*#\s*ifdef\s+" + re.escape(override) + r"\b", text):
            problems.append(f"{source}에 `#ifdef {override}` 경로 override 블록이 없다")
    return problems


def _relative(path: Path) -> str:
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def run(files: list[Path], src_dir: Path, allowlist: dict[tuple[str, str, str], str],
        seam_problems: list[str] | None = None) -> int:

    seam_problems = seam_problems or []
    if not files:
        print("ERROR: 검사할 tests/*.c 파일이 없다", file=sys.stderr)
        return 2
    prod_macros = production_macros(src_dir)
    violations: list[Violation] = []
    for path in files:
        if not path.is_file():
            print(f"ERROR: {path} 미존재", file=sys.stderr)
            return 2
        try:
            violations.extend(analyze_file(path, _relative(path), prod_macros))
        except ValueError as exc:
            print(f"ERROR: {path} parse 실패: {exc}", file=sys.stderr)
            return 2

    weak = [key for key, reason in allowlist.items() if len(reason.strip()) < 10]
    used = set()
    remaining = []
    for v in violations:
        if v.key() in allowlist:
            used.add(v.key())
        else:
            remaining.append(v)
    scanned = {_relative(p) for p in files}
    stale = [key for key in allowlist if key not in used and key[0] in scanned]

    print(f"[check-test-hermetic] {len(files)}개 파일, 운영 경로 매크로 {len(prod_macros)}개, "
          f"허용 {len(used)}건")
    if remaining or stale or weak or seam_problems:
        print("\033[31m[FAIL]\033[0m 테스트 호스트 격리 계약 위반:", file=sys.stderr)
        for problem in seam_problems:
            print(f"  - link seam: {problem}", file=sys.stderr)
        for v in remaining:
            print(f"  - {v.path}:{v.line} [{v.func}] {v.kind}: {v.token}", file=sys.stderr)
        for key in stale:
            print(f"  - stale 허용 목록(더 이상 위반 없음, 항목 삭제): {key}", file=sys.stderr)
        for key in weak:
            print(f"  - 허용 사유 부족(10자 이상 필요): {key}", file=sys.stderr)
        print("       조치: 시험 seam(임시 경로)으로 바꾸거나 PCV_*_LIVE=1 opt-in 가드 뒤로 옮긴다.\n"
              "       근거: docs/DEVELOPMENT_VERIFICATION_POLICY.md 운영 겸용 호스트 root 테스트 규칙",
              file=sys.stderr)
        return 1
    print("\033[32m[PASS]\033[0m tests/*.c 운영 경로·호스트 변이 참조가 모두 opt-in 가드·seam·허용 사유를 가진다")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="C 단위 테스트 호스트 격리(hermetic) 정적 게이트")
    parser.add_argument("files", nargs="*", type=Path, help="검사할 C 파일(기본: tests/*.c)")
    parser.add_argument("--src-dir", type=Path, default=ROOT / "src",
                        help="운영 경로 매크로를 추출할 헤더 루트(기본: src)")
    args = parser.parse_args(argv[1:])
    files = args.files or sorted((ROOT / "tests").glob("*.c"))
    seam_problems: list[str] = []
    if not args.files:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        sources = {src: (ROOT / src).read_text(encoding="utf-8") for _, _, src, _ in LINK_SEAMS
                   if (ROOT / src).is_file()}
        seam_problems = link_seam_problems(makefile, sources)
    return run(files, args.src_dir, ALLOWLIST, seam_problems)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
