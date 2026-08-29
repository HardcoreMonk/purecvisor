import { createHash } from "node:crypto";
import { readdir, readFile, stat } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import {
  guideChapters,
  guideEntryPath,
  guideGroups,
  guidePath,
  readerDocuments,
  supplementalDocuments
} from "./guide-routes.mjs";

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
  ...readerDocuments.map((document) => `${document.contentSlug}/index.html`)
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
const architectureAsset = "/assets/diagrams/purecvisor-single-full-architecture.svg";
const architectureSvg = await readFile(path.join(distRoot, architectureAsset));
const architectureSvgText = architectureSvg.toString("utf8");
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
  const architectureMapPosition = source.indexOf('<figure class="pcv-control-map pcv-architecture-source"');
  if (heroCopyPosition < 0 || architectureMapPosition < 0 || heroCopyPosition >= architectureMapPosition) {
    throw new Error(`${name} hero copy and architecture order mismatch`);
  }
  if (!source.includes(`<figure class="pcv-control-map pcv-architecture-source" aria-labelledby="pcv-architecture-map-title">`)) {
    throw new Error(`${name} source architecture figure missing`);
  }
  const mapTitle = name === "english" ? "Single Edge service architecture" : "Single Edge 서비스 아키텍처";
  if (!source.includes(`id="pcv-architecture-map-title">${mapTitle}</`)) {
    throw new Error(`${name} architecture map title missing`);
  }
  const sourceImage = `<img class="pcv-architecture-source-image" src="${architectureAsset}" width="1849.5234375" height="2798" loading="eager" decoding="async" alt="`;
  if (!source.includes(sourceImage)) {
    throw new Error(`${name} source architecture SVG image missing`);
  }
  if (!source.includes(`<a class="pcv-architecture-source-open" href="${architectureAsset}" target="_blank" rel="noopener">`)) {
    throw new Error(`${name} architecture source link missing`);
  }
  if (!source.includes(`<a class="pcv-architecture-source-canvas" href="${architectureAsset}" target="_blank" rel="noopener" aria-label="`)) {
    throw new Error(`${name} fitted architecture canvas link missing`);
  }
  if (!source.includes(`<ul class="pcv-architecture-legend" aria-label="`)) {
    throw new Error(`${name} architecture semantic color legend missing`);
  }
  for (const layer of ["clients", "config", "transport", "control", "domain", "persistent", "host"]) {
    if (!source.includes(`class="pcv-layer-key is-${layer}"`)) {
      throw new Error(`${name} architecture legend layer missing: ${layer}`);
    }
  }
  if ((source.match(/class="pcv-architecture-source-image"/g) || []).length !== 1) {
    throw new Error(`${name} architecture source image count mismatch`);
  }
  const architectureMap = source.match(/<figure class="pcv-control-map pcv-architecture-source"[\s\S]*?<\/figure>/)?.[0] || "";
  for (const staleMarker of [
    "data-active-path",
    "data-pcv-path-output",
    "pcv-arch-layer",
    "pcv-arch-node",
    "pcv-arch-path-trigger",
    "aria-pressed",
    "role=\"toolbar\"",
    "pcv-architecture-source-viewport",
    "Multi Edge"
  ]) {
    if (architectureMap.includes(staleMarker)) {
      throw new Error(`${name} stale reconstructed architecture marker found: ${staleMarker}`);
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
  ".pcv-architecture-source-viewport"
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
  ".pcv-architecture-legend {\n  display: grid;",
  "grid-template-columns: repeat(7, minmax(0, 1fr));",
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
  [".pcv-architecture-legend", ["grid-template-columns: repeat(7, minmax(0, 1fr));", "list-style: none;"]],
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
for (const interactionContract of [
  ".pcv-architecture-source-open:is(:hover, :focus-visible)",
  ".pcv-architecture-source-canvas:focus-visible",
  "@media (prefers-reduced-motion: reduce)",
  "animation-iteration-count: 1 !important"
]) {
  if (!landingStyles.includes(interactionContract)) {
    throw new Error(`source architecture interaction contract missing: ${interactionContract}`);
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
  for (const sourceContract of name === "english"
    ? ["Source SVG structure", "Open enlarged", "single-process purecvisorsd control plane", "fitted to the current width", "Monitoring Source v2", "optional DPDK lifecycle", "audit-only BPF LSM boundary", "External entry points", "Configuration/secrets", "Protocol boundary", "Core control flow", "Business logic", "State storage", "Physical/kernel resources"]
    : ["SVG 원본 구조", "확대해서 보기", "purecvisorsd 단일 프로세스", "현재 화면 폭에 맞춰 표시", "Monitoring Source v2", "선택형 DPDK 수명주기", "audit-only BPF LSM 경계", "외부 진입점", "설정/비밀", "프로토콜 경계", "핵심 제어 흐름", "비즈니스 로직", "상태 저장", "물리/커널 자원"]) {
    if (!source.includes(sourceContract)) throw new Error(`${name} localized source architecture contract missing: ${sourceContract}`);
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
if (!installation.includes("2.1 시스템 요구사항") || installation.includes("fetch(")) {
  throw new Error("installation body is not statically rendered");
}
for (const marker of [
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
