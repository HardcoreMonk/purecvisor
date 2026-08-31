import { createHash } from "node:crypto";
import { readdir, readFile, stat } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import {
  guideChapters,
  guideEntryPath,
  guideGroups,
  guidePath,
  landingDocuments,
  readerDocuments,
  supplementalDocuments
} from "./guide-routes.mjs";
import {
  architectureLayerNodeIds,
  resolveArchitectureEdge
} from "../src/scripts/architecture-interactions.js";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const siteRoot = path.resolve(scriptDir, "..");
const distRoot = path.join(siteRoot, "dist");
const networkExampleAssets = [
  ["bridge-network.svg", "브릿지 네트워크 활용 예제 구성도"],
  ["managed-firewall.svg", "관리형 방화벽 활용 예제 구성도"],
  ["vlan-filtering.svg", "VLAN 필터링 활용 예제 구성도"],
  ["qos.svg", "QoS 활용 예제 구성도"],
  ["ovs-vxlan.svg", "OVS VXLAN 활용 예제 구성도"],
  ["ovn-sdn.svg", "OVN SDN 활용 예제 구성도"],
  ["security-group.svg", "보안 그룹 활용 예제 구성도"],
  ["dpdk.svg", "DPDK 활용 예제 구성도"],
  ["sriov.svg", "SR-IOV 활용 예제 구성도"],
  ["debugging.svg", "네트워크 디버깅 활용 예제 구성도"],
  ["prometheus-metrics.svg", "네트워크 Prometheus 메트릭 활용 예제 구성도"],
  ["suricata.svg", "Suricata IDS IPS 활용 예제 구성도"],
  ["local-vpc.svg", "Local VPC 활용 예제 구성도"]
];
const networkingExampleAssets = networkExampleAssets.filter(([file]) => file !== "suricata.svg");
const requiredFiles = [
  "index.html",
  "ko/index.html",
  "en/index.html",
  "docs.html",
  "favicon.svg",
  "assets/diagrams/purecvisor-single-full-architecture.svg",
  "assets/diagrams/purecvisor-single-direct-https-architecture.svg",
  "assets/diagrams/purecvisor-single-database-architecture.svg",
  "assets/diagrams/purecvisor-single-network-services.svg",
  ...networkExampleAssets.map(([file]) => `assets/diagrams/network-examples/${file}`),
  ...readerDocuments.map((document) => `${document.contentSlug}/index.html`)
];
const forbiddenText = [
  ["HardcoreMonk", "purecvisor-single"].join("/"),
  ["Private", "repository"].join(" "),
  ["192", "168", "3", "51"].join("."),
  ["192", "168", "3", "53"].join("."),
  "T2FA-F4(WebAuthn/step-up)",
  "멀티 제어면 참고 기록",
  "Multi-control-plane notes",
  "/ko/infrastructure/multi-control-plane-notes/",
  "클러스터 제어면, 라이브 마이그레이션, 페더레이션, 노드 드레인/리밸런싱 같은",
  "2026-08-04의 임시"
];

async function walk(directory) {
  const files = [];
  for (const entry of await readdir(directory, { withFileTypes: true })) {
    const full = path.join(directory, entry.name);
    if (entry.isDirectory()) files.push(...await walk(full));
    if (entry.isFile()) files.push(full);
  }
  return files;
}

for (const relative of requiredFiles) {
  const info = await stat(path.join(distRoot, relative));
  if (!info.isFile() || info.size === 0) throw new Error(`invalid artifact: ${relative}`);
}

const outputFiles = await walk(distRoot);
if (outputFiles.some((file) => file.endsWith(".map"))) throw new Error("source map found");
if (outputFiles.some((file) => path.relative(distRoot, file) === "guide.html")) {
  throw new Error("retired guide.html artifact found");
}

for (const file of outputFiles) {
  if (!/\.(?:css|html|js|json|md|svg|txt|xml)$/i.test(file)) continue;
  const source = await readFile(file, "utf8");
  if (/\.(?:css|js)$/i.test(file) && source.includes("sourceMappingURL")) {
    throw new Error(`source map reference found in ${path.relative(distRoot, file)}`);
  }
  for (const marker of forbiddenText) {
    if (source.includes(marker)) throw new Error(`${marker} found in ${path.relative(distRoot, file)}`);
  }
}

for (const file of outputFiles.filter((item) => item.endsWith(".html") && !item.endsWith("404.html"))) {
  const source = await readFile(file, "utf8");
  for (const match of source.matchAll(/<a\b[^>]*\bhref="(\/[^"]*)"/g)) {
    const href = match[1];
    const pathname = decodeURI(href.split("#")[0].split("?")[0] || "/");
    if (pathname.startsWith("/_astro/") || pathname.startsWith("/pagefind/")) continue;
    const target = pathname === "/"
      ? path.join(distRoot, "index.html")
      : pathname.endsWith("/")
        ? path.join(distRoot, pathname, "index.html")
        : path.join(distRoot, pathname);
    try {
      const info = await stat(target);
      if (!info.isFile()) throw new Error("not a file");
    } catch {
      throw new Error(`broken internal link: ${path.relative(distRoot, file)} -> ${href}`);
    }
  }
}

const index = await readFile(path.join(distRoot, "index.html"), "utf8");
const korean = await readFile(path.join(distRoot, "ko", "index.html"), "utf8");
const english = await readFile(path.join(distRoot, "en", "index.html"), "utf8");
const docs = await readFile(path.join(distRoot, "docs.html"), "utf8");
const overview = await readFile(
  path.join(distRoot, guideChapters[0].contentSlug, "index.html"),
  "utf8"
);
const networking = await readFile(
  path.join(distRoot, guideChapters[5].contentSlug, "index.html"),
  "utf8"
);
const securityChapter = guideChapters.find((chapter) => chapter.number === 10);
if (!securityChapter) throw new Error("security guide chapter missing");
const securityGuide = await readFile(
  path.join(distRoot, securityChapter.contentSlug, "index.html"),
  "utf8"
);
const guideSource = await readFile(path.join(siteRoot, "..", "docs", "GUIDE.md"), "utf8");
const databaseStructureSource = await readFile(
  path.join(siteRoot, "..", "docs", "DATABASE_STRUCTURE.md"),
  "utf8"
);
const invalidGuideBreakLines = guideSource
  .split(/\r?\n/)
  .map((line, index) => ({ index: index + 1, line }))
  .filter(({ line }) => line.trimEnd().endsWith("<br>") && !line.trimEnd().slice(0, -4).trimEnd().endsWith("."));
