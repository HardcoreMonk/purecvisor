# 데이터베이스 아키텍처 공개 문서 경로 UI 리뷰

> **일자:** 2026-08-30
> **판정:** LIVE-PASS — 로컬·GitHub Pages·custom domain 구조·시각·접근성 검증 완료
> **대상:** `/ko/development/database-architecture/`와 Starlight `개발·출시` navigation
> **관련 결정:** ADR-0046, ADR-0047

## 1. 설계 brief

PureCvisor Single Edge를 운영하거나 개발하는 사용자가 Web 문서에서 SQLite 저장소 10개의
책임, schema, cross-DB 일관성, 장애 축소 동작과 백업·복구 경계를 한 문서로 확인하게 한다.
정확하고 조밀한 기술 문서 톤을 유지하며, 가장 큰 위험은 `docs/DATABASE_STRUCTURE.md`와
Pages 본문이 이중 정본으로 갈라지거나 긴 문서를 새 시각 shell로 감싸 기존 탐색을 깨는 것이다.
Astro·Starlight, 한국어 정본, Single Edge 공개 경계와 공개 소스 정책을 제약으로 고정한다.

## 2. 변경 전 상태

- 공개 저장소의 `docs/DATABASE_STRUCTURE.md`는 GitHub source로만 존재하고
  `purecvisor.site`에는 독립 route가 없었다.
- 공개 reader는 `docs/GUIDE.md`에서 생성한 8개 그룹·22개 운영 가이드만 제공했다.
- 데이터베이스 설명은 현재 schema와 비교해 Audit hashchain, TOTP, overlay, Monitoring
  Evidence, Web Push와 장애·복구 경계가 오래된 상태였다.
- landing, Header와 reader의 색상·타이포그래피·3열 layout은 이번 변경 대상이 아니다.

## 3. 연구 근거와 reference lock

이번 변경은 승인된 문서 reader 안에 같은 형식의 article 한 건을 추가하는 후속 구현이다.
새 시각 방향을 설계하지 않고 다음 기존 연구를 그대로 승계했다.

| 근거 | 확인한 결정 | 이번 적용 |
|---|---|---|
| [`2026-08-24-operations-guide-routes.md`](2026-08-24-operations-guide-routes.md) | Refero Raycast Docs의 좌측 전체 navigation·중앙 680–820px 본문·우측 목차, Refero 1Password Docs의 독립 article·active sidebar·이전/다음 탐색 | DB 설명을 고유 directory route로 제공하고 같은 Starlight reader를 사용 |
| [`2026-08-22-public-product-docs-layout.md`](2026-08-22-public-product-docs-layout.md) | 검색, sidebar collapse, 긴 본문과 code overflow를 현재 Starlight shell이 소유 | 새 reader·CSS·client runtime을 추가하지 않음 |
| [`PUBLIC_DOCUMENTATION_SITE.md`](../PUBLIC_DOCUMENTATION_SITE.md) | 공개 source와 Pages artifact 분리, 한국어 정본, 내부 주소·private 표식 금지 | 별도 source를 build 시점에 결정적으로 변환하고 공개 금지 증거를 gate로 차단 |

Reference lock은 Starlight header, 좌측 그룹 navigation, 중앙 Markdown article, 우측 현재
목차, 하단 pagination, 검색과 mobile drawer다. 기존 token과 spacing을 그대로 보존한다.
보조 근거는 route와 sidebar 배치에만 사용하고 외부 브랜드·색상·문구는 차용하지 않는다.
승인된 후속 구현 범위이므로 새 Refero 평균값이나 별도 시각 옵션을 만들지 않았다.

## 4. 결정 ledger

| 결정 | 출처와 역할 | 이유 |
|---|---|---|
| `/ko/development/database-architecture/` 고유 route | 기존 언어·분류·문서 URL 계약 | 공유·검색·canonical과 현재 위치를 URL에 드러냄 |
| `개발·출시` 그룹 마지막에 `데이터베이스 아키텍처` 추가 | 기존 sidebar 그룹과 독립 기술 article 패턴 | 새 최상위 그룹 없이 관련 개발 문맥에 연결 |
| `docs/DATABASE_STRUCTURE.md`를 작성 정본으로 유지 | 사용자 요청과 공개 문서 동기화 계약 | 생성된 Markdown을 수작업 관리하는 이중 정본 방지 |
| 원문 H1만 Starlight title로 치환하고 H2 이하·표·code fence 보존 | 기존 reader content 변환 규칙 | 15개 section의 정보 계층과 GitHub source 가독성을 동시에 유지 |
| landing·Header에 별도 CTA를 추가하지 않음 | 기존 landing 단일 Hero와 문서 탐색 분리 결정 | 한 문서 추가로 전역 navigation 우선순위를 바꾸지 않음 |
| Mermaid client integration을 추가하지 않음 | source map·client runtime 최소화와 기존 code block 계약 | 한 diagram 때문에 새 dependency와 시각 체계를 도입하지 않음 |
| 사설 노드·인계·commit 식별자를 공개본에서 제거 | 공개 소스 정책 | schema 설명은 유지하면서 실제 운영 환경 정보를 노출하지 않음 |
| Markdown table에 build-time `tabindex=0` 부여 | keyboard focus와 가로 스크롤 접근성 규칙 | 작은 화면에서 overflow table을 키보드로 진입·스크롤 가능하게 하고 기본 focus outline을 보존 |

