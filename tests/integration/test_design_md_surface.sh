#!/usr/bin/env bash
                                                                                           
                                                  
                                                               
set -euo pipefail

                                                
                                                                               
                                                                 

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

fail() {
  printf 'FAIL: %s\n' "$1" >&2
  exit 1
}

require_file() {
  local file="$1"
  [[ -f "$ROOT/$file" ]] || fail "$file must exist"
}

require_literal() {
  local needle="$1"
  local file="$2"
  local message="$3"
  if ! rg -Fq -- "$needle" "$ROOT/$file"; then
    printf 'FAIL: %s\n' "$message" >&2
    printf '  missing: %s in %s\n' "$needle" "$file" >&2
    exit 1
  fi
}

require_regex() {
  local pattern="$1"
  local file="$2"
  local message="$3"
  if ! rg -UPq -- "$pattern" "$ROOT/$file"; then
    printf 'FAIL: %s\n' "$message" >&2
    printf '  pattern: %s in %s\n' "$pattern" "$file" >&2
    exit 1
  fi
}

reject_literal() {
  local needle="$1"
  local file="$2"
  local message="$3"
  if rg -Fq -- "$needle" "$ROOT/$file"; then
    printf 'FAIL: %s\n' "$message" >&2
    printf '  unexpected: %s in %s\n' "$needle" "$file" >&2
    exit 1
  fi
}

require_file "DESIGN.md"
require_file "scripts/check_design_md.py"
require_file "ui/samples/design-system-preview.html"
require_file "ui/docs.html"

require_literal "DESIGN.md" "AGENTS.md" "AGENTS.md must require DESIGN.md before UI work"
require_literal "scripts/check_design_md.py" "AGENTS.md" "AGENTS.md must name the DESIGN.md checker"
require_literal "DESIGN.md" "docs/GUIDE.md" "GUIDE.md must link to the separated visual contract"
require_literal "ui/docs.html" "docs/GUIDE.md" "GUIDE.md must publish the documentation portal asset"
require_literal "ui/samples/design-system-preview.html" "docs/GUIDE.md" "GUIDE.md must link to the design preview"
require_literal "DESIGN.md" "ui/guide-content.md" "UI guide content must mention the separated visual contract"
require_literal "ui/docs.html" "ui/guide-content.md" "UI guide content must publish the documentation portal asset"
require_literal "ui/samples/design-system-preview.html" "ui/guide-content.md" "UI guide content must link to the preview"
require_literal "bridge/dedicated" "docs/GUIDE.md" "GUIDE.md must document the dedicated physical uplink mode"
require_literal "bridge/shared" "docs/GUIDE.md" "GUIDE.md must document the shared physical uplink mode"
require_literal "--confirm-shared-uplink" "ui/guide-content.md" "UI guide content must document the shared uplink confirmation"
require_literal "/var/lib/purecvisor/networks/<bridge>.json" "ui/guide-content.md" "UI guide content must name persistent physical bridge desired state"
require_literal "02:16:3e:44:55:66" "ui/guide-content.md" "UI guide content must identify the temporary shared guest MAC"
require_literal "sudo pcvctl --format=json network list" "ui/guide-content.md" "UI guide content must use the real CLI inventory route"
require_literal "ui/samples" "scripts/deploy.sh" "deploy script must publish linked UI samples"
require_literal "pcv_ui_samples" "scripts/deploy.sh" "deploy script must use a UI samples deploy staging directory"

