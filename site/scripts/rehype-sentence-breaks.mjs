const sentenceContainers = new Set(["p", "li", "figcaption"]);
const excludedElements = new Set(["a", "code", "kbd", "pre", "samp", "script", "style", "svg", "table"]);

function splitSentenceText(value, previousTextValue = "") {
  const nodes = [];
  let offset = 0;
  for (const match of value.matchAll(/\.(?=\s+\S)/g)) {
    const previousCharacter = match.index > 0
      ? value[match.index - 1]
      : previousTextValue.at(-1) || "";
    if (/[0-9]/.test(previousCharacter)) continue;
    const end = match.index + 1;
    nodes.push({ type: "text", value: value.slice(offset, end) });
    nodes.push({ type: "element", tagName: "br", properties: {}, children: [] });
    offset = end;
  }
  if (offset === 0) return null;
  nodes.push({ type: "text", value: value.slice(offset) });
  return nodes;
}

function applySentenceBreaks(node, inSentenceContainer = false, excluded = false) {
  if (!node?.children) return;
  const nextExcluded = excluded || (node.type === "element" && excludedElements.has(node.tagName));
  const nextContainer = inSentenceContainer || (node.type === "element" && sentenceContainers.has(node.tagName));
  const children = [];
  let previousTextValue = "";
  for (const child of node.children) {
    if (child.type === "text" && nextContainer && !nextExcluded) {
      children.push(...(splitSentenceText(child.value, previousTextValue) || [child]));
      previousTextValue = child.value;
      continue;
    }
    applySentenceBreaks(child, nextContainer, nextExcluded);
    children.push(child);
    previousTextValue = child.type === "text" ? child.value : "";
  }
  node.children = children;
}

export default function rehypeSentenceBreaks() {
  return applySentenceBreaks;
}
