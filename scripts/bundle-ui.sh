#!/bin/bash
# F-5: PureCVisor UI bundler — `make ui-bundle` 위임 래퍼 (하위호환 진입점)
#
# 2026-07-06 프론트 #2/#3 이후 번들 파이프라인의 단일 소스는 Makefile의
# `ui-bundle` 타깃이다: 모듈 동기화 검증(BUG-22 가드) + esbuild 민파이/
# 소스맵(src-sha1 배너) + sw.js CACHE_NAME 자동 bump. 이 스크립트는 기존
# 문서·워크플로가 참조하는 진입점과 로컬 데몬 배포 동작만 유지한다.
#
# Run from repo root: ./scripts/bundle-ui.sh   (PCV_NO_DEPLOY=1 로 배포 생략)
set -e
cd "$(dirname "$0")/.."

make ui-bundle

OUT="ui/app.bundle.js"
MAP="ui/app.bundle.js.map"
SW="ui/sw.js"

# 로컬 데몬이 서빙하는 경로로 배포 (rest_server.c::ui_base_dir).
# PCV_NO_DEPLOY=1 로 비활성화 가능 (CI 등 권한 없는 환경).
INSTALL_DIR="/usr/local/share/purecvisor/ui"
if [ "${PCV_NO_DEPLOY:-0}" = "1" ]; then
  echo "[bundle] PCV_NO_DEPLOY=1 — skipping install to $INSTALL_DIR"
elif [ -d "$INSTALL_DIR" ]; then
  if sudo -n true 2>/dev/null; then
    sudo cp "$OUT" "$INSTALL_DIR/app.bundle.js"
    [ -f "$MAP" ] && sudo cp "$MAP" "$INSTALL_DIR/app.bundle.js.map"
    [ -f "$SW" ] && sudo cp "$SW" "$INSTALL_DIR/sw.js"
    echo "[bundle] installed → $INSTALL_DIR"
  else
    echo "[bundle] WARN: sudo password 필요 — 수동 배포: sudo cp $OUT $MAP $SW $INSTALL_DIR/"
  fi
else
  echo "[bundle] INFO: $INSTALL_DIR 미존재 — 데몬 미설치 환경, 배포 생략"
fi