require_literal "Color Tokens & Roles" "DESIGN.md" "DESIGN.md must define color token roles"
require_literal "Typography Rules" "DESIGN.md" "DESIGN.md must define typography rules"
require_literal "Component States" "DESIGN.md" "DESIGN.md must define component states"
require_literal "Dashboard Density" "DESIGN.md" "DESIGN.md must define dashboard density"
require_literal "Table Rules" "DESIGN.md" "DESIGN.md must define table rules"
require_literal "Card Rules" "DESIGN.md" "DESIGN.md must define card rules"
require_literal "Button Rules" "DESIGN.md" "DESIGN.md must define button rules"
require_literal "Modal Rules" "DESIGN.md" "DESIGN.md must define modal rules"
require_literal "Reference Pattern Borrowing" "DESIGN.md" "DESIGN.md must define selective reference borrowing"
require_literal "Documentation Portal" "DESIGN.md" "DESIGN.md must define the documentation portal contract"
require_literal "ui/docs.html" "DESIGN.md" "DESIGN.md must include the documentation portal in scope"
require_literal "guide-content.md" "ui/docs.html" "documentation portal must use the guide markdown as source"
require_literal "vendor/coolicons/coolicons.svg#ci-" "ui/docs.html" "documentation portal must use local Coolicons"
require_literal "replaceChildren" "ui/docs.html" "documentation search must render results with DOM nodes"
require_literal "site-header-inner" "ui/docs.html" "documentation portal must expose the approved glass header"
require_literal "backdrop-filter: blur(18px)" "ui/docs.html" "documentation glass header must retain the approved blur"
require_literal "reader-mobile-toc" "ui/docs.html" "documentation reader must expose the compact current-section strip"
require_literal "reader-code-toolbar" "ui/docs.html" "documentation code blocks must expose the approved toolbar frame"
require_literal "behavior: 'instant'" "ui/docs.html" "documentation deep links must align immediately after font loading"
require_literal "22개" "DESIGN.md" "DESIGN.md must require full numbered chapter visibility"
require_literal "chapter-card" "ui/docs.html" "documentation portal must expose individual chapter cards"
require_literal '$(UI_DIR)/docs.html' "Makefile" "documentation portal must participate in the service worker cache identity"
require_literal "docs.html guide.html guide-content.md" "scripts/deploy.sh" "deploy must publish the docs portal with its source and detail reader"
require_regex "restguide\\s*:\\s*['\\\"]\\/ui\\/docs\\.html#14-rest-api['\\\"]" "ui/modules/uxlib.js" "retired REST bookmark must replace into the canonical chapter"
require_regex "href\\s*:\\s*['\\\"]\\/ui\\/docs\\.html#14-rest-api['\\\"]" "ui/modules/accounts.js" "API management must link directly to the canonical REST chapter"
reject_literal "restguide" "ui/modules/nav.js" "retired REST route must not remain in the dispatcher"
reject_literal "restguide" "ui/modules/shell.js" "retired REST route must not remain in shell navigation"
reject_literal "restguide" "ui/app.js" "retired REST route must not remain in app navigation metadata"
reject_literal "restguide" "ui/i18n.js" "retired REST menu label must not remain in i18n"
reject_literal "renderRestGuide" "ui/modules/help.js" "retired REST renderer must be deleted"
reject_literal ".rest-docs" "ui/style.css" "retired REST reader CSS must be deleted"
require_literal "### 14.7 브라우저 푸시 (SP2b)" "ui/guide-content.md" "canonical REST chapter must include the browser push section"
require_literal "PushSubscription.toJSON()" "ui/guide-content.md" "canonical REST chapter must include the browser subscription contract"
require_literal "/api/v1/push/vapid/rotate" "ui/guide-content.md" "canonical REST chapter must include the VAPID rotation example"
require_literal "### 14.8 2.0 RPC 예제 (RPC 전용)" "ui/guide-content.md" "canonical REST chapter must include RPC-only calls"
require_literal "tenant_overlay.create" "ui/guide-content.md" "canonical REST chapter must include the tenant overlay RPC example"
require_literal "debug.trace.start" "ui/guide-content.md" "canonical REST chapter must include the trace RPC example"
reject_literal "renderServiceGuide" "ui/modules/help.js" "retired service guide renderer must remain deleted"
reject_literal "filterGuide" "ui/modules/help.js" "retired service guide filter must remain deleted"
reject_literal "| 도움말 | 서비스 가이드" "docs/GUIDE.md" "GUIDE.md must not advertise the retired in-app service guide"
reject_literal "| 도움말 | 서비스 가이드" "ui/guide-content.md" "UI guide content must not advertise the retired in-app service guide"
require_literal "Linear" "DESIGN.md" "DESIGN.md must record Linear-inspired density guidance"
require_literal "Sentry" "DESIGN.md" "DESIGN.md must record Sentry-inspired triage guidance"
require_literal "IBM Carbon" "DESIGN.md" "DESIGN.md must record IBM Carbon-inspired table/form guidance"
require_literal "Raycast" "DESIGN.md" "DESIGN.md must record Raycast-inspired command guidance"
require_literal "--font-sans" "DESIGN.md" "DESIGN.md must define the sans typography token"
require_literal "Coolicons" "DESIGN.md" "DESIGN.md must define the local icon baseline"
require_literal "--accent" "DESIGN.md" "DESIGN.md must reference the runtime accent token"
require_literal "supanova-mockup" "DESIGN.md" "DESIGN.md must document the approved mockup theme"
require_literal "'supanova-mockup'" "ui/modules/theme.js" "runtime theme allowlist must include the approved mockup theme"
require_literal '[data-theme="supanova"]' "ui/style.css" "default Supanova must keep its docs-aligned exact theme override"
require_literal "--bg: #ffffff" "ui/style.css" "default Supanova must use the documentation canvas"
require_literal "--accent: #12627a" "ui/style.css" "default Supanova must use the documentation accent"
require_literal "SUPANOVA DOCS" "ui/modules/theme.js" "theme preview must identify the docs-aligned default"
require_literal '<meta name="theme-color" content="#ffffff">' "ui/index.html" "browser chrome must match the light default canvas"
require_literal ".hc" "DESIGN.md" "DESIGN.md must name the card shell class"
require_literal ".btn" "DESIGN.md" "DESIGN.md must name the button class"
require_literal ".modal" "DESIGN.md" "DESIGN.md must name the modal class"
require_literal "라벨 전체" "DESIGN.md" "DESIGN.md must require unclipped progress labels"
require_literal "ui/samples/design-system-preview.html" "DESIGN.md" "DESIGN.md must link to its preview"
require_literal 'class="pb-t">2.0%' "ui/samples/design-system-preview.html" "preview must include a low progress label"
require_file "ui/samples/design-borrowing-mockup.html"
require_literal "PureCVisor Operations Triage Mockup" "ui/samples/design-borrowing-mockup.html" "borrowed-pattern mockup must exist"
require_literal "../vendor/pretendard/pretendard.css" "ui/samples/design-borrowing-mockup.html" "borrowed-pattern mockup must load self-hosted Pretendard"
require_literal "../vendor/coolicons/coolicons.svg#ci-" "ui/samples/design-borrowing-mockup.html" "borrowed-pattern mockup must use local Coolicons"
require_literal 'href="../vendor/pretendard/pretendard.css"' "ui/samples/design-system-preview.html" "preview must load self-hosted Pretendard"
require_literal "../vendor/coolicons/coolicons.svg#ci-" "ui/samples/design-system-preview.html" "preview must demonstrate local Coolicons"
require_literal "ui/vendor/coolicons/coolicons.svg" "docs/GUIDE.md" "GUIDE.md must mention the local Coolicons asset"
require_literal "ui/vendor/coolicons/coolicons.svg" "ui/guide-content.md" "UI guide content must mention the local Coolicons asset"
require_literal "height: 18px" "ui/style.css" "progress track must fit label inside its border"
require_literal "white-space: nowrap" "ui/style.css" "progress label must not wrap or clip horizontally"
require_literal "line-height: 16px" "ui/style.css" "progress label line-height must fit the track"
require_literal "font-family: var(--font-sans)" "ui/style.css" "body must use the sans typography token"
require_literal "letter-spacing: 0" "ui/style.css" "body typography must keep default tracking at zero"
require_literal "shellIcon(it.ico || 'ci-info', 'shell-ico')" "ui/modules/shell.js" "app shell navigation must render local Coolicons nodes"
require_literal "shellIcon('ci-search', 'shell-search-ico')" "ui/modules/shell.js" "global search must render the local Coolicons search icon"
require_regex \
  "\.dash2-qa:focus-within[[:space:]]*\{[[:space:]]*opacity:[[:space:]]*1" \
  "ui/style.css" \
  "dashboard quick actions must become visible for keyboard focus"
