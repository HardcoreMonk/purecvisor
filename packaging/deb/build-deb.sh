#!/usr/bin/env bash
                          
                                                          
                                                            
                                                                   
                                                        
                                                          
                                                                
 
                      
                                                       
                                                     
                                                                               
                                                       
                                                                               
                                                                   
                                                                  
 
                                                
                                                                   
                                                    
 
                                                                       
                                                                               
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$PROJECT_DIR"

ARCH="${DEB_ARCH:-amd64}"
                                                    
                                  
                                                     
VER_RAW="$(sed -n 's/.*PCV_PRODUCT_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' include/purecvisor/version.h)"
case "$VER_RAW" in
    *.*.*) PKG_VER="$VER_RAW" ;;                                 
    *)     PKG_VER="${VER_RAW}.${DEB_PATCH:-0}" ;;                         
esac
PKG_NAME="purecvisor-single"

DIST_DIR="$PROJECT_DIR/dist"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

echo "[deb] 패키지 $PKG_NAME v$PKG_VER ($ARCH) 조립..."

                                                           
for b in bin/purecvisorsd bin/pcvctl; do
    [ -x "$b" ] || { echo "[deb] ERROR: $b 없음 — 먼저 'make release' 실행"; exit 1; }
done
[ -f ui/app.bundle.js ] || { echo "[deb] ERROR: ui/app.bundle.js 없음 — 먼저 'make ui-bundle'"; exit 1; }

                                                              
mkdir -p "$STAGE/DEBIAN" \
         "$STAGE/usr/local/bin" \
         "$STAGE/usr/local/sbin" \
         "$STAGE/usr/local/share/purecvisor/ui" \
         "$STAGE/etc/systemd/system" \
         "$STAGE/etc/purecvisor" \
         "$STAGE/etc/apparmor.d" \
         "$STAGE/etc/modules-load.d"

install -m755 bin/purecvisorsd bin/pcvctl "$STAGE/usr/local/bin/"
strip "$STAGE/usr/local/bin/purecvisorsd" \
      "$STAGE/usr/local/bin/pcvctl" 2>/dev/null || true

                                                                 
install -m755 packaging/apparmor/pcv-apparmor "$STAGE/usr/local/sbin/pcv-apparmor"
install -m755 scripts/install-ovn-single.sh "$STAGE/usr/local/sbin/purecvisor-ovn-single"

                         
cp -a ui/*.js ui/*.html ui/*.css ui/*.md ui/*.json ui/*.png "$STAGE/usr/local/share/purecvisor/ui/" 2>/dev/null || true
[ -d ui/vendor ]  && cp -a ui/vendor  "$STAGE/usr/local/share/purecvisor/ui/"
[ -d ui/modules ] && cp -a ui/modules "$STAGE/usr/local/share/purecvisor/ui/"
                                                                
                                                
for f in index.html style.css app.bundle.js sw.js i18n.js manifest.json; do
    [ -f "$STAGE/usr/local/share/purecvisor/ui/$f" ] || { echo "[deb] ERROR: UI 필수 자산 누락: $f"; exit 1; }
done

                               
install -m644 packaging/deb/purecvisorsd.service "$STAGE/etc/systemd/system/purecvisorsd.service"
install -m644 packaging/deb/daemon.conf.sample   "$STAGE/etc/purecvisor/daemon.conf.sample"

                                                                         
install -d "$STAGE/etc/systemd/system/purecvisorsd.service.d"
install -m644 packaging/systemd/96-hidepid.conf     "$STAGE/etc/systemd/system/purecvisorsd.service.d/96-hidepid.conf"
install -m644 packaging/systemd/proc-hidepid.service "$STAGE/etc/systemd/system/proc-hidepid.service"

                                                                                    
install -m644 packaging/systemd/purecvisor-host-tuning.service "$STAGE/etc/systemd/system/purecvisor-host-tuning.service"

                                                      
                                                            
                                                               
                                                          
                                                        
install -m644 packaging/apparmor/usr.local.bin.purecvisorsd \
    "$STAGE/etc/apparmor.d/usr.local.bin.purecvisorsd"

                                                                        
                                                                       
                                                                   
                                                       
                                               
                                                 
                                                            
                                                
install -m644 packaging/deb/purecvisor-lio.conf \
    "$STAGE/etc/modules-load.d/purecvisor-lio.conf"

                                                   
resolve_deps() {
    local so real pkg
    ldd bin/purecvisorsd bin/pcvctl 2>/dev/null \
        | awk '/=>/{print $3}' | sort -u | while read -r so; do
        [ -e "$so" ] || continue
        real="$(readlink -f "$so")"
        pkg="$(dpkg-query -S "$real" 2>/dev/null | grep -vi diversion | head -1 | cut -d: -f1)"
        [ -n "$pkg" ] && echo "$pkg"
    done | sort -u
}
LIBDEPS="$(resolve_deps | paste -sd, || true)"

                                                   
                                                    
                                                           
                                                   
                                                            
                                                    
                                               
SECURITY_FLOOR_LIBS="libssl3 libssl3t64 libsoup-3.0-0 libsqlite3-0 libglib2.0-0 libglib2.0-0t64"

apply_security_floors() {
                                                           
    local deps="$1" out="" pkg ver mm floor
    local IFS=','
    for pkg in $deps; do
        pkg="$(echo "$pkg" | xargs)"             
        [ -z "$pkg" ] && continue
        floor=""
        case " $SECURITY_FLOOR_LIBS " in
            *" $pkg "*)
                ver="$(dpkg-query -W -f='${Version}' "$pkg" 2>/dev/null || true)"
                                                                          
                                                                                   
                mm="$(printf '%s' "$ver" | sed -e 's/^[0-9]*://' -e 's/-.*//' \
                        | awk -F. 'NF>=2{print $1"."$2}')"
                [ -n "$mm" ] && floor=" (>= $mm)"
                ;;
        esac
        out="${out:+$out, }${pkg}${floor}"
    done
    printf '%s' "$out"
}
LIBDEPS="$(apply_security_floors "$LIBDEPS")"

                      
SVCDEPS="libvirt-daemon-system, qemu-system-x86, dnsmasq-base, nftables, iproute2"
ALLDEPS="${LIBDEPS:+$LIBDEPS, }$SVCDEPS"

