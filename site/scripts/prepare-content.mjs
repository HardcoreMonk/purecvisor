import { copyFile, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const siteRoot = path.resolve(scriptDir, "..");
const repoRoot = path.resolve(siteRoot, "..");
const guideSource = path.join(repoRoot, "docs", "GUIDE.md");
const docsTarget = path.join(siteRoot, "src", "content", "docs", "docs.md");
const retiredTarget = path.join(siteRoot, "src", "content", "docs", "guide.md");
const fontSource = path.join(repoRoot, "ui", "vendor", "pretendard", "woff2");
const fontTarget = path.join(siteRoot, "public", "assets", "pretendard");
const repoBlobBase = "https://github.com/HardcoreMonk/purecvisor/blob/main";

function publicSourceLink(target) {
  const hashIndex = target.indexOf("#");
  const filePart = hashIndex >= 0 ? target.slice(0, hashIndex) : target;
  const hashPart = hashIndex >= 0 ? target.slice(hashIndex) : "";
  const normalized = filePart.startsWith("../")
    ? filePart.slice(3)
    : path.posix.join("docs", filePart);
  return `${repoBlobBase}/${normalized}${hashPart}`;
}

function rewriteRelativeLinks(markdown) {
  return markdown.replace(/\]\(([^)]+)\)/g, (match, target) => {
    if (
      target.startsWith("#") ||
      target.startsWith("/") ||
      /^[a-z][a-z0-9+.-]*:/i.test(target)
    ) {
      return match;
    }
    return `](${publicSourceLink(target)})`;
  });
}

const rawGuide = await readFile(guideSource, "utf8");
const guideBody = rewriteRelativeLinks(rawGuide.replace(/^# .+\r?\n+/, ""));
const guideFrontmatter = `---
title: PureCVisor Single Edge 운영 가이드
description: PureCVisor 2.0.0 설치, 운영, API, 보안과 품질 게이트 가이드
tableOfContents:
  minHeadingLevel: 2
  maxHeadingLevel: 2
---

`;

await rm(retiredTarget, { force: true });
await mkdir(path.dirname(docsTarget), { recursive: true });
await writeFile(docsTarget, guideFrontmatter + guideBody);
await rm(fontTarget, { recursive: true, force: true });
await mkdir(fontTarget, { recursive: true });

for (const file of [
  "Pretendard-Regular.woff2",
  "Pretendard-Medium.woff2",
  "Pretendard-SemiBold.woff2",
  "Pretendard-Bold.woff2"
]) {
  await copyFile(path.join(fontSource, file), path.join(fontTarget, file));
}
