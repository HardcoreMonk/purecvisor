#!/usr/bin/env bash
                                                                       
                                                                       
                                                                            
                                         
                                                            
                                                         
 
                                      
                                                                            
                                                                     
                                                                    
                                                                        
                                                                       
                                                    
                                                                                      
                                                                          
                                                                 
                                                              
                                                                            
                                                                      
                                                                
                                                         
                                                                              
                                                          
                                                                                        
                                                                          
                                                                      
                                                                                  
                                                                 
                                                                      
                                                                          
                                                                     
                                                                                     
                                                                                  
                                                                     
                                                                                        
                                                                
                                                                   
                        
                                              
                                                      
                                                                             
                                                                        
                                                                 
                                                                                 
                                                     
 
                                                                              
                                                                                      
                                                          
                                                        
                                                                                              
                                                                
                                  

                                                                    
PCV_ISO_MASK_DIRS=(
    /run                                                                                            
                                                                         
    /var/log                                                                
    /sys/fs/bpf                                                 
    /var/lib/lxc                            
    /var/lib/libvirt/images                
    /var/lib/purecvisor                                         
    /etc/purecvisor                                                 
)

                                                                                
PCV_ISO_TEST_BWRAP=/usr/local/lib/purecvisor-test/bwrap
if [ -z "${PCV_ISO_BWRAP:-}" ]; then
    if [ -x "$PCV_ISO_TEST_BWRAP" ]; then
        PCV_ISO_BWRAP="$PCV_ISO_TEST_BWRAP"
    else
        PCV_ISO_BWRAP=bwrap
    fi
fi

PCV_ISO_BWRAP_ARGS=(
    --unshare-user --uid 0 --gid 0
    --unshare-net
    --unshare-pid
    --unshare-ipc
    --die-with-parent
    --cap-add ALL
    --setenv PCV_LIBVIRT_URI test:///default
    --setenv PURECVISOR_LIBVIRT_URI test:///default
    --setenv ZFS_MODULE_TIMEOUT 0
    --ro-bind / /
    --dev /dev
    --proc /proc
    --tmpfs /tmp
)
for _pcv_iso_dir in "${PCV_ISO_MASK_DIRS[@]}"; do
    if [ -d "$_pcv_iso_dir" ] && [ ! -L "$_pcv_iso_dir" ]; then
        PCV_ISO_BWRAP_ARGS+=(--tmpfs "$_pcv_iso_dir")
    fi
done
unset _pcv_iso_dir

                                                                                                      
                                                                
                                                                                          
                                                                                             
                                                                                                       
                                                                                            
                                                                                                
                                                                                  
