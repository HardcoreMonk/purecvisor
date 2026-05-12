#!/bin/bash


set -e
cd "$(dirname "$0")/.."

UI_DIR="ui"
OUT="$UI_DIR/app.bundle.js"

ORDER=(
  api endpoints ui uxlib modal charts monitor security vm container
  network storage cloud help nav theme accounts advanced
  selfhealing
)





order_sorted=$(printf '%s\n' "${ORDER[@]}" | sort -u)
actual_sorted=$(cd "$UI_DIR/modules" 2>/dev/null && ls -1 *.js 2>/dev/null | sed 's/\.js$//' | sort -u)
if [ -z "$actual_sorted" ]; then
  echo "ERROR: $UI_DIR/modules/*.js 파일을 찾을 수 없음" >&2
  exit 1
fi
missing_in_order=$(comm -23 <(echo "$actual_sorted") <(echo "$order_sorted"))
ghost_in_order=$(comm -13 <(echo "$actual_sorted") <(echo "$order_sorted"))
if [ -n "$missing_in_order" ] || [ -n "$ghost_in_order" ]; then
  echo "ERROR: bundle-ui.sh ORDER와 $UI_DIR/modules/*.js 불일치" >&2
  if [ -n "$missing_in_order" ]; then
    echo "  ORDER에 누락된 모듈(번들에 포함 안 됨):" >&2
    echo "$missing_in_order" | sed 's/^/    - /' >&2
  fi
  if [ -n "$ghost_in_order" ]; then
    echo "  ORDER에만 있는 유령 모듈(파일 없음):" >&2
    echo "$ghost_in_order" | sed 's/^/    - /' >&2
  fi
  echo "  → scripts/bundle-ui.sh의 ORDER 배열을 갱신하거나 파일을 추가/삭제하세요." >&2
  exit 1
fi
echo "[bundle] ORDER 검증 OK — ${#ORDER[@]}개 모듈 전부 존재"

{
  for m in "${ORDER[@]}"; do
    f="$UI_DIR/modules/${m}.js"
    if [ ! -f "$f" ]; then
      echo "ERROR: $f not found" >&2; exit 1
    fi
    echo
    cat "$f"
  done
} > "$OUT"


SW="$UI_DIR/sw.js"
if [ -f "$SW" ]; then
  HASH=$(sha1sum "$OUT" | cut -c1-8)
  sed -i "s|const CACHE_NAME = 'pcv-ui-v[^']*';|const CACHE_NAME = 'pcv-ui-v$HASH';|" "$SW"
  echo "[bundle] SW cache: pcv-ui-v$HASH"
fi


if command -v node >/dev/null 2>&1; then
  if node -c "$OUT" 2>&1; then
    echo "[bundle] OK — $(wc -l < "$OUT") lines, $(du -h "$OUT" | cut -f1)"
  else
    echo "[bundle] FAIL — syntax error in $OUT" >&2
    exit 1
  fi
else
  echo "[bundle] generated (node not available for syntax check) — $(wc -l < "$OUT") lines"
fi



INSTALL_DIR="/usr/local/share/purecvisor/ui"
if [ "${PCV_NO_DEPLOY:-0}" = "1" ]; then
  echo "[bundle] PCV_NO_DEPLOY=1 — skipping install to $INSTALL_DIR"
elif [ -d "$INSTALL_DIR" ]; then
  if sudo -n true 2>/dev/null; then
    sudo cp "$OUT" "$INSTALL_DIR/app.bundle.js"
    [ -f "$SW" ] && sudo cp "$SW" "$INSTALL_DIR/sw.js"
    echo "[bundle] installed → $INSTALL_DIR"
  else
    echo "[bundle] WARN: sudo password 필요 — 수동 배포: sudo cp $OUT $SW $INSTALL_DIR/"
  fi
else
  echo "[bundle] INFO: $INSTALL_DIR 미존재 — 데몬 미설치 환경, 배포 생략"
fi
