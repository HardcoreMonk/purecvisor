import { copyFile, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const siteRoot = path.resolve(scriptDir, "..");
const repoRoot = path.resolve(siteRoot, "..");
const distRoot = path.join(siteRoot, "dist");
const docsSource = path.join(repoRoot, "ui", "docs.html");
const guideSource = path.join(repoRoot, "ui", "guide-content.md");
const iconSource = path.join(repoRoot, "ui", "icon-192.png");
const vendorSource = path.join(repoRoot, "ui", "vendor");
const docsTarget = path.join(distRoot, "docs.html");
const guideTarget = path.join(distRoot, "guide-content.md");
const vendorTarget = path.join(distRoot, "vendor");
const repoBlobBase = "https://github.com/HardcoreMonk/purecvisor/blob/main";

function publicSourceLink(target) {
  const hashIndex = target.indexOf("#");
  const filePart = hashIndex >= 0 ? target.slice(0, hashIndex) : target;
  const hashPart = hashIndex >= 0 ? target.slice(hashIndex) : "";
  const normalized = filePart.startsWith("../")
    ? filePart.slice(3)
    : path.posix.join("ui", filePart);
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

const docsHtml = (await readFile(docsSource, "utf8"))
  .replace('<base href="/ui/">', '<base href="/">')
  .replaceAll('href="/ui/"', 'href="/"')
  .replaceAll("운영 콘솔", "서비스 홈");
const guide = rewriteRelativeLinks(await readFile(guideSource, "utf8"));

await writeFile(docsTarget, docsHtml);
await writeFile(guideTarget, guide);
await copyFile(iconSource, path.join(distRoot, "icon-192.png"));
await rm(vendorTarget, { recursive: true, force: true });
await mkdir(path.join(vendorTarget, "pretendard", "woff2"), { recursive: true });
await mkdir(path.join(vendorTarget, "coolicons"), { recursive: true });
await copyFile(
  path.join(vendorSource, "pretendard", "pretendard.css"),
  path.join(vendorTarget, "pretendard", "pretendard.css")
);
await copyFile(
  path.join(vendorSource, "coolicons", "coolicons.svg"),
  path.join(vendorTarget, "coolicons", "coolicons.svg")
);

for (const file of [
  "Pretendard-Black.woff2",
  "Pretendard-Bold.woff2",
  "Pretendard-ExtraBold.woff2",
  "Pretendard-Medium.woff2",
  "Pretendard-Regular.woff2",
  "Pretendard-SemiBold.woff2"
]) {
  await copyFile(
    path.join(vendorSource, "pretendard", "woff2", file),
    path.join(vendorTarget, "pretendard", "woff2", file)
  );
}