pcv_iso_trusted_root_args() {
    PCV_ISO_TRUSTED_ARGS=()
    local -a args=("${PCV_ISO_BWRAP_ARGS[@]}")
    local i=0 e n
    while [ "$i" -lt "${#args[@]}" ]; do
        if [ "${args[$i]}" = --ro-bind ] && [ "${args[$((i + 1))]:-}" = / ] && [ "${args[$((i + 2))]:-}" = / ]; then
            for e in /*; do
                n=${e#/}
                case "$n" in proc|dev|tmp|etc) continue ;; esac
                if [ -L "$e" ]; then PCV_ISO_TRUSTED_ARGS+=(--symlink "$(readlink "$e")" "$e")
                elif [ -d "$e" ]; then PCV_ISO_TRUSTED_ARGS+=(--ro-bind "$e" "$e"); fi
            done
            PCV_ISO_TRUSTED_ARGS+=(--perms 0755 --tmpfs /etc)
            for e in /etc/* /etc/.[!.]*; do
                [ -e "$e" ] || [ -L "$e" ] || continue
                [ "${e#/etc/}" = purecvisor ] && continue
                if [ -L "$e" ]; then PCV_ISO_TRUSTED_ARGS+=(--symlink "$(readlink "$e")" "$e")
                else PCV_ISO_TRUSTED_ARGS+=(--ro-bind "$e" "$e"); fi
            done
            i=$((i + 3))
            continue
        fi
        PCV_ISO_TRUSTED_ARGS+=("${args[$i]}")
        i=$((i + 1))
    done
}

                                                                                       
                                                                                        
PCV_ISO_WATCH_PREFIXES=(
    /etc/purecvisor
    /var/lib/purecvisor
    /usr/lib/purecvisor/bpf
    /etc/systemd/system/purecvisor
    /usr/lib/systemd/system/purecvisor
    /lib/systemd/system/purecvisor
)

                                                                      
                                                                        
                                                           
                                                              
pcv_iso_watch_devs() {
    local majmin target p
                                                                           
                                               
    findmnt -rn -o MAJ:MIN,TARGET 2>/dev/null | while read -r majmin target; do
        for p in "${PCV_ISO_WATCH_PREFIXES[@]}"; do
            if [ "$target" = / ] || [ "$target" = "$p" ] || [[ "$p" == "$target"/* ]] ||
               [[ "$target" == "$p"* ]]; then
                printf '%s\n' "$majmin"
                break
            fi
        done
    done | sort -u
}

                                                                              
                                                                         
                                                                                      
                                                                               
                                                                     
                                         
                                                     
                                                                              
pcv_iso_check_state_fs() {
    local state="${1:-}" sdev sfstype d
    if [ -z "$state" ] || [ ! -d "$state" ]; then
        printf 'FAIL: 격리 상태 폴더(%s)가 없어 파일시스템을 확인할 수 없다\n' "$state" >&2
        return 1
    fi
    if ! command -v findmnt >/dev/null 2>&1; then
        printf 'FAIL: findmnt 미설치 — 격리 상태 폴더(%s)의 파일시스템을 확인할 수 없다\n' "$state" >&2
        return 1
    fi
    sdev="$(findmnt -n -o MAJ:MIN -T "$state" 2>/dev/null | head -n 1 | tr -d '[:space:]')"
    if [ -z "$sdev" ]; then
        printf 'FAIL: 격리 상태 폴더(%s)의 장치를 판정하지 못했다(findmnt -T)\n' "$state" >&2
        return 1
    fi
    while read -r d; do
        if [ "$d" = "$sdev" ]; then
            printf 'FAIL: 격리 상태 폴더 %s(장치 %s)가 운영 LSM 감시 경로와 같은 파일시스템이다 — 시험 데몬의 임시 파일 열기가 운영 보안 사건과 구분되지 않는다. TMPDIR 을 감시 경로와 다른 fs(기본 /tmp tmpfs)로 두고 다시 실행한다\n' "$state" "$sdev" >&2
            return 1
        fi
    done < <(pcv_iso_watch_devs)
    sfstype="$(findmnt -n -o FSTYPE -T "$state" 2>/dev/null | head -n 1 | tr -d '[:space:]')"
    if [ -z "$sfstype" ]; then
        printf 'FAIL: 격리 상태 폴더(%s)의 파일시스템 종류를 판정하지 못했다(findmnt -T)\n' "$state" >&2
        return 1
    fi
    if [ "$sfstype" != tmpfs ]; then
        printf 'FAIL: 격리 상태 폴더 %s(파일시스템 %s)가 tmpfs 가 아니다 — 운영 LSM 은 tmpfs 의 장치만 구분하므로 시험 데몬의 임시 파일 열기가 운영 보안 사건이 된다. TMPDIR 을 tmpfs(기본 /tmp)로 두고 다시 실행한다\n' "$state" "$sfstype" >&2
        return 1
    fi
    return 0
}

                                
                           
PCV_ISO_SKIP_REASON=""

                                                      
                                                              
                                                                  
                                 
                                                             
pcv_iso_capable() {
    PCV_ISO_SKIP_REASON=""
    local tool
    for tool in bwrap nft nsenter pgrep python3; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            PCV_ISO_SKIP_REASON="$tool 미설치 — 격리 데몬 하네스 실행 불가"
            return 1
        fi
    done
                                                                  
    if [ "$PCV_ISO_BWRAP" = "$PCV_ISO_TEST_BWRAP" ] &&
       ! cmp -s "$PCV_ISO_BWRAP" "$(command -v bwrap)"; then
        PCV_ISO_SKIP_REASON="시험용 bwrap 사본이 시스템 bwrap 과 다름(패키지 갱신 뒤) — sudo scripts/install_test_bwrap_profile.sh 재실행 필요"
        return 1
    fi
    if ! "$PCV_ISO_BWRAP" "${PCV_ISO_BWRAP_ARGS[@]}" /bin/true >/dev/null 2>&1; then
        PCV_ISO_SKIP_REASON="격리 샌드박스(user/net/pid/ipc 네임스페이스 + 운영 경로 마스킹) 생성 불가"
        return 1
    fi
    if ! "$PCV_ISO_BWRAP" "${PCV_ISO_BWRAP_ARGS[@]}" nft list tables >/dev/null 2>&1; then
        PCV_ISO_SKIP_REASON="격리 샌드박스 안에서 nft netlink 불가 — 사용자 네임스페이스 capability 가 막힘(예: AppArmor bwrap-userns-restrict 는 root 실행도 막고, kernel.apparmor_restrict_unprivileged_userns=1 은 비root 를 막는다). sudo scripts/install_test_bwrap_profile.sh 로 전용 사본을 설치하거나 제한 없는 호스트·VM 필요"
        return 1
    fi
    return 0
}

                                                                                          
                                                           
                                                 
pcv_iso_sandbox_pid() {
    local bp="${1:-}" child
    [ -n "$bp" ] || return 1
    for _ in $(seq 1 20); do
        child="$(pgrep -o -P "$bp" 2>/dev/null)"
        if [ -n "$child" ]; then
            printf '%s\n' "$child"
            return 0
        fi
        kill -0 "$bp" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

                                                                       
                                                               
pcv_iso_daemon_pid() {
    local sb
    sb="$(pcv_iso_sandbox_pid "${1:-}")" || return 1
    pgrep -o -P "$sb" 2>/dev/null
}

                                                                           
                                                                      
                                                  
                                                                     
                                                                               
                                                                  
pcv_iso_exec() {
    local bp="${1:-}" sb
    shift
    sb="$(pcv_iso_sandbox_pid "$bp")" || return 125
    nsenter --target "$sb" --user --net --preserve-credentials -- "$@"
}

                                                                         
                                                                
                                                             
                                                                                  
pcv_iso_check_bin() {
    local bin="${1:-}"
    if "$PCV_ISO_BWRAP" "${PCV_ISO_BWRAP_ARGS[@]}" /bin/sh -c '[ -r "$1" ] && [ -x "$1" ]' pcv-iso-check-bin "$bin" \
            >/dev/null 2>&1; then
        return 0
    fi
    PCV_ISO_SKIP_REASON="샌드박스 uid 0 이 데몬 바이너리($bin)를 읽거나 실행할 수 없음 — 사용자 네임스페이스의 uid 0 은 매핑되지 않은 호스트 소유자 파일에 DAC override 가 없어 바이너리까지의 모든 상위 디렉터리가 o+x 여야 한다(예: 0750 홈 아래 체크아웃을 sudo 로 실행). 저장소를 world-traversable 경로에 둘 것"
    return 1
}

                                                                                           
                                                                 
                                                                          
                                                                  
                                                                      
                                                            
pcv_iso_uds_nc() {
    python3 -c '
import os, selectors, socket, sys, time

deadline = time.monotonic() + float(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2] else None

def write_all(data):
    view = memoryview(data)
    while view:
        n = os.write(1, view)
        view = view[n:]

def remaining():
    if deadline is None:
        return None
    left = deadline - time.monotonic()
    if left <= 0:
        sys.exit(124)
    return left

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.settimeout(remaining())
try:
    sock.connect(sys.argv[1])
except socket.timeout:
    sys.exit(124)
except OSError as exc:
    sys.stderr.write("pcv_iso_uds_nc: connect %s: %s\n" % (sys.argv[1], exc))
    sys.exit(1)
sock.setblocking(True)
sel = selectors.DefaultSelector()
sel.register(sock, selectors.EVENT_READ)
sending = True
try:
    sel.register(0, selectors.EVENT_READ)
except (OSError, ValueError):
                                                         
    data = sys.stdin.buffer.read()
    if data:
        sock.sendall(data)
while True:
    events = sel.select(remaining())
    if not events:
        sys.exit(124)
    for key, _ in events:
        if key.fileobj is sock:
            try:
                data = sock.recv(65536)
            except ConnectionResetError:
                sys.exit(1)
            if not data:
                sys.exit(0)
            write_all(data)
        else:
            data = os.read(0, 65536)
            if not data:
                sel.unregister(0)
                continue
            if sending:
                try:
                    sock.sendall(data)
                except (BrokenPipeError, ConnectionResetError):
                    sending = False
' "$1" "${2:-}"
}

                                                                                
                                                   
                                                                                   
                                                                                             
                                                                  
                                                 
pcv_iso_rest_ready() {
    local bp="${1:-}" url="${2:-}" out code body
    out="$(pcv_iso_exec "$bp" curl -s -w '\n%{http_code}' --max-time "${3:-2}" "$url" 2>/dev/null)"
    [ -n "$out" ] || out=$'\n000'
    code="${out##*$'\n'}"
    body="${out%$'\n'*}"
    printf '%s\n' "$code"
    [ "$code" = "200" ] && return 0
    [ "$code" = "503" ] || return 1
    printf '%s' "$body" | python3 -c '
import json, sys
try:
    checks = json.load(sys.stdin).get("checks") or {}
except (ValueError, AttributeError):
    sys.exit(1)
def ok(name):
    entry = checks.get(name)
    return entry.get("ok") if isinstance(entry, dict) else None
sys.exit(0 if ok("libvirt") is False and ok("disk") is True and ok("audit_chain") is True else 1)
'
}
