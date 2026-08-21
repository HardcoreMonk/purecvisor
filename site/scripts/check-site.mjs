import { readdir, readFile, stat } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const siteRoot = path.resolve(scriptDir, "..");
const distRoot = path.join(siteRoot, "dist");
const requiredFiles = ["index.html", "docs.html", "favicon.svg"];
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
  if (!/\.(?:css|html|js|json|svg|txt|xml)$/i.test(file)) continue;
  const source = await readFile(file, "utf8");
  if (/\.(?:css|js)$/i.test(file) && source.includes("sourceMappingURL")) {
    throw new Error(`source map reference found in ${path.relative(distRoot, file)}`);
  }
  for (const marker of forbiddenText) {
    if (source.includes(marker)) throw new Error(`${marker} found in ${path.relative(distRoot, file)}`);
  }
}

const index = await readFile(path.join(distRoot, "index.html"), "utf8");
const docs = await readFile(path.join(distRoot, "docs.html"), "utf8");
if (!index.includes("하나의 노드,") || !index.includes("하나의 제어면")) {
  throw new Error("landing content missing");
}
if (!index.includes("8개 작업 카테고리 · 22개 장")) throw new Error("landing taxonomy missing");
if (!index.includes("한 노드의 가상화 운영을")) throw new Error("service introduction missing");
if (!index.includes("Single Edge 시작 흐름")) throw new Error("quickstart scene missing");
if (!index.includes("Single Edge에 집중한 공개판")) throw new Error("public scope missing");
if ((index.match(/class="pcv-capability(?:\s|\")/g) || []).length !== 6) {
  throw new Error("service capability count mismatch");
}
if ((index.match(/class="pcv-doc-category"/g) || []).length !== 8) {
  throw new Error("landing category count mismatch");
}
if (index.includes("/guide.html")) throw new Error("retired guide.html link found");
if (!docs.includes("PureCVisor Single Edge 운영 가이드")) throw new Error("docs title missing");
if (!docs.includes("22. 품질 게이트 가이드")) throw new Error("docs content incomplete");

const chapterSlugs = [
  "1-시작하기",
  "2-설치-및-환경-구성",
  "3-vm-관리",
  "4-컨테이너-관리",
  "5-스토리지",
  "6-네트워크",
  "7-멀티-제어면-참고-기록",
  "8-모니터링--알림",
  "9-백업--복원",
  "10-보안",
  "11-클라우드-마이그레이션",
  "12-ai--자가치유",
  "13-web-ui",
  "14-rest-api",
  "15-cli-레퍼런스",
  "16-설정-레퍼런스",
  "17-트러블슈팅",
  "18-부록",
  "19-개발자--엔지니어-가이드",
  "20-영업--마케팅-가이드",
  "21-아키텍처-리팩토링-가이드",
  "22-품질-게이트-가이드"
];

for (const slug of chapterSlugs) {
  if (!index.includes(`/docs.html#${slug}`)) throw new Error(`landing chapter link missing: ${slug}`);
  if (!docs.includes(`id="${slug}"`)) throw new Error(`docs chapter anchor missing: ${slug}`);
}

process.stdout.write(`pages artifact verified: ${outputFiles.length} files\n`);