require_regex \
  "(?s)@media[[:space:]]*\(max-width:[[:space:]]*768px\)[[:space:]]*\{[^}]*\.dash2-qa[[:space:]]*\{[^}]*opacity:[[:space:]]*1" \
  "ui/style.css" \
  "tablet dashboard quick actions must remain visible without hover"
require_regex \
  "(?s)\.dash2-timeseg button,[[:space:]]*\.filterbar \.chip[[:space:]]*\{[^}]*min-width:[[:space:]]*40px[^}]*min-height:[[:space:]]*40px" \
  "ui/style.css" \
  "tablet dashboard controls must keep 40px touch targets"
require_regex \
  "(?s)\.shell-search,[[:space:]]*\.shell-iconbtn,[[:space:]]*\.mobile-menu-btn,[[:space:]]*\.shell-navitem[[:space:]]*\{[^}]*min-width:[[:space:]]*40px[^}]*min-height:[[:space:]]*40px" \
  "ui/style.css" \
  "tablet shell controls and navigation must keep 40px touch targets"

                                                            
                                                 
                                                 
require_literal "class: 'alert-webhook-grid'" "ui/modules/monitor.js" "alert webhook fields must use the reviewed label/input grid"
require_literal "'aria-pressed': notifFilter === f ? 'true' : 'false'" "ui/modules/nav.js" "notification filters must expose their selected state"
require_regex \
  "(?s)\.notif-filter-btn,[[:space:]]*\.notif-close[[:space:]]*\{[^}]*min-width:[[:space:]]*40px[^}]*min-height:[[:space:]]*40px" \
  "ui/style.css" \
  "notification filters and close action must keep 40px touch targets"
