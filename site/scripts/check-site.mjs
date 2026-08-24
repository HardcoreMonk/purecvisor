import { readdir, readFile, stat } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { guideChapters, guideEntryPath, guideGroups, guidePath } from "./guide-routes.mjs";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const siteRoot = path.resolve(scriptDir, "..");
const distRoot = path.join(siteRoot, "dist");
const requiredFiles = [
  "index.html",
  "ko/index.html",
  "en/index.html",
  "docs.html",
  "favicon.svg",
  ...guideChapters.map((chapter) => `${chapter.contentSlug}/index.html`)
];
const forbiddenText = [
  ["HardcoreMonk", "purecvisor-single"].join("/"),
  ["Private", "repository"].join(" "),
  ["192", "168", "3", "51"].join("."),
  ["192", "168", "3", "53"].join(".")
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
const landingStyles = await readFile(path.join(siteRoot, "src", "styles", "custom.css"), "utf8");
const architectureInteraction = await readFile(
  path.join(siteRoot, "src", "components", "ArchitectureMapScript.astro"),
  "utf8"
);
const koreanHeroTitle = "하나의 Linux/KVM 노드, 하나의 제어면.";
const englishHeroTitle = "One Linux/KVM node. One control plane.";
const koreanHeroCopy = "VM, 컨테이너, ZFS 스토리지와 네트워크 가상화를 한곳에서 운영합니다.";
const englishHeroCopy = "Operate VMs, containers, ZFS storage, and network virtualization in one place.";
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
    "id=\"documentation\"",
    "pcv-docs-title",
    "pcv-doc-shortcuts",
    "pcv-doc-directory",
    "pcv-doc-category",
    "pcv-role-paths",
    "필요한 작업에서 시작하세요.",
    "Start with the task you need.",
    "8개 작업 카테고리 · 22개 장",
    "8 task categories and 22 chapters",
    "RECOMMENDED PATHS"
  ]) {
    if (source.includes(marker)) throw new Error(`${name} removed landing scene found: ${marker}`);
  }
  const sectionIds = [...source.matchAll(/<section\b[^>]*\bid="([^"]+)"/g)].map((match) => match[1]);
  if (JSON.stringify(sectionIds) !== JSON.stringify(["_top"])) {
    throw new Error(`${name} landing section order mismatch: ${sectionIds.join(" -> ")}`);
  }
  const heroCopyPosition = source.indexOf('<div class="pcv-hero-copy">');
  const architectureMapPosition = source.indexOf('<figure class="pcv-control-map"');
  if (heroCopyPosition < 0 || architectureMapPosition < 0 || heroCopyPosition >= architectureMapPosition) {
    throw new Error(`${name} hero copy and architecture order mismatch`);
  }
  if (!source.includes('<figure class="pcv-control-map" data-active-path="workloads" aria-labelledby="pcv-architecture-map-title">')) {
    throw new Error(`${name} accessible architecture map missing`);
  }
  const mapTitle = name === "english" ? "Single Edge service architecture" : "Single Edge 서비스 아키텍처";
  if (!source.includes(`id="pcv-architecture-map-title">${mapTitle}</`)) {
    throw new Error(`${name} architecture map title missing`);
  }
  if ((source.match(/class="pcv-arch-layer pcv-arch-layer-/g) || []).length !== 5) {
    throw new Error(`${name} architecture layer count mismatch`);
  }
  if ((source.match(/class="pcv-arch-node pcv-arch-link pcv-arch-access-link"/g) || []).length !== 3) {
    throw new Error(`${name} architecture access link count mismatch`);
  }
  if ((source.match(/class="pcv-arch-path-trigger"/g) || []).length !== 5) {
    throw new Error(`${name} architecture path control count mismatch`);
  }
  if ((source.match(/class="pcv-arch-path-doc pcv-arch-path-/g) || []).length !== 5) {
    throw new Error(`${name} architecture capability guide link count mismatch`);
  }
  if ((source.match(/aria-pressed="true"/g) || []).length !== 1
    || (source.match(/aria-pressed="false"/g) || []).length !== 4) {
    throw new Error(`${name} architecture path selection state mismatch`);
  }
  if ((source.match(/data-pcv-path-output="true"/g) || []).length !== 2
    || !source.includes('role="status" aria-live="polite"')) {
    throw new Error(`${name} architecture path live output missing`);
  }
  const runtimeLayer = source.match(/<section class="pcv-arch-layer pcv-arch-layer-runtime"[\s\S]*?<\/section>/)?.[0] || "";
  const hostLayer = source.match(/<section class="pcv-arch-layer pcv-arch-layer-host"[\s\S]*?<\/section>/)?.[0] || "";
  if ((runtimeLayer.match(/class="pcv-arch-node /g) || []).length !== 16) {
    throw new Error(`${name} architecture runtime node count mismatch`);
  }
  if ((hostLayer.match(/class="pcv-arch-node /g) || []).length !== 9) {
    throw new Error(`${name} architecture host node count mismatch`);
  }
  for (const icon of ["workloads", "network", "storage", "security", "operations"]) {
    if (!source.includes(`class="pcv-arch-icon pcv-arch-icon-${icon}" aria-hidden="true"`)) {
      throw new Error(`${name} architecture service icon missing: ${icon}`);
    }
  }
  for (const [className, href] of [
    ["pcv-arch-node pcv-arch-link pcv-arch-access-link", "/ko/interfaces/web-ui/"],
    ["pcv-arch-node pcv-arch-link pcv-arch-access-link", "/ko/interfaces/rest-api/"],
    ["pcv-arch-node pcv-arch-link pcv-arch-access-link", "/ko/interfaces/cli/"],
    ["pcv-arch-path-doc pcv-arch-path-workloads", "/ko/workloads/virtual-machines/"],
    ["pcv-arch-path-doc pcv-arch-path-network", "/ko/infrastructure/networking/"],
    ["pcv-arch-path-doc pcv-arch-path-storage", "/ko/infrastructure/storage/"],
    ["pcv-arch-path-doc pcv-arch-path-security", "/ko/security/security/"],
    ["pcv-arch-path-doc pcv-arch-path-operations", "/ko/operations/monitoring-alerts/"]
  ]) {
    if (!source.includes(`<a class="${className}" href="${href}"`)) {
      throw new Error(`${name} architecture guide link missing: ${href}`);
    }
  }
  if ((source.match(/href="\/ko\/infrastructure\/networking\/"/g) || []).length < 1) {
    throw new Error(`${name} network architecture guide link count mismatch`);
  }
  for (const capability of [
    "01 · ACCESS",
    "02 · CONTROL PLANE",
    "03 · CAPABILITY SERVICES",
    "04 · STATE &amp; ADAPTERS",
    "05 · LINUX/KVM HOST",
    "purecvisorsd",
    "C23 · GMainLoop",
    "Bootstrap · lifecycle · hot reload",
    "DAEMON TLS",
    "NGINX OPT-IN",
    "JSON-RPC 2.0",
    "libsoup3 · TLS",
    "protobuf-c",
    "DISPATCH GATE",
    "O(1) method route",
    "RBAC · owner scope",
    "SYNC COMPLETION · ADR-0038",
    "Canonical response envelope",
    "ASYNC COMPLETION · ADR-0018",
    "Accepted Job ID → GTask worker",
    "accepted ≠ success",
    "libvirt · KVM/QEMU",
    "Linux Bridge · OVS/OVN",
    "ZFS · LIO · open-iscsi",
    "nftables · Suricata",
    "SQLite WAL",
    "cgroups · PSI"
  ]) {
    if (!source.includes(capability)) throw new Error(`${name} architecture capability missing: ${capability}`);
  }
  const architectureMap = source.match(/<figure class="pcv-control-map"[\s\S]*?<\/figure>/)?.[0] || "";
  for (const forbiddenCapability of [
    "purecvisord",
    "C11 + GMainLoop",
    "v0.9.5",
    "data-path=\"fabric\"",
    "data-path=\"vpc\"",
    "role=\"toolbar\"",
    "04 · RUNTIME ADAPTERS",
    "etcd cluster",
    "VM migrate",
    "Multi Edge"
  ]) {
    if (architectureMap.includes(forbiddenCapability)) {
      throw new Error(`${name} stale architecture capability found: ${forbiddenCapability}`);
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
  ".pcv-feature-index"
]) {
  if (landingStyles.includes(selector)) throw new Error(`retired landing selector found: ${selector}`);
}
if (landingStyles.includes(".pcv-hero-copy h1")) {
  throw new Error("retired hero-scale heading selector found");
}
if (landingStyles.includes("grid-template-columns: minmax(0, 1.02fr)")) {
  throw new Error("retired two-column hero layout found");
}
for (const layoutContract of [
  "grid-template-columns: minmax(0, 1fr);",
  "white-space: nowrap;",
  ".pcv-hero-lead {\n    white-space: normal;",
  ".pcv-arch-grid-access {\n  grid-template-columns: repeat(5, minmax(0, 1fr));",
  ".pcv-arch-grid-transport {\n  grid-template-columns: repeat(4, minmax(0, 1fr));",
  ".pcv-arch-grid-services {\n  grid-template-columns: repeat(5, minmax(0, 1fr));",
  ".pcv-arch-completion-grid {\n  display: grid;\n  grid-template-columns: repeat(2, minmax(0, 1fr));",
  ".pcv-arch-runtime-split {\n  display: grid;\n  grid-template-columns: minmax(0, 1.25fr) minmax(0, 0.75fr);",
  ".pcv-arch-grid-runtime {\n  grid-template-columns: repeat(auto-fit, minmax(10rem, 1fr));",
  ".pcv-arch-grid-services {\n    grid-template-columns: repeat(2, minmax(0, 1fr));"
]) {
  if (!landingStyles.includes(layoutContract)) {
    throw new Error(`bottom architecture layout contract missing: ${layoutContract}`);
  }
}
for (const themeContract of [
  "--pcv-hero-bg: #ffffff;",
  "--pcv-map-bg: #eef3f7;",
  "--pcv-map-node: #ffffff;",
  "--pcv-hero-bg: #050a14;",
  "--pcv-map-bg: #09101d;",
  "--pcv-map-node: #0f1c2d;"
]) {
  if (!landingStyles.includes(themeContract)) {
    throw new Error(`landing theme contract missing: ${themeContract}`);
  }
}
for (const [pathName, color] of [
  ["workloads", "#94dbff"],
  ["network", "#8ce3bd"],
  ["storage", "#d6ca6f"],
  ["security", "#ffa3c2"],
  ["operations", "#ddccff"]
]) {
  for (const contract of [
    `--pcv-path-${pathName}: ${color};`,
    `.pcv-control-map[data-active-path="${pathName}"]`,
    `--pcv-path-active: var(--pcv-path-${pathName});`
  ]) {
    if (!landingStyles.includes(contract)) {
      throw new Error(`architecture path theme contract missing: ${pathName}`);
    }
  }
}
for (const visualContract of [
  "border-radius: 2rem;",
  ".pcv-arch-active-route {",
  "border-radius: 1rem;",
  "box-shadow: 6px 6px 0 color-mix(in srgb, var(--pcv-path-active) 32%, transparent);",
  "font-family: var(--sl-font);",
  "background-size: 1.5rem 1.5rem;"
]) {
  if (!landingStyles.includes(visualContract)) {
    throw new Error(`architecture reference-lock contract missing: ${visualContract}`);
  }
}
for (const [selector, declarations] of [
  [".pcv-control-map", ["font-family: var(--sl-font);"]],
  [".pcv-map-bar", ["font-size: 0.8125rem;", "min-height: 3.5rem;"]],
  [".pcv-arch-layer-index", ["font-size: 0.75rem;"]],
  [".pcv-arch-layer-head small", ["font-size: 0.75rem;"]],
  [".pcv-arch-node", ["font-size: 0.75rem;", "min-height: 3.5rem;"]],
  [".pcv-arch-node strong", ["font-size: 0.8125rem;"]],
  [".pcv-arch-node small", ["font-size: 0.75rem;"]],
  [".pcv-arch-stage-label", ["font-family: var(--sl-font-mono);", "font-size: 0.75rem;"]],
  [".pcv-arch-path-trigger", ["min-height: 5rem;"]],
  [".pcv-arch-path-trigger .pcv-arch-node-copy strong", ["font-size: 0.875rem;"]],
  [".pcv-arch-path-trigger .pcv-arch-node-copy small", ["font-size: 0.75rem;"]]
]) {
  const start = landingStyles.indexOf(`${selector} {`);
  const end = start < 0 ? -1 : landingStyles.indexOf("}", start);
  const block = end < 0 ? "" : landingStyles.slice(start, end);
  for (const declaration of declarations) {
    if (!block.includes(declaration)) {
      throw new Error(`architecture readability contract missing: ${selector} ${declaration}`);
    }
  }
}
for (const motionContract of [
  ".pcv-arch-link:focus-visible",
  ".pcv-arch-path-trigger:focus-visible",
  ".pcv-arch-path-trigger[aria-pressed=\"true\"]",
  ":has(:is(.pcv-arch-link, .pcv-arch-path-trigger):is(:hover, :focus-visible))",
  "@keyframes pcv-arch-flow-line",
  "@keyframes pcv-arch-flow-y",
  "@keyframes pcv-arch-node-scan",
  "@keyframes pcv-arch-node-signal",
  "@keyframes pcv-arch-icon-draw",
  "@media (prefers-reduced-motion: reduce)",
  "animation-iteration-count: 1 !important"
]) {
  if (!landingStyles.includes(motionContract)) {
    throw new Error(`architecture map motion contract missing: ${motionContract}`);
  }
}
for (const interactionContract of [
  'map.dataset.activePath = path;',
  'item.setAttribute("aria-pressed", active ? "true" : "false");',
  'event.key === "ArrowRight"',
  'event.key === "ArrowDown"',
  'event.key === "ArrowLeft"',
  'event.key === "ArrowUp"',
  'event.key === "Home"',
  'event.key === "End"'
]) {
  if (!architectureInteraction.includes(interactionContract)) {
    throw new Error(`architecture interaction contract missing: ${interactionContract}`);
  }
}
for (const [name, source, language, heroTitle, heroCopy, canonical] of [
  ["root", index, "ko", koreanHeroTitle, koreanHeroCopy, "https://purecvisor.site/"],
  ["korean", korean, "ko", koreanHeroTitle, koreanHeroCopy, "https://purecvisor.site/ko/"],
  ["english", english, "en", englishHeroTitle, englishHeroCopy, "https://purecvisor.site/en/"]
]) {
  if (!source.includes(`<html lang="${language}"`)) throw new Error(`${name} language mismatch`);
  if ((source.match(/<h1\b/g) || []).length !== 1) throw new Error(`${name} H1 count mismatch`);
  if (!source.includes(`<p class="pcv-eyebrow">PURECVISOR 2.0.0 · SINGLE EDGE</p>`)) {
    throw new Error(`${name} product eyebrow missing`);
  }
  if (!source.includes(`<h1 class="pcv-hero-title" id="pcv-hero-title">${heroTitle}</h1>`)) {
    throw new Error(`${name} value H1 missing`);
  }
  if ((source.match(new RegExp(heroCopy.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "g")) || []).length !== 1) {
    throw new Error(`${name} hero copy mismatch`);
  }
  for (const capability of name === "english"
    ? ["Management and observability", "single process", "Select a domain to inspect state and host boundaries", "Local persistence · Host integration", "Single-node platform boundary", "Workloads", "Network", "Storage", "Security", "Operations", "Actual result → Audit · Prometheus", "Worker result → Job DB · Audit · WebSocket"]
    : ["관리·관측 인터페이스", "단일 프로세스", "도메인을 선택해 상태와 호스트 경계 확인", "로컬 영속 상태 · 호스트 통합", "단일 노드 플랫폼 경계", "워크로드", "네트워크", "스토리지", "보안", "운영", "실제 결과 → Audit · Prometheus", "Worker 결과 → Job DB · Audit · WebSocket"]) {
    if (!source.includes(capability)) throw new Error(`${name} localized architecture capability missing: ${capability}`);
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
if (!korean.includes(`href="${guideEntryPath}">전체 운영 가이드</a>`)) {
  throw new Error("korean full operations guide link missing");
}
if (!english.includes(`href="${guideEntryPath}">Full operations guide</a>`)) {
  throw new Error("english full operations guide link missing");
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
  for (const linkedChapter of guideChapters) {
    if (!page.includes(`href="${linkedChapter.path}"`)) {
      throw new Error(`guide sidebar link missing: ${linkedChapter.path}`);
    }
  }
  if (!docs.includes(chapter.legacyAnchor) || !docs.includes(chapter.path)) {
    throw new Error(`legacy guide mapping missing: ${chapter.legacyAnchor}`);
  }
}

const installation = await readFile(
  path.join(distRoot, "ko", "getting-started", "installation", "index.html"),
  "utf8"
);
if (!installation.includes("2.1 시스템 요구사항") || installation.includes("fetch(")) {
  throw new Error("installation body is not statically rendered");
}
if (!installation.includes(`href="${guidePath(1)}"`) || !installation.includes(`href="${guidePath(3)}"`)) {
  throw new Error("installation pagination contract missing");
}

process.stdout.write(`pages artifact verified: ${outputFiles.length} files\n`);
