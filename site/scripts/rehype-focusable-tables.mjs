function addFocusableTables(node) {
  if (node?.type === "element" && node.tagName === "table") {
    node.properties = { ...node.properties, tabIndex: 0 };
  }
  for (const child of node?.children || []) addFocusableTables(child);
}

export default function rehypeFocusableTables() {
  return addFocusableTables;
}