require_literal "icon: 'ci-house-01'" "ui/modules/mobile.js" "mobile navigation must use local Coolicons symbols"
require_literal "function _pageHead(title, subtitle)" "ui/modules/mobile.js" "each mobile view must provide a reviewed page heading"
require_literal "m-name m-copy" "ui/modules/mobile.js" "mobile alert and healing context must opt into multiline copy"
require_literal ".m-listcard .m-name.m-copy { overflow: visible; text-overflow: clip; white-space: normal; overflow-wrap: anywhere;" "ui/style.css" "mobile action context must wrap instead of clipping"
require_literal ".login-aside { order: -1; }" "ui/style.css" "narrow login layouts must present authentication before health detail"
require_literal "width: 40px; height: 40px" "ui/style.css" "the login password toggle must keep a 40px target"
require_literal "var _loginPageInFlight = false" "ui/modules/api.js" "login submit must retain its single-flight guard"
require_literal "'login.loading': '로그인 중…'" "ui/i18n.js" "login busy state must have Korean visible text"

                                                                        
                                                    
require_literal "aria-activedescendant" "DESIGN.md" "DESIGN.md must define command composite focus semantics"
require_literal "opts.ariaLabel" "ui/modules/modal-core.js" "modal core must accept an explicit accessible name"
require_literal "querySelector('h1, h2, h3, h4')" "ui/modules/modal-core.js" "modal core must name dialogs from card headings through h4"
require_literal "role: 'combobox'" "ui/modules/nav.js" "command input must expose combobox semantics"
require_literal "role: 'listbox'" "ui/modules/nav.js" "command results must expose listbox semantics"
require_literal "role: 'option'" "ui/modules/nav.js" "command rows must expose option semantics"
require_literal "class: 'iso-browser-file'" "ui/modules/vm-lifecycle.js" "ISO file rows must use the reviewed native button class"
require_regex \
  "(?s)@media[[:space:]]*\\(max-width:[[:space:]]*480px\\).*?\\.kbd-grid[[:space:]]*\\{[^}]*grid-template-columns:[[:space:]]*1fr" \
  "ui/style.css" \
  "mobile keyboard help must use one column"
require_literal ".iso-browser-actions .btn { min-height: 40px; }" "ui/style.css" "mobile ISO actions must keep 40px targets"
require_literal "native dialog backdrop" "DESIGN.md" "modal errors must remain visible inside the top-layer dialog"
require_literal "고정 상한" "DESIGN.md" "capacity notes must not invent backend limits"
require_literal "if (closeOnSuccess) throw failure;" "ui/modules/vpc.js" "modal VPC mutations must reach the inline role=alert"
require_literal "class: 'vpc-form-group'" "ui/modules/vpc.js" "compound VPC create fields must keep reviewed semantic groups"
require_literal "VPC + Subnet 생성" "DESIGN.md" "compound VPC create action must name the whole mutation"

lanes=(crit warn idle)
labels=(CRIT WARN UNKNOWN)
for index in "${!lanes[@]}"; do
  lane="${lanes[$index]}"
  label="${labels[$index]}"
  require_regex \
    "^\\|[^|]*alert-row-$lane[^|]*\\|[^|]*--st-$lane[^|]*\\|" \
    "DESIGN.md" \
    "DESIGN.md must map alert-row-$lane to --st-$lane in one contract row"
  require_regex \
    "\\.alert-row-$lane[[:space:]]*\\{[^}]*border-inline-start:[[:space:]]*3px[[:space:]]+solid[[:space:]]+var\\(--st-$lane\\)" \
    "ui/style.css" \
    "style.css must connect alert-row-$lane to --st-$lane"
  require_regex \
    "(?s)<tr[^>]*class=\"[^\"]*alert-row-$lane[^\"]*\"[^>]*>(?:(?!</tr>).)*<span[^>]*class=\"[^\"]*pill-$lane[^\"]*\"[^>]*>[[:space:]]*$label[[:space:]]*</span>(?:(?!</tr>).)*</tr>" \
    "ui/samples/design-system-preview.html" \
    "preview alert-row-$lane must contain pill-$lane with $label"
done

python3 "$ROOT/scripts/check_design_md.py" --self-test
python3 "$ROOT/scripts/check_design_md.py"

printf 'DESIGN.md surface OK\n'
