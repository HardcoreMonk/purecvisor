import { defineRouteMiddleware } from "@astrojs/starlight/route-data";
import NotFoundNote from "./components/NotFoundNote.astro";
import { readerDocuments } from "../scripts/guide-routes.mjs";

export const publishedPagePaths = new Set([
  "/",
  "/ko/",
  "/en/",
  ...readerDocuments.map((document) => document.path)
]);

export const onRequest = defineRouteMiddleware((context) => {
  const route = context.locals.starlightRoute;
  route.head = route.head.filter((entry) => {
    if (entry.tag !== "link" || entry.attrs?.rel !== "alternate") return true;
    const href = entry.attrs.href;
    return typeof href === "string" && publishedPagePaths.has(new URL(href).pathname);
  });
  if (route.entry.id === "404") route.Content = NotFoundNote;
});
