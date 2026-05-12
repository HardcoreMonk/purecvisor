#!/usr/bin/env bash






set -uo pipefail

I18N="${1:-ui/i18n.js}"
[ -f "$I18N" ] || { echo "ERROR: $I18N not found"; exit 2; }


extract_keys() {
    local lang="$1"
    awk -v lang="$lang" '
        $0 ~ lang"[[:space:]]*:[[:space:]]*\\{" { in_block=1; brace=1; next }
        in_block {
            n = gsub(/\{/, "{")
            m = gsub(/\}/, "}")
            brace += n - m
            if (brace <= 0) { in_block=0; exit }
            if (match($0, /'\''[^'\'']+'\''[[:space:]]*:/)) {
                k = substr($0, RSTART+1, RLENGTH-3)
                print k
            }
        }
    ' "$I18N" | sort -u
}

KO=$(extract_keys ko)
EN=$(extract_keys en)
KO_COUNT=$(echo "$KO" | grep -c .)
EN_COUNT=$(echo "$EN" | grep -c .)

ONLY_KO=$(comm -23 <(echo "$KO") <(echo "$EN"))
ONLY_EN=$(comm -13 <(echo "$KO") <(echo "$EN"))
MISSING_KO=$(echo "$ONLY_EN" | grep -c . || true)
MISSING_EN=$(echo "$ONLY_KO" | grep -c . || true)

echo "i18n keys: ko=${KO_COUNT}, en=${EN_COUNT}"

if [ "$MISSING_EN" -gt 0 ] || [ "$MISSING_KO" -gt 0 ]; then
    echo "FAIL: i18n key mismatch"
    [ "$MISSING_EN" -gt 0 ] && { echo "  en에 누락된 키 (${MISSING_EN}건):"; echo "$ONLY_KO" | sed 's/^/    /'; }
    [ "$MISSING_KO" -gt 0 ] && { echo "  ko에 누락된 키 (${MISSING_KO}건):"; echo "$ONLY_EN" | sed 's/^/    /'; }
    exit 1
fi

echo "PASS: i18n keys synchronized"
exit 0
