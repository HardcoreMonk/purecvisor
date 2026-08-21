import { readdir, readFile, stat } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const siteRoot = path.resolve(scriptDir, "..");
const distRoot = path.join(siteRoot, "dist");
const requiredFiles = ["index.html", "guide.html", "favicon.svg"];
const forbiddenText = [
  ["HardcoreMonk", "purecvisor-single"].join("/"),
  ["Private", "repository"].join(" ")
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
const guide = await readFile(path.join(distRoot, "guide.html"), "utf8");
if (!index.includes("하나의 노드, 하나의 제어면")) throw new Error("landing content missing");
if (!guide.includes("PureCVisor Single Edge 운영 가이드")) throw new Error("guide title missing");
if (!guide.includes("22. 품질 게이트 가이드")) throw new Error("guide content incomplete");

process.stdout.write(`pages artifact verified: ${outputFiles.length} files\n`);
