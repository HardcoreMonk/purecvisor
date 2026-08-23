import { writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { guideChapters, guideEntryPath, guidePath } from "./guide-routes.mjs";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const siteRoot = path.resolve(scriptDir, "..");
const docsTarget = path.join(siteRoot, "dist", "docs.html");
const legacyRoutes = Object.fromEntries(
  guideChapters.map((chapter) => [chapter.legacyAnchor, chapter.path])
);

legacyRoutes["14-5분-퀵스타트"] = guidePath(1, "#14-5분-퀵스타트");
legacyRoutes["12-아키텍처-개요"] = guidePath(1, "#12-아키텍처-개요");

const redirect = `<!doctype html>
<html lang="ko">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PureCVisor 운영 가이드로 이동</title>
<link rel="canonical" href="https://purecvisor.site${guideEntryPath}">
<meta http-equiv="refresh" content="0;url=${guideEntryPath}">
<script>
const routes=${JSON.stringify(legacyRoutes)};
const hash=decodeURIComponent(location.hash.slice(1));
location.replace(routes[hash]||${JSON.stringify(guideEntryPath)});
</script>
</head>
<body>
<main><p><a href="${guideEntryPath}">PureCVisor 운영 가이드로 이동</a></p></main>
</body>
</html>
`;

await writeFile(docsTarget, redirect);
