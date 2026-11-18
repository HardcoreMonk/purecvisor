#!/usr/bin/env bash

set -uo pipefail

PASS=0; FAIL=0
pass() { echo "[PASS] $*"; PASS=$((PASS+1)); }
fail() { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }
info() { echo "[INFO] $*"; }
skip() { echo "[SKIP] OVN 폭발 반경 효과-테스트: $*"; exit 0; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="${PCV_DAEMON_BIN:-$REPO/bin/purecvisorsd}"

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

for tool in bwrap python3 setsid sqlite3 nsenter ovsdb-server ovsdb-tool ovn-northd ovn-nbctl ovn-sbctl ovs-vsctl dnsmasq nft; do
    command -v "$tool" >/dev/null 2>&1 || skip "$tool 미설치"
done
[ -f /usr/share/ovn/ovn-nb.ovsschema ] || skip "OVN schema 없음"
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 없음 ($DAEMON_BIN)"
pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"

STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-ovnblast.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
chmod 0755 "$STATE"
mkdir -p "$STATE/var-lib/ovn" "$STATE/etc" "$STATE/mock" "$STATE/sysnet"
OVN="$STATE/var-lib/ovn"
INNER=/var/lib/purecvisor/ovn
SOCK="$STATE/var-lib/daemon.sock"
DB="$STATE/var-lib/vpc.db"

NB_ARGS=(--no-chdir "--pidfile=$OVN/nb.pid" --detach "--log-file=$OVN/nb.log"
         "--remote=punix:$OVN/nb.sock" "--unixctl=$OVN/nb.ctl" "$OVN/nb.db")
nb_start() { ovsdb-server "${NB_ARGS[@]}" >/dev/null 2>&1; }
nb_stop() { [ -f "$OVN/nb.pid" ] && kill "$(cat "$OVN/nb.pid")" 2>/dev/null; rm -f "$OVN/nb.sock"; sleep 0.3; }
host_nb() { ovn-nbctl "--db=unix:$OVN/nb.sock" --timeout=10 "$@"; }

kill_daemon() {
    [ -f "$STATE/bwrap.pid" ] || return 0
    local bp i; bp="$(cat "$STATE/bwrap.pid" 2>/dev/null || true)"
    [ -n "${bp:-}" ] || return 0
    kill -- "-$bp" 2>/dev/null || true
    kill "$bp" 2>/dev/null || true
    for i in $(seq 1 16); do kill -0 "$bp" 2>/dev/null || break; sleep 0.25; done
    kill -9 -- "-$bp" 2>/dev/null || true
    kill -9 "$bp" 2>/dev/null || true
    rm -f "$STATE/bwrap.pid" "$SOCK"
}
cleanup() {
    kill_daemon
    local p
    for p in "$OVN"/*.pid; do [ -f "$p" ] && kill "$(cat "$p")" 2>/dev/null; done
    sleep 0.3
    rm -rf "$STATE" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

ovsdb-tool create "$OVN/nb.db" /usr/share/ovn/ovn-nb.ovsschema
ovsdb-tool create "$OVN/sb.db" /usr/share/ovn/ovn-sb.ovsschema
nb_start
ovsdb-server --no-chdir "--pidfile=$OVN/sb.pid" --detach "--log-file=$OVN/sb.log" \
    "--remote=punix:$OVN/sb.sock" "--unixctl=$OVN/sb.ctl" "$OVN/sb.db" >/dev/null 2>&1
ovn-nbctl "--db=unix:$OVN/nb.sock" --no-wait init
ovn-northd --no-chdir "--pidfile=$OVN/northd.pid" --detach "--log-file=$OVN/northd.log" \
    "--unixctl=$OVN/northd.ctl" "--ovnnb-db=unix:$OVN/nb.sock" "--ovnsb-db=unix:$OVN/sb.sock" >/dev/null 2>&1
ovn-sbctl "--db=unix:$OVN/sb.sock" --timeout=10 chassis-add chassis-test geneve 127.0.0.1
cp "$(command -v ovn-nbctl)" "$OVN/real-ovn-nbctl"
cp "$(command -v ovn-sbctl)" "$OVN/real-ovn-sbctl"
cp "$(command -v nft)" "$OVN/real-nft"

cat > "$STATE/mock/ovn-nbctl" <<MOCK
#!/bin/sh
F=$INNER
if [ -e "\$F/nb-flap" ]; then
    case " \$* " in *" get "*) echo "ovn-nbctl: unix:\$F/nb.sock: database connection failed (flap)" >&2; exit 1 ;; esac
fi
if [ -e "\$F/acl-fail" ]; then
    case " \$* " in *" acl-add "*) echo "ovn-nbctl: acl-add failed (test)" >&2; exit 1 ;; esac
fi

if [ -e "\$F/acl-timeout" ]; then
    case " \$* " in *" acl-add "*)
        "\$F/real-ovn-nbctl" "--db=unix:\$F/nb.sock" --no-wait --may-exist --type=port-group acl-add "\$6" "\$7" "\$8" "\$9" drop
        echo "terminating with signal 14 (Alarm clock)" >&2; exit 142 ;;
    esac
fi
n=\$#
while [ "\$n" -gt 0 ]; do
    a=\$1; shift; n=\$((n-1))
    [ "\$a" = "--wait=hv" ] && a=--wait=sb
    set -- "\$@" "\$a"
done
exec "\$F/real-ovn-nbctl" "--db=unix:\$F/nb.sock" "\$@"
MOCK
cat > "$STATE/mock/ovn-sbctl" <<MOCK
#!/bin/sh
exec $INNER/real-ovn-sbctl "--db=unix:$INNER/sb.sock" "\$@"
MOCK

cat > "$STATE/mock/ovs-vsctl" <<'MOCK'
#!/bin/sh
D=/var/lib/purecvisor/ovn
iface=""; prev=""
for a in "$@"; do
    case "$prev" in Interface) iface=$a ;; esac
    prev=$a
done
case " $* " in
    *" add-port br-int "*)
        prev=""
        for a in "$@"; do
            if [ "$prev" = "br-int" ]; then iface=$a; fi
            case "$a" in external_ids:purecvisor-vpc-id=*) vpc=${a#*=} ;; esac
            prev=$a
        done
        ip link add "$iface" type dummy 2>/dev/null
        echo "$vpc" > "$D/ovs-$iface" ;;
    *" del-port br-int "*)
        for a in "$@"; do iface=$a; done
        ip link del "$iface" 2>/dev/null
        rm -f "$D/ovs-$iface" ;;
    *" get Interface "*)
        [ -e "$D/ovs-$iface" ] || exit 0
        case " $* " in
            *" _uuid "*) echo 22222222-2222-2222-2222-222222222222 ;;
            *purecvisor-owner*) echo local-vpc ;;
            *purecvisor-vpc-id*) cat "$D/ovs-$iface" ;;
        esac ;;
    *"external_ids:system-id"*) echo '"chassis-test"' ;;
    *"external_ids:ovn-remote"*) echo '"unix:/var/lib/purecvisor/ovn/sb.sock"' ;;
esac
exit 0
MOCK

cat > "$STATE/mock/dnsmasq" <<MOCK
#!/bin/sh
[ -e $INNER/dnsmasq-fail ] && { echo "dnsmasq: failed (test)" >&2; exit 1; }
exit 0
MOCK

cat > "$STATE/mock/nft" <<MOCK
#!/bin/sh
[ -e $INNER/nft-fail ] && { echo "nft: failed (test)" >&2; exit 1; }
exec $INNER/real-nft "\$@"
MOCK
chmod +x "$STATE"/mock/*

cat > "$STATE/etc/daemon.conf" <<CONF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
libvirt_uri = test:///default
rest_port = 0
admin_user = admin
admin_password = OvnBlastRadius-not-for-prod
jwt_secret = ovn-blast-radius-fixed-secret-not-for-prod-0001
log_level = info
drain_timeout = 1
CONF

send_rpc() { echo "$1" | pcv_iso_uds_nc "$SOCK" 2>/dev/null || true; }
rpc() { send_rpc '{"jsonrpc":"2.0","method":"'"$1"'","params":'"$2"',"id":"t"}'; }

boot() {
    setsid "$PCV_ISO_BWRAP" "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --bind "$STATE/sysnet" /sys/class/net \
        --ro-bind "$STATE/mock/ovn-nbctl" "$(command -v ovn-nbctl)" \
        --ro-bind "$STATE/mock/ovn-sbctl" "$(command -v ovn-sbctl)" \
        --ro-bind "$STATE/mock/ovs-vsctl" "$(command -v ovs-vsctl)" \
        --ro-bind "$STATE/mock/dnsmasq" "$(command -v dnsmasq)" \
        --ro-bind "$STATE/mock/nft" "$(command -v nft)" \
        --chdir / \
        "$DAEMON_BIN" >> "$STATE/daemon.log" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"
    local i
    for i in $(seq 1 100); do
        kill -0 "$!" 2>/dev/null || return 1
        [ -S "$SOCK" ] && [ -n "$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"p"}')" ] && return 0
        sleep 0.3
    done
    return 1
}

job_wait() {
    local id i out
    id="$(printf '%s' "$1" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')"
    if [ -z "$id" ]; then printf '접수 실패: %s' "$1" > "$STATE/job.last"; echo "rejected"; return; fi
    for i in $(seq 1 200); do
        out="$(rpc jobs.get '{"job_id":"'"$id"'"}')"
        printf '%s' "$out" > "$STATE/job.last"
        case "$out" in
            *'"status":"completed"'*) echo completed; return ;;
            *'"status":"failed"'*) echo failed; return ;;
        esac
        sleep 0.3
    done
    echo timeout
}
job_last() { cut -c1-600 "$STATE/job.last" 2>/dev/null; }
sql() { sqlite3 -cmd '.timeout 5000' "$DB" "$1"; }
iso() { pcv_iso_exec "$(cat "$STATE/bwrap.pid")" "$@"; }
link_is_up() { iso ip -o link show "$1" 2>/dev/null | grep -Eq '[<,]UP[,>]'; }
nft_quarantined() { iso nft list tables 2>/dev/null | grep -q pcv_vpc_quarantine; }
vpc_state() { sql "SELECT state FROM vpcs WHERE name='$1';"; }

mark_bridges() {
    local b
    for b in $(sql "SELECT bridge_name FROM subnets WHERE bridge_name IS NOT NULL;"); do
        mkdir -p "$STATE/sysnet/$b/bridge"
    done
}

make_degraded_ovn() {
    touch "$OVN/nb-flap"
    local r s
    r="$(rpc vpc.create '{"tenant":"t-b","name":"'"$1"'","egress_mode":"nat","backend":"ovn"}')"
    s="$(job_wait "$r")"
    rm -f "$OVN/nb-flap"
    [ "$s" = failed ] && [ "$(vpc_state "$1")" = DEGRADED ] || { info "DEGRADED 생성 실패: $s $(job_last | cut -c100-) state=$(vpc_state "$1")"; return 1; }
}

if ! boot; then
    echo "---- daemon.log ----"; tail -30 "$STATE/daemon.log"; fail "S0: 격리 데몬 기동 실패"; exit 1
fi
pass "S0: 격리 데몬 기동(실제 nft·일회용 NB/SB)"

R="$(rpc vpc.create '{"tenant":"t-a","name":"linux-a","egress_mode":"nat","subnet_name":"s1","subnet_cidr":"10.201.1.0/24"}')"
S="$(job_wait "$R")"
if [ "$S" = completed ]; then pass "S1: Linux VPC 생성"; else fail "S1: Linux VPC 생성 $S: $(job_last)"; echo "결과: PASS $PASS / FAIL $FAIL"; exit 1; fi
BRIDGE="$(sql "SELECT bridge_name FROM subnets WHERE name='s1';")"
LINUX_VPC="$(sql "SELECT id FROM vpcs WHERE name='linux-a';")"
mark_bridges
if link_is_up "$BRIDGE"; then pass "S1': Linux bridge $BRIDGE UP"; else fail "S1': Linux bridge $BRIDGE 가 UP 이 아님"; fi

if make_degraded_ovn ovn-b; then pass "S2: 생성 중 NB 장애로 DEGRADED 잔존"; else fail "S2: DEGRADED 재현 실패"; fi
OVN_VPC="$(sql "SELECT id FROM vpcs WHERE name='ovn-b';")"
PG="pcvv_pg_$(printf '%s' "$OVN_VPC" | sha256sum | cut -c1-16)"
if [ -z "$(host_nb --if-exists get Port_Group "$PG" _uuid)" ]; then
    pass "S2': 실제 NB 에 Port_Group $PG 없음"
else
    fail "S2': Port_Group 이 있어 전제가 깨짐"
fi

if OUT="$(host_nb --type=port-group --may-exist acl-add "$PG" from-lport 30000 "inport == @$PG && (ip || arp)" drop 2>&1)"; then
    fail "S2'': 없는 Port_Group acl-add 가 성공함 — 시험 전제가 달라짐"
else
    pass "S2'': 없는 Port_Group acl-add 비0 종료: $OUT"
fi

REV="$(sql "SELECT revision FROM vpcs WHERE id='$LINUX_VPC';")"
R="$(rpc vpc.subnet.create '{"tenant":"t-a","vpc_id":"'"$LINUX_VPC"'","name":"s2","cidr":"10.201.2.0/24","expected_revision":'"$REV"'}')"
S="$(job_wait "$R")"
if [ "$S" = completed ]; then
    pass "S3: Port_Group 없는 DEGRADED VPC 가 다른 테넌트 변경을 막지 않음"
else
    fail "S3: 다른 테넌트 subnet 생성 $S: $(job_last)"
fi
mark_bridges

nb_stop
R="$(rpc vpc.reconcile '{}')"
S="$(job_wait "$R")"
info "S4: reconcile=$S $(job_last)"
if link_is_up "$BRIDGE"; then pass "S4: NB 장애 reconcile 뒤 Linux bridge UP"; else fail "S4: NB 장애 reconcile 이 Linux bridge 를 DOWN 시킴"; fi
if nft_quarantined; then fail "S4': nft quarantine 이 Linux 트래픽을 잠근 채 남음"; else pass "S4': nft quarantine 해제"; fi
if [ "$S" = failed ]; then pass "S4'': OVN 실패를 성공으로 숨기지 않음"; else fail "S4'': reconcile=$S"; fi
if [ "$(vpc_state ovn-b)" = DEGRADED ]; then pass "S4''': OVN VPC 는 DEGRADED 유지"; else fail "S4''': OVN VPC 상태=$(vpc_state ovn-b)"; fi

R="$(rpc vpc.delete '{"tenant":"t-b","vpc_id":"'"$OVN_VPC"'"}')"
S="$(job_wait "$R")"
if [ "$S" = failed ] && [ -n "$(vpc_state ovn-b)" ]; then
    pass "S5: NB 장애 중 vpc.delete 는 실패하고 row 유지(NB 장애와 ABSENT 구분)"
else
    fail "S5: NB 장애 중 vpc.delete=$S row=$(vpc_state ovn-b)"
fi
if nft_quarantined; then fail "S5': 실패한 delete 가 Linux quarantine 을 남김"; else pass "S5': 실패한 delete 뒤 Linux 트래픽 잠금 없음"; fi

nb_start
sleep 0.5
R="$(rpc vpc.delete '{"tenant":"t-b","vpc_id":"'"$OVN_VPC"'"}')"
S="$(job_wait "$R")"
if [ "$S" = completed ] && [ -z "$(vpc_state ovn-b)" ]; then
    pass "S7: DEGRADED VPC 를 API 로 삭제"
else
    fail "S7: DEGRADED VPC 삭제 $S: $(job_last)"
fi

if make_degraded_ovn ovn-e; then
    OVN_E="$(sql "SELECT id FROM vpcs WHERE name='ovn-e';")"
    PG_E="pcvv_pg_$(printf '%s' "$OVN_E" | sha256sum | cut -c1-16)"
    host_nb -- pg-add "$PG_E" -- set Port_Group "$PG_E" external_ids:purecvisor-owner=local-vpc \
        "external_ids:purecvisor-vpc-id=$OVN_E"
    touch "$OVN/acl-fail"
    R="$(rpc vpc.delete '{"tenant":"t-b","vpc_id":"'"$OVN_E"'"}')"
    S="$(job_wait "$R")"
    rm -f "$OVN/acl-fail"
    if [ "$S" = completed ] && [ -z "$(vpc_state ovn-e)" ]; then
        pass "S7b: 자기 격리가 실패해도 DEGRADED VPC 를 삭제"
    else
        fail "S7b: $S row=$(vpc_state ovn-e): $(job_last | cut -c100-)"
    fi
else
    fail "S7b: DEGRADED 재현 실패"
fi

if make_degraded_ovn ovn-c; then
    OVN_C="$(sql "SELECT id FROM vpcs WHERE name='ovn-c';")"
    PG_C="pcvv_pg_$(printf '%s' "$OVN_C" | sha256sum | cut -c1-16)"
    R="$(rpc vpc.reconcile '{}')"
    S="$(job_wait "$R")"
    if [ "$S" = completed ] && [ -n "$(host_nb --if-exists get Port_Group "$PG_C" _uuid)" ] &&
       [ "$(vpc_state ovn-c)" = ACTIVE ]; then
        pass "S8: NB 복구 뒤 reconcile 이 Port_Group 을 만들고 VPC 를 ACTIVE 로 되돌림"
    else
        fail "S8: reconcile=$S state=$(vpc_state ovn-c): $(job_last | cut -c100-)"
    fi
    if ACLS="$(host_nb --type=port-group acl-list "$PG_C" 2>&1)" && ! printf '%s' "$ACLS" | grep -q 30000; then
        pass "S8': quarantine ACL 해제"
    else
        fail "S8': quarantine ACL 잔존 또는 조회 실패: $ACLS"
    fi
else
    fail "S8: DEGRADED 재현 실패"
fi

acl_quarantine_count() { host_nb --type=port-group acl-list "$1" 2>/dev/null | grep -c 30000; }
assert_unfenced() {
    local n; n="$(acl_quarantine_count "$PG_C")"
    if [ "$n" = 0 ]; then pass "$1: ACTIVE OVN VPC 에 격리 ACL 없음"; else fail "$1: 격리 ACL $n 건 잔존"; fi
    if nft_quarantined; then fail "$1: nft quarantine 잔존"; else pass "$1: nft quarantine 없음"; fi
}
REV="$(sql "SELECT revision FROM vpcs WHERE id='$LINUX_VPC';")"
R="$(rpc vpc.subnet.create '{"tenant":"t-a","vpc_id":"'"$LINUX_VPC"'","name":"s3","cidr":"10.201.3.0/24","expected_revision":'"$REV"'}')"
S="$(job_wait "$R")"
mark_bridges
if [ "$S" = completed ]; then pass "S10: Linux vpc.subnet.create 완료"; else fail "S10: $S: $(job_last | cut -c100-)"; fi
assert_unfenced "S10a(Linux subnet.create)"

R="$(rpc vpc.create '{"tenant":"t-a","name":"linux-g","egress_mode":"nat","subnet_name":"g1","subnet_cidr":"10.201.4.0/24"}')"
S="$(job_wait "$R")"
mark_bridges
if [ "$S" = completed ]; then pass "S10b: 첫 subnet 있는 Linux vpc.create 완료"; else fail "S10b: $S: $(job_last | cut -c100-)"; fi
assert_unfenced "S10b(Linux vpc.create)"

REV="$(sql "SELECT revision FROM vpcs WHERE id='$LINUX_VPC';")"
touch "$OVN/dnsmasq-fail"
R="$(rpc vpc.subnet.create '{"tenant":"t-a","vpc_id":"'"$LINUX_VPC"'","name":"s9","cidr":"10.201.9.0/24","expected_revision":'"$REV"'}')"
S="$(job_wait "$R")"
rm -f "$OVN/dnsmasq-fail"
if [ "$S" = failed ]; then pass "S10c: DHCP 실패 subnet.create 는 실패"; else fail "S10c: $S"; fi
assert_unfenced "S10c(실패한 subnet.create)"
sql "UPDATE vpcs SET state='ACTIVE' WHERE id='$LINUX_VPC'; DELETE FROM subnets WHERE name='s9';"

S3_ID="$(sql "SELECT id FROM subnets WHERE name='s3';")"
sql "CREATE TRIGGER t_keep_s3 BEFORE DELETE ON subnets WHEN old.name='s3' BEGIN SELECT RAISE(ABORT,'test keep'); END;"
R="$(rpc vpc.subnet.delete '{"tenant":"t-a","subnet_id":"'"$S3_ID"'"}')"
S="$(job_wait "$R")"
sql "DROP TRIGGER t_keep_s3;"
if [ "$S" = failed ]; then pass "S10d: 저장소 실패 subnet.delete 는 실패"; else fail "S10d: $S"; fi
assert_unfenced "S10d(실패한 subnet.delete)"
sql "DELETE FROM subnets WHERE name='s3';"

S2_ID="$(sql "SELECT id FROM subnets WHERE name='s2';")"
ATT=aaaaaaaa-0000-4000-8000-000000000001
PUB=aaaaaaaa-0000-4000-8000-000000000002
sql "INSERT INTO attachments(id,subnet_id,vm_uuid,vm_name_snapshot,owner_subject,mac_address,ip_address,state) VALUES('$ATT','$S2_ID','aaaaaaaa-0000-4000-8000-000000000003','ghost','admin','52:54:00:aa:bb:01','10.201.2.50','ACTIVE');
INSERT INTO service_publishes(id,vpc_id,attachment_id,protocol,listen_address,listen_port,target_port,allowed_sources,state) VALUES('$PUB','$LINUX_VPC','$ATT','tcp','127.0.0.1',18080,80,'[\"10.0.0.0/8\"]','ACTIVE');"
R="$(rpc vpc.service.unpublish '{"tenant":"t-a","publish_id":"'"$PUB"'"}')"
S="$(job_wait "$R")"
if [ "$S" = completed ]; then pass "S10e: vpc.service.unpublish 완료"; else fail "S10e: $S: $(job_last | cut -c100-)"; fi
assert_unfenced "S10e(service.unpublish)"
sql "DELETE FROM service_publishes WHERE id='$PUB'; DELETE FROM attachments WHERE id='$ATT';"

sql "INSERT INTO attachments(id,subnet_id,vm_uuid,vm_name_snapshot,owner_subject,mac_address,ip_address,state) VALUES('$ATT','$S2_ID','aaaaaaaa-0000-4000-8000-000000000003','ghost','admin','52:54:00:aa:bb:01','10.201.2.50','ACTIVE');
INSERT INTO service_publishes(id,vpc_id,attachment_id,protocol,listen_address,listen_port,target_port,allowed_sources,state) VALUES('$PUB','$LINUX_VPC','$ATT','tcp','127.0.0.1',18080,80,'[\"10.0.0.0/8\"]','ACTIVE');"
touch "$OVN/acl-fail"
R="$(rpc vpc.service.unpublish '{"tenant":"t-a","publish_id":"'"$PUB"'"}')"
S="$(job_wait "$R")"
rm -f "$OVN/acl-fail"
PUB_STATE="$(sql "SELECT state FROM service_publishes WHERE id='$PUB';")"
if [ "$S" = failed ] && [ "$PUB_STATE" = ACTIVE ]; then
    pass "S13: 격리에서 거부된 unpublish 는 publish 를 ACTIVE 로 둠"
else
    fail "S13: unpublish=$S publish 상태=$PUB_STATE"
fi
sql "DELETE FROM service_publishes WHERE id='$PUB'; DELETE FROM attachments WHERE id='$ATT';"

REV="$(sql "SELECT revision FROM vpcs WHERE id='$LINUX_VPC';")"
touch "$OVN/acl-timeout"
R="$(rpc vpc.subnet.create '{"tenant":"t-a","vpc_id":"'"$LINUX_VPC"'","name":"s8","cidr":"10.201.8.0/24","expected_revision":'"$REV"'}')"
S="$(job_wait "$R")"
rm -f "$OVN/acl-timeout"
if [ "$S" = failed ]; then pass "S12: acl-add 시간 초과면 변경은 실패"; else fail "S12: subnet.create=$S"; fi
assert_unfenced "S12(시간 초과로 실패한 VPC 자신)"

SC_ID=aaaaaaaa-0000-4000-8000-000000000011
ATT_C=aaaaaaaa-0000-4000-8000-000000000012
EDGE_C="pcve$(printf '%s' "$OVN_C" | sha256sum | cut -c1-11)"
break_port_group() {
    sql "INSERT INTO subnets(id,vpc_id,name,cidr,network_start,network_end,prefix,gateway,allocation_start,allocation_end,mtu,backend_ref,state) VALUES('$SC_ID','$OVN_C','c1','10.202.1.0/24',0,0,24,'10.202.1.1','10.202.1.10','10.202.1.200',1500,'pcvv_ls_test','ACTIVE');
INSERT INTO attachments(id,subnet_id,vm_uuid,vm_name_snapshot,owner_subject,mac_address,ip_address,state) VALUES('$ATT_C','$SC_ID','aaaaaaaa-0000-4000-8000-000000000013','ghost-c','admin','52:54:00:aa:bb:02','10.202.1.50','ACTIVE');"
    host_nb pg-del "$PG_C"
}
assert_closed() {
    if [ "$(vpc_state ovn-c)" = DEGRADED ]; then pass "$1: ovn-c 는 DEGRADED"; else fail "$1: ovn-c 상태=$(vpc_state ovn-c)"; fi
    if link_is_up "$EDGE_C"; then fail "$1: ovn-c edge 가 UP 으로 남음"; else pass "$1: ovn-c edge DOWN"; fi
    if link_is_up "$BRIDGE"; then pass "$1: Linux bridge UP"; else fail "$1: Linux bridge DOWN"; fi
    if nft_quarantined; then fail "$1: nft quarantine 잔존"; else pass "$1: nft quarantine 없음"; fi
}
repair_port_group() {
    sql "DELETE FROM attachments WHERE id='$ATT_C'; DELETE FROM subnets WHERE id='$SC_ID';"
    local r st
    r="$(rpc vpc.reconcile '{}')"
    st="$(job_wait "$r")"
    if [ "$st" = completed ] && [ -n "$(host_nb --if-exists get Port_Group "$PG_C" _uuid)" ] && [ "$(vpc_state ovn-c)" = ACTIVE ]; then
        pass "$1: reconcile 이 Port_Group 을 되살리고 VPC 는 ACTIVE"
    else
        fail "$1: reconcile=$st state=$(vpc_state ovn-c): $(job_last | cut -c100-)"
    fi
    if link_is_up "$EDGE_C"; then pass "$1: reconcile 뒤 ovn-c edge UP"; else fail "$1: reconcile 뒤 ovn-c edge DOWN"; fi
}
if link_is_up "$EDGE_C"; then pass "S11-전제: $EDGE_C UP"; else fail "S11-전제: $EDGE_C 가 UP 이 아님"; fi

break_port_group
REV="$(sql "SELECT revision FROM vpcs WHERE id='$OVN_C';")"
R="$(rpc vpc.egress.set '{"tenant":"t-b","vpc_id":"'"$OVN_C"'","egress_mode":"isolated","expected_revision":'"$REV"'}')"
S="$(job_wait "$R")"
if [ "$S" = failed ] && job_last | grep -q 'Port_Group'; then
    pass "S11e: 같은 VPC 대상 egress.set 은 Port_Group 부재로 실패"
else
    fail "S11e: egress.set=$S: $(job_last | cut -c100-)"
fi
assert_closed "S11f(대상 자신)"
repair_port_group "S11g(대상 자신)"

break_port_group
REV="$(sql "SELECT revision FROM vpcs WHERE id='$LINUX_VPC';")"
R="$(rpc vpc.subnet.create '{"tenant":"t-a","vpc_id":"'"$LINUX_VPC"'","name":"s5","cidr":"10.201.5.0/24","expected_revision":'"$REV"'}')"
S="$(job_wait "$R")"
mark_bridges
if [ "$S" = completed ]; then pass "S11: 무관한 Linux subnet.create 완료"; else fail "S11: subnet.create=$S: $(job_last | cut -c100-)"; fi
assert_closed "S11a(무관한 변경)"
repair_port_group "S11b(무관한 변경)"

touch "$OVN/acl-fail"
R="$(rpc vpc.delete '{"tenant":"t-b","vpc_id":"'"${OVN_C:-}"'"}')"
S="$(job_wait "$R")"
rm -f "$OVN/acl-fail"
if [ "$S" = failed ] && [ -n "$(vpc_state ovn-c)" ]; then
    pass "S8b: ACTIVE VPC 는 격리 실패 시 삭제되지 않음"
else
    fail "S8b: ACTIVE VPC 삭제=$S row=$(vpc_state ovn-c)"
fi

touch "$OVN/nft-fail"
R="$(rpc vpc.reconcile '{}')"
S="$(job_wait "$R")"
rm -f "$OVN/nft-fail"
if [ "$S" = failed ] && ! link_is_up "$BRIDGE"; then
    pass "S9: Linux nft 실패 reconcile 은 Linux bridge 를 DOWN 시킴(fail-closed 유지)"
else
    fail "S9: nft 실패 reconcile=$S, bridge 가 UP 으로 남음"
fi
R="$(rpc vpc.reconcile '{}')"
S="$(job_wait "$R")"
if [ "$S" = completed ] && link_is_up "$BRIDGE"; then
    pass "S9a: 정책 복구 뒤 reconcile 이 Linux bridge 를 되살림"
else
    fail "S9a: reconcile=$S: $(job_last | cut -c100-)"
fi

if ! make_degraded_ovn ovn-d; then fail "S6-전제: DEGRADED 재현 실패"; fi
nb_stop
kill_daemon
if boot; then
    pass "S6: NB 장애 중 기동 성공(safe-degraded)"
    if link_is_up "$BRIDGE"; then pass "S6a: 재시작 뒤 Linux bridge UP"; else fail "S6a: 재시작 뒤 Linux bridge DOWN"; fi
    if nft_quarantined; then fail "S6b: 재시작 뒤 Linux quarantine 잔존"; else pass "S6b: 재시작 뒤 Linux quarantine 해제"; fi
    if [ "$(vpc_state ovn-d)" = DEGRADED ]; then pass "S6c: OVN VPC 는 DEGRADED"; else fail "S6c: 상태=$(vpc_state ovn-d)"; fi
else
    fail "S6: NB 장애 때문에 데몬이 뜨지 못함"
    grep -F 'startup reconcile' "$STATE/daemon.log" | tail -1 | cut -c1-220
fi

echo "결과: PASS $PASS / FAIL $FAIL"
[ "$FAIL" -eq 0 ]
