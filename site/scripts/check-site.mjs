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
const requiredFiles = [
  "index.html",
  "ko/index.html",
  "en/index.html",
  "docs.html",
  "favicon.svg",
  "assets/diagrams/purecvisor-single-full-architecture.svg",
  "assets/diagrams/purecvisor-single-direct-https-architecture.svg",
  ...readerDocuments.map((document) => `${document.contentSlug}/index.html`)
];
const forbiddenText = [
  ["HardcoreMonk", "purecvisor-single"].join("/"),
  ["Private", "repository"].join(" "),
  ["192", "168", "3", "51"].join("."),
  ["192", "168", "3", "53"].join("."),
  "T2FA-F4(WebAuthn/step-up)",
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
const guideSource = await readFile(path.join(siteRoot, "..", "docs", "GUIDE.md"), "utf8");
const invalidGuideBreakLines = guideSource
  .split(/\r?\n/)
  .map((line, index) => ({ index: index + 1, line }))
  .filter(({ line }) => line.trimEnd().endsWith("<br>") && !line.trimEnd().slice(0, -4).trimEnd().endsWith("."));
if (invalidGuideBreakLines.length) {
  throw new Error(`GUIDE sentence break must follow a period: ${invalidGuideBreakLines.map(({ index }) => index).join(", ")}`);
}
const landingStyles = await readFile(path.join(siteRoot, "src", "styles", "custom.css"), "utf8");
const headerComponent = await readFile(path.join(siteRoot, "src", "components", "Header.astro"), "utf8");
const architectureInteractions = await readFile(
  path.join(siteRoot, "src", "scripts", "architecture-interactions.js"),
  "utf8"
);
const directArchitectureSource = await readFile(
  path.join(siteRoot, "..", "docs", "architecture", "purecvisor-single-direct-https-architecture.mmd"),
  "utf8"
);
for (const contract of [
  "--pcv-prose-width: 46rem;",
  "--pcv-technical-width: 60rem;",
  "--pcv-architecture-width: 75rem;",
  "--pcv-reader-rail-inset-max: 7.5rem;",
  "--sl-content-width: var(--pcv-architecture-width);"
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
  "Monitoring Source request handler는 immutable cache만 읽습니다.",
  "vm_state.db",
  "pcv_audit.db",
  "pcv_jobs.db",
  "rbac.db",
  "pcv_security.db",
  "security_groups.db",
  "vpc.db",
  "cloud_jobs.db",
  "pcv_monitoring.db",
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
  "장시간 작업은 fire-and-forget 패턴으로 즉시 응답 후 GTask 비동기 실행"
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
if (architectureSvgHash !== "f64b3756dbe546ac65245fa5363d61cbd30e03b1652b53c612ca72e33d685c3b") {
  throw new Error(`architecture source SVG checksum mismatch: ${architectureSvgHash}`);
}
const architectureStructure = architectureSvgText.replace(/<style>[\s\S]*?<\/style>/, "<style></style>");
const architectureStructureHash = createHash("sha256").update(architectureStructure).digest("hex");
if (architectureStructureHash !== "0f3f3a26d1dc2b128a0b58da6f63bad61d71637e6a3d4aa2f01aff9f137778be") {
  throw new Error(`architecture SVG structure/content mismatch: ${architectureStructureHash}`);
}
for (const marker of [
  'width="100%"',
  'viewBox="4 4 1849.5234375 2798"',
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
  'pcv_monitoring.db',
  'pcv_webpush.db',
  'DPDK'
]) {
  if (!architectureSvgText.includes(marker)) throw new Error(`architecture source SVG contract missing: ${marker}`);
}
for (const marker of [/<script\b/i, /<foreignObject\b/i, /\bon\w+\s*=/i, /\b(?:xlink:)?href\s*=/i]) {
  if (marker.test(architectureSvgText)) throw new Error(`unsafe architecture source SVG marker: ${marker}`);
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
if (directArchitectureSvgHash !== "f728f3460a50d44ccf388f3daf56d48882323b005de58838cbfa3385e52431b7") {
  throw new Error(`direct HTTPS architecture SVG checksum mismatch: ${directArchitectureSvgHash}`);
}
const directArchitectureStructure = directArchitectureSvgText.replace(/<style>[\s\S]*?<\/style>/, "<style></style>");
const directArchitectureStructureHash = createHash("sha256").update(directArchitectureStructure).digest("hex");
if (directArchitectureStructureHash !== "caa00de89a242a2060ca56753e51b0348a643643195759c568ad2e243f8de6dd") {
  throw new Error(`direct HTTPS architecture SVG structure/content mismatch: ${directArchitectureStructureHash}`);
}
for (const marker of [
  'width="100%"',
  'viewBox="4 4 2056.10986328125 2463.699951171875"',
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
  "pcv_monitoring.db",
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
  "class udsApi,restApi,grpcApi,wsApi ingressNode;"
]) {
  if (!directArchitectureSource.includes(marker)) {
    throw new Error(`direct HTTPS Mermaid source contract missing: ${marker}`);
  }
}
if (/nginx/i.test(directArchitectureSource)) {
  throw new Error("NGINX found in direct HTTPS Mermaid source");
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
  'svg.setAttribute("aria-hidden", "true")'
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
  for (const excludedMarker of [
    "멀티 제어면 참고 기록",
    "Multi-control-plane notes",
    'class="pcv-directory-link" href="/ko/infrastructure/multi-control-plane-notes/"'
  ]) {
    if (source.includes(excludedMarker)) {
      throw new Error(`${name} excluded documentation entry found: ${excludedMarker}`);
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
  ".pcv-architecture-inline.pcv-is-inspecting .node:not(.pcv-is-related-node)",
  ".pcv-architecture-inline.pcv-is-inspecting .flowchart-link:not(.pcv-is-related-edge)",
  ".pcv-architecture-inline .flowchart-link.pcv-is-related-edge",
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
    "로컬 SQLite 파일 10개",
    "DB 사이의 원자성",
    "Audit DB",
    "Monitoring Evidence DB",
    "Web Push DB",
    "pcv_monitoring.db",
    "pcv_webpush.db",
    "config.backup",
    "스키마 변경 체크리스트"
  ]) {
    if (!page.includes(marker)) {
      throw new Error(`supplemental document body contract missing: ${marker}`);
    }
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
  '<h4 id="소스-배포의-lio-모듈-설정">소스 배포의 LIO 모듈 설정</h4>'
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
