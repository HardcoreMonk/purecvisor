#!/usr/bin/env bash
                                                                                            
                                                                      
                                                                                     
                                                                 
                                                                     
                                                                  
                                                                            
                                                  
                                                                       
                                                                
 
                                                                
                                                           
set -euo pipefail

DEST_DIR=/usr/local/lib/purecvisor-test
DEST="$DEST_DIR/bwrap"
PROFILE=/etc/apparmor.d/purecvisor-test-bwrap
PROFILE_NAME=purecvisor-test-bwrap
APT_HOOK=/etc/apt/apt.conf.d/99purecvisor-test-bwrap

die() { echo "install_test_bwrap_profile: $*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "root 로 실행해야 한다 (sudo)"
command -v apparmor_parser >/dev/null 2>&1 || die "apparmor_parser 가 없다"

MODE=install
GROUP="${SUDO_USER:+$(id -gn "$SUDO_USER")}"
while [ $# -gt 0 ]; do
    case "$1" in
        --remove) MODE=remove ;;
        --group) [ $# -ge 2 ] || die "--group 값이 없다"; GROUP="$2"; shift ;;
        *) die "알 수 없는 인자: $1" ;;
    esac
    shift
done

if [ "$MODE" = remove ]; then
    if [ -f "$PROFILE" ]; then
        apparmor_parser -R "$PROFILE" 2>/dev/null || true
        rm -f -- "$PROFILE"
    fi
    rm -f -- "$DEST" "$APT_HOOK"
    rmdir -- "$DEST_DIR" 2>/dev/null || true
    echo "removed: $DEST, $PROFILE, $APT_HOOK"
    exit 0
fi

SYSTEM_BWRAP="$(command -v bwrap)" || die "bwrap(bubblewrap) 이 설치되어 있지 않다"
[ -n "$GROUP" ] || die "--group 을 지정해야 한다 (sudo 없이 root 로 직접 실행한 경우)"
getent group "$GROUP" >/dev/null || die "그룹 없음: $GROUP"

install -d -m 0755 -o root -g root "$DEST_DIR"
install -m 0750 -o root -g "$GROUP" "$SYSTEM_BWRAP" "$DEST"

TMP_PROFILE="$(mktemp "${PROFILE}.XXXXXX")"
trap 'rm -f -- "$TMP_PROFILE"' EXIT
cat > "$TMP_PROFILE" <<EOF
                                                                                 
abi <abi/4.0>,
include <tunables/global>

profile $PROFILE_NAME $DEST flags=(unconfined) {
  userns,

  include if exists <local/$PROFILE_NAME>
}
EOF
chmod 0644 "$TMP_PROFILE"
mv -f -- "$TMP_PROFILE" "$PROFILE"
trap - EXIT
apparmor_parser -r -W "$PROFILE"

                                                                  
if [ -d /etc/apt/apt.conf.d ]; then
    cat > "$APT_HOOK" <<EOF
// PureCVisor 격리 E2E 전용 bwrap 사본 동기화. scripts/install_test_bwrap_profile.sh 가 관리한다.
DPkg::Post-Invoke { "if [ -x $DEST ] && [ -x $SYSTEM_BWRAP ] && ! cmp -s $SYSTEM_BWRAP $DEST; then install -m 0750 -o root -g $GROUP $SYSTEM_BWRAP $DEST; fi; true"; };
EOF
    chmod 0644 "$APT_HOOK"
fi

                                                          
"$DEST" --unshare-user --unshare-net --uid 0 --gid 0 --cap-add ALL \
    --ro-bind / / --dev /dev --proc /proc --tmpfs /tmp nft list tables >/dev/null \
    || die "설치했지만 사본 안에서 nft 가 열리지 않는다"
cmp -s "$DEST" "$SYSTEM_BWRAP" || die "사본이 시스템 bwrap 과 다르다"
echo "installed: $DEST (root:$GROUP 0750), profile $PROFILE_NAME loaded, apt hook $APT_HOOK"
