#!/usr/bin/env bash
# =============================================================================
# PureCVisor Single Edge — .deb 패키지 빌드 (make deb 에서 호출)
# =============================================================================
# 전제: 릴리스 바이너리(bin/{purecvisorsd,pcvctl})와 UI 번들(ui/app.bundle.js)이
#       이미 빌드되어 있어야 한다. Makefile 의 deb 타깃이 release + ui-bundle 선행.
#
# 산출: dist/purecvisor-single_<version>_amd64.deb
# 버전: include/purecvisor/version.h 의 PCV_PRODUCT_VERSION 단일 소스에서 파생
#       (PATCH 는 인자 또는 기본 0 → <MINOR>.0). 태그와 정합 유지.
#
# 의존: dpkg-deb, fakeroot (빌드 호스트). 런타임 Depends 는 ldd → dpkg-query 로 산출.
# =============================================================================
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$PROJECT_DIR"

ARCH="${DEB_ARCH:-amd64}"
# 패키지 버전은 version.h(PCV_PRODUCT_VERSION) 단일 소스에서 파생.
#   - full semver("1.1.1")면 그대로 사용
#   - MINOR-only("1.1")면 DEB_PATCH(기본 0) 를 붙여 "1.1.0"
VER_RAW="$(sed -n 's/.*PCV_PRODUCT_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' include/purecvisor/version.h)"
case "$VER_RAW" in
    *.*.*) PKG_VER="$VER_RAW" ;;                 # 이미 full semver
    *)     PKG_VER="${VER_RAW}.${DEB_PATCH:-0}" ;;  # MINOR-only → patch 부착
esac
PKG_NAME="purecvisor-single"

DIST_DIR="$PROJECT_DIR/dist"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

echo "[deb] 패키지 $PKG_NAME v$PKG_VER ($ARCH) 조립..."

# ── 선행 산출물 확인 ────────────────────────────────────────────
for b in bin/purecvisorsd bin/pcvctl; do
    [ -x "$b" ] || { echo "[deb] ERROR: $b 없음 — 먼저 'make release' 실행"; exit 1; }
done
[ -f ui/app.bundle.js ] || { echo "[deb] ERROR: ui/app.bundle.js 없음 — 먼저 'make ui-bundle'"; exit 1; }

# ── 트리 조립 ───────────────────────────────────────────────────
mkdir -p "$STAGE/DEBIAN" \
         "$STAGE/usr/local/bin" \
         "$STAGE/usr/local/share/purecvisor/ui" \
         "$STAGE/etc/systemd/system" \
         "$STAGE/etc/purecvisor"

install -m755 bin/purecvisorsd bin/pcvctl "$STAGE/usr/local/bin/"
strip "$STAGE/usr/local/bin/purecvisorsd" \
      "$STAGE/usr/local/bin/pcvctl" 2>/dev/null || true

# UI 자산 (번들/모듈/vendor/에셋)
cp -a ui/*.js ui/*.html ui/*.css ui/*.md ui/*.json ui/*.png "$STAGE/usr/local/share/purecvisor/ui/" 2>/dev/null || true
[ -d ui/vendor ]  && cp -a ui/vendor  "$STAGE/usr/local/share/purecvisor/ui/"
[ -d ui/modules ] && cp -a ui/modules "$STAGE/usr/local/share/purecvisor/ui/"
# 위 cp 는 2>/dev/null || true 로 실패를 삼키므로, index.html/sw.js 가 참조하는
# 필수 자산은 스테이징 존재를 명시 검증한다 (glob 누락 시 여기서 빌드 실패).
for f in index.html style.css app.bundle.js sw.js i18n.js manifest.json; do
    [ -f "$STAGE/usr/local/share/purecvisor/ui/$f" ] || { echo "[deb] ERROR: UI 필수 자산 누락: $f"; exit 1; }
done

# systemd 유닛 + 설정 샘플 (패키징 소스에서)
install -m644 packaging/deb/purecvisorsd.service "$STAGE/etc/systemd/system/purecvisorsd.service"
install -m644 packaging/deb/daemon.conf.sample   "$STAGE/etc/purecvisor/daemon.conf.sample"

# ── 런타임 라이브러리 Depends 산출 (직접 링크 → dpkg 패키지) ──────
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
# 서비스 런타임 도구 (라이브러리 외)
SVCDEPS="libvirt-daemon-system, qemu-system-x86, dnsmasq-base, nftables, iproute2"
ALLDEPS="${LIBDEPS:+$LIBDEPS, }$SVCDEPS"

INSTALLED_KB="$(du -sk "$STAGE/usr" "$STAGE/etc" | awk '{s+=$1} END{print s}')"

# ── DEBIAN 메타 ─────────────────────────────────────────────────
cat > "$STAGE/DEBIAN/control" <<CTRL
Package: $PKG_NAME
Version: $PKG_VER
Section: admin
Priority: optional
Architecture: $ARCH
Maintainer: PureCVisor <ops@purecvisor.local>
Installed-Size: $INSTALLED_KB
Depends: $ALLDEPS
Recommends: openvswitch-switch, ovn-host, zfsutils-linux, cloud-image-utils
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
    # admin_password 등 자격증명이 담기므로 root 전용 권한 강제 (world-readable 방지)
    chmod 600 /etc/purecvisor/daemon.conf 2>/dev/null || true
    chown root:root /etc/purecvisor/daemon.conf 2>/dev/null || true
    systemctl daemon-reload || true
    systemctl enable purecvisorsd.service || true
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
exit 0
PRM

chmod 0755 "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/prerm" "$STAGE/DEBIAN/postrm"

# md5sums (deb 관례)
( cd "$STAGE" && find usr etc -type f -exec md5sum {} \; > DEBIAN/md5sums )

# ── 빌드 ───────────────────────────────────────────────────────
mkdir -p "$DIST_DIR"
OUT="$DIST_DIR/${PKG_NAME}_${PKG_VER}_${ARCH}.deb"
fakeroot dpkg-deb --build --root-owner-group "$STAGE" "$OUT" >/dev/null

echo "[deb] 완료: $OUT"
dpkg-deb -I "$OUT" | sed -n 's/^ //p' | grep -E "^(Package|Version|Installed-Size|Depends):"
echo "[deb] 크기: $(du -h "$OUT" | cut -f1)"
