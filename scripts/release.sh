#!/bin/bash











set -euo pipefail

VERSION="${1:-}"
TAG_ONLY=false
[ "${2:-}" = "--tag-only" ] && TAG_ONLY=true

if [ -z "$VERSION" ]; then
    echo "Usage: $0 <VERSION> [--tag-only]"
    echo "Example: $0 1.0"
    exit 1
fi

RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
STAGING_DIR="$(mktemp -d)"
cleanup() {
    rm -rf "$STAGING_DIR"
}
trap cleanup EXIT

echo "═══════════════════════════════════════════"
echo "  PureCVisor Single Edge Release v${VERSION}"
echo "═══════════════════════════════════════════"

if ! $TAG_ONLY; then
    echo ""
    echo -e "${GREEN}[1/4] Building Single Edge...${NC}"
    make clean
    make release
    SINGLE_SIZE=$(stat -c%s bin/purecvisorsd)
    echo "  purecvisorsd: ${SINGLE_SIZE} bytes"
    for artifact in purecvisorsd pcvctl pcvtui; do
        if [ ! -s "bin/${artifact}" ]; then
            echo -e "${RED}Required artifact missing or empty: bin/${artifact}${NC}"
            exit 1
        fi
        cp "bin/${artifact}" "$STAGING_DIR/"
    done

    echo ""
    echo -e "${GREEN}[2/4] Running tests...${NC}"
    make test 2>&1 | tail -3
fi

echo ""
echo -e "${GREEN}[3/4] Creating SHA256 manifest + optional GPG signature...${NC}"
RELEASE_DIR="release"
MANIFEST_CREATED=false
rm -rf "$RELEASE_DIR"
mkdir -p "$RELEASE_DIR"
if ! $TAG_ONLY; then
    for artifact in purecvisorsd pcvctl pcvtui; do
        cp "$STAGING_DIR/${artifact}" "$RELEASE_DIR/"
    done
    tar czf "$RELEASE_DIR/purecvisor-ui-v${VERSION}.tar.gz" -C ui .
fi

cd "$RELEASE_DIR"
if compgen -G "*" >/dev/null; then
    sha256sum * > SHA256SUMS
    MANIFEST_CREATED=true
    cat SHA256SUMS
else
    echo "No artifacts generated in tag-only mode"
fi
cd ..

GPG_KEY="release@purecvisor.io"
if ! $MANIFEST_CREATED; then
    echo "  SHA256SUMS signature skipped — no manifest in tag-only mode"
elif gpg --list-secret-keys "$GPG_KEY" >/dev/null 2>&1; then
    echo ""
    gpg --detach-sign --armor --local-user "$GPG_KEY" "$RELEASE_DIR/SHA256SUMS"
    echo "  SHA256SUMS.asc created"
else
    echo -e "${RED}  GPG key '$GPG_KEY' not found — signatures skipped${NC}"
fi

echo ""
echo -e "${GREEN}[4/4] Creating tag...${NC}"
TAG="v${VERSION}-single"
if git tag -l "$TAG" | grep -q "$TAG"; then
    echo -e "${RED}Tag $TAG already exists. Use 'git tag -d $TAG' to remove first.${NC}"
    exit 1
fi

git tag -s "$TAG" -u "$GPG_KEY" -m "PureCVisor v${VERSION} Single Edge Release" 2>/dev/null \
  || git tag -a "$TAG" -m "PureCVisor v${VERSION} Single Edge Release"

echo "  Created: $TAG"
echo "  Push tag: git push origin $TAG"
