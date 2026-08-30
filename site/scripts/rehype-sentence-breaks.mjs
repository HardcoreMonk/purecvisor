const sentenceContainers = new Set(["p", "li", "figcaption"]);
const excludedElements = new Set(["a", "code", "kbd", "pre", "samp", "script", "style", "svg", "table"]);

function splitSentenceText(value) {
  const nodes = [];
  let offset = 0;
  for (const match of value.matchAll(/\.(?=\s+\S)/g)) {
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
  for (const child of node.children) {
    if (child.type === "text" && nextContainer && !nextExcluded) {
      children.push(...(splitSentenceText(child.value) || [child]));
      continue;
    }
    applySentenceBreaks(child, nextContainer, nextExcluded);
    children.push(child);
  }
  node.children = children;
}

export default function rehypeSentenceBreaks() {
  return applySentenceBreaks;
}
