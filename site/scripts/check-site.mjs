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
const koreanHeroCopy = "VM, 컨테이너, ZFS 스토리지와 네트워크 가상화를 하나의 Linux/KVM 노드에서 운영합니다.";
const englishHeroCopy = "Operate VMs, containers, ZFS storage, and network virtualization on one Linux/KVM node.";
if (!index.includes("8개 작업 카테고리 · 22개 장")) throw new Error("landing taxonomy missing");
if ((index.match(/class="pcv-doc-category"/g) || []).length !== 8) {
  throw new Error("landing category count mismatch");
}
if (!index.includes('class="pcv-scene pcv-docs" id="documentation"')) {
  throw new Error("landing documentation scene missing");
}
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
    "하나의 노드,",
    "하나의 제어면",
    "소프트웨어 정의 네트워크를 하나의 Linux/KVM 노드에서 운영합니다.",
    "One node,",
    "one control plane",
    "software-defined networking on one Linux/KVM node",
    "pcv-final-cta",
    "pcv-final-title",
    "한 노드부터 명확하게 운영하세요.",
    "Operate clearly, starting with one node.",
    "pcv-map-resources"
  ]) {
    if (source.includes(marker)) throw new Error(`${name} removed landing scene found: ${marker}`);
  }
  const sectionIds = [...source.matchAll(/<section\b[^>]*\bid="([^"]+)"/g)].map((match) => match[1]);
  if (JSON.stringify(sectionIds) !== JSON.stringify(["_top", "documentation"])) {
    throw new Error(`${name} landing section order mismatch: ${sectionIds.join(" -> ")}`);
  }
  if (!source.includes('<figure class="pcv-control-map" aria-labelledby="pcv-capability-map-title">')) {
    throw new Error(`${name} accessible capability map missing`);
  }
  if (!source.includes('id="pcv-capability-map-title">Single Edge capability map</')) {
    throw new Error(`${name} capability map title missing`);
  }
  if ((source.match(/class="pcv-map-capability"/g) || []).length !== 4) {
    throw new Error(`${name} capability group count mismatch`);
  }
  for (const capability of [
    "KVM VM",
    "LXC",
    "iSCSI",
    "Linux Bridge",
    "OVS · OVN",
    "VLAN · QoS",
    "Local VPC",
    "VXLAN Overlay",
    "RBAC · Audit",
    "Jobs · Alerts",
    "Self-healing"
  ]) {
    if (!source.includes(capability)) throw new Error(`${name} capability missing: ${capability}`);
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
  ".pcv-map-core small"
]) {
  if (landingStyles.includes(selector)) throw new Error(`retired landing selector found: ${selector}`);
}
if (landingStyles.includes(".pcv-hero-copy h1")) {
  throw new Error("retired hero-scale heading selector found");
}
for (const [name, source, language, heroCopy, canonical] of [
  ["root", index, "ko", koreanHeroCopy, "https://purecvisor.site/"],
  ["korean", korean, "ko", koreanHeroCopy, "https://purecvisor.site/ko/"],
  ["english", english, "en", englishHeroCopy, "https://purecvisor.site/en/"]
]) {
  if (!source.includes(`<html lang="${language}"`)) throw new Error(`${name} language mismatch`);
  if ((source.match(/<h1\b/g) || []).length !== 1) throw new Error(`${name} H1 count mismatch`);
  if (!/<h1\b[^>]*id="pcv-hero-title"[^>]*>PURECVISOR 2\.0\.0 · SINGLE EDGE<\/h1>/.test(source)) {
    throw new Error(`${name} product H1 missing`);
  }
  if ((source.match(new RegExp(heroCopy.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "g")) || []).length !== 1) {
    throw new Error(`${name} hero copy mismatch`);
  }
  for (const capability of name === "english"
    ? ["Snapshots · Clones", "ZFS Pool · Zvol", "Backup · Restore", "Firewall · Security Groups"]
    : ["스냅샷 · 클론", "ZFS 풀 · Zvol", "백업 · 복원", "방화벽 · 보안 그룹"]) {
    if (!source.includes(capability)) throw new Error(`${name} localized capability missing: ${capability}`);
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
  if (!index.includes(`href="${chapter.path}"`)) {
    throw new Error(`korean landing chapter link missing: ${chapter.path}`);
  }
  if (!english.includes(`href="${chapter.path}"`)) {
    throw new Error(`english landing chapter link missing: ${chapter.path}`);
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
