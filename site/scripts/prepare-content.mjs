import { copyFile, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { guideChapters, supplementalDocuments } from "./guide-routes.mjs";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const siteRoot = path.resolve(scriptDir, "..");
const repoRoot = path.resolve(siteRoot, "..");
const guideSource = path.join(repoRoot, "docs", "GUIDE.md");
const docsTarget = path.join(siteRoot, "src", "content", "docs", "docs.md");
const retiredTarget = path.join(siteRoot, "src", "content", "docs", "guide.md");
const koreanLandingSource = path.join(siteRoot, "src", "content", "docs", "index.mdx");
const koreanLandingTarget = path.join(siteRoot, "src", "content", "docs", "ko", "index.mdx");
const koreanDocsRoot = path.join(siteRoot, "src", "content", "docs", "ko");
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

function promoteSectionHeadings(markdown) {
  let inFence = false;
  return markdown
    .split(/\r?\n/)
    .map((line) => {
      if (/^\s*(?:`{3,}|~{3,})/.test(line)) {
        inFence = !inFence;
        return line;
      }
      if (inFence) return line;
      return line.replace(/^(#{3,6})\s/, (match, hashes) => `${hashes.slice(1)} `);
    })
    .join("\n");
}

const rawGuide = await readFile(guideSource, "utf8");
const chapterMatches = [...rawGuide.matchAll(/^## (\d+)\. (.+)$/gm)];
if (chapterMatches.length !== guideChapters.length) {
  throw new Error(`guide chapter count mismatch: ${chapterMatches.length}`);
}

await rm(retiredTarget, { force: true });
await rm(docsTarget, { force: true });
await rm(koreanDocsRoot, { recursive: true, force: true });
await mkdir(koreanDocsRoot, { recursive: true });
const koreanLanding = await readFile(koreanLandingSource, "utf8");
const koreanLandingImport = "../../components/DocumentationMap.astro";
if (!koreanLanding.includes(koreanLandingImport)) {
  throw new Error("korean landing documentation map import missing");
}
await writeFile(
  koreanLandingTarget,
  koreanLanding.replace(koreanLandingImport, "../../../components/DocumentationMap.astro")
);

for (const [index, match] of chapterMatches.entries()) {
  const chapter = guideChapters[index];
  const number = Number.parseInt(match[1], 10);
  const title = match[2].trim();
  if (number !== chapter.number || title !== chapter.title) {
    throw new Error(`guide chapter manifest mismatch: ${number}. ${title}`);
  }
  const start = match.index + match[0].length;
  const end = chapterMatches[index + 1]?.index ?? rawGuide.length;
  const body = promoteSectionHeadings(rewriteRelativeLinks(rawGuide.slice(start, end).trim()));
  const frontmatter = `---
title: ${JSON.stringify(chapter.title)}
description: PureCVisor Single Edge ${chapter.title} 운영 문서
tableOfContents:
  minHeadingLevel: 2
  maxHeadingLevel: 3
---

`;
  const target = path.join(koreanDocsRoot, chapter.directory, `${chapter.slug}.md`);
  await mkdir(path.dirname(target), { recursive: true });
  await writeFile(target, `${frontmatter}${body}\n`);
}

for (const document of supplementalDocuments) {
  const rawDocument = await readFile(path.join(repoRoot, "docs", document.source), "utf8");
  const heading = rawDocument.match(/^# (.+)$/m);
  if (!heading || heading.index !== 0 || heading[1].trim() !== document.sourceTitle) {
    throw new Error(`supplemental document title mismatch: ${document.source}`);
  }
  const body = rewriteRelativeLinks(rawDocument.slice(heading[0].length).trim());
  const frontmatter = `---
title: ${JSON.stringify(document.title)}
description: ${JSON.stringify(document.description)}
tableOfContents:
  minHeadingLevel: 2
  maxHeadingLevel: 3
---

`;
  const target = path.join(koreanDocsRoot, document.directory, `${document.slug}.md`);
  await mkdir(path.dirname(target), { recursive: true });
  await writeFile(target, `${frontmatter}${body}\n`);
}

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