INSTALLED_KB="$(du -sk "$STAGE/usr" "$STAGE/etc" | awk '{s+=$1} END{print s}')"

                                                                
cat > "$STAGE/DEBIAN/control" <<CTRL
Package: $PKG_NAME
Version: $PKG_VER
Section: admin
Priority: optional
Architecture: $ARCH
Maintainer: PureCVisor <ops@purecvisor.local>
Installed-Size: $INSTALLED_KB
Depends: $ALLDEPS
Recommends: openvswitch-switch, ovn-central, ovn-host, zfsutils-linux, cloud-image-utils, apparmor-utils
Conflicts: purecvisor-multi
Homepage: https://purecvisor.example.com
Description: PureCVisor Single Edge — C23 KVM 하이퍼바이저 오케스트레이터
 단일 서버 KVM/libvirt 오케스트레이션 데몬(purecvisorsd) + CLI(pcvctl).
 관리형 NAT 네트워크(pcvnat0), 호스트 방화벽 자동 공존, 보안 그룹(nftables
 스코프 체인), REST(:8080)/UDS(io_uring) API, Web UI 를 제공한다.
 이 패키지는 Single Edge 공개판이며 클러스터/멀티엣지 기능은 포함하지 않는다.
CTRL

cat > "$STAGE/DEBIAN/conffiles" <<'CF'
/etc/systemd/system/purecvisorsd.service
/etc/apparmor.d/usr.local.bin.purecvisorsd
/etc/modules-load.d/purecvisor-lio.conf
CF

