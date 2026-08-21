#!/usr/bin/env python3
                          
                                                                     
                                                                                      
                                                                                                                            
 
                      
                                                                                                                              
                                                                    

                                                                
                                                  

    
                                                                             
                                                                            
                                                           
                                                                                
                                                        
                                                                 
                                   
                                                         
                                                               
                                                              
                                                          
                                                         
                                       

                                                      
                                                                             
                     

                                                              
                                                       
   
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
                                                 
    code = strip_comments(ebpf_text)
    if MAP_FUNC + "(" not in code:
        return False, f"ebpf_telemetry 루프가 {MAP_FUNC}() 미사용 (인라인 else→0 매핑 회귀?)"
    if RECOVER_FUNC + "(" not in code:
        return False, f"ebpf_telemetry 루프가 {RECOVER_FUNC}() 미호출 (자동복구 경로 제거?)"
    if '"SUSPENDED"' not in code:
        return False, 'ebpf_telemetry 루프에 "SUSPENDED" 분기 없음 (복구 트리거 게이트 제거?)'
    return True, f"{MAP_FUNC}() + {RECOVER_FUNC}() 배선됨"


def check_recover_guards(zfs_text: str):
                                                           
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
