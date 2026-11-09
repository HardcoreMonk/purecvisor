#!/usr/bin/env bash
                          
                                                                                 
                                                                               
                                                                    
                                                                                   
                                                                                               
                                                                           
                                                                             
                                                                 
                                                                                         
                                                                       
                                                                               
                                                            
                                                                                      
                                                                                    
                                                                                 
                                                                                    
 
                      
                                                          
                                                          
                                                               

set -uo pipefail

ROOT="${PCV_MEMCHECK_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
RUNNER="${1:-./test_runner}"
LOG_DIR="${MEMCHECK_LOG_DIR:-valgrind_shards}"
SEED_FILE="tests/memcheck_durations.tsv"

                                                                                             
VALGRIND_OPTS=(
    --leak-check=full
    --show-leak-kinds=all
    --errors-for-leak-kinds=definite
    --child-silent-after-fork=yes
    --show-realloc-size-zero=no
    --track-origins=yes
    --error-exitcode=1
    --suppressions=/usr/share/glib-2.0/valgrind/glib.supp
    --suppressions=tests/valgrind.supp
)

die() {
    printf 'memcheck-sharded: %s\n' "$1" >&2
    exit 2
}

default_jobs() {
    local cpus jobs
    cpus=$(nproc 2>/dev/null || echo 4)
    jobs=$(( cpus / 4 ))
    (( jobs > 16 )) && jobs=16
    (( jobs < 1 )) && jobs=1
    echo "$jobs"
}

cd "$ROOT" || die "작업 루트로 이동하지 못했다: $ROOT"
command -v valgrind >/dev/null || die "valgrind 미설치 (sudo apt-get install valgrind)"
[ -x "$RUNNER" ] || die "test_runner 실행 파일이 없다: $RUNNER (make test_runner)"

JOBS="${MEMCHECK_JOBS:-$(default_jobs)}"
case "$JOBS" in
    ''|*[!0-9]*) die "MEMCHECK_JOBS는 양의 정수여야 한다: '$JOBS'" ;;
