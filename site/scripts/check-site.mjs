import { createHash } from "node:crypto";
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
  "assets/diagrams/purecvisor-single-full-architecture.svg",
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
const architectureAsset = "/assets/diagrams/purecvisor-single-full-architecture.svg";
const architectureSvg = await readFile(path.join(distRoot, architectureAsset));
const architectureSvgText = architectureSvg.toString("utf8");
const architectureSvgHash = createHash("sha256").update(architectureSvg).digest("hex");
if (architectureSvgHash !== "0890224b4854f36dfb9b7dc6ae4be78b855fa9623a97d7ba2fbffb1edf7d9ca1") {
  throw new Error(`architecture source SVG checksum mismatch: ${architectureSvgHash}`);
}
for (const marker of [
  'width="1885.3125"',
  'height="2845.599853515625"',
  'viewBox="4 4 1885.3125 2845.599853515625"',
  'role="graphics-document document"',
  'aria-roledescription="flowchart-elk"'
]) {
  if (!architectureSvgText.includes(marker)) throw new Error(`architecture source SVG contract missing: ${marker}`);
}
for (const marker of [/<script\b/i, /<foreignObject\b/i, /\bon\w+\s*=/i, /\b(?:xlink:)?href\s*=/i]) {
  if (marker.test(architectureSvgText)) throw new Error(`unsafe architecture source SVG marker: ${marker}`);
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
  const sourceImage = `<img class="pcv-architecture-source-image" src="${architectureAsset}" width="1885.3125" height="2845.599853515625" loading="eager" decoding="async" alt="`;
  if (!source.includes(sourceImage)) {
    throw new Error(`${name} source architecture SVG image missing`);
  }
  if (!source.includes(`<a class="pcv-architecture-source-open" href="${architectureAsset}" target="_blank" rel="noopener">`)) {
    throw new Error(`${name} architecture source link missing`);
  }
  if (!source.includes(`class="pcv-architecture-source-viewport" tabindex="0" role="region" aria-label="`)) {
    throw new Error(`${name} keyboard-scrollable architecture viewport missing`);
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
  ".pcv-arch-active-route"
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
  ".pcv-architecture-source-viewport {\n  height: min(72vh, 58rem);",
  "overflow: auto;",
  "scrollbar-gutter: stable both-edges;",
  ".pcv-architecture-source-image {\n  display: block;\n  width: 90rem;",
  "max-width: none;"
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
  [".pcv-architecture-source-viewport", ["height: min(72vh, 58rem);", "min-height: 38rem;", "overflow: auto;", "touch-action: pan-x pan-y;"]],
  [".pcv-architecture-source-image", ["width: 90rem;", "max-width: none;", "height: auto;", "background: #f8fafc;"]],
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
  ".pcv-architecture-source-viewport:focus-visible",
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
    ? ["Source SVG", "Open source", "single-process purecvisorsd control plane", "native SVG zoom"]
    : ["SVG 원본", "원본 열기", "purecvisorsd 단일 프로세스", "원본을 새 탭에서 확대"]) {
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
