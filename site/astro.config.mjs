import { defineConfig } from "astro/config";
import starlight from "@astrojs/starlight";

export default defineConfig({
  site: "https://purecvisor.site",
  output: "static",
  trailingSlash: "always",
  build: {
    format: "directory"
  },
  vite: {
    build: {
      sourcemap: false
    }
  },
  integrations: [
    starlight({
      title: "PureCVisor",
      description: "Linux/KVM Single Edge 하이퍼바이저 운영 문서",
      favicon: "/favicon.svg",
      defaultLocale: "root",
      locales: {
        root: {
          label: "한국어 (기본)",
          lang: "ko"
        },
        ko: {
          label: "한국어",
          lang: "ko"
        },
        en: {
          label: "English",
          lang: "en"
        }
      },
      social: [
        {
          icon: "github",
          label: "GitHub",
          href: "https://github.com/HardcoreMonk/purecvisor"
        }
      ],
      customCss: ["./src/styles/custom.css"],
      components: {
        Header: "./src/components/Header.astro",
        PageTitle: "./src/components/PageTitle.astro"
      },
      head: [
        {
          tag: "meta",
          attrs: {
            name: "theme-color",
            content: "#ffffff"
          }
        }
      ],
      sidebar: [
        {
          label: "PureCVisor 2.0.0",
          items: [
            {
              label: "문서 홈",
              slug: ""
            },
            {
              label: "전체 운영 가이드",
              slug: "docs"
            }
          ]
        }
      ]
    })
  ]
});
