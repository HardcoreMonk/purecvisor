#!/usr/bin/env bash
                                                                                     
                                                                           
                                                            
set -euo pipefail

                     
 
                                                 
                                                 
                                                                     
                                                   
                                                    
 
                  
                                            
                                               
                                                       
              
                                                              
                          
                                          

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
INDEX="$ROOT/ui/index.html"
STYLE="$ROOT/ui/style.css"
API_JS="$ROOT/ui/modules/api.js"
APP_JS="$ROOT/ui/app.js"

fail() {
  printf 'FAIL: %s\n' "$1" >&2
  exit 1
}

require_literal() {
  local needle="$1"
  local file="$2"
  local label="$3"
  if ! rg -Fq "$needle" "$file"; then
    fail "$label"
  fi
}

reject_literal() {
  local needle="$1"
  local file="$2"
  local label="$3"
  if rg -Fq "$needle" "$file"; then
    fail "$label"
  fi
}

                                                  
require_literal 'class="login-pitch"' "$INDEX" "login page must keep the pitch panel the single-surface gate slices"
require_literal "단일 노드 운영" "$INDEX" "login pitch must describe single-node operations"
require_literal "웹 콘솔 · REST API" "$INDEX" "login pitch must name the concrete operating surfaces"
require_literal 'id="lh-dot"' "$INDEX" "node status panel must expose the overall health dot"
require_literal 'id="lh-state"' "$INDEX" "node status panel must expose the overall health label"
require_literal 'id="lh-node"' "$INDEX" "node status panel must expose the node name slot"
require_literal 'id="lh-version"' "$INDEX" "node status panel must expose the version slot"
require_literal 'id="lh-uptime"' "$INDEX" "node status panel must expose the uptime slot"
require_literal 'id="lh-kvm"' "$INDEX" "node status panel must expose the KVM check slot"
require_literal 'id="lh-disk"' "$INDEX" "node status panel must expose the disk check slot"
require_literal "노드 응답 대기" "$INDEX" "node status panel must ship an honest idle state before /health answers"
require_literal 'aria-live="polite"' "$INDEX" "node status panel updates must be announced"

                                   
reject_literal "login-console-preview" "$INDEX" "login page must not restore the hardcoded console preview"
reject_literal "Node pcv-edge-1" "$INDEX" "login page must not restore the fake node context"
reject_literal "12 running" "$INDEX" "login page must not restore fabricated workload counts"
reject_literal "data-edition-" "$INDEX" "login page must not restore the edition paint script attributes"

                                                             
reject_literal 'placeholder="admin"' "$INDEX" "login form must not suggest a default admin username"
require_literal 'id="login-user" class="login-input" placeholder="계정 이름" autocomplete="username" autocapitalize="none" autocorrect="off" spellcheck="false"' "$INDEX" "username input must disable mobile autocorrect/autocapitalize"
require_literal 'id="login-pass" type="password" class="login-input" placeholder="••••••••" autocomplete="current-password" autocapitalize="none" autocorrect="off" spellcheck="false"' "$INDEX" "password input must disable mobile autocorrect/autocapitalize"

                                                              
require_literal "로그인 실패와 세션 변경은 audit 기록에 남습니다." "$INDEX" "login form must state audit behavior"

                                                          
require_literal "document.getElementById('login-user')?.value.trim()" "$API_JS" "login code must trim username copy/paste whitespace"
require_literal "pcvSetLoginVisible" "$API_JS" "auth module must toggle login-active state"
require_literal "login-tls-compact" "$APP_JS" "TLS status must use compact mobile-friendly markup"
require_literal "_loginHealthTick" "$APP_JS" "login page must poll the unauthenticated health endpoint"
require_literal ".login-input" "$STYLE" "CSS must keep the input selector shared with the register/change-password modals"
require_literal ".login-tls-compact" "$STYLE" "CSS must style the compact TLS badge app.js builds"
require_literal ".login-node" "$STYLE" "CSS must style the node status panel"

printf 'PASS: login homepage surface contract found\n'