if (invalidGuideBreakLines.length) {
  throw new Error(`GUIDE sentence break must follow a period: ${invalidGuideBreakLines.map(({ index }) => index).join(", ")}`);
}
if ((databaseStructureSource.match(/^### 스키마$/gm) || []).length !== 9) {
  throw new Error("database document schema group count mismatch");
}
if ((databaseStructureSource.match(/^#### 테이블:/gm) || []).length !== 26) {
  throw new Error("database document table heading count mismatch");
}
if (/^### (?:테이블:|.*인덱스$|인덱스$)/m.test(databaseStructureSource)) {
  throw new Error("database table or index heading leaked into the reader TOC");
}
const landingStyles = await readFile(path.join(siteRoot, "src", "styles", "custom.css"), "utf8");
const headerComponent = await readFile(path.join(siteRoot, "src", "components", "Header.astro"), "utf8");
const pageTitleComponent = await readFile(path.join(siteRoot, "src", "components", "PageTitle.astro"), "utf8");
const prepareContent = await readFile(path.join(siteRoot, "scripts", "prepare-content.mjs"), "utf8");
const architectureInteractions = await readFile(
  path.join(siteRoot, "src", "scripts", "architecture-interactions.js"),
  "utf8"
);
const sentenceBreakPlugin = await readFile(
  path.join(siteRoot, "scripts", "rehype-sentence-breaks.mjs"),
  "utf8"
);
const directArchitectureSource = await readFile(
  path.join(siteRoot, "..", "docs", "architecture", "purecvisor-single-direct-https-architecture.mmd"),
  "utf8"
);
const fullArchitectureSource = await readFile(
  path.join(siteRoot, "..", "docs", "architecture", "purecvisor-single-full-architecture.mmd"),
  "utf8"
);
for (const marker of [
  '["Networking", guidePath(6)]',
  '["네트워크", guidePath(6)]'
]) {
  if (!headerComponent.includes(marker)) {
    throw new Error(`header storage/networking direct link missing: ${marker}`);
  }
}
for (const marker of [
  "const maxHeadingLevel = chapter.number === 6 ? 2 : 3;",
  "maxHeadingLevel: ${maxHeadingLevel}"
]) {
  if (!prepareContent.includes(marker)) {
    throw new Error(`network TOC hierarchy missing: ${marker}`);
  }
}
for (const staleMarker of ["Storage & networking", "스토리지 · 네트워크"]) {
  if (headerComponent.includes(staleMarker)) {
    throw new Error(`combined header destination remained: ${staleMarker}`);
  }
}
for (const contract of [
  "--pcv-prose-width: 56rem;",
  "--pcv-technical-width: 72rem;",
  "--pcv-architecture-width: 88rem;",
  "--pcv-reader-rail-inset-max: 4rem;",
  "--sl-content-width: var(--pcv-architecture-width);",
  "--sl-sidebar-width: 16rem;"
]) {
  if (!landingStyles.includes(contract)) throw new Error(`reader width token missing: ${contract}`);
}
if (landingStyles.includes("--sl-content-width: 50rem;")) {
  throw new Error("reader outer canvas is still fixed to prose width");
}
for (const [selector, declarations] of [
  [".sl-markdown-content:not(:has(.pcv-landing))", ["--pcv-reader-rail-inset: clamp("]],
  [".sl-markdown-content:not(:has(.pcv-landing)) > *", ["width: 100%;", "max-width: var(--pcv-prose-width);", "margin-inline-start: var(--pcv-reader-rail-inset);", "margin-inline-end: auto;"]],
  [".sl-markdown-content:not(:has(.pcv-landing)) > .sl-heading-wrapper.level-h2 > h2", ["font-size: clamp(1.75rem, 2.4vw, 2rem);", "line-height: 1.22;", "word-break: keep-all;"]],
  [".sl-markdown-content:not(:has(.pcv-landing)) > .sl-heading-wrapper.level-h3 > h3", ["font-size: clamp(1.35rem, 2vw, 1.5rem);", "line-height: 1.3;", "word-break: keep-all;"]],
  [".sl-markdown-content:not(:has(.pcv-landing)) > :is(table, .expressive-code, pre, .pcv-technical-wide)", ["width: fit-content;", "min-width: min(100%, var(--pcv-prose-width));", "max-width: min(100%, var(--pcv-technical-width));"]],
  [".sl-markdown-content:not(:has(.pcv-landing)) > :is(figure, .pcv-architecture-wide)", ["width: 100%;", "max-width: var(--pcv-architecture-width);", "margin-inline: auto;"]],
  [".sl-markdown-content:not(:has(.pcv-landing)) :is(p, li, figcaption) > br", ["display: block;", "margin-block-end: 0.3rem;", "content: \"\";"]],
  ["main:not(:has(.pcv-landing)) > .content-panel > .sl-container > :is(h1, footer)", ["width: 100%;", "max-width: var(--pcv-prose-width);", "margin-inline-start: clamp(", "margin-inline-end: auto;"]]
]) {
  const start = landingStyles.indexOf(`${selector} {`);
  const end = start < 0 ? -1 : landingStyles.indexOf("}", start);
  const block = end < 0 ? "" : landingStyles.slice(start, end);
  for (const declaration of declarations) {
    if (!block.includes(declaration)) {
      throw new Error(`reader width contract missing: ${selector} ${declaration}`);
    }
  }
}
if (landingStyles.includes("main:has(.pcv-network-page-kicker) .sl-markdown-content :is(p, li, figcaption) > br")) {
  throw new Error("network reader must not override period-based sentence breaks");
}
for (const [selector, declarations] of [
  [".pcv-network-example-diagram", ["max-width: var(--pcv-technical-width) !important;", "border: 1px solid var(--pcv-map-line);", "border-radius: 0.5rem;"]],
  [".pcv-network-example-canvas", ["display: block;", "overflow-x: auto;", "overscroll-behavior-inline: contain;", "scrollbar-gutter: stable;"]],
  [".pcv-network-example-canvas img", ["width: max(100%, 54rem);", "min-width: 54rem;", "height: auto;"]],
  [".pcv-network-example-diagram figcaption", ["border-top: 1px solid var(--pcv-map-line);", "font-size: 0.75rem;", "line-height: 1.5;"]]
]) {
  const start = landingStyles.indexOf(`${selector} {`);
  const end = start < 0 ? -1 : landingStyles.indexOf("}", start);
  const block = end < 0 ? "" : landingStyles.slice(start, end);
  for (const declaration of declarations) {
    if (!block.includes(declaration)) {
      throw new Error(`network example diagram style missing: ${selector} ${declaration}`);
    }
  }
}
for (const marker of [
  'Astro.url.pathname === "/ko/infrastructure/networking/"',
  'class="pcv-network-page-kicker"',
  'class="pcv-network-page-lead"'
]) {
  if (!pageTitleComponent.includes(marker)) {
    throw new Error(`network page title hierarchy missing: ${marker}`);
  }
}
for (const [selector, declarations] of [
  [".pcv-network-page-kicker", ["font-family: var(--sl-font-mono);", "font-size: 0.75rem;", "letter-spacing: 0.08em;"]],
  [".pcv-network-page-lead", ["max-width: var(--pcv-prose-width);", "font-size: 1.125rem;", "line-height: 1.72;", "word-break: keep-all;"]],
  ["main:has(.pcv-network-page-kicker) .sl-markdown-content > .sl-heading-wrapper.level-h2", ["border-top: 2px solid var(--sl-color-gray-1);", "padding-top: 1.4rem;"]],
  ["main:has(.pcv-network-page-kicker) .sl-markdown-content > .sl-heading-wrapper.level-h3", ["border-inline-start: 3px solid var(--sl-color-accent);", "padding: 0.15rem 0 0.15rem 1rem;"]],
  ["main:has(.pcv-network-page-kicker) .sl-markdown-content table :is(th, td)", ["word-break: keep-all;"]],
  ["main:has(.pcv-network-page-kicker) .sl-markdown-content table :is(th, td):first-child", ["min-width: 7.5rem;"]]
]) {
  const start = landingStyles.indexOf(`${selector} {`);
  const end = start < 0 ? -1 : landingStyles.indexOf("}", start);
  const block = end < 0 ? "" : landingStyles.slice(start, end);
  for (const declaration of declarations) {
    if (!block.includes(declaration)) {
      throw new Error(`network reader hierarchy missing: ${selector} ${declaration}`);
    }
  }
}
if (landingStyles.includes("main:has(.pcv-network-page-kicker) .sl-markdown-content {\n  --pcv-prose-width:")) {
  throw new Error("network reader must use the common prose width");
}
for (const marker of [
  'new Set(["p", "li", "figcaption"])',
  'breakAtEnd ? /\\.(?=\\s+\\S|\\s*$)/g : /\\.(?=\\s+\\S)/g',
  "hasFollowingContent(node.children, index)",
  "if (/[0-9]/.test(previousCharacter)) continue;",
  'tagName: "br"'
]) {
  if (!sentenceBreakPlugin.includes(marker)) {
    throw new Error(`period-based sentence break contract missing: ${marker}`);
  }
}
for (const [selector, declarations] of [
  [".pcv-database-summary", ["grid-template-columns: repeat(4, minmax(0, 1fr));", "border: 1px solid var(--pcv-line);", "border-radius: 0.5rem;", "background: var(--pcv-line);"]],
  [".pcv-database-summary-item", ["min-height: 6.25rem;", "background: var(--pcv-canvas);"]],
  [".pcv-database-architecture", ["border-radius: 0.75rem;"]],
  [".pcv-database-architecture-canvas", ["overflow-x: auto;", "overscroll-behavior-inline: contain;", "scrollbar-gutter: stable;"]],
  [".pcv-database-architecture-image", ["width: max(100%, 84rem);", "min-width: 84rem;", "max-width: none;"]]
]) {
  const start = landingStyles.indexOf(`${selector} {`);
  const end = start < 0 ? -1 : landingStyles.indexOf("}", start);
  const block = end < 0 ? "" : landingStyles.slice(start, end);
  for (const declaration of declarations) {
    if (!block.includes(declaration)) {
      throw new Error(`database architecture style missing: ${selector} ${declaration}`);
    }
  }
}
for (const marker of [
  ".pcv-database-summary {\n    grid-template-columns: repeat(2, minmax(0, 1fr));",
  ".pcv-database-architecture-image {\n  width: max(100%, 84rem);\n  min-width: 84rem;\n  max-width: none;",
  "@media (max-width: 22rem) {\n  .pcv-database-summary {\n    grid-template-columns: minmax(0, 1fr);"
]) {
  if (!landingStyles.includes(marker)) {
    throw new Error(`database architecture responsive style missing: ${marker}`);
  }
}
const releaseSummaryLineBreakContract = `현재 공개 제품 버전은 <code dir="auto">2.0.0</code>입니다.<br>
단일 노드 배포 뒤 <code dir="auto">purecvisorsd</code>는 항상 active여야 하고, NGINX는 선택형 외부 TLS 종료 모드에서만 active 조건입니다.<br>
선택한 모드의 <code dir="auto">/api/v1/health</code>, <code dir="auto">/api/v1/version</code>과 BPF 상태 검사가 통과해야 합니다.`;
if (!overview.includes(releaseSummaryLineBreakContract)) {
  throw new Error("release summary three-line contract missing");
}
const productSummaryLineBreakContract = `PureCVisor Single Edge는 C23 기반 KVM 하이퍼바이저 오케스트레이터입니다.<br>
단일 프로세스 데몬 <code dir="auto">purecvisorsd</code>가 fork 없이 GMainLoop 이벤트 루프로 동작하며, VM, 컨테이너, 스토리지, 네트워크를 통합 관리합니다.`;
if (!overview.includes(productSummaryLineBreakContract)) {
  throw new Error("product summary sentence-line contract missing");
}
const architectureSummaryLineBreakContract = `<code dir="auto">purecvisorsd</code>는 API transport, dispatcher, 도메인 핸들러와 서비스 모듈을 한 프로세스에 두고 <code dir="auto">GMainLoop</code>가 전체 수명주기를 소유합니다.<br>
짧은 작업은 이벤트 루프에서 응답을 끝내고, 긴 작업만 제한된 <code dir="auto">GTask</code> 워커 풀로 보냅니다.<br>
아래 탭에서 실제 TLS 배포 모드를 선택하면 클라이언트와 부팅 입력부터 로컬 영속 상태와 Linux/KVM 호스트까지 이어지는 Single Edge 전체 구조를 해당 진입 경계로 확인할 수 있습니다.<br>
기본 선택은 NGINX가 없는 <code dir="auto">purecvisorsd</code> 직접 HTTPS 모드입니다.<br>
마우스를 사용하는 환경에서는 SVG의 서비스 레이어 또는 컴포넌트에 포인터를 올려 직접 연결된 화살표의 흐름을 강조할 수 있습니다.`;
if (!overview.includes(architectureSummaryLineBreakContract)) {
  throw new Error("architecture summary sentence-line contract missing");
}
for (const marker of [
  "기본 모드입니다.<br>별도 NGINX 프로세스가 없습니다.",
  "선택형 모드입니다.<br>ADR-0029의 host-loopback 신뢰 경계",
  "첫 커밋으로 올립니다.<br> 기존 개발 저장소의 <code dir=\"auto\">.git</code> 이력",
  "가져가지 않습니다.<br> 공개 URL은 생성 후"
]) {
  if (!overview.includes(marker)) throw new Error(`overview automatic sentence break missing: ${marker}`);
}
for (const marker of [
  'class="pcv-overview-architecture-tabs pcv-architecture-wide"',
  "data-pcv-architecture-tabs",
  'class="pcv-architecture-tablist" role="tablist"',
  'id="pcv-architecture-tab-direct" type="button" role="tab" aria-selected="true" aria-controls="pcv-architecture-panel-direct" tabindex="0"',
  'id="pcv-architecture-tab-nginx" type="button" role="tab" aria-selected="false" aria-controls="pcv-architecture-panel-nginx" tabindex="-1"',
  'id="pcv-architecture-panel-direct" role="tabpanel" aria-labelledby="pcv-architecture-tab-direct"',
  'id="pcv-architecture-panel-nginx" role="tabpanel" aria-labelledby="pcv-architecture-tab-nginx"',
  'data-pcv-architecture-panel="" hidden',
  'id="pcv-overview-architecture-direct-title"',
  'id="pcv-overview-architecture-nginx-title"',
  'class="pcv-overview-architecture-image pcv-architecture-source-image"',
  'data-pcv-architecture-interactive="direct"',
  'data-pcv-architecture-interactive="nginx"',
  'href="/assets/diagrams/purecvisor-single-direct-https-architecture.svg"',
  'href="/assets/diagrams/purecvisor-single-full-architecture.svg"',
  "서비스 레이어 또는 컴포넌트에 포인터를",
  "직접 연결된 화살표의 흐름을 강조할 수 있습니다.",
  "기본 · NGINX 없음",
  "선택형 · host-loopback",
  "1.2.1 런타임·접근 경계",
  "1.2.2 요청·권한·완료 흐름",
  "1.2.3 서비스 도메인",
  "1.2.4 영속 상태와 호스트 통합",
  "1.2.5 Single Edge 경계와 상세 문서",
  "accepted 응답은 실제 성공을 뜻하지 않습니다.",
  "Monitoring 경로는 <code dir=\"auto\">handler_monitor</code>, telemetry, process monitor와 eBPF telemetry가 제공하는",
  "공개 소스에는 systemd D-Bus availability writer나 별도 Monitoring SQLite DB가 없습니다.",
  "상태 registry를 사용하는 경로만 <code dir=\"auto\">pcv_jobs.db</code> 행을 생성하며",
  "로컬 SQLite WAL 데이터베이스 9개",
  "Operations DB 2개",
  "vm_state.db",
  "pcv_audit.db",
  "pcv_jobs.db",
  "rbac.db",
  "pcv_security.db",
  "security_groups.db",
  "vpc.db",
  "cloud_jobs.db",
  "pcv_webpush.db",
  "audit-only 경계입니다.",
  'id="121-검증-문서-맵"',
  'id="122-설계-결정-빠른-보기"'
]) {
  if (!overview.includes(marker)) throw new Error(`overview architecture contract missing: ${marker}`);
}
for (const marker of [
  "auth_manager",
  "src/modules/dispatcher/ + src/modules/network/",
  "+-------+-------+--------------------+",
  "장시간 작업은 fire-and-forget 패턴으로 즉시 응답 후 GTask 비동기 실행",
  "Monitoring Source request handler는 immutable cache만 읽습니다.",
  "pcv_monitoring.db",
  "로컬 SQLite 데이터베이스 10개",
  "Operations DB 3개"
]) {
  if (overview.includes(marker)) throw new Error(`stale overview architecture found: ${marker}`);
}
if ((overview.match(/class="pcv-overview-architecture-image pcv-architecture-source-image"/g) || []).length !== 2) {
  throw new Error("overview architecture image count mismatch");
}
if ((overview.match(/data-pcv-architecture-interactive=/g) || []).length !== 2) {
  throw new Error("overview interactive architecture count mismatch");
}
if ((overview.match(/role="tab"/g) || []).length !== 2 || (overview.match(/role="tabpanel"/g) || []).length !== 2) {
  throw new Error("overview architecture tab count mismatch");
}

function validateInteractiveArchitectureTopology(name, source) {
  const nodeIds = new Set(
    [...source.matchAll(/<g class="node[^"]*" id="my-svg-flowchart-([^"]+)-[0-9]+"/g)]
      .map((match) => match[1])
  );
  const edgeIds = [...source.matchAll(/<path\b[^>]*\bdata-edge="true"[^>]*\bdata-id="(L_[^"]+)"/g)]
    .map((match) => match[1]);
  const layerIds = [...source.matchAll(/<g class="cluster" id="my-svg-([^"]+)"/g)]
    .map((match) => match[1]);
  if (nodeIds.size === 0 || edgeIds.length === 0 || layerIds.length === 0) {
    throw new Error(`${name} interactive topology is empty`);
  }
  for (const edgeId of edgeIds) {
    if (!resolveArchitectureEdge(edgeId, nodeIds)) {
      throw new Error(`${name} interactive edge cannot be resolved: ${edgeId}`);
    }
  }
  for (const layerId of layerIds) {
    const members = architectureLayerNodeIds[layerId];
    if (!members || !members.some((nodeId) => nodeIds.has(nodeId))) {
      throw new Error(`${name} interactive layer cannot be resolved: ${layerId}`);
    }
  }
}

const architectureAsset = "/assets/diagrams/purecvisor-single-full-architecture.svg";
const architectureSvg = await readFile(path.join(distRoot, architectureAsset));
const architectureSvgText = architectureSvg.toString("utf8");
validateInteractiveArchitectureTopology("NGINX architecture", architectureSvgText);
const architectureSvgHash = createHash("sha256").update(architectureSvg).digest("hex");
if (architectureSvgHash !== "8929be785dce35aa307b1dd299f3dc939f192b95c1f3f339ff66b62bdd31b293") {
  throw new Error(`architecture source SVG checksum mismatch: ${architectureSvgHash}`);
}
const architectureStructure = architectureSvgText.replace(/<style>[\s\S]*?<\/style>/, "<style></style>");
const architectureStructureHash = createHash("sha256").update(architectureStructure).digest("hex");
if (architectureStructureHash !== "b1a5596b79c43e219cd27216e0606288a47bcecc81fee995b11b518445e72bc6") {
  throw new Error(`architecture SVG structure/content mismatch: ${architectureStructureHash}`);
}
for (const marker of [
  'width="100%"',
  'viewBox="4 4 1699.064208984375 2488"',
  'role="graphics-document document"',
  'aria-roledescription="flowchart-elk"',
  'my-svg-flowchart-monitorDomain-22',
  'my-svg-flowchart-coreDb-24',
  'my-svg-flowchart-operationsDb-25',
  'my-svg-flowchart-securityDomain-21',
  'my-svg-flowchart-accelPlatform-31',
  'vm_state.db',
  'pcv_audit.db',
  'pcv_jobs.db',
  'rbac.db',
  'pcv_security.db',
  'security_groups.db',
  'vpc.db',
  'cloud_jobs.db',
  'pcv_webpush.db',
  'DPDK'
]) {
  if (!architectureSvgText.includes(marker)) throw new Error(`architecture source SVG contract missing: ${marker}`);
}
for (const marker of [/<script\b/i, /<foreignObject\b/i, /\bon\w+\s*=/i, /\b(?:xlink:)?href\s*=/i]) {
  if (marker.test(architectureSvgText)) throw new Error(`unsafe architecture source SVG marker: ${marker}`);
}
for (const marker of ["pcv_monitoring.db", "availability writer", "systemd D-Bus collection"]) {
  if (architectureSvgText.includes(marker)) throw new Error(`stale architecture source SVG marker: ${marker}`);
}
for (const marker of [
  ".pcv-semantic-layer-colors-start{}",
  ".clientNode rect",
  "fill:#e7f3ff!important;stroke:#3a78b8!important;color:#0d3454!important;",
  "#my-svg-flowchart-configStore-6 rect",
  "fill:#ffebd8!important;stroke:#c45a0a!important;color:#5b2c08!important;",
  ".ingressNode rect",
  "fill:#f3edff!important;stroke:#7650a8!important;color:#2d1b46!important;",
  ".runtimeNode rect",
  "fill:#e6f7ed!important;stroke:#27845d!important;color:#123b2a!important;",
  ".domainNode rect",
  "fill:#e5f7f6!important;stroke:#287f7a!important;color:#123b39!important;",
  ".dataNode rect",
  "fill:#fff4e8!important;stroke:#b87543!important;color:#4d2d17!important;",
  ".hostNode rect",
  "fill:#f0f2f5!important;stroke:#667085!important;color:#1d2939!important;",
  "#my-svg #my-svg-persistence>rect{fill:#fff9f3!important;stroke:#d49a72!important;}",
  ".pcv-semantic-layer-colors-end{}"
]) {
  if (!architectureSvgText.includes(marker)) throw new Error(`architecture semantic color contract missing: ${marker}`);
}

const directArchitectureAsset = "/assets/diagrams/purecvisor-single-direct-https-architecture.svg";
const directArchitectureSvg = await readFile(path.join(distRoot, directArchitectureAsset));
const directArchitectureSvgText = directArchitectureSvg.toString("utf8");
validateInteractiveArchitectureTopology("direct HTTPS architecture", directArchitectureSvgText);
const directArchitectureSvgHash = createHash("sha256").update(directArchitectureSvg).digest("hex");
if (directArchitectureSvgHash !== "093aadbafec9100e2ed8ec82190d0d0324af66697ef13191f8df910830bce2d3") {
  throw new Error(`direct HTTPS architecture SVG checksum mismatch: ${directArchitectureSvgHash}`);
}
const directArchitectureStructure = directArchitectureSvgText.replace(/<style>[\s\S]*?<\/style>/, "<style></style>");
const directArchitectureStructureHash = createHash("sha256").update(directArchitectureStructure).digest("hex");
if (directArchitectureStructureHash !== "6b2918ca11c217aaf8ac8cec81b78d01ef4b70b50370cfb2c2a39286f10a963c") {
  throw new Error(`direct HTTPS architecture SVG structure/content mismatch: ${directArchitectureStructureHash}`);
}
for (const marker of [
  'width="100%"',
  'viewBox="4 4 1817.8671875 2313.699951171875"',
  'role="graphics-document document"',
  'aria-roledescription="flowchart-elk"',
  'id="my-svg-clients"',
  'id="my-svg-bootInputs"',
  'id="my-svg-daemon"',
  'id="my-svg-transports"',
  'id="my-svg-core"',
  'id="my-svg-domains"',
  'id="my-svg-persistence"',
  'id="my-svg-host"',
  'id="my-svg-L_webUi_restApi_0"',
  'id="my-svg-L_webUi_wsApi_0"',
  'id="my-svg-L_apiClient_restApi_0"',
  'id="my-svg-L_promClient_restApi_0"',
  'id="my-svg-flowchart-restApi-8"',
  'id="my-svg-flowchart-wsApi-10"',
  'id="my-svg-flowchart-completion-16"',
  'id="my-svg-flowchart-workloadDomain-17"',
  'id="my-svg-flowchart-networkDomain-18"',
  'id="my-svg-flowchart-storageDomain-19"',
  'id="my-svg-flowchart-securityDomain-20"',
  'id="my-svg-flowchart-monitorDomain-21"',
  'id="my-svg-flowchart-opsDomain-22"',
  "vm_state.db",
  "pcv_audit.db",
  "pcv_jobs.db",
  "rbac.db",
  "pcv_security.db",
  "security_groups.db",
  "vpc.db",
  "cloud_jobs.db",
  "pcv_webpush.db",
  'id="my-svg-flowchart-desiredStore-25"',
  'id="my-svg-flowchart-virtPlatform-26"',
  'id="my-svg-flowchart-storagePlatform-27"',
  'id="my-svg-flowchart-networkPlatform-28"',
  'id="my-svg-flowchart-securityPlatform-29"',
  "DPDK"
]) {
  if (!directArchitectureSvgText.includes(marker)) {
    throw new Error(`direct HTTPS architecture SVG contract missing: ${marker}`);
  }
}
for (const marker of [/<script\b/i, /<foreignObject\b/i, /\bon\w+\s*=/i, /\b(?:xlink:)?href\s*=/i]) {
  if (marker.test(directArchitectureSvgText)) {
    throw new Error(`unsafe direct HTTPS architecture SVG marker: ${marker}`);
  }
}
if (/nginx/i.test(directArchitectureSvgText)) {
  throw new Error("NGINX found in direct HTTPS architecture SVG");
}
for (const marker of ["pcv_monitoring.db", "availability writer", "systemd D-Bus collection"]) {
  if (directArchitectureSvgText.includes(marker)) {
    throw new Error(`stale direct HTTPS architecture SVG marker: ${marker}`);
  }
}
for (const marker of [
  ".pcv-semantic-layer-colors-start{}",
  ".clientNode rect",
  ".ingressNode rect",
  ".runtimeNode rect",
  ".domainNode rect",
  ".dataNode rect",
  ".hostNode rect",
  "#my-svg-flowchart-configStore-5 rect",
  "#my-svg #my-svg-persistence>rect{fill:#fff9f3!important;stroke:#d49a72!important;}",
  ".pcv-semantic-layer-colors-end{}"
]) {
  if (!directArchitectureSvgText.includes(marker)) {
    throw new Error(`direct HTTPS architecture semantic color contract missing: ${marker}`);
  }
}
for (const marker of [
  'subgraph daemon ["purecvisorsd 단일 프로세스"]',
  'subgraph transports ["API transport and direct TLS termination"]',
  'webUi <-->|"HTTPS: static UI and REST"| restApi',
  'webUi <-->|"WebSocket TLS"| wsApi',
  'apiClient <-->|"HTTPS"| restApi',
  'promClient -->|"HTTPS metrics scrape"| restApi',
  'monitorDomain["Monitoring: host telemetry, process status, Prometheus"]',
  'operationsDb["Operations DBs (SQLite WAL, 2 files)<br/>cloud_jobs.db, pcv_webpush.db"]',
  'handlers -->|"VIEWER read"| monitorDomain',
  'monitorDomain -.->|"Host and process probes"| securityPlatform',
  "class udsApi,restApi,grpcApi,wsApi ingressNode;"
]) {
  if (!directArchitectureSource.includes(marker)) {
    throw new Error(`direct HTTPS Mermaid source contract missing: ${marker}`);
  }
}
if (/nginx/i.test(directArchitectureSource)) {
  throw new Error("NGINX found in direct HTTPS Mermaid source");
}
for (const marker of ["pcv_monitoring.db", "availability writer", "systemd D-Bus", "monitorDomain --> operationsDb"]) {
  if (directArchitectureSource.includes(marker)) {
    throw new Error(`stale direct HTTPS Mermaid source marker: ${marker}`);
  }
}
for (const marker of [
  'subgraph edgeGateway ["외부 TLS 경계"]',
  'nginx["nginx TLS termination"]',
  'nginx <-->|"Static UI and REST"| restApi',
  'monitorDomain["Monitoring: host telemetry, process status, Prometheus"]',
  'operationsDb["Operations DBs (SQLite WAL, 2 files)<br/>cloud_jobs.db, pcv_webpush.db"]',
  'handlers -->|"VIEWER read"| monitorDomain',
  'monitorDomain -.->|"Host and process probes"| securityPlatform'
]) {
  if (!fullArchitectureSource.includes(marker)) {
    throw new Error(`NGINX architecture Mermaid source contract missing: ${marker}`);
  }
}
for (const marker of ["pcv_monitoring.db", "availability writer", "systemd D-Bus", "monitorDomain --> operationsDb"]) {
  if (fullArchitectureSource.includes(marker)) {
    throw new Error(`stale NGINX architecture Mermaid source marker: ${marker}`);
  }
}

const databaseArchitectureAsset = "/assets/diagrams/purecvisor-single-database-architecture.svg";
const databaseArchitectureSvg = await readFile(path.join(distRoot, databaseArchitectureAsset));
const databaseArchitectureSvgText = databaseArchitectureSvg.toString("utf8");
const databaseArchitectureSvgHash = createHash("sha256")
  .update(databaseArchitectureSvg)
  .digest("hex");
if (databaseArchitectureSvgHash !== "1d452424de7547c0a9e68043343759efe7ae44a0412dfe9942ba76fbc8bb41e7") {
  throw new Error(`database architecture SVG checksum mismatch: ${databaseArchitectureSvgHash}`);
}
for (const marker of [
  'width="1440" height="1040" viewBox="0 0 1440 1040"',
  'role="img" aria-labelledby="pcv-db-title pcv-db-desc"',
  '<title id="pcv-db-title">PureCVisor Single Edge 데이터베이스 아키텍처</title>',
  '<desc id="pcv-db-desc">',
  "9 local SQLite files · 26 permanent tables",
  "no cross-DB transaction",
  "즉시 응답 · accepted 먼저",
  "긴 작업만",
  "관련 정책 read/write",
  "VM lock · 선택형 job register",
  "정본과 정책 · 4 files / 19 tables",
  "작업 상태 · 3 files / 3 tables",
  "증거와 외부 통합 · 2 files / 4 tables",
  "rbac.db",
  "vpc.db",
  "security_groups.db",
  "pcv_security.db",
  "vm_state.db",
  "pcv_jobs.db",
  "cloud_jobs.db",
  "pcv_audit.db",
  "pcv_webpush.db",
  "선택형 ZFS",
  "후보 OVN backend",
  "DB 복원 ≠ host actual state 복원"
]) {
  if (!databaseArchitectureSvgText.includes(marker)) {
    throw new Error(`database architecture SVG contract missing: ${marker}`);
  }
}
if ((databaseArchitectureSvgText.match(/<rect class="database"/g) || []).length !== 9) {
  throw new Error("database architecture SVG store count mismatch");
}
for (const marker of [
  /<script\b/i,
  /<foreignObject\b/i,
  /\bon\w+\s*=/i,
  /\b(?:xlink:)?href\s*=/i,
  /url\(\s*["']?(?:https?:|\/\/)/i
]) {
  if (marker.test(databaseArchitectureSvgText)) {
    throw new Error(`unsafe database architecture SVG marker: ${marker}`);
  }
}

const networkArchitectureAsset = "/assets/diagrams/purecvisor-single-network-services.svg";
const networkArchitectureSvgText = await readFile(
  path.join(distRoot, networkArchitectureAsset),
  "utf8"
);
for (const marker of [
  'width="1440" height="1080" viewBox="0 0 1440 1080"',
  'role="img" aria-labelledby="pcv-network-title pcv-network-desc"',
  '<title id="pcv-network-title">PureCVisor Single Edge 네트워크 서비스 구성도</title>',
  '<desc id="pcv-network-desc">',
  "NETWORK SERVICE FAMILIES",
  "기본 연결",
  "가상 네트워크",
  "정책 · 보호",
  "가속 · 관측",
  "Linux bridge · dnsmasq",
  "nftables · TC · NFQUEUE",
  "OVS · OVN · WireGuard",
  "VFIO · IOMMU · PF/VF",
  "libvirt · VM NIC",
  "Local VPC OVN backend · 추가 검증 전 후보",
  "공개 범위 · 한 Linux/KVM 호스트",
  'data-pcv-node-id="networkBasic"',
  'data-pcv-node-id="actualLinux"',
  'data-pcv-node-id="outcomePublish"',
  'class="control pcv-network-edge"',
  'class="data pcv-network-edge"',
  'class="accelerated pcv-network-edge"'
]) {
  if (!networkArchitectureSvgText.includes(marker)) {
    throw new Error(`network architecture SVG contract missing: ${marker}`);
  }
}
if ((networkArchitectureSvgText.match(/<rect class="family"/g) || []).length !== 4) {
  throw new Error("network architecture SVG service family count mismatch");
}
if ((networkArchitectureSvgText.match(/data-pcv-node-id=/g) || []).length !== 19) {
  throw new Error("network architecture interactive node count mismatch");
}
if ((networkArchitectureSvgText.match(/data-pcv-edge-id=/g) || []).length !== 18) {
  throw new Error("network architecture interactive edge count mismatch");
}
for (const marker of [
  /<script\b/i,
  /<foreignObject\b/i,
  /\bon\w+\s*=/i,
  /\b(?:xlink:)?href\s*=/i,
  /url\(\s*["']?(?:https?:|\/\/)/i
]) {
  if (marker.test(networkArchitectureSvgText)) {
    throw new Error(`unsafe network architecture SVG marker: ${marker}`);
  }
}

for (const [file, title] of networkExampleAssets) {
  const source = await readFile(
    path.join(distRoot, "assets", "diagrams", "network-examples", file),
    "utf8"
  );
  for (const marker of [
    'width="960" height="280" viewBox="0 0 960 280"',
    'role="img" aria-labelledby=',
    `<title`,
    title,
    "<desc",
    '<path class="edge"'
  ]) {
    if (!source.includes(marker)) {
      throw new Error(`network example SVG contract missing: ${file} ${marker}`);
    }
  }
  for (const marker of [
    /<script\b/i,
    /<foreignObject\b/i,
    /\bon\w+\s*=/i,
    /\b(?:xlink:)?href\s*=/i,
    /url\(\s*["']?(?:https?:|\/\/)/i
  ]) {
    if (marker.test(source)) {
      throw new Error(`unsafe network example SVG marker: ${file} ${marker}`);
    }
  }
}

for (const marker of [
  "const navCloseDelayMs = 160;",
  "const navCloseTimers = new WeakMap();",
  "function scheduleGroupClose(group)",
  'group.addEventListener("pointerleave", () => scheduleGroupClose(group))',
  ".pcv-nav-group::after",
  "width: max(100%, 13.5rem);",
  "height: 0.45rem;",
  ".pcv-nav-group:hover::after",
  ".pcv-nav-group:focus-within::after",
  "pointer-events: auto;"
]) {
  if (!headerComponent.includes(marker)) {
    throw new Error(`header navigation pointer corridor contract missing: ${marker}`);
  }
}

for (const marker of [
  '[data-pcv-architecture-tabs]',
  '[data-pcv-architecture-tab]',
  '[data-pcv-architecture-panel]',
  "activateArchitectureTab",
  'setAttribute("aria-selected"',
  "panel.hidden = panel.id !== nextPanelId",
  'event.key === "ArrowRight"',
  'event.key === "ArrowLeft"',
  'event.key === "Home"',
  'event.key === "End"',
  "event.preventDefault()",
  "nextTab.focus()",
  "enhanceArchitecturePanel",
  "resetArchitecturePanel",
  "initialPanel"
]) {
  if (!headerComponent.includes(marker)) {
    throw new Error(`overview architecture tab interaction missing: ${marker}`);
  }
}
for (const marker of [
  'document.querySelectorAll("[data-pcv-architecture-interactive]")',
  'canvas.closest("[data-pcv-architecture-tabs]")',
  "canvas.parentElement"
]) {
  if (!headerComponent.includes(marker)) {
    throw new Error(`standalone architecture interaction missing: ${marker}`);
  }
}
for (const marker of [
  "new DOMParser()",
  '"image/svg+xml"',
  'svg.querySelector("script, foreignObject")',
  "/^(?:href|xlink:href)$/i",
  "namespaceSvg",
  "resolveArchitectureEdge",
  "architectureLayerNodeIds",
  '"pcv-is-related-edge"',
  '"pointerover"',
  'event.pointerType === "touch"',
  'canvas.dataset.pcvArchitectureState = "fallback"',
  'svg.setAttribute("aria-hidden", "true")',
  'svg.querySelectorAll("[data-pcv-node-id]")',
  'svg.querySelectorAll("[data-pcv-edge-id][data-pcv-from][data-pcv-to]")',
  'img.pcv-architecture-source-image',
  "for (const className of image.classList)"
]) {
  if (!architectureInteractions.includes(marker)) {
    throw new Error(`overview architecture rollover interaction missing: ${marker}`);
  }
}
const heroTitle = "PURECVISOR 2.0.0";
const koreanHeroCopy = "한 대의 Linux/KVM 호스트에서 VM, 컨테이너, ZFS 스토리지와 네트워크를 설치하고 운영합니다.";
const koreanHeroFollowup = "Web UI, REST API와 CLI가 하나의 로컬 제어면을 사용합니다.";
const englishHeroCopy = "Install and operate VMs, containers, ZFS storage, and networking on one Linux/KVM host.";
const englishHeroFollowup = "The Web UI, REST API, and CLI share one local control plane.";
for (const [name, source] of [["root", index], ["korean", korean], ["english", english]]) {
  for (const marker of [
    "#capabilities",
    "#quickstart",
    "#scope",
    "pcv-hero-links",
    "class=\"pcv-capability",
    "pcv-quickstart",
    "pcv-scope-list",
    "한 노드의 가상화 운영을",
    "Single Edge 시작 흐름",
    "Single Edge에 집중한 공개판",
    "One operational flow",
    "Start a Single Edge node",
    "A public edition focused on Single Edge",
    "소프트웨어 정의 네트워크를 하나의 Linux/KVM 노드에서 운영합니다.",
    "software-defined networking on one Linux/KVM node",
    "pcv-final-cta",
    "pcv-final-title",
    "한 노드부터 명확하게 운영하세요.",
    "Operate clearly, starting with one node.",
    "pcv-map-resources",
    "pcv-capability-map-title",
    "Single Edge capability map",
    "pcv-docs-title",
    "pcv-doc-shortcuts",
    "pcv-doc-directory",
    "pcv-doc-category",
    "pcv-role-paths",
    "필요한 작업에서 시작하세요.",
    "Start with the task you need.",
    "8개 작업 카테고리 · 22개 장",
    "8 task categories and 22 chapters",
    "RECOMMENDED PATHS",
    "pcv-control-map pcv-architecture-source",
    "pcv-architecture-map-title",
    "pcv-architecture-legend",
    "pcv-architecture-source-canvas",
    "pcv-architecture-source-image",
    "purecvisor-single-full-architecture.svg",
    "Single Edge 서비스 아키텍처",
    "Single Edge service architecture",
    "SVG 원본 구조",
    "Source SVG structure"
  ]) {
    if (source.includes(marker)) throw new Error(`${name} removed landing scene found: ${marker}`);
  }
  const sectionIds = [...source.matchAll(/<section\b[^>]*\bid="([^"]+)"/g)].map((match) => match[1]);
  if (JSON.stringify(sectionIds) !== JSON.stringify(["_top", "documentation"])) {
    throw new Error(`${name} landing section order mismatch: ${sectionIds.join(" -> ")}`);
  }
  const heroCopyPosition = source.indexOf('<div class="pcv-hero-copy">');
  if (heroCopyPosition < 0) {
    throw new Error(`${name} hero copy missing`);
  }
  if ((source.match(/<figure\b/g) || []).length !== 0 || (source.match(/<img\b[^>]*\/assets\/diagrams\//g) || []).length !== 0) {
    throw new Error(`${name} landing architecture media must not be rendered`);
  }
  const documentationTitle = name === "english" ? "Explore the documentation" : "문서 살펴보기";
  if (!source.includes(`<h2 id="pcv-documentation-title">${documentationTitle}</h2>`)) {
    throw new Error(`${name} documentation map title missing`);
  }
  if (!source.includes('class="pcv-documentation" id="documentation" aria-labelledby="pcv-documentation-title"')) {
    throw new Error(`${name} documentation map accessible name missing`);
  }
  if (!source.includes(`DOCUMENTATION · ${landingDocuments.length} DOCUMENTS`)) {
    throw new Error(`${name} documentation map count label mismatch`);
  }
  if ((source.match(/class="pcv-directory-group"/g) || []).length !== guideGroups.length) {
    throw new Error(`${name} documentation group count mismatch`);
  }
  if ((source.match(/class="pcv-directory-description"/g) || []).length !== guideGroups.length) {
    throw new Error(`${name} documentation group description count mismatch`);
  }
  const groupDescription = name === "english"
    ? "Review the recommended profile and install a Single Edge node."
    : "권장 사양을 확인하고 Single Edge 노드를 설치합니다.";
  if (!source.includes(groupDescription)) {
    throw new Error(`${name} documentation group description missing`);
  }
  if ((source.match(/class="pcv-directory-link"/g) || []).length !== landingDocuments.length) {
    throw new Error(`${name} documentation link count mismatch`);
  }
  for (const document of landingDocuments) {
    if (!source.includes(`class="pcv-directory-link" href="${document.path}"`)) {
      throw new Error(`${name} documentation link missing: ${document.path}`);
    }
  }
}
for (const selector of [
  ".pcv-hero-links",
  ".pcv-section-heading-wide",
  ".pcv-capability",
  ".pcv-quickstart",
  ".pcv-kinetic",
  ".pcv-terminal",
  ".pcv-text-link",
  ".pcv-scope",
  ".pcv-final-cta",
  ".pcv-map-resources",
  ".pcv-map-core small",
  ".pcv-map-clients",
  ".pcv-map-route",
  ".pcv-map-core",
  ".pcv-map-capabilities",
  ".pcv-map-capability",
  ".pcv-map-icon",
  ".pcv-scene",
  ".pcv-section-heading",
  ".pcv-docs",
  ".pcv-doc-shortcuts",
  ".pcv-doc-directory",
  ".pcv-doc-category",
  ".pcv-role-paths",
  ".pcv-role-heading",
  ".pcv-kicker",
  ".pcv-category-index",
  ".pcv-feature-index",
  ".pcv-arch-stack",
  ".pcv-arch-layer",
  ".pcv-arch-node",
  ".pcv-arch-path-trigger",
  ".pcv-arch-active-route",
  ".pcv-architecture-source-viewport",
  ".pcv-architecture-legend",
  ".pcv-layer-key",
  ".pcv-layer-swatch"
]) {
  if (landingStyles.includes(selector)) throw new Error(`retired landing selector found: ${selector}`);
}
if (landingStyles.includes(".pcv-hero-copy h1")) {
  throw new Error("retired hero-scale heading selector found");
}
if (landingStyles.includes("grid-template-columns: minmax(0, 1.02fr)")) {
  throw new Error("retired two-column hero layout found");
}
for (const [selector, declarations] of [
  [".pcv-documentation", ["margin-top: 0 !important;", "background: var(--pcv-soft);", "border-bottom: 1px solid var(--pcv-line);"]],
  [".pcv-documentation-heading", ["grid-template-columns: minmax(0, 1fr) minmax(18rem, 0.7fr);", "align-items: end;"]],
  [".pcv-directory-grid", ["grid-template-columns: repeat(4, minmax(0, 1fr));", "gap: 1px;", "border: 1px solid var(--pcv-line);", "border-radius: 0.5rem;", "background: var(--pcv-line);"]],
  [".pcv-directory-group", ["min-width: 0;", "margin-top: 0 !important;", "background: var(--pcv-canvas);"]],
  [".pcv-directory-description", ["min-height: 4.5rem;", "font-size: 0.875rem;", "line-height: 1.6;", "word-break: keep-all;"]],
  [".pcv-directory-link", ["min-height: 2.75rem;", "touch-action: manipulation;", "transition: color 160ms ease;"]]
]) {
  const start = landingStyles.indexOf(`${selector} {`);
  const end = start < 0 ? -1 : landingStyles.indexOf("}", start);
  const block = end < 0 ? "" : landingStyles.slice(start, end);
  for (const declaration of declarations) {
    if (!block.includes(declaration)) {
      throw new Error(`documentation map style missing: ${selector} ${declaration}`);
    }
  }
}
for (const responsiveContract of [
  ".pcv-directory-grid {\n    grid-template-columns: repeat(2, minmax(0, 1fr));",
  ".pcv-directory-grid {\n    grid-template-columns: minmax(0, 1fr);"
]) {
  if (!landingStyles.includes(responsiveContract)) {
    throw new Error(`documentation map responsive contract missing: ${responsiveContract}`);
  }
}
for (const layoutContract of [
  "grid-template-columns: minmax(0, 1fr);",
  "white-space: nowrap;",
  ".pcv-hero-lead {\n    white-space: normal;",
  ".pcv-hero-lead span {\n  display: block;",
  ".pcv-architecture-source-canvas {\n  display: block;",
  ".pcv-architecture-source-image {\n  display: block;\n  width: 100%;",
  "max-width: 100%;"
]) {
  if (!landingStyles.includes(layoutContract)) {
    throw new Error(`source architecture layout contract missing: ${layoutContract}`);
  }
}
for (const themeContract of [
  "--pcv-hero-bg: #ffffff;",
  "--pcv-map-bg: #eef3f7;",
  "--pcv-map-node: #ffffff;",
  "--pcv-hero-bg: #050a14;",
  "--pcv-map-bg: #09101d;",
  "--pcv-map-node: #0f1c2d;",
  "background: #f8fafc;"
]) {
  if (!landingStyles.includes(themeContract)) {
    throw new Error(`landing theme contract missing: ${themeContract}`);
  }
}
for (const [selector, declarations] of [
  [".pcv-control-map", ["border-radius: 2rem;", "font-family: var(--sl-font);"]],
  [".pcv-map-bar", ["font-size: 0.8125rem;", "min-height: 3.5rem;"]],
  [".pcv-architecture-source-open", ["min-height: 2.75rem;", "white-space: nowrap;"]],
  [".pcv-architecture-source-canvas", ["display: block;", "overflow: hidden;", "cursor: zoom-in;"]],
  [".pcv-architecture-source-image", ["width: 100%;", "max-width: 100%;", "height: auto;", "background: #f8fafc;"]],
  [".pcv-architecture-source-note", ["font-size: 0.75rem;", "line-height: 1.5;"]]
]) {
  const start = landingStyles.indexOf(`${selector} {`);
  const end = start < 0 ? -1 : landingStyles.indexOf("}", start);
  const block = end < 0 ? "" : landingStyles.slice(start, end);
  for (const declaration of declarations) {
    if (!block.includes(declaration)) {
      throw new Error(`source architecture contract missing: ${selector} ${declaration}`);
    }
  }
}
for (const [selector, declarations] of [
  [".pcv-overview-architecture-tabs", ["width: 100%;"]],
  [".pcv-architecture-tablist", ["grid-template-columns: repeat(2, minmax(0, 1fr));", "border: 1px solid var(--pcv-map-line);", "background: var(--pcv-map-bar-bg);"]],
  [".pcv-architecture-tab", ["align-self: stretch;", "min-height: 3rem;", "height: 100%;", "margin: 0;", "font-size: 0.8125rem;", "overflow-wrap: anywhere;", "transition: border-color 140ms ease, background-color 140ms ease, color 140ms ease;"]],
  ['.pcv-architecture-tab[aria-selected="true"]', ["border-color: var(--pcv-map-signal);", "background: var(--pcv-map-node);", "box-shadow: inset 0 -3px 0 var(--pcv-map-signal);"]],
  [".pcv-architecture-tab:focus-visible", ["outline: 3px solid var(--pcv-map-signal);", "outline-offset: 2px;"]],
  [".pcv-architecture-panel[hidden]", ["display: none;"]],
  [".pcv-architecture-panel .pcv-control-map", ["margin: 0;"]]
]) {
  const start = landingStyles.indexOf(`${selector} {`);
  const end = start < 0 ? -1 : landingStyles.indexOf("}", start);
  const block = end < 0 ? "" : landingStyles.slice(start, end);
  for (const declaration of declarations) {
    if (!block.includes(declaration)) {
      throw new Error(`overview architecture tab style missing: ${selector} ${declaration}`);
    }
  }
}
if (!landingStyles.includes(".pcv-architecture-tab {\n    min-height: 3.25rem;")) {
  throw new Error("overview architecture mobile tab target contract missing");
}
for (const interactionContract of [
  ".pcv-architecture-source-open:is(:hover, :focus-visible)",
  ".pcv-architecture-source-canvas:focus-visible",
  '.pcv-architecture-tab:not([aria-selected="true"]):hover',
  ".pcv-architecture-tab:focus-visible",
  "@keyframes pcv-architecture-flow",
  "--pcv-architecture-flow: #12627a;",
  ".pcv-architecture-inline [data-pcv-node-id]",
  ".pcv-architecture-inline [data-pcv-layer-id]",
  ".pcv-architecture-inline.pcv-is-inspecting [data-pcv-node-id]:not(.pcv-is-related-node)",
  ".pcv-architecture-inline.pcv-is-inspecting :is(.flowchart-link, .pcv-network-edge):not(.pcv-is-related-edge)",
  ".pcv-architecture-inline .flowchart-link.pcv-is-related-edge",
  ".pcv-architecture-inline .pcv-network-edge.pcv-is-related-edge",
  ".pcv-architecture-inline .pcv-network-edge.pcv-is-related-edge.accelerated",
  "stroke: var(--pcv-architecture-flow) !important;",
  "animation: pcv-architecture-flow 560ms linear infinite;",
  ".pcv-architecture-inline marker .arrowMarkerPath",
  "@media (prefers-reduced-motion: reduce)",
  "animation-iteration-count: 1 !important",
  "animation: none !important;"
]) {
  if (!landingStyles.includes(interactionContract)) {
    throw new Error(`source architecture interaction contract missing: ${interactionContract}`);
  }
}
for (const [name, source, language, heroCopy, heroFollowup, canonical] of [
  ["root", index, "ko", koreanHeroCopy, koreanHeroFollowup, "https://purecvisor.site/"],
  ["korean", korean, "ko", koreanHeroCopy, koreanHeroFollowup, "https://purecvisor.site/ko/"],
  ["english", english, "en", englishHeroCopy, englishHeroFollowup, "https://purecvisor.site/en/"]
]) {
  if (!source.includes(`<html lang="${language}"`)) throw new Error(`${name} language mismatch`);
  if ((source.match(/<h1\b/g) || []).length !== 1) throw new Error(`${name} H1 count mismatch`);
  const heroStart = source.indexOf('<section class="pcv-hero"');
  const heroEnd = source.indexOf("</section>", heroStart);
  if (heroStart === -1 || heroEnd === -1) throw new Error(`${name} hero markup missing`);
  const heroMarkup = source.slice(heroStart, heroEnd);
  if (heroMarkup.includes('class="pcv-eyebrow"')) throw new Error(`${name} hero eyebrow remained`);
  if (source.includes("PURECVISOR 2.0.0 · SINGLE EDGE")) {
    throw new Error(`${name} legacy product eyebrow remained`);
  }
  for (const legacyTitle of [
    "하나의 Linux/KVM 노드, 하나의 제어면.",
    "One Linux/KVM node. One control plane."
  ]) {
    if (source.includes(legacyTitle)) throw new Error(`${name} legacy hero title remained`);
  }
  if (!source.includes(`<h1 class="pcv-hero-title" id="pcv-hero-title">${heroTitle}</h1>`)) {
    throw new Error(`${name} value H1 missing`);
  }
  if ((source.match(new RegExp(heroCopy.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "g")) || []).length !== 1) {
    throw new Error(`${name} hero copy mismatch`);
  }
  if (!source.includes(heroFollowup)) {
    throw new Error(`${name} hero followup mismatch`);
  }
  for (const sourceContract of name === "english"
    ? ["5-minute quickstart", "Installation guide", "Ubuntu Server 26.04.1 LTS · Single Edge · HTTPS :443 by default"]
    : ["5분 퀵스타트", "설치 가이드", "Ubuntu Server 26.04.1 LTS · Single Edge · 기본 HTTPS :443"]) {
    if (!source.includes(sourceContract)) throw new Error(`${name} localized landing content missing: ${sourceContract}`);
  }
  if (!source.includes(`<link rel="canonical" href="${canonical}"`)) {
    throw new Error(`${name} canonical route mismatch`);
  }
  if (!source.includes('datetime="2026-08-31T00:00:00.000Z"')) {
    throw new Error(`${name} landing lastUpdated mismatch`);
  }
  if ((source.match(/class="pcv-nav-group\b/g) || []).length !== 4) {
    throw new Error(`${name} navigation group count mismatch`);
  }
  if ((source.match(/class="pcv-nav-menu\b/g) || []).length !== 4) {
    throw new Error(`${name} navigation submenu count mismatch`);
  }
  if (!source.includes('aria-haspopup="true"') || !source.includes('aria-expanded="false"')) {
    throw new Error(`${name} navigation disclosure contract missing`);
  }
  if (!source.includes('href="/ko/"') || !source.includes('href="/en/"')) {
    throw new Error(`${name} language routes missing`);
  }
  for (const href of [
    guidePath(1),
    guidePath(1, "#14-5분-퀵스타트"),
    guidePath(1, "#공개-범위-핵심")
  ]) {
    if (!source.includes(`href="${href}"`)) {
      throw new Error(`${name} guide-backed header link missing: ${href}`);
    }
  }
}
if (!korean.includes(`href="${guideEntryPath}">설치 가이드</a>`)) {
  throw new Error("korean installation guide link missing");
}
if (!english.includes(`href="${guideEntryPath}">Installation guide</a>`)) {
  throw new Error("english installation guide link missing");
}
if (!korean.includes(`<a class="pcv-button pcv-button-primary" href="${guidePath(1, "#14-5분-퀵스타트")}">`)
  || !korean.includes(`<a class="pcv-button pcv-button-ghost" href="${guideEntryPath}">`)) {
  throw new Error("korean hero guide actions missing");
}
if (!english.includes(`<a class="pcv-button pcv-button-primary" href="${guidePath(1, "#14-5분-퀵스타트")}">`)
  || !english.includes(`<a class="pcv-button pcv-button-ghost" href="${guideEntryPath}">`)) {
  throw new Error("english hero guide actions missing");
}
if (!new RegExp(`<a\\b[^>]*href="${guidePath(6)}"[^>]*>네트워크</a>`).test(korean)) {
  throw new Error("korean networking header direct link missing");
}
if (!new RegExp(`<a\\b[^>]*href="${guidePath(6)}"[^>]*>Networking</a>`).test(english)) {
  throw new Error("english networking header direct link missing");
}
for (const [name, source] of [["root", index], ["korean", korean], ["english", english]]) {
  if (source.includes("/docs.html")) throw new Error(`${name} legacy docs link found`);
  if (source.includes("/guide.html")) throw new Error(`${name} retired guide link found`);
}
if (!docs.includes(`content="0;url=${guideEntryPath}"`)) {
  throw new Error("legacy docs redirect missing");
}
if (docs.includes("reader-shell") || docs.includes("guide-content.md")) {
  throw new Error("legacy product docs reader found");
}
if (outputFiles.some((file) => path.relative(distRoot, file) === "guide-content.md")) {
  throw new Error("retired guide content artifact found");
}

for (const marker of [
  "network.host.info",
  "/api/v1/networks/host-baseline",
  "/api/v1/vpcs/status",
  "subnet_cidrs",
  "호스트 네트워크 기준선",
  "partial",
  "unavailable",
  "pcvctl ovn switch create ls-web",
  "/api/v1/ovn/acl?switch=ls-web",
  "/api/v1/ovn/nat?router=lr-main",
  "-32602",
  "ownership marker",
  "NET-OVN-01~07",
  "Local VPC OVN backend",
  "서비스별 예제 지도",
  "pcv-network-architecture",
  'href="/assets/diagrams/purecvisor-single-network-services.svg"',
  'src="/assets/diagrams/purecvisor-single-network-services.svg"',
  'width="1440" height="1080" loading="lazy" decoding="async"',
  "활용 예제 — 연결 목적에 맞는 기본 네트워크 생성",
  "활용 예제 — NAT와 내부 격리 경계 비교",
  "활용 예제 — VM을 upstream VLAN 100에 연결",
  "활용 예제 — 기존 vnet 제한과 VM·tenant SLA 적용",
  "활용 예제 — 외부 VXLAN endpoint와 수동 peer 구성",
  "활용 예제 — 웹 논리 네트워크에 DHCP·ACL·SNAT 적용",
  "활용 예제 — 웹 포트와 관리 CIDR의 SSH만 허용",
  "활용 예제 — 전용 PCI NIC로 OVS-DPDK bridge 구성",
  "활용 예제 — VLAN 100 VF를 웹 VM에 직접 할당",
  "활용 예제 — link에서 VM NIC까지 계층별 장애 격리",
  "활용 예제 — NIC drop과 conntrack 포화 징후 확인",
  "활용 예제 — Linux Local VPC의 VM 서비스를 제한 게시",
  "pcvctl security-group create web-sg",
  "pcvctl dpdk bridge create dpdk-br0 0000:03:00.0",
  "pcvctl sriov set eno2 0",
  "node_nf_conntrack_entries_limit"
]) {
  if (!networking.includes(marker)) {
    throw new Error(`networking current contract missing: ${marker}`);
  }
}

if ((networking.match(/purecvisor-single-network-services\.svg/g) || []).length !== 3) {
  throw new Error("network architecture asset reference count mismatch");
}
if (!networking.includes('data-pcv-architecture-interactive="network"')) {
  throw new Error("network architecture rollover entry point missing");
}
if ((networking.match(/class="pcv-network-example-diagram/g) || []).length !== networkingExampleAssets.length) {
  throw new Error("network example diagram count mismatch");
}
for (const [file] of networkingExampleAssets) {
  const asset = `/assets/diagrams/network-examples/${file}`;
  if ((networking.match(new RegExp(asset.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "g")) || []).length !== 2) {
    throw new Error(`network example diagram reference mismatch: ${file}`);
  }
}
for (const marker of [
  "10.12 Suricata DPI/IDS/IPS (2.0, D13)",
  "활용 예제 — IDS 상태 확인 후 선택 SID만 IPS 차단",
  "/api/v1/suricata/ips/status",
  "SID 선정 주의"
]) {
  if (!securityGuide.includes(marker)) {
    throw new Error(`security Suricata contract missing: ${marker}`);
  }
}
if (networking.includes("6.12 Suricata DPI/IDS/IPS")) {
  throw new Error("Suricata section remained in networking chapter");
}
if (!networking.includes("6.12 Local VPC")) {
  throw new Error("Local VPC section renumbering missing");
}
if ((securityGuide.match(/class="pcv-network-example-diagram/g) || []).length !== 1) {
  throw new Error("security Suricata example diagram count mismatch");
}
const suricataAsset = "/assets/diagrams/network-examples/suricata.svg";
if ((securityGuide.match(new RegExp(suricataAsset.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "g")) || []).length !== 2) {
  throw new Error("security Suricata example diagram reference mismatch");
}
if (!networking.includes("로컬 네트워크 제어면입니다.<br>")) {
  throw new Error("network period-based sentence break is not rendered");
}
for (const staleMarker of [
  '&quot;method&quot;:&quot;firewall.rule.add&quot;',
  '&quot;method&quot;:&quot;network.vlan.add&quot;',
  "pcvctl sg create",
  "pcvctl sg rule",
  "pcvctl sg apply"
]) {
  if (networking.includes(staleMarker)) {
    throw new Error(`stale networking example found: ${staleMarker}`);
  }
}

const genericOvnInventoryStart = networking.indexOf(
  '<h3 id="등록된-generic-ovn-rpc--정확히-18개">'
);
const genericOvnInventoryEnd = networking.indexOf(
  '<h3 id="등록되지-않은-역동작과-공개-제외-기능">',
  genericOvnInventoryStart
);
if (genericOvnInventoryStart < 0 || genericOvnInventoryEnd <= genericOvnInventoryStart) {
  throw new Error("generic OVN 18-RPC inventory section missing");
}
const genericOvnInventory = networking.slice(genericOvnInventoryStart, genericOvnInventoryEnd);
const expectedGenericOvnMethods = [
  "ovn.status",
  "ovn.switch.create",
  "ovn.switch.delete",
  "ovn.switch.list",
  "ovn.switch.detail",
  "ovn.port.add",
  "ovn.port.remove",
  "ovn.acl.add",
  "ovn.acl.list",
  "ovn.router.create",
  "ovn.router.delete",
  "ovn.router.list",
  "ovn.router.detail",
  "ovn.router.add_port",
  "ovn.dhcp.enable",
  "ovn.nat.add",
  "ovn.nat.list",
  "ovn.tenant.create"
].sort();
const actualGenericOvnMethods = [...new Set(
  genericOvnInventory.match(/\bovn\.[a-z][a-z0-9_.]*\b/g) || []
)].sort();
if (JSON.stringify(actualGenericOvnMethods) !== JSON.stringify(expectedGenericOvnMethods)) {
  throw new Error(`generic OVN RPC inventory mismatch: ${actualGenericOvnMethods.join(", ")}`);
}

for (const marker of [
  "pcvctl ovn switch create ls-web --subnet",
  "vm_port",
  "pcv_ovn_vm_port_setup",
  "pcv_ovn_vm_port_cleanup",
  "nfv.lb.create",
  "pcvctl ovn lb",
  "/data/recordings/",
  "ovn-sdn-recording-handoff"
]) {
  if (networking.includes(marker)) {
    throw new Error(`stale or private networking contract found: ${marker}`);
  }
}

for (const chapter of guideChapters) {
  const page = await readFile(
    path.join(distRoot, chapter.contentSlug, "index.html"),
    "utf8"
  );
  if (!page.includes('<html lang="ko"')) throw new Error(`guide language mismatch: ${chapter.path}`);
  if (!page.includes(chapter.title)) throw new Error(`guide title missing: ${chapter.path}`);
  if (!page.includes(`<link rel="canonical" href="https://purecvisor.site${chapter.path}"`)) {
    throw new Error(`guide canonical mismatch: ${chapter.path}`);
  }
  if (!page.includes('aria-current="page"')) {
    throw new Error(`guide active navigation missing: ${chapter.path}`);
  }
  if (/<h[2-6]\b[^>]*>\s*[0-9]+\.<br>/i.test(page)) {
    throw new Error(`guide numeric heading sentence break found: ${chapter.path}`);
  }
  for (const group of guideGroups) {
    if (!page.includes(group.label)) throw new Error(`guide group missing: ${group.label}`);
  }
  for (const linkedDocument of readerDocuments) {
    if (!page.includes(`href="${linkedDocument.path}"`)) {
      throw new Error(`guide sidebar link missing: ${linkedDocument.path}`);
    }
  }
  if (!docs.includes(chapter.legacyAnchor) || !docs.includes(chapter.path)) {
    throw new Error(`legacy guide mapping missing: ${chapter.legacyAnchor}`);
  }
}

for (const document of supplementalDocuments) {
  const page = await readFile(
    path.join(distRoot, document.contentSlug, "index.html"),
    "utf8"
  );
  if (!page.includes('<html lang="ko"')) {
    throw new Error(`supplemental document language mismatch: ${document.path}`);
  }
  if (!page.includes(document.title)) {
    throw new Error(`supplemental document title missing: ${document.path}`);
  }
  if (!page.includes(`<link rel="canonical" href="https://purecvisor.site${document.path}"`)) {
    throw new Error(`supplemental document canonical mismatch: ${document.path}`);
  }
  if (!page.includes('aria-current="page"')) {
    throw new Error(`supplemental document active navigation missing: ${document.path}`);
  }
  if (/<h[2-6]\b[^>]*>\s*[0-9]+\.<br>/i.test(page)) {
    throw new Error(`supplemental numeric heading sentence break found: ${document.path}`);
  }
  for (const group of guideGroups) {
    if (!page.includes(group.label)) {
      throw new Error(`supplemental document group missing: ${group.label}`);
    }
  }
  for (const linkedDocument of readerDocuments) {
    if (!page.includes(`href="${linkedDocument.path}"`)) {
      throw new Error(`supplemental document sidebar link missing: ${linkedDocument.path}`);
    }
  }
  for (const marker of [
    "로컬 SQLite 파일 9개",
    "영구 테이블 26개",
    "pcv-database-summary",
    "pcv-database-architecture",
    'href="/assets/diagrams/purecvisor-single-database-architecture.svg"',
    'src="/assets/diagrams/purecvisor-single-database-architecture.svg"',
    'width="1440" height="1040" loading="lazy" decoding="async"',
    'aria-describedby="pcv-database-architecture-note"',
    "비동기 실행·진행·완료 통지와 증거 기록",
    "그림 영역을 좌우로 스크롤",
    "DB 사이의 원자성",
    "변조 탐지형 감사 증거",
    "Audit DB",
    "Web Push DB",
    "pcv_webpush.db",
    "config.backup",
    "스키마 변경 체크리스트"
  ]) {
    if (!page.includes(marker)) {
      throw new Error(`supplemental document body contract missing: ${marker}`);
    }
  }
  for (const staleMarker of [
    'data-language="mermaid"',
    "flowchart TB",
    "Monitoring Evidence DB",
    "pcv_monitoring.db",
    "../site/public/assets/"
  ]) {
    if (page.includes(staleMarker)) {
      throw new Error(`stale supplemental document marker found: ${staleMarker}`);
    }
  }
  if ((page.match(/purecvisor-single-database-architecture\.svg/g) || []).length !== 3) {
    throw new Error("supplemental database architecture asset reference count mismatch");
  }
  for (const privateMarker of [
    "6306cafd",
    "2026-08-27-104-bpf-lsm-boot-activation-handoff",
    "2026-08-28-p1-live-certification-handoff",
    "2026-08-29-51-bpf-lsm-boot-activation-handoff"
  ]) {
    if (page.includes(privateMarker)) {
      throw new Error(`private operations evidence found: ${privateMarker}`);
    }
  }
  const tableCount = (page.match(/<table\b/g) || []).length;
  const focusableTableCount = (page.match(/<table\b[^>]*\btabindex="0"/g) || []).length;
  if (tableCount === 0 || focusableTableCount !== tableCount) {
    throw new Error(`supplemental document table focus contract mismatch: ${focusableTableCount}/${tableCount}`);
  }
}

const installation = await readFile(
  path.join(distRoot, "ko", "getting-started", "installation", "index.html"),
  "utf8"
);
if (!installation.includes("2.1 솔루션 권장사항") || installation.includes("fetch(")) {
  throw new Error("installation body is not statically rendered");
}
for (const headingContract of [
  '<h2 id="22-솔루션-설치">2.2 솔루션 설치</h2>',
  '<h3 id="공통-런타임-의존성-사용-기능별">공통 런타임 의존성 (사용 기능별)</h3>',
  '<h4 id="ovn-single-edge-구성">OVN Single Edge 구성</h4>',
  '<h4 id="iscsi-런타임">iSCSI 런타임</h4>',
  '<h4 id="소스-배포의-lio-모듈-설정">소스 배포의 LIO 모듈 설정</h4>',
  '<h4 id="zfs를-사용하지-않는-구성">ZFS를 사용하지 않는 구성</h4>',
  '<h4 id="zfs-기능을-사용하는-구성">ZFS 기능을 사용하는 구성</h4>'
]) {
  if (!installation.includes(headingContract)) {
    throw new Error(`installation hierarchy contract missing: ${headingContract}`);
  }
}
if (installation.includes("2.2 패키지 설치")) {
  throw new Error("legacy installation section title remained");
}
if (!installation.includes("한 번에 설치합니다.<br>")) {
  throw new Error("installation automatic sentence break missing");
}
for (const marker of [
  "Ubuntu 26.04.1 LTS",
  "unused-management-ipv4",
  "configured-management-ipv4",
  "qemu-system-x86",
  "purecvisor-ovn-single",
  "두 명령을 모두 실행하지 말고",
  "--encap-ip",
  "--verify-only",
  "ZFS는 선택형 런타임입니다",
  "ZFS volume은 서비스 시작의 필수 조건이 아닙니다.",
  "command -v qemu-img",
  "rpool/data/purecvisor/vms",
  "로컬 Single Edge 배포",
  "TLS 배포 모드 선택",
  "purecvisorsd",
  "선택형 NGINX 외부 TLS 종료",
  "mode=internal",
  "mode=external_termination",
  "PCV_NGINX_BIND_IP",
  "GitHub Pages"
]) {
  if (!installation.includes(marker)) throw new Error(`installation TLS mode contract missing: ${marker}`);
}
if (!installation.includes(`href="${guidePath(1)}"`) || !installation.includes(`href="${guidePath(3)}"`)) {
  throw new Error("installation pagination contract missing");
}

process.stdout.write(`pages artifact verified: ${outputFiles.length} files\n`);