cat > "$STAGE/DEBIAN/postinst" <<'POST'
#!/bin/sh
set -e
case "$1" in
  configure)
    mkdir -p /var/lib/purecvisor /var/log/purecvisor /etc/purecvisor
    if [ ! -e /etc/purecvisor/daemon.conf ]; then
        cp -a /etc/purecvisor/daemon.conf.sample /etc/purecvisor/daemon.conf
        echo "purecvisor-single: /etc/purecvisor/daemon.conf 생성(sample 기반) — admin_password 등 편집 필요"
    fi
                                                                   
    chmod 600 /etc/purecvisor/daemon.conf 2>/dev/null || true
    chown root:root /etc/purecvisor/daemon.conf 2>/dev/null || true
                                                                                    
    getent group pcvmon >/dev/null 2>&1 || groupadd --system pcvmon
    systemctl daemon-reload || true
    systemctl enable purecvisorsd.service || true
                                                                              
                                                             
    systemctl enable proc-hidepid.service || true
                                                            
                                                          
    systemctl enable purecvisor-host-tuning.service || true

                                                             
                                                                 
                                                    
                                                         
                                                      
                                                                 
                         
                                                                
                                                           
    systemctl restart systemd-modules-load.service >/dev/null 2>&1 || true
    echo "purecvisor-single: /etc/modules-load.d/purecvisor-lio.conf 설치 — LIO 3종+nf_conntrack_bridge 부팅 로드."
    echo "  확인: lsmod | grep -E '^(target_core_mod|iscsi_target_mod|target_core_iblock|nf_conntrack_bridge) '  및  ls -d /sys/kernel/config/target"
    echo "  (systemd-modules-load 는 모듈을 못 찾아도 성공으로 끝나므로 status 를 신뢰하지 말 것)"

                                                                      
                                                              
                                                                  
                                                             
                                    
                                                                
                                                             
                                                          
    APROF=/etc/apparmor.d/usr.local.bin.purecvisorsd
    DISDIR=/etc/apparmor.d/disable
    DISLINK="$DISDIR/usr.local.bin.purecvisorsd"
    FCLINK=/etc/apparmor.d/force-complain/usr.local.bin.purecvisorsd
                                                            
                                                               
                                                        
                                                     
                                                         
    mkdir -p "$DISDIR"
    ln -sfn ../usr.local.bin.purecvisorsd "$DISLINK"
                                                             
    rm -f "$FCLINK" 2>/dev/null || true
                                                  
                                                            
                                                                
                                                             
                                            
    if command -v apparmor_parser >/dev/null 2>&1 \
       && [ -d /sys/kernel/security/apparmor ] \
       && [ -f "$APROF" ]; then
        apparmor_parser -R "$APROF" >/dev/null 2>&1 || true
    fi
    echo "purecvisor-single: AppArmor 프로필은 참조용 아티팩트로만 존치($APROF) — ADR-0028 에 따라 2.0 데몬에 부착하지 않음(disable 심링크로 부팅 자동 로드 차단 + 로드된 프로필 해제)."
    echo "  재부착은 운영자 명시 opt-in 행위다: pcv-apparmor enforce|complain (hidepid=2 환경에서는 libvirt 연결 파손 위험)."

    echo "purecvisor-single 설치 완료. 시작: systemctl start purecvisorsd"
    echo "  nginx 리버스 프록시(:80/:443 -> :8080)는 별도 구성 필요(패키지 미포함)."
    ;;
esac
exit 0
POST

cat > "$STAGE/DEBIAN/prerm" <<'PRE'
#!/bin/sh
set -e
case "$1" in
  remove|deconfigure)
    systemctl stop purecvisorsd.service || true
    systemctl disable purecvisorsd.service || true
    ;;
esac
exit 0
PRE

cat > "$STAGE/DEBIAN/postrm" <<'PRM'
#!/bin/sh
set -e
case "$1" in
  remove|purge)
    systemctl daemon-reload || true
    ;;
esac
                                              
                                                                
                                                            
if [ "$1" = "purge" ]; then
    rm -f /etc/apparmor.d/disable/usr.local.bin.purecvisorsd 2>/dev/null || true
    rm -f /etc/apparmor.d/force-complain/usr.local.bin.purecvisorsd 2>/dev/null || true
    if command -v apparmor_parser >/dev/null 2>&1 \
       && [ -d /sys/kernel/security/apparmor ] \
       && [ -f /etc/apparmor.d/usr.local.bin.purecvisorsd ]; then
        apparmor_parser -R /etc/apparmor.d/usr.local.bin.purecvisorsd >/dev/null 2>&1 || true
    fi
fi
exit 0
PRM

chmod 0755 "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/prerm" "$STAGE/DEBIAN/postrm"

                  
( cd "$STAGE" && find usr etc -type f -exec md5sum {} \; > DEBIAN/md5sums )

                                                               
mkdir -p "$DIST_DIR"
OUT="$DIST_DIR/${PKG_NAME}_${PKG_VER}_${ARCH}.deb"
fakeroot dpkg-deb --build --root-owner-group "$STAGE" "$OUT" >/dev/null

echo "[deb] 완료: $OUT"
dpkg-deb -I "$OUT" | sed -n 's/^ //p' | grep -E "^(Package|Version|Installed-Size|Depends):"
echo "[deb] 크기: $(du -h "$OUT" | cut -f1)"
