#!/bin/bash
                                                                 
                                                 
 
      
                              
                                         
 
     
                            
 
                          
                                                               
                                                           
                                                   
                                                      
                                                                    
             
                                                                  
                                                      
                                                                        
 
                      
                                                    
                                                      
                                 
                                                                 

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

VERSION="${1:-}"
TAG_ONLY=false
if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Usage: $0 <VERSION> [--tag-only]" >&2
    exit 2
fi
if [[ $# -eq 2 ]]; then
    if [[ "$2" != "--tag-only" ]]; then
        echo "Unknown release option: $2" >&2
        exit 2
    fi
    TAG_ONLY=true
fi

if [ -z "$VERSION" ]; then
    echo "Usage: $0 <VERSION> [--tag-only]"
    echo "Example: $0 2.0.0"
    exit 2
fi
if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "VERSION must be canonical MAJOR.MINOR.PATCH (example: 2.0.0)" >&2
    exit 2
fi
PRODUCT_VERSION="$(awk '$1 == "#define" && $2 == "PCV_PRODUCT_VERSION" {gsub(/\"/, "", $3); print $3}' \
    include/purecvisor/version.h)"
if [[ -z "$PRODUCT_VERSION" || "$VERSION" != "$PRODUCT_VERSION" ]]; then
    echo "VERSION must match PCV_PRODUCT_VERSION ($PRODUCT_VERSION)" >&2
    exit 2
fi

RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
STAGING_DIR="$(mktemp -d)"
cleanup() {
    rm -rf "$STAGING_DIR"
}
trap cleanup EXIT

require_clean_tree() {
    local status

    status="$(git status --porcelain --untracked-files=normal)"
    if [[ -n "$status" ]]; then
        echo -e "${RED}Release requires a clean worktree and index.${NC}" >&2
        printf '%s\n' "$status" >&2
        return 1
    fi
}

                                                     
require_clean_tree

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
    for artifact in purecvisorsd pcvctl; do
        if [ ! -s "bin/${artifact}" ]; then
            echo -e "${RED}Required artifact missing or empty: bin/${artifact}${NC}"
            exit 1
        fi
        cp "bin/${artifact}" "$STAGING_DIR/"
    done

    echo ""
    echo -e "${GREEN}[2/4] Running release and public-boundary gates...${NC}"
    make test
    make check-all
    PCV_NO_DEPLOY=1 scripts/bundle-ui.sh
    python3 scripts/check_ui_bundle_fresh.py
    node --check ui/app.bundle.js
    node --check ui/sw.js
    python3 scripts/check_xss.py
    python3 scripts/check_design_md.py
    tests/integration/test_single_ovn_ovs_layout.sh
    tests/integration/test_single_ui_surface.sh
    tests/integration/test_single_backend_build_boundaries.sh

    forbidden_pattern='purecvisormd|make multi|vm\.migrate|cluster\.|federation\.site'
                                                         
                                                             
    for artifact in purecvisorsd pcvctl; do
        staged_artifact="$STAGING_DIR/$artifact"
        strings_file="$STAGING_DIR/$artifact.strings"
        strings "$staged_artifact" >"$strings_file"
        if rg -q "$forbidden_pattern" "$strings_file"; then
            echo -e "${RED}Forbidden public surface found in staged $artifact${NC}" >&2
            exit 1
        else
            rg_status=$?
            [[ "$rg_status" -eq 1 ]] || exit "$rg_status"
        fi
    done
fi

                                                                     
                                                   
                                                           
require_clean_tree

RELEASE_DIR="release"
MANIFEST_CREATED=false
if ! $TAG_ONLY; then
    echo ""
    echo -e "${GREEN}[3/4] Creating SHA256 manifest + optional GPG signature...${NC}"
    rm -rf "$RELEASE_DIR"
    mkdir -p "$RELEASE_DIR"
    for artifact in purecvisorsd pcvctl; do
        cp "$STAGING_DIR/${artifact}" "$RELEASE_DIR/"
    done
    tar czf "$RELEASE_DIR/purecvisor-single-ui-v${VERSION}.tar.gz" -C ui .

    cd "$RELEASE_DIR"
    sha256sum -- * > SHA256SUMS
    MANIFEST_CREATED=true
    cat SHA256SUMS
    cd ..
else
    echo ""
    echo -e "${GREEN}[3/4] Tag-only: keeping existing release artifacts unchanged${NC}"
fi

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
TAG="v${VERSION}"
if git tag -l "$TAG" | grep -q "$TAG"; then
    echo -e "${RED}Tag $TAG already exists. Use 'git tag -d $TAG' to remove first.${NC}"
    exit 1
fi

git tag -s "$TAG" -u "$GPG_KEY" -m "PureCVisor v${VERSION} Single Edge Release" 2>/dev/null \
  || git tag -a "$TAG" -m "PureCVisor v${VERSION} Single Edge Release"

echo "  Created: $TAG"
echo "  Push tag: git push origin $TAG"
