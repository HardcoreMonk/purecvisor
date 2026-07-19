
"""check_zpool_suspend_recover.py — ZFS 풀 SUSPENDED 탐지+가드된 자동복구 게이트.

근거: 단일 USB SSD 풀(pcvpool)이 USB 단절로 SUSPENDED 됐으나 데몬이 SUSPENDED 를
"정상(0)"으로 매핑해 34시간 미인지·수동복구한 사건. 아래 불변식이 회귀하면 RED.

불변식:
  1. [매핑]  src/modules/storage/zfs_driver.c 의 pcv_zfs_pool_state_metric_val 가
     "SUSPENDED" 를 비0 값으로 매핑한다("SUSPENDED"→return N, N≠0). else→0 회귀 시 FAIL.
  2. [배선]  src/modules/daemons/ebpf_telemetry.c 의 텔레메트리 루프가
     pcv_zfs_pool_state_metric_val() 로 상태를 매핑하고 pcv_zfs_pool_recover_suspended()
     로 자동복구를 태운다. 인라인 매핑(else→0) 되돌림 또는 복구 호출 제거 시 FAIL.
  3. [가드]  pcv_zfs_pool_recover_suspended 본문에 자동복구 안전 가드가 모두 존재하고
     zpool clear 는 그 가드들 뒤에서만 실행된다:
       (a) 디바이스-읽기 가드 _zfs_vdev_readable(...) + 읽기실패 조기반환
           (PCV_ZFS_RECOVER_DEV_UNREADABLE) — 죽은 디바이스에 clear 금지
       (b) 서킷브레이커 pcv_zfs_recover_guard_allow(...) + 상한초과 조기반환
           (PCV_ZFS_RECOVER_CB_TRIPPED) — 무한 clear-loop 금지
       (c) "clear" argv 가 (a),(b) 두 가드 호출 뒤(텍스트 인덱스)에 위치.
     가드를 제거하거나 clear 를 가드 앞으로 옮기면 FAIL.

반사실: SUSPENDED 매핑을 0 으로 되돌리거나, 디바이스-읽기 가드/서킷브레이커를 제거하면
게이트가 RED(exit 1)가 된다. self-test(scripts/tests/test_zpool_suspend_recover.py)가
temp 사본으로 각 회귀를 실증한다.

CLI: 인자 없으면 정본 파일 검사. --zfs <path> / --ebpf <path> 로 검사 대상 파일을
오버라이드할 수 있다(self-test 가 되돌린 temp 사본을 원본 훼손 없이 검사하기 위함).
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ZFS_REL = "src/modules/storage/zfs_driver.c"
EBPF_REL = "src/modules/daemons/ebpf_telemetry.c"

MAP_FUNC = "pcv_zfs_pool_state_metric_val"
RECOVER_FUNC = "pcv_zfs_pool_recover_suspended"

def strip_comments(text: str) -> str:
    """C 주석(//, /* */)만 공백/개행으로 치환하고 문자열·문자 리터럴은 보존한다.

    반사실 게이트의 핵심: 검사 대상은 실코드다. 코드를 걷어내도 주석에 "SUSPENDED"/
    "clear" 가 남아 게이트가 통과하면 안 되므로 주석을 반드시 지운다. 반면 검사 앵커
    자체가 문자열 리터럴 인자("SUSPENDED"/"clear")이므로 문자열은 보존한다.
    줄 번호 유지를 위해 개행은 개행으로 치환한다. (check_cors_anchor 와 동일 관례)
    """
    out = []
    i, n = 0, len(text)
    in_block = in_line = False
    in_str = None
    while i < n:
        ch = text[i]
        if in_line:
            out.append('\n' if ch == '\n' else ' ')
            if ch == '\n':
                in_line = False
            i += 1
            continue
        if in_block:
            if ch == '*' and i + 1 < n and text[i + 1] == '/':
                out.append('  ')
                i += 2
                in_block = False
            else:
                out.append('\n' if ch == '\n' else ' ')
                i += 1
            continue
        if in_str:
            out.append(ch)
            if ch == '\\' and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if ch == in_str:
                in_str = None
            i += 1
            continue
        if ch == '/' and i + 1 < n and text[i + 1] == '*':
            in_block = True
            out.append('  ')
            i += 2
            continue
        if ch == '/' and i + 1 < n and text[i + 1] == '/':
            in_line = True
            out.append('  ')
            i += 2
            continue
        if ch == '"' or ch == "'":
            in_str = ch
            out.append(ch)
            i += 1
            continue
        out.append(ch)
        i += 1
    return ''.join(out)

def extract_func_body(code: str, func_name: str):
    """comment-stripped code 에서 func_name 정의 본문({..})을 문자열-인지 brace 매칭으로
    추출한다. 반환: 본문 문자열(중괄호 포함) 또는 None(정의 없음).

    호출부가 아닌 '정의'를 잡기 위해, func_name( 뒤 첫 ')' 다음에 '{' 가 오는 형태만
    본문으로 인정한다(프로토타입/호출은 뒤에 ';' 또는 ',' 라 배제)."""
    for m in re.finditer(re.escape(func_name) + r'\s*\(', code):

        i = m.end() - 1
        depth = 0
        n = len(code)
        in_str = None
        while i < n:
            c = code[i]
            if in_str:
                if c == '\\':
                    i += 2
                    continue
                if c == in_str:
                    in_str = None
                i += 1
                continue
            if c in '"\'':
                in_str = c
            elif c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    i += 1
                    break
            i += 1

        j = i
        while j < n and code[j] in ' \t\r\n':
            j += 1
        if j >= n or code[j] != '{':
            continue

        depth = 0
        k = j
        in_str = None
        while k < n:
            c = code[k]
            if in_str:
                if c == '\\':
                    k += 2
                    continue
                if c == in_str:
                    in_str = None
                k += 1
                continue
            if c in '"\'':
                in_str = c
            elif c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    return code[j:k + 1]
            k += 1
        return code[j:]
    return None

def check_mapping(zfs_text: str):
    """(ok, msg). SUSPENDED → 비0 매핑."""
    code = strip_comments(zfs_text)
    body = extract_func_body(code, MAP_FUNC)
    if body is None:
        return False, f"{MAP_FUNC} 정의를 찾지 못함 (상태→값 매핑 함수 제거?)"
    m = re.search(r'"SUSPENDED"[^;]*?return\s+([0-9]+(?:\.[0-9]+)?)', body)
    if not m:
        return False, f'{MAP_FUNC} 본문에 "SUSPENDED"→return 매핑 없음 (SUSPENDED 미매핑 회귀)'
    val = float(m.group(1))
    if val == 0.0:
        return False, f'{MAP_FUNC} 가 "SUSPENDED"→{m.group(1)}(0=정상)으로 매핑 (원 버그 회귀)'
    return True, f'"SUSPENDED"→{m.group(1)} (critical, 비0)'

def check_ebpf_wiring(ebpf_text: str):
    """(ok, msg). 텔레메트리 루프가 매핑 함수 + 복구 함수를 호출."""
    code = strip_comments(ebpf_text)
    if MAP_FUNC + "(" not in code:
        return False, f"ebpf_telemetry 루프가 {MAP_FUNC}() 미사용 (인라인 else→0 매핑 회귀?)"
    if RECOVER_FUNC + "(" not in code:
        return False, f"ebpf_telemetry 루프가 {RECOVER_FUNC}() 미호출 (자동복구 경로 제거?)"
    if '"SUSPENDED"' not in code:
        return False, 'ebpf_telemetry 루프에 "SUSPENDED" 분기 없음 (복구 트리거 게이트 제거?)'
    return True, f"{MAP_FUNC}() + {RECOVER_FUNC}() 배선됨"

def check_recover_guards(zfs_text: str):
    """(ok, msg). 복구 본문의 디바이스-읽기 가드 + 서킷브레이커 + clear 순서."""
    code = strip_comments(zfs_text)
    body = extract_func_body(code, RECOVER_FUNC)
    if body is None:
        return False, f"{RECOVER_FUNC} 정의를 찾지 못함 (자동복구 함수 제거?)"

    dev_idx = body.find("_zfs_vdev_readable(")
    if dev_idx < 0:
        return False, "디바이스-읽기 가드 _zfs_vdev_readable() 호출 없음 "\
                      "(죽은 디바이스 무한 clear 위험)"
    if "PCV_ZFS_RECOVER_DEV_UNREADABLE" not in body:
        return False, "읽기실패 조기반환(PCV_ZFS_RECOVER_DEV_UNREADABLE) 없음 "\
                      "(읽기실패 시 clear 로 폴스루)"

    cb_idx = body.find("pcv_zfs_recover_guard_allow(")
    if cb_idx < 0:
        return False, "서킷브레이커 pcv_zfs_recover_guard_allow() 호출 없음 "\
                      "(무한 clear-loop 방지 없음)"
    if "PCV_ZFS_RECOVER_CB_TRIPPED" not in body:
        return False, "상한초과 조기반환(PCV_ZFS_RECOVER_CB_TRIPPED) 없음 (flapping 미차단)"

    clear_idx = body.find('"clear"')
    if clear_idx < 0:
        return False, 'zpool "clear" argv 없음 (복구 액션 부재)'

    if clear_idx < dev_idx or clear_idx < cb_idx:
        return False, 'zpool "clear" 가 디바이스-읽기/서킷브레이커 가드보다 앞에 위치 '\
                      '(가드 우회)'
    return True, "device-read 가드 + 서킷브레이커 뒤에서만 zpool clear"

def run(zfs_path: Path, ebpf_path: Path) -> int:
    zfs_text = Path(zfs_path).read_text(errors="replace")
    ebpf_text = Path(ebpf_path).read_text(errors="replace")

    checks = [
        ("SUSPENDED 매핑",        check_mapping(zfs_text)),
        ("ebpf 배선",             check_ebpf_wiring(ebpf_text)),
        ("자동복구 가드",         check_recover_guards(zfs_text)),
    ]
    failed = [(name, msg) for name, (ok, msg) in checks if not ok]

    print(f"[check-zpool-suspend-recover] {len(checks) - len(failed)}/{len(checks)} 불변식 충족")
    if failed:
        for name, msg in failed:
            print(f"[FAIL] {name}: {msg}", file=sys.stderr)
        return 1
    for name, (_ok, msg) in checks:
        print(f"[PASS] {name}: {msg}")
    return 0

def main(argv=None) -> int:
    p = argparse.ArgumentParser(add_help=True)
    p.add_argument("--zfs", default=str(ROOT / ZFS_REL))
    p.add_argument("--ebpf", default=str(ROOT / EBPF_REL))
    args = p.parse_args(argv)
    return run(Path(args.zfs), Path(args.ebpf))

if __name__ == "__main__":
    sys.exit(main())
