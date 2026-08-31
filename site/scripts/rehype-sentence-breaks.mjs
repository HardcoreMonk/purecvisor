const sentenceContainers = new Set(["p", "li", "figcaption"]);
const excludedElements = new Set(["a", "code", "kbd", "pre", "samp", "script", "style", "svg", "table"]);

function splitSentenceText(value, previousTextValue = "", breakAtEnd = false) {
  const nodes = [];
  let offset = 0;
  const sentenceBoundary = breakAtEnd ? /\.(?=\s+\S|\s*$)/g : /\.(?=\s+\S)/g;
  for (const match of value.matchAll(sentenceBoundary)) {
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

function hasFollowingContent(children, index) {
  for (const child of children.slice(index + 1)) {
    if (child.type === "element" && child.tagName === "br") return false;
    if (child.type === "raw" && /^\s*<br\s*\/?>(?:\s*)$/i.test(child.value)) return false;
    if (child.type === "text" && child.value.trim()) return true;
    if (child.type !== "text") return true;
  }
  return false;
}

function applySentenceBreaks(node, inSentenceContainer = false, excluded = false) {
  if (!node?.children) return;
  const nextExcluded = excluded || (node.type === "element" && excludedElements.has(node.tagName));
  const nextContainer = inSentenceContainer || (node.type === "element" && sentenceContainers.has(node.tagName));
  const children = [];
  let previousTextValue = "";
  for (const [index, child] of node.children.entries()) {
    if (child.type === "text" && nextContainer && !nextExcluded) {
      children.push(...(
        splitSentenceText(child.value, previousTextValue, hasFollowingContent(node.children, index)) || [child]
      ));
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
