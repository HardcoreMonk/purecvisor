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
         "$STAGE/usr/local/sbin" \
         "$STAGE/usr/local/share/purecvisor/ui" \
         "$STAGE/etc/systemd/system" \
         "$STAGE/etc/purecvisor" \
         "$STAGE/etc/apparmor.d"

install -m755 bin/purecvisorsd bin/pcvctl "$STAGE/usr/local/bin/"
strip "$STAGE/usr/local/bin/purecvisorsd" \
      "$STAGE/usr/local/bin/pcvctl" 2>/dev/null || true

# AppArmor 모드 토글 헬퍼(우리 프로필만 대상 → 타 프로필 충돌에 견고, apparmor-utils 불요)
install -m755 packaging/apparmor/pcv-apparmor "$STAGE/usr/local/sbin/pcv-apparmor"

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

# AppArmor MAC 프로필 (G1-③) — 파일만 배포. postinst 는 COMPLAIN(감사-only)로만
# 로드하고 절대 enforce 하지 않는다(데몬 동작 무변경). 프로필 파일 자체에
# flags=(complain) 이 박혀 있다. enforce 전환은 운영자 opt-in(aa-enforce).
install -m644 packaging/apparmor/usr.local.bin.purecvisorsd \
    "$STAGE/etc/apparmor.d/usr.local.bin.purecvisorsd"

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

# ── 보안 핵심 라이브러리 버전 하한(floor) 보강 (평가 A06) ─────────
# ldd 자동 산출 Depends 는 패키지명만 담고 버전 제약이 없어, 알려진 취약 구버전
# (예: openssl<3.0, glib<2.80)이 의존성으로 허용될 수 있다. 아래 보안 lib 에는
# "(>= <설치 major.minor>)" floor 를 부여해 보수적으로 하한을 건다.
# 패키지명은 빌드 호스트에 따라 t64 변종(libssl3t64 / libglib2.0-0t64 등)일 수
# 있으므로, ldd 로 실제 해석된 패키지의 설치 버전에서 major.minor 를 뽑아 그
# 패키지에만 floor 를 건다(존재하지 않는 패키지에 임의 제약을 만들지 않음).
SECURITY_FLOOR_LIBS="libssl3 libssl3t64 libsoup-3.0-0 libsqlite3-0 libglib2.0-0 libglib2.0-0t64"

apply_security_floors() {
    # $1: 콤마구분 Depends 문자열 → floor 보강된 콤마구분 문자열을 stdout 으로.
    local deps="$1" out="" pkg ver mm floor
    local IFS=','
    for pkg in $deps; do
        pkg="$(echo "$pkg" | xargs)"   # 앞뒤 공백 제거
        [ -z "$pkg" ] && continue
        floor=""
        case " $SECURITY_FLOOR_LIBS " in
            *" $pkg "*)
                ver="$(dpkg-query -W -f='${Version}' "$pkg" 2>/dev/null || true)"
                # 업스트림 major.minor 추출: epoch(앞 'N:') 제거 → 데비안 리비전('-…') 제거
                #   → 첫 두 필드. 예: "3.0.13-0ubuntu3.11" → "3.0", "2.80.0-6…" → "2.80"
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
Recommends: openvswitch-switch, ovn-host, zfsutils-linux, cloud-image-utils, apparmor-utils
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

    # --- AppArmor MAC 프로필 (G1-③): 기본 COMPLAIN, force-complain 심링크 강제 --
    # 프로필 파일엔 flags 없음(enforce-default). COMPLAIN 은 force-complain 심링크로
    # 강제한다(v1.3.6~). 데몬 동작 무변경(차단 없이 위반만 커널 audit 에 기록).
    # enforce 전환은 운영자 opt-in(aa-enforce = 심링크 제거). 로드 실패가 설치를 깨지
    # 않도록 전 구간 방어.
    APROF=/etc/apparmor.d/usr.local.bin.purecvisorsd
    FCDIR=/etc/apparmor.d/force-complain
    FCLINK="$FCDIR/usr.local.bin.purecvisorsd"
    # 심링크 관리(모드 보존이 핵심 — enforce 노드가 업그레이드로 complain 되돌아가지 않게):
    #  - 첫 설치($2 없음): complain 심링크 생성(안전 기본).
    #  - 구 스킴(<1.3.6, in-file flag·무심링크)에서 업그레이드: complain 심링크 생성(마이그레이션).
    #  - 신 스킴(>=1.3.6)에서 업그레이드: 심링크 상태 무변경 → 운영자 모드 보존
    #    (enforce=무심링크 유지, complain=심링크 유지). 무심링크를 마이그레이션으로 오인 금지.
    if [ -z "$2" ]; then
        mkdir -p "$FCDIR"
        ln -sf ../usr.local.bin.purecvisorsd "$FCLINK"
    elif dpkg --compare-versions "$2" lt 1.3.6 2>/dev/null; then
        if [ ! -L "$FCLINK" ] && [ ! -e "$FCLINK" ]; then
            mkdir -p "$FCDIR"
            ln -sf ../usr.local.bin.purecvisorsd "$FCLINK"
        fi
    fi
    # (>=1.3.6 업그레이드는 심링크 무변경 — 운영자 enforce/complain 선택 보존)
    if command -v apparmor_parser >/dev/null 2>&1 \
       && [ -d /sys/kernel/security/apparmor ] \
       && [ -f "$APROF" ]; then
        if [ -L "$FCLINK" ]; then
            # COMPLAIN 강제(-C): 심링크 존재 = 감사-only
            if apparmor_parser -r -W -C "$APROF" >/dev/null 2>&1; then
                echo "purecvisor-single: AppArmor 프로필 COMPLAIN(감사-only) 로드됨 — 차단 없음. enforce 전환: pcv-apparmor enforce"
            else
                echo "purecvisor-single: AppArmor 프로필 로드 생략(비치명적)."
            fi
        else
            # ENFORCE: 운영자가 pcv-apparmor enforce 로 심링크 제거함 → enforce 유지 로드
            if apparmor_parser -r -W "$APROF" >/dev/null 2>&1; then
                echo "purecvisor-single: AppArmor 프로필 ENFORCE 로드됨(운영자 opt-in 유지). 롤백: pcv-apparmor complain"
            else
                echo "purecvisor-single: AppArmor 프로필 로드 생략(비치명적)."
            fi
        fi
    else
        echo "purecvisor-single: AppArmor 미가동/미설치 — 프로필 파일만 배치($APROF). 활성화는 문서 참조."
    fi

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
# purge 시에만 AppArmor 프로필을 커널에서 언로드 + force-complain 심링크 제거
# (conffile 자체는 dpkg 가 제거). remove(설정 보존)에서는 언로드하지 않아 재설치 시 유지.
if [ "$1" = "purge" ]; then
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

# md5sums (deb 관례)
( cd "$STAGE" && find usr etc -type f -exec md5sum {} \; > DEBIAN/md5sums )

# ── 빌드 ───────────────────────────────────────────────────────
mkdir -p "$DIST_DIR"
OUT="$DIST_DIR/${PKG_NAME}_${PKG_VER}_${ARCH}.deb"
fakeroot dpkg-deb --build --root-owner-group "$STAGE" "$OUT" >/dev/null

echo "[deb] 완료: $OUT"
dpkg-deb -I "$OUT" | sed -n 's/^ //p' | grep -E "^(Package|Version|Installed-Size|Depends):"
echo "[deb] 크기: $(du -h "$OUT" | cut -f1)"