esac
(( JOBS >= 1 )) || die "MEMCHECK_JOBS는 1 이상이어야 한다: $JOBS"

                                                                 
LIST_OUT=$("$RUNNER" -l 2>&1)
LIST_RC=$?
(( LIST_RC == 0 )) || { printf '%s\n' "$LIST_OUT" | tail -5 >&2; die "test_runner -l 실패 (종료 코드 $LIST_RC)"; }
declare -A UNIT_LEAVES=()
UNITS=()
LISTED=0
while IFS= read -r line; do
    [[ "$line" =~ ^#\ (/[^[:space:]]+)$ ]] || continue
    path="${BASH_REMATCH[1]}"
    unit="${path#/}"
    unit="${unit%%/*}"
    [[ "$unit" =~ ^[A-Za-z0-9_.-]+$ ]] || die "단위 이름으로 쓸 수 없는 경로: $path"
    if [ -z "${UNIT_LEAVES[$unit]+x}" ]; then
        UNIT_LEAVES[$unit]=0
        UNITS+=("$unit")
    fi
    UNIT_LEAVES[$unit]=$(( UNIT_LEAVES[$unit] + 1 ))
    LISTED=$(( LISTED + 1 ))
done <<<"$LIST_OUT"
(( LISTED > 0 )) || die "test_runner -l이 시험 경로를 하나도 내지 않았다"

                                                                        
                                                                  
mkdir -p -- "$LOG_DIR" || die "로그 디렉터리를 만들지 못했다: $LOG_DIR"
declare -A PREV_SECS=()
load_unit_secs() {
    local unit secs
    [ -r "$1" ] || return 0
    while IFS=$'\t' read -r unit secs; do
        [[ "$secs" =~ ^[0-9]+(\.[0-9]+)?$ ]] && PREV_SECS[$unit]="$secs"
    done <"$1"
}
load_unit_secs "$SEED_FILE"
load_unit_secs "$LOG_DIR/durations.tsv"                      
mapfile -t ORDER < <(
    for unit in "${UNITS[@]}"; do
        if [ -n "${PREV_SECS[$unit]+x}" ]; then
            printf '1\t%s\t%s\n' "${PREV_SECS[$unit]}" "$unit"
        else
            printf '0\t%s\t%s\n' "${UNIT_LEAVES[$unit]}" "$unit"
        fi
    done | sort -t $'\t' -k1,1n -k2,2gr -k3,3 | cut -f3
)
rm -f -- "$LOG_DIR"/*.log "$LOG_DIR/summary.txt"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/pcv-memcheck.XXXXXX") || die "임시 디렉터리를 만들지 못했다"
                                                                 
chmod 0755 "$WORK" && mkdir -m 0755 "$WORK/tmp" || die "임시 디렉터리 권한을 맞추지 못했다: $WORK"

declare -A UNIT_OF_PID=() START_US=() UNIT_RC=() UNIT_SECS=()
CLEANED=0

cleanup() {
    (( CLEANED )) && return 0
    CLEANED=1
    local pid
    for pid in "${!UNIT_OF_PID[@]}"; do
        kill -TERM "$pid" 2>/dev/null || true                         
    done
    for pid in "${!UNIT_OF_PID[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
    if [ -d "$WORK/tmp" ]; then
        sudo rm -rf -- "$WORK/tmp" 2>/dev/null || printf 'memcheck-sharded: 임시 디렉터리 일부를 지우지 못함: %s\n' "$WORK/tmp" >&2
    fi
    rm -rf -- "$WORK" 2>/dev/null || true
}
trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

                                                                                  
                                                
if ! sudo -n true 2>/dev/null; then
    sudo -v || die "sudo 인증 실패"
fi

start_unit() {
    local unit="$1" tmp="$WORK/tmp/$1"
    mkdir -m 1777 "$tmp" || die "단위 TMPDIR를 만들지 못했다: $tmp"
                                                                   
                               
    sudo env TMPDIR="$tmp" G_SLICE=always-malloc G_DEBUG=gc-friendly valgrind "${VALGRIND_OPTS[@]}" \
        "$RUNNER" -v -r "/$unit" >"$LOG_DIR/$unit.log" 2>&1 </dev/null &
    UNIT_OF_PID[$!]="$unit"
    START_US[$unit]="${EPOCHREALTIME//[.,]/}"
}

FINISHED=0
reap_one() {
    local pid="" rc unit now
    wait -n -p pid
    rc=$?
    [ -n "$pid" ] || return 0                            
    unit="${UNIT_OF_PID[$pid]}"
    unset "UNIT_OF_PID[$pid]"
    now="${EPOCHREALTIME//[.,]/}"
    UNIT_RC[$unit]=$rc
    UNIT_SECS[$unit]=$(awk -v us=$(( now - START_US[$unit] )) 'BEGIN { printf "%.1f", us / 1e6 }')
    FINISHED=$(( FINISHED + 1 ))
    printf '[%d/%d] %s rc=%d %ss\n' "$FINISHED" "${#ORDER[@]}" "$unit" "$rc" "${UNIT_SECS[$unit]}"
}

printf '🔍 Valgrind memcheck 샤딩: 단위 %d개(최상위 suite), 동시 %d, 등록 leaf %d개 — 로그 %s/\n' \
    "${#ORDER[@]}" "$JOBS" "$LISTED" "$LOG_DIR"
T0="${EPOCHREALTIME//[.,]/}"
for unit in "${ORDER[@]}"; do
    while (( ${#UNIT_OF_PID[@]} >= JOBS )); do
        reap_one
    done
    start_unit "$unit"
done
while (( ${#UNIT_OF_PID[@]} > 0 )); do
    reap_one
done
WALL=$(awk -v us=$(( ${EPOCHREALTIME//[.,]/} - T0 )) 'BEGIN { printf "%.0f", us / 1e6 }')

             
RUN_LEAVES=0 NOT_OK=0 ERRORS=0 DEF_BYTES=0 DEF_BLOCKS=0
FAILED_UNITS=()
for unit in "${ORDER[@]}"; do
    read -r leaves notok errors summaries bytes blocks < <(awk '
        /^(ok|not ok) [0-9]+ / { leaves++ }
        /^not ok [0-9]+ / { notok++ }
        /^==[0-9]+== ERROR SUMMARY: / { v = $4; gsub(/,/, "", v); errors += v; summaries++ }
        /^==[0-9]+== +definitely lost: / { b = $4; k = $7; gsub(/,/, "", b); gsub(/,/, "", k); bytes += b; blocks += k }
        END { printf "%d %d %d %d %d %d\n", leaves, notok, errors, summaries, bytes, blocks }
    ' "$LOG_DIR/$unit.log")
    RUN_LEAVES=$(( RUN_LEAVES + leaves ))
    NOT_OK=$(( NOT_OK + notok ))
    ERRORS=$(( ERRORS + errors ))
    DEF_BYTES=$(( DEF_BYTES + bytes ))
    DEF_BLOCKS=$(( DEF_BLOCKS + blocks ))
    if [ "${UNIT_RC[$unit]:-125}" -ne 0 ] || (( summaries == 0 )); then
        FAILED_UNITS+=("$unit(rc=${UNIT_RC[$unit]:-?},errors=$errors,not_ok=$notok,leaves=$leaves/${UNIT_LEAVES[$unit]})")
    fi
done

DUR_TMP="$LOG_DIR/durations.tsv.tmp.$$"
for unit in "${ORDER[@]}"; do
    printf '%s\t%s\n' "$unit" "${UNIT_SECS[$unit]:-0}"
done >"$DUR_TMP" && mv -f -- "$DUR_TMP" "$LOG_DIR/durations.tsv"

STATUS=0
{
    printf '📄 Valgrind 샤딩 요약 (단위 로그: %s/<suite>.log)\n' "$LOG_DIR"
    printf '단위 %d개, 동시 %d, 벽시계 %s초\n' "${#ORDER[@]}" "$JOBS" "$WALL"
    printf '실행 leaf %d / 등록 leaf %d, not ok %d\n' "$RUN_LEAVES" "$LISTED" "$NOT_OK"
    printf 'ERROR SUMMARY 합: %d errors\n' "$ERRORS"
    printf 'definitely lost 합: %d bytes in %d blocks\n' "$DEF_BYTES" "$DEF_BLOCKS"
    if (( ${#FAILED_UNITS[@]} > 0 )); then
        printf '실패 단위 %d개: %s\n' "${#FAILED_UNITS[@]}" "${FAILED_UNITS[*]}"
    else
        printf '실패 단위: 없음\n'
    fi
    if (( RUN_LEAVES != LISTED )); then
        printf '실행 leaf 수가 등록 leaf 수와 다르다 — 단위 나누기 또는 러너 중단 확인 필요\n'
    fi
} | tee "$LOG_DIR/summary.txt"
if (( ${#FAILED_UNITS[@]} > 0 || RUN_LEAVES != LISTED )); then
    STATUS=1
fi
exit "$STATUS"
