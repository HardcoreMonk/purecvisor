const transportNodes = Object.freeze(["udsApi", "restApi", "grpcApi", "wsApi"]);
const controlNodes = Object.freeze([
  "mainLoop",
  "dispatcher",
  "handlers",
  "syncPath",
  "workerPool",
  "completion"
]);
const domainNodes = Object.freeze([
  "workloadDomain",
  "networkDomain",
  "storageDomain",
  "securityDomain",
  "monitorDomain",
  "opsDomain"
]);

export const architectureLayerNodeIds = Object.freeze({
  clients: Object.freeze(["webUi", "pcvctl", "apiClient", "grpcClient", "promClient"]),
  bootInputs: Object.freeze(["configStore", "kernelLsm"]),
  edgeGateway: Object.freeze(["nginx"]),
  transports: transportNodes,
  core: controlNodes,
  domains: domainNodes,
  daemon: Object.freeze([...transportNodes, ...controlNodes, ...domainNodes]),
  persistence: Object.freeze(["coreDb", "operationsDb", "desiredStore"]),
  host: Object.freeze([
    "virtPlatform",
    "storagePlatform",
    "networkPlatform",
    "securityPlatform",
    "accelPlatform"
  ])
});

const resetBySvg = new WeakMap();

export function resolveArchitectureEdge(rawId, nodeIds) {
  if (typeof rawId !== "string") return null;
  const match = rawId.match(/^L_(.+)_([0-9]+)$/);
  if (!match) return null;
  const pair = match[1];
  const knownNodes = new Set(nodeIds);
  const sources = [...knownNodes].sort((left, right) => right.length - left.length);
  for (const from of sources) {
    const prefix = `${from}_`;
    if (!pair.startsWith(prefix)) continue;
    const to = pair.slice(prefix.length);
    if (knownNodes.has(to)) return { from, to };
  }
  return null;
}

