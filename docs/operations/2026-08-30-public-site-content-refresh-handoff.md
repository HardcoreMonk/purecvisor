# 2026-08-30 `purecvisor.site` 콘텐츠 현행화 인계

## 1. 변경 목적

2026-08-25 공개 랜딩의 구조·색상·탐색 흐름은 유지하면서 Single Edge 아키텍처 자산과 설명을
현재 제품 경계에 맞췄다. 기존 자산에 없던 Monitoring Source v2,
`pcv_monitoring.db`, 선택형 DPDK 수명주기와 audit-only BPF LSM 경계를 추가했다.

## 2. 변경 범위

- `site/public/assets/diagrams/purecvisor-single-full-architecture.svg`
- `site/src/content/docs/index.mdx`
- `site/src/content/docs/en/index.mdx`
- `site/scripts/check-site.mjs`
- `docs/PUBLIC_DOCUMENTATION_SITE.md`
- `docs/ui-reviews/2026-08-30-public-site-current-state-refresh.md`

Hero H1·lead·CTA, header disclosure, 7계층 palette·범례와 responsive CSS는 변경하지 않았다.
한국어·영어 alt와 figure note에는 DPDK·LSM의 host prerequisite와 audit-only 제한을 명시했다.

## 3. SVG 안전 처리

최신 Mermaid 기본 산출물은 `<foreignObject>` 60개를 사용해 공개 SVG 안전 계약에 맞지 않았다.
공식 Mermaid의 root `htmlLabels=false`, `markdownAutoWrap=false` 설정으로 같은 source를
`<text>/<tspan>` 기반으로 다시 렌더한 뒤 기존 semantic palette 변환을 적용했다.

| 항목 | 결과 |
|---|---:|
| file SHA-256 | `f8b155814b517425827570309782d43b4cf840140b20f6d9f1d3e404abc9f906` |
| `<style>` 제외 SHA-256 | `ad0ca4159c538fd7eb96ae8250a887ea5d1188faa3cef0bf194e2191675cf5db` |
| viewBox | `1936.322265625×2511.60009765625` |
| `<text>` / `<tspan>` | 60 / 326 |
| `<script>` / `<foreignObject>` / handler / 외부 link | 0 / 0 / 0 / 0 |

## 4. 검증

| 검증 | 결과 |
|---|---|
| `cd site && npm run check` | PASS, 26 pages·87 artifacts |
| `python3 scripts/check_design_md.py` | PASS |
| `make check-public-comments` | PASS, first-party source·JS comments 0 |
| `xmllint --noout` | PASS |
| `/`, `/ko/`, `/en/` × desktop/mobile × light/dark | PASS, 12조합 |
| 브라우저 오류·가로 overflow | 0 / 0 |
| Axe WCAG A/AA | violation 0 |
| `git diff --check` | PASS |

시각 캡처와 SHA-256, reference lock, 채택·기각 결정은
[제품 UI 리뷰](../ui-reviews/2026-08-30-public-site-current-state-refresh.md)에 기록했다.

## 5. 배포 경계

현재 판정은 로컬 `PASS`다. commit·push와 GitHub Pages workflow는 실행하지 않았다. live
`https://purecvisor.site` 반영 완료 판정에는 public main 포함, Pages job 성공, `/`, `/ko/`,
`/en/`과 SVG hash의 외부 재확인이 추가로 필요하다.
