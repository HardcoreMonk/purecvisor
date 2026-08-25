import { readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const svgPath = path.join(scriptDir, "..", "public", "assets", "diagrams", "purecvisor-single-full-architecture.svg");
const start = "#my-svg .pcv-semantic-layer-colors-start{}";
const end = "#my-svg .pcv-semantic-layer-colors-end{}";
const shapes = ["rect", "polygon", "ellipse", "circle", "path"];
const palettes = [
  [".clientNode", "#e7f3ff", "#3a78b8", "#0d3454"],
  [".ingressNode", "#f3edff", "#7650a8", "#2d1b46"],
  [".runtimeNode", "#e6f7ed", "#27845d", "#123b2a"],
  [".domainNode", "#e5f7f6", "#287f7a", "#123b39"],
  [".dataNode", "#fff4e8", "#b87543", "#4d2d17"],
  [".hostNode", "#f0f2f5", "#667085", "#1d2939"],
  ["#my-svg-flowchart-configStore-6", "#ffebd8", "#c45a0a", "#5b2c08"]
];
const clusterPalettes = [
  ["#my-svg-clients", "#f5faff", "#92b9dd"],
  ["#my-svg-edgeGateway", "#faf7ff", "#a78acb"],
  ["#my-svg-bootInputs", "#fff8f1", "#dc9360"],
  ["#my-svg-daemon", "#ffffff", "#cbd3dd"],
  ["#my-svg-transports", "#faf7ff", "#a78acb"],
  ["#my-svg-core", "#f3fbf6", "#74b293"],
  ["#my-svg-domains", "#f2fbfa", "#6aaba7"],
  ["#my-svg-persistence", "#fff9f3", "#d49a72"],
  ["#my-svg-host", "#f7f8fa", "#98a2b3"]
];

const nodeRules = palettes.flatMap(([selector, fill, stroke, text]) => {
  const scope = `#my-svg ${selector}`;
  return [
    `${shapes.map((shape) => `${scope} ${shape}`).join(",")}{fill:${fill}!important;stroke:${stroke}!important;color:${text}!important;}`,
    `${scope} tspan{fill:${text}!important;}`
  ];
});
const clusterRules = clusterPalettes.map(([selector, fill, stroke]) =>
  `#my-svg ${selector}>rect{fill:${fill}!important;stroke:${stroke}!important;}`
);
const semanticStyles = `${start}${nodeRules.join("")}${clusterRules.join("")}${end}`;
const source = await readFile(svgPath, "utf8");
const escapedStart = start.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
const escapedEnd = end.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
const base = source.replace(new RegExp(`${escapedStart}[\\s\\S]*?${escapedEnd}`), "");
const output = base.replace("</style>", `${semanticStyles}</style>`);

if (output === base) throw new Error("architecture SVG style element missing");
if (output !== source) await writeFile(svgPath, output);