function parseSvg(source) {
  const parsed = new DOMParser().parseFromString(source, "image/svg+xml");
  if (parsed.querySelector("parsererror")) return null;
  const svg = parsed.documentElement;
  if (svg.localName !== "svg" || svg.namespaceURI !== "http://www.w3.org/2000/svg") return null;
  if (svg.querySelector("script, foreignObject")) return null;

  const elements = [svg, ...svg.querySelectorAll("*")];
  for (const element of elements) {
    for (const attribute of element.getAttributeNames()) {
      if (/^on/i.test(attribute) || /^(?:href|xlink:href)$/i.test(attribute)) return null;
    }
  }
  for (const style of svg.querySelectorAll("style")) {
    const localReferencesRemoved = style.textContent.replace(/url\(\s*["']?#[^)]+\)/gi, "");
    if (/@import|url\(/i.test(localReferencesRemoved)) return null;
  }
  return svg;
}

function namespaceSvg(svg, instanceKey) {
  const safeKey = instanceKey.replace(/[^a-z0-9-]/gi, "-").toLowerCase();
  const prefix = `pcv-architecture-${safeKey}-`;
  const elements = [svg, ...svg.querySelectorAll("*")];
  const replacements = elements
    .filter((element) => element.id)
    .map((element) => [element.id, `${prefix}${element.id}`])
    .sort((left, right) => right[0].length - left[0].length);
  const idMap = new Map(replacements);

  function replaceHashReferences(value) {
    let next = value;
    for (const [from, to] of replacements) next = next.split(`#${from}`).join(`#${to}`);
    return next;
  }

  for (const style of svg.querySelectorAll("style")) {
    let value = replaceHashReferences(style.textContent);
    value = value
      .replaceAll("@keyframes edge-animation-frame", `@keyframes ${prefix}edge-animation-frame`)
      .replaceAll("animation:edge-animation-frame", `animation:${prefix}edge-animation-frame`)
      .replaceAll("@keyframes dash", `@keyframes ${prefix}dash`)
      .replaceAll("animation:dash", `animation:${prefix}dash`);
    style.textContent = value;
  }

  for (const element of elements) {
    for (const attribute of element.getAttributeNames()) {
      if (attribute === "id") continue;
      const value = element.getAttribute(attribute);
      if (value === null) continue;
      if (attribute === "aria-labelledby" || attribute === "aria-describedby") {
        element.setAttribute(
          attribute,
          value.split(/\s+/).map((id) => idMap.get(id) || id).join(" ")
        );
      } else {
        element.setAttribute(attribute, replaceHashReferences(value));
      }
    }
  }

  for (const element of elements) {
    if (element.id) element.id = idMap.get(element.id) || element.id;
  }
}

function annotateSvg(svg) {
  const nodes = [];
  const nodeIds = new Set();
  for (const node of svg.querySelectorAll("g.node[id]")) {
    const match = node.id.match(/^my-svg-flowchart-(.+)-[0-9]+$/);
    if (!match) continue;
    node.dataset.pcvNodeId = match[1];
    nodeIds.add(match[1]);
    nodes.push(node);
  }

  const edgeLabels = new Map();
  for (const label of svg.querySelectorAll(".edgeLabel .label[data-id]")) {
    const rawId = label.getAttribute("data-id");
    const group = label.closest(".edgeLabel");
    if (rawId && group) edgeLabels.set(rawId, group);
  }

  const edges = [];
  for (const edge of svg.querySelectorAll("path[data-edge='true'][data-id]")) {
    const rawId = edge.getAttribute("data-id");
    const endpoints = resolveArchitectureEdge(rawId, nodeIds);
    if (!rawId || !endpoints) continue;
    edge.dataset.pcvEdgeId = rawId;
    edge.dataset.pcvFrom = endpoints.from;
    edge.dataset.pcvTo = endpoints.to;
    const label = edgeLabels.get(rawId);
    if (label) label.dataset.pcvEdgeLabelId = rawId;
    edges.push(edge);
  }

  const layers = [];
  for (const layer of svg.querySelectorAll("g.cluster[id]")) {
    const match = layer.id.match(/^my-svg-(.+)$/);
    const members = match ? architectureLayerNodeIds[match[1]] : undefined;
    if (!match || !members?.some((nodeId) => nodeIds.has(nodeId))) continue;
    layer.dataset.pcvLayerId = match[1];
    layers.push(layer);
  }

  return { nodes, edges, edgeLabels, layers, nodeIds };
}

function attachInteraction(svg, topology) {
  let activeTarget = null;

  function clear() {
    activeTarget = null;
    svg.classList.remove("pcv-is-inspecting");
    for (const node of topology.nodes) {
      node.classList.remove("pcv-is-active-target", "pcv-is-related-node", "pcv-is-layer-member");
    }
    for (const layer of topology.layers) layer.classList.remove("pcv-is-active-target");
    for (const edge of topology.edges) edge.classList.remove("pcv-is-related-edge");
    for (const label of topology.edgeLabels.values()) label.classList.remove("pcv-is-related-edge-label");
  }

  function activate(target) {
    const nodeId = target.dataset.pcvNodeId;
    const layerId = target.dataset.pcvLayerId;
    const selectedNodes = new Set(
      nodeId
        ? [nodeId]
        : (architectureLayerNodeIds[layerId] || []).filter((member) => topology.nodeIds.has(member))
    );
    if (selectedNodes.size === 0) {
      clear();
      return;
    }

    activeTarget = target;
    const relatedNodes = new Set(selectedNodes);
    const relatedEdgeIds = new Set();
    for (const edge of topology.edges) {
      const from = edge.dataset.pcvFrom;
      const to = edge.dataset.pcvTo;
      const related = selectedNodes.has(from) || selectedNodes.has(to);
      edge.classList.toggle("pcv-is-related-edge", related);
      if (!related) continue;
      relatedNodes.add(from);
      relatedNodes.add(to);
      relatedEdgeIds.add(edge.dataset.pcvEdgeId);
    }

    for (const node of topology.nodes) {
      const id = node.dataset.pcvNodeId;
      node.classList.toggle("pcv-is-active-target", node === target);
      node.classList.toggle("pcv-is-related-node", relatedNodes.has(id));
      node.classList.toggle("pcv-is-layer-member", Boolean(layerId) && selectedNodes.has(id));
    }
    for (const layer of topology.layers) layer.classList.toggle("pcv-is-active-target", layer === target);
    for (const [edgeId, label] of topology.edgeLabels) {
      label.classList.toggle("pcv-is-related-edge-label", relatedEdgeIds.has(edgeId));
    }
    svg.classList.add("pcv-is-inspecting");
  }

  function findTarget(value) {
    if (!(value instanceof Element)) return null;
    const target = value.closest("[data-pcv-node-id], [data-pcv-layer-id]");
    return target && svg.contains(target) ? target : null;
  }

  svg.addEventListener("pointerover", (event) => {
    if (event.pointerType === "touch") return;
    const target = findTarget(event.target);
    if (!target || target === activeTarget) return;
    activate(target);
  });
  svg.addEventListener("pointerout", (event) => {
    if (event.pointerType === "touch" || !activeTarget) return;
    const nextTarget = findTarget(event.relatedTarget);
    if (nextTarget === activeTarget) return;
    if (nextTarget) activate(nextTarget);
    else clear();
  });
  svg.addEventListener("pointerleave", clear);
  svg.addEventListener("pointercancel", clear);
  resetBySvg.set(svg, clear);
}

async function enhanceArchitectureCanvas(canvas) {
  if (!(canvas instanceof HTMLAnchorElement)) return;
  if (canvas.dataset.pcvArchitectureState) return;
  const image = canvas.querySelector("img.pcv-overview-architecture-image");
  if (!(image instanceof HTMLImageElement)) return;
  const source = image.getAttribute("src");
  const instanceKey = canvas.dataset.pcvArchitectureInteractive;
  if (!source || !instanceKey) return;

  const sourceUrl = new URL(source, window.location.href);
  if (sourceUrl.origin !== window.location.origin || sourceUrl.pathname.split("/").pop()?.endsWith(".svg") !== true) {
    return;
  }

  canvas.dataset.pcvArchitectureState = "loading";
  try {
    const response = await fetch(sourceUrl, { credentials: "same-origin" });
    const contentType = response.headers.get("content-type")?.split(";", 1)[0].trim();
    if (!response.ok || contentType !== "image/svg+xml") throw new Error("invalid SVG response");
    const svg = parseSvg(await response.text());
    if (!svg) throw new Error("unsafe SVG response");
    const topology = annotateSvg(svg);
    if (topology.nodes.length === 0 || topology.edges.length === 0 || topology.layers.length === 0) {
      throw new Error("incomplete SVG topology");
    }
    namespaceSvg(svg, instanceKey);
    svg.classList.add("pcv-architecture-source-image", "pcv-architecture-inline");
    svg.setAttribute("aria-hidden", "true");
    svg.setAttribute("focusable", "false");
    svg.removeAttribute("role");
    svg.removeAttribute("aria-roledescription");
    attachInteraction(svg, topology);
    if (!image.isConnected || !canvas.isConnected) throw new Error("detached architecture canvas");
    image.replaceWith(svg);
    canvas.dataset.pcvArchitectureState = "ready";
  } catch {
    canvas.dataset.pcvArchitectureState = "fallback";
  }
}

export function enhanceArchitecturePanel(root) {
  if (!(root instanceof Element)) return Promise.resolve();
  return Promise.all(
    [...root.querySelectorAll("[data-pcv-architecture-interactive]")].map(enhanceArchitectureCanvas)
  );
}

export function resetArchitecturePanel(root) {
  if (!(root instanceof Element)) return;
  for (const svg of root.querySelectorAll("svg.pcv-architecture-inline")) resetBySvg.get(svg)?.();
}
