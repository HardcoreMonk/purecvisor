#!/usr/bin/env bash
                                                                                        
                                            
                                                                          
set -euo pipefail

                                                                      
 
     
                                                       
                                                                                   
                                                      
     
                                                              
                                                        
                        
          
              
 
                
                                
                                                        
                                                       
                                                          
                                                      
                                                          
                                                    
                                                 
                                        
                                                                  
                                                   

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

fail() {
  printf 'FAIL: %s\n' "$1" >&2
  exit 1
}

CONF="$ROOT_DIR/packaging/deb/purecvisor-lio.conf"
BUILD="$ROOT_DIR/packaging/deb/build-deb.sh"

[[ -f "$CONF" ]]  || fail "정본 부재: packaging/deb/purecvisor-lio.conf"
[[ -f "$BUILD" ]] || fail "packaging/deb/build-deb.sh 부재"

python3 - "$ROOT_DIR" "$CONF" "$BUILD" <<'PY'
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
conf_path, build_path = sys.argv[2:4]

EXPECTED_MODULES = [
    "target_core_mod", "iscsi_target_mod", "target_core_iblock", "nf_conntrack_bridge"
]
DEST = "/etc/modules-load.d/purecvisor-lio.conf"
HEREDOC_OPEN = "sudo tee /etc/modules-load.d/purecvisor-lio.conf >/dev/null <<'EOF'"

                                                 
                                             
DOCS_WITH_TEE = [
    "docs/GUIDE.md",
    "ui/guide-content.md",
]


def fail(message):
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


conf_text = open(conf_path, encoding="utf-8").read()
build_text = open(build_path, encoding="utf-8").read()

                                                            
modules = []
for lineno, raw in enumerate(conf_text.split("\n"), start=1):
    line = raw.strip()
    if not line or line.startswith("#"):
        continue
                                                    
                                              
    if "#" in raw or len(line.split()) != 1:
        fail(f"P13 위반 — purecvisor-lio.conf:{lineno} 모듈명 행에 주석/추가 토큰: {raw!r}")
    modules.append(line)

if modules != EXPECTED_MODULES:
    fail(f"모듈 목록 불일치 — 기대 {EXPECTED_MODULES}, 실제 {modules}")

                                                                
install_re = re.compile(
    r"install\s+-m644\s+packaging/deb/purecvisor-lio\.conf\s+\\?\s*"
    r'"\$STAGE'+re.escape(DEST)+r'"'
)
if not install_re.search(build_text):
    fail("build-deb.sh 에 purecvisor-lio.conf 설치 규칙 없음 "
         f"(기대: install -m644 packaging/deb/purecvisor-lio.conf \"$STAGE{DEST}\")")

if '"$STAGE/etc/modules-load.d"' not in build_text:
    fail("build-deb.sh 가 $STAGE/etc/modules-load.d 를 만들지 않는다")

                                                          
conffiles = re.search(r'cat > "\$STAGE/DEBIAN/conffiles" <<\'CF\'\n(.*?)\nCF\n',
                      build_text, re.S)
if not conffiles:
    fail("build-deb.sh 의 DEBIAN/conffiles 블록을 찾지 못했다")
if DEST not in conffiles.group(1).split("\n"):
    fail(f"conffiles 에 {DEST} 미등재")

                                                            
                                              
                                               
                                                  
for rel in DOCS_WITH_TEE:
    path = root / rel
    if not path.is_file():
        fail(f"문서 부재: {rel}")
    text = path.read_text(encoding="utf-8")
    start = text.find(HEREDOC_OPEN)
    if start < 0:
        fail(f"{rel} 에서 수작업 heredoc 을 찾지 못했다: {HEREDOC_OPEN!r}")
    body_start = start + len(HEREDOC_OPEN) + 1                     
    end = text.find("\nEOF\n", body_start)
    if end < 0:
        fail(f"{rel} 의 heredoc 이 닫히지 않았다(EOF 없음)")
    doc_body = text[body_start:end + 1]                                

    doc_modules = [
        line.strip()
        for line in doc_body.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if doc_modules != modules:
        fail(f"{rel} 의 tee 모듈 목록이 정본과 갈렸다 "
             f"(문서={doc_modules!r} / 정본={modules!r})")

print(f"PASS: LIO modules-load packaging contract ({len(DOCS_WITH_TEE)} docs in sync)")
PY