## 5. 수용 기준

| 우선순위 | 기준 |
|---|---|
| P0 | 새 route가 정적 HTML로 생성되고 제목, canonical, active sidebar item을 가진다. |
| P0 | 22개 가이드와 새 문서 모두 좌측 8개 그룹·23개 문서 link를 제공한다. |
| P0 | 본문에 SQLite 10개, cross-DB 일관성, Audit, Monitoring Evidence, Web Push, 백업·schema 변경 경계가 존재한다. |
| P0 | 실제 운영 노드 주소·사설 인계 링크·사설 commit 식별자가 artifact에 없다. |
| P1 | Pagefind가 27개 HTML을 색인하고 내부 link와 source link 변환이 깨지지 않는다. |
| P1 | 1440px와 390px에서 heading·표·code block에 가로 page overflow가 없고 keyboard navigation이 유지된다. |
| P1 | GitHub Pages run 성공 후 custom domain route가 HTTP 200, 한국어, canonical과 최신 본문을 반환한다. |

## 6. 로컬 구현 검증

- `cd site && npm run check`: PASS
- Astro: 27개 page build, Pagefind 27개 HTML 색인
- Pages artifact gate: 90개 파일, 새 route·canonical·active navigation·8개 그룹·23개 link,
  본문 marker와 private 운영 증거 부재 확인
- Chromium `1440×1000`, `390×844`: H1 1개, H2 15개, 표 52개, code block 9개,
  active navigation과 고유 reader link 23개, page-level 가로 overflow 0 확인. mobile에서 폭을
  넘는 표 29개는 각 표 내부에서만 scroll하며 focus 후 방향키 horizontal scroll을 확인했다.
- 최초 Axe 검사에서 overflow table의 keyboard focus 위반을 확인했다. build-time rehype
  변환으로 표 52개에 `tabindex=0`을 부여한 뒤 두 viewport 모두 WCAG A/AA 위반 0,
  console·page·request 오류 0을 확인했다. 첫 표의 기본 focus outline도 실제 계산값으로 확인했다.
- Pagefind에서 `Monitoring Evidence DB` 검색 결과가 새 정본 route와 제목을 반환했다.
- 로컬 캡처:
  - `.scratch/ui-reviews/2026-08-30-database-architecture-route/desktop.png`
    — `6eb08c5cf25f5fd1d9380dfad25a3411bc7a7e372cc4cef56b79846d06ddb21e`
  - `.scratch/ui-reviews/2026-08-30-database-architecture-route/mobile.png`
    — `4dc260f7e7bb47370fd9595746673b77c2155c4e2f6b3d456cf15fcc7416d4d4`

## 7. GitHub Pages·custom domain 검증

- 구현 commit `d07e4c253e86a09583da7fee054c12f59855b919`을 public `main`에 push했다.
- GitHub Pages run
  [`33277437435`](https://github.com/HardcoreMonk/purecvisor/actions/runs/33277437435)의
  `npm ci`, `npm run check`, artifact upload와 deploy가 모두 성공했다.
- `https://purecvisor.site/ko/development/database-architecture/`는 HTTP 200과
  `text/html; charset=utf-8`, 정확한 canonical, H1 1개, active item, 8개 그룹·23개 문서 link,
  표 52개와 최신 Audit·Monitoring Evidence·Web Push 본문을 반환했다.
- live Chromium `1440×1000`, `390×844`에서 page-level 가로 overflow 0,
  Axe WCAG A/AA 위반 0, console·page·request 오류 0을 확인했다. mobile의 내부 scroll table은
  29개이며 keyboard 방향키로 `scrollLeft 0→258` 이동했다.
- live Pagefind의 `Monitoring Evidence DB` 검색은 새 route와 `데이터베이스 아키텍처` 제목을
  반환했다.
- live desktop·mobile 캡처 SHA-256은 위 로컬 캡처와 각각 동일했다. keyboard table focus
  캡처는 `.scratch/ui-reviews/2026-08-30-database-architecture-route/live-mobile-table-focus.png`
  — `eaf602c7cce314b407afac1e8edaee88005cf2ed97cd4f7496d314eba5d44cf6`다.

따라서 이 리뷰를 **LIVE-PASS**로 확정한다.
