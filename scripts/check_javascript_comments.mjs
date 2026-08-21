import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { parse } from "acorn";

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), "..");
const targets = [];

function walk(dir) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    const rel = path.relative(root, full).split(path.sep).join("/");
    if (entry.isDirectory()) {
      if (entry.name === ".astro" || entry.name === ".git" || entry.name === "dist" || entry.name === "node_modules" || rel === "ui/vendor" || rel.startsWith("ui/vendor/")) continue;
      walk(full);
      continue;
    }
    if (entry.isFile() && (entry.name.endsWith(".js") || entry.name.endsWith(".mjs"))) targets.push(full);
  }
}

walk(root);
const failures = [];
const strip = process.argv.includes("--strip");
for (const file of targets.sort()) {
  const source = fs.readFileSync(file, "utf8");
  const comments = [];
  parse(source, {
    ecmaVersion: "latest",
    sourceType: "module",
    allowHashBang: true,
    onComment: comments,
  });
  const meaningful = comments.filter((comment) => !(comment.start === 0 && source.startsWith("#!")));
  if (strip && meaningful.length) {
    let output = source;
    for (const comment of meaningful.slice().sort((a, b) => b.start - a.start)) {
      const blank = source.slice(comment.start, comment.end).replace(/[^\r\n]/g, " ");
      output = output.slice(0, comment.start) + blank + output.slice(comment.end);
    }
    fs.writeFileSync(file, output);
    continue;
  }
  if (meaningful.length) failures.push(`${path.relative(root, file)}: ${meaningful.length}`);
}

if (failures.length) {
  process.stderr.write(`${failures.join("\n")}\n`);
  process.exit(1);
}
if (strip) {
  process.stdout.write(`first-party JavaScript comments stripped (${targets.length} files)\n`);
  process.exit(0);
}
process.stdout.write(`first-party JavaScript comments: 0 (${targets.length} files)\n`);
