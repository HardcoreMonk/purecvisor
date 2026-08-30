# 공개 문서 사이트 운영 기준

> 상태: 운영 중, main push 기반 Pages 자동 배포
> 승인일: 2026-08-21
> 공개 주소: `https://purecvisor.site`
> 저장소: `HardcoreMonk/purecvisor`

## 목적

PureCVisor 2.0.0 공개 문서는 public GitHub 저장소에서 Astro와 Starlight로 빌드하고 GitHub
Pages에 정적 artifact로 배포한다. 공개 서비스 landing은 제품 Web UI runtime과 분리하면서
`docs/GUIDE.md`의 숫자형 22개 장을 언어·분류·문서별 Starlight 정적 page로 분할한다.
`docs/DATABASE_STRUCTURE.md`는 별도 작성 정본을 유지하면서 같은 reader의 독립 데이터베이스
아키텍처 page로 생성한다.

## 운영 경계

- 제품 runtime UI는 `ui/`의 Vanilla JavaScript 계약을 유지한다.
- 공개 서비스 landing과 Pages build는 `site/`에서 Astro 7.2.4와 Starlight 0.41.7을 사용한다.
- 공개 운영 가이드 정본은 `/ko/<분류>/<문서>/` directory route다.
- 데이터베이스 아키텍처 정본 route는 `/ko/development/database-architecture/`다.
- GitHub Actions는 `site/dist/`만 Pages artifact로 업로드한다.
- source map, 비공개 운영 기록, 인증정보와 allowlist 밖 파일은 Pages에 포함하지 않는다.
- `https://purecvisor.site/ko/getting-started/installation/`을 전체 운영 가이드 기본 진입 URL로
  유지한다.
- `/docs.html`은 기존 bookmark와 숫자형 장 hash를 새 정본 route로 보내는 호환 redirect다.
- `guide.html`은 생성하거나 navigation과 landing link에서 사용하지 않는다.
- `site/scripts/guide-routes.mjs`가 8개 그룹, 운영 가이드 22개 장, 별도 아키텍처 문서와
  legacy hash의 route manifest를 소유한다.
- `site/scripts/prepare-content.mjs`가 `docs/GUIDE.md`의 숫자형 H2 장을 22개 Astro content page로
  분할하고 `docs/DATABASE_STRUCTURE.md`를 독립 content page로 변환한다.
  `site/scripts/publish-product-docs.mjs`는 `/docs.html` 호환 redirect를 생성한다.
- Pages workflow의 push path에는 `docs/GUIDE.md`, `docs/DATABASE_STRUCTURE.md`, `site/**`와
  workflow 자체를 포함해 두 작성 정본 중 어느 하나만 바뀌어도 재배포한다.
- landing source는 `site/src/content/docs/index.mdx`이며 제품 범위·기본 action·현재 Single Edge
  아키텍처를 Hero 하나로 제공한다. 8개 그룹·23개 문서의 전체 탐색은 문서 reader가 소유한다.
- 기본 `/`과 명시적 `/ko/`는 한국어 landing을 제공하고 `/en/`은 영어 landing을 제공한다.
  한국어 콘텐츠 정본은 root `index.mdx`이며 build 준비 단계가 `/ko/`용 source를 복제한다.
- 상단 `서비스`, `시작하기`, `공개 범위`, `문서`는 각각 하위 링크를 가진 disclosure navigation이다.
  `문서 > 전체 운영 가이드`와 영어 `Documentation > Full operations guide`는 모두 한국어 정본
  `/ko/getting-started/installation/`로 이동한다.

## 콘텐츠 동기화 계약

- `docs/GUIDE.md`가 전체 운영 절차와 22개 장의 공개 작성 정본이다.
- `docs/DATABASE_STRUCTURE.md`가 SQLite 저장소의 책임, schema, 일관성·장애·백업 경계를
  설명하는 공개 작성 정본이다. build 준비 단계는 원문의 H1만 Starlight page title로 치환하고
  나머지 heading·표·code fence를 보존한다.
- `ui/guide-content.md`와 `ui/docs.html`은 제품 Web UI 문서 shell의 정본으로 남으며 공개 Pages
  reader의 runtime dependency가 아니다.
- `site/src/content/docs/index.mdx`는 Hero action과 상단 disclosure에서 대표 정본 route만 연결하고
  전체 23개 문서 목록을 복제하지 않는다.
- 분할기는 장 안의 상대 source link를 공개 GitHub 저장소 URL로 정규화하고 H3 이하 heading을
  독립 page의 H2 이하 계층으로 승격한다.
- 데이터베이스 공개본에는 실제 운영 노드 식별자, 비공개 인계 링크와 비공개 commit 식별자를
  포함하지 않는다.
- build-time rehype 변환은 Markdown table에 `tabindex=0`을 부여해 작은 화면의 가로 스크롤
  영역이 keyboard focus와 기본 focus outline을 갖게 한다.
- 설치 page는 제품 노드의 기본 `purecvisorsd` 자체 HTTPS와 선택형 NGINX 외부 TLS 종료를
  구분하고, 설정·요청 흐름·health·금지 조합과 `purecvisor.site`의 GitHub Pages hosting 경계를
  함께 제공한다. NGINX를 모든 설치의 필수 의존성으로 안내하지 않는다.
- 카테고리나 장 구성이 바뀌면 제품 포털, route manifest와 `site/scripts/check-site.mjs`의
  reader route·link gate를 같은 릴리스 단위로 갱신한다.

## 시각 기준

- PureCVisor의 흰 canvas, soft gray, ink와 teal token 역할을 유지한다.
- 첫 페이지는 제품 label·단일 노드 운영 범위·기본 action·전체 서비스 아키텍처 SVG를 담은 Hero
  하나만 본문으로 제공한다. 서비스 기능, 시작 흐름, 공개 범위와 8개 그룹·23개 문서 전체 목록은
  첫 페이지에서 반복하지 않고 Hero action과 상단 disclosure에서 운영 가이드로 연결한다.
- Hero의 Single Edge 서비스 아키텍처는
  `site/public/assets/diagrams/purecvisor-single-full-architecture.svg` 원본을 직접 사용한다. SVG는 클라이언트·설정 입력,
  TLS 경계, `purecvisorsd` 단일 프로세스, transport·dispatcher, 동기·비동기 완료, 6개 서비스
  도메인, 책임별 7개·3개 저장소로 구분한 로컬 SQLite DB 10개, Monitoring Source v2,
  선택형 DPDK 수명주기, audit-only BPF LSM, 영속 상태와 Linux/KVM host 연결을 한 화면에
  유지하며 Multi Edge 전용 기능은 표시하지 않는다. 두 DB 노드는 `vm_state.db`,
  `pcv_audit.db`, `pcv_jobs.db`, `rbac.db`, `pcv_security.db`, `security_groups.db`, `vpc.db`,
  `cloud_jobs.db`, `pcv_monitoring.db`, `pcv_webpush.db`를 모두 명시한다.
- Hero는 version label 다음에 “하나의 Linux/KVM 노드, 하나의 제어면”을 실제 H1으로 두고 범위
  문장과 action을 이어서 제공한다. 아키텍처 지도는 같은 shell의 전체 폭 하단에 배치하며 범위
  문장은 1024px 이상에서 한 줄, 768px 이하에서 자연 줄바꿈한다.
- SVG 파일은 `1849.5234375×2798` viewBox와 node·edge·label·좌표를 유지한다.
  `<style>`을 제외한 구조·내용 SHA-256은
  `0f3f3a26d1dc2b128a0b58da6f63bad61d71637e6a3d4aa2f01aff9f137778be`, 배포 파일 SHA-256은
  `f64b3756dbe546ac65245fa5363d61cbd30e03b1652b53c612ca72e33d685c3b`로 고정한다.
  `<script>`, event handler, `<foreignObject>`와 외부 link를 허용하지 않는다.
- SVG 색은 Clients 하늘색, Config 주황색, API Transport 보라색, GMainLoop Control 초록색,
  Domain Modules 청록색, Persistent 살구색, Host 회색의 의미 체계를 사용한다. 페이지 범례는
  색상과 layer·의미 label을 함께 제공해 색만으로 책임을 전달하지 않는다.
- SVG palette는 `site/scripts/update-architecture-colors.mjs`로 재현한다. 스크립트는 기존
  `<style>` 안의 semantic color block만 교체하며 node·edge·label과 좌표는 수정하지 않는다.
- Hero와 figure shell은 Starlight의 `data-theme` 계약을 따른다. SVG는 밝은 중립 canvas에서 현재
  콘텐츠 폭에 맞춰 전체를 표시하고 제한 높이·고정 90rem·내부 양방향 scroll을 사용하지 않는다.
  다이어그램 전체와 별도 `확대해서 보기` link는 같은 SVG를 새 탭에서 native zoom으로 제공한다.
- figure는 설명 media 역할의 32px radius를 유지한다. `<img>`에는 intrinsic width·height와 현재
  언어의 전체 구조 alt를 제공하며, 전체 SVG link에는 언어별 accessible name을 제공한다. 기존
  code-native domain selector, guide node link, 경로 animation과 전용 JavaScript는 사용하지 않는다.
- 첫 페이지에는 문서 directory, 역할별 추천 경로와 별도 최종 CTA 구역을 두지 않는다. 전체
  문서 탐색은 Hero의 `전체 운영 가이드`와 Header의 `문서` disclosure에서 reader로 이동해 수행한다.
- opencodex.me에서 확인한 서비스 소개에서 문서 탐색으로 이어지는 정보 계층, 검색과 3단
  reader 구조, 그룹 제목과 단순 링크 목록으로 구성한 상단 disclosure navigation만 참고한다.
- 각 운영 가이드와 데이터베이스 아키텍처 page는 Starlight header, 좌측 8개 그룹·23개 문서
  navigation, 중앙 본문,
  우측 현재 page 목차와 하단 이전·다음 navigation을 사용한다.
- 시작하기의 `1.2 아키텍처 개요`는 landing과 같은 전체 SVG를 직접 재사용하고, 본문만으로도
  기본 daemon HTTPS·선택형 NGINX 외부 종료, 부팅 입력, 4개 transport, 동기·비동기 완료,
  6개 서비스 도메인, 로컬 SQLite DB 10개·desired state, Linux/KVM host와 Single Edge 제외
  경계를 읽을 수 있어야 한다. ASCII 아키텍처를 별도 정본으로 유지하지 않는다.
- reader의 폭은 자료 역할에 따라 구분한다. 일반 문장·heading·pagination은 최대 50rem,
  표와 code block은 기본 50rem에서 콘텐츠가 요구하는 만큼 최대 60rem까지 적응형으로
  확장한다. H1~H6, 본문, 표와 code는 공통 좌측 읽기 축을 유지하고 아키텍처 figure만 최대
  75rem으로 독립 확장한다. 이 계약은 절대 고정 폭이 아니며 좁은 화면에서는 reader의 가용
  폭을 사용한다.
- 표와 code block의 초과 너비는 해당 요소 안에서만 scroll하고 page-level 가로 scroll을
  만들지 않는다. 표는 keyboard focus를 유지하며, 아키텍처 자료는 전체 구조를 폭에 맞춰
  표시하는 inline view와 원본 확대 link를 함께 제공한다.
- 외부 사이트의 logo, 고유 문구, 이미지와 브랜드 자산은 복제하지 않는다.
- 본문 가독성, keyboard focus, mobile navigation과 code overflow를 운영 기준으로 검증한다.
- landing 근거는 `docs/ui-reviews/2026-08-22-public-service-landing.md`, 전체 가이드 reader
  근거는 `docs/ui-reviews/2026-08-22-public-product-docs-layout.md`, 언어·상단 navigation 근거는
  `docs/ui-reviews/2026-08-23-public-site-i18n-navigation.md`, 아키텍처 지도 근거는
  `docs/ui-reviews/2026-08-24-landing-single-edge-architecture-layers.md`와
  `docs/ui-reviews/2026-08-24-landing-architecture-bottom-layer-colors.md`, 판독성·테마 근거는
  `docs/ui-reviews/2026-08-24-landing-architecture-readability-themes.md`, 선택 경로·Refero 합성 근거는
  `docs/ui-reviews/2026-08-24-landing-architecture-route-explorer.md`, 서비스 아키텍처 보강 근거는
  `docs/ui-reviews/2026-08-24-landing-service-architecture-completion-domains.md`, SVG 원본 교체 근거는
  `docs/ui-reviews/2026-08-25-landing-service-architecture-source-svg.md`, 색상 의미와 폭 맞춤 전환 근거는
  `docs/ui-reviews/2026-08-25-landing-architecture-fit-semantic-colors.md`, 문서 디렉터리 제거 근거는
  `docs/ui-reviews/2026-08-24-landing-documentation-section-removal.md`, 2026-08-30 콘텐츠 현행화는
  `docs/ui-reviews/2026-08-30-public-site-current-state-refresh.md`를 따른다. 데이터베이스
  아키텍처 route 추가는 `docs/ui-reviews/2026-08-30-database-architecture-route.md`, landing
  SVG의 DB 계층 보강은 `docs/ui-reviews/2026-08-30-landing-architecture-database-layer.md`,
  reader의 의미 기반 폭 체계는
  `docs/ui-reviews/2026-08-30-public-documentation-content-widths.md`, 공통 읽기 축 재조정은
  `docs/ui-reviews/2026-08-30-public-documentation-reading-axis.md`, 시작하기의 전체 아키텍처
  보강은 `docs/ui-reviews/2026-08-30-overview-architecture-completeness.md`를 따른다.

## 배포 흐름

```text
docs/GUIDE.md + docs/DATABASE_STRUCTURE.md + route manifest
                            |
                            v
       22개 가이드 + 1개 DB 아키텍처 page 생성
                            |
                            v
              Astro/Starlight build + search
                            |
                            v
               /docs.html legacy redirect 생성
                            |
                            v
                 site/dist Pages artifact
                            |
                            v
                GitHub Pages + purecvisor.site
```

workflow action은 tag가 아니라 검증된 commit SHA로 고정한다. 현재 고정 기준은 다음과 같다.

| Action | 버전 |
|---|---|
| `actions/checkout` | 7.0.1 |
| `actions/setup-node` | 7.0.0 |
| `actions/configure-pages` | 6.0.0 |
| `actions/upload-pages-artifact` | 5.0.0 |
| `actions/deploy-pages` | 5.0.0 |

## 도메인 전환

Gabia DNS를 변경하기 전에 GitHub 계정에서 `purecvisor.site` 소유권을 TXT record로 검증하고
repository Pages 설정에 custom domain을 먼저 등록한다. apex A record는 GitHub Pages의 네
주소로 교체하고 `www.purecvisor.site`는 `hardcoremonk.github.io` CNAME으로 연결한다.

```text
185.199.108.153
185.199.109.153
185.199.110.153
185.199.111.153
```

DNS 전환은 기존 서비스를 apex domain에서 분리하는 운영 변경이다. 이 site는 custom domain의
root 경로를 기준으로 build하므로 기본 `github.io` project URL은 최종 수용 기준으로 사용하지
않는다. local HTTP와 Pages deployment job·artifact를 먼저 검증하고, domain 소유권, HTTPS
certificate와 rollback 기준을 확인한 뒤 별도 cutover로 실행한다.

## 검증

```bash
cd site
npm ci
npm run check
```

검증은 `index.html`, `ko/index.html`, `en/index.html`, 22개 가이드와 1개 데이터베이스
아키텍처 directory page, 8개 sidebar group, 현재 page, 이전·다음 navigation,
`/docs.html` legacy mapping, 네 disclosure menu와 언어 route,
landing 문서 directory·역할별 경로·최종 CTA 부재, Hero action, 내부 link 무결성,
`guide.html`·`guide-content.md` artifact 부재, 금지된 내부 주소·private repository 표식과 source
map 부재를 확인한다. Hero 한 열, desktop 범위 문장 한 줄, mobile 줄바꿈, light/dark surface token,
32px figure, SVG 파일·구조 hash, 7개 layer 색상·의미 범례, 폭 맞춤 image와 내부 scroll container
부재, 일반 본문 50rem·표/code 50~60rem 적응형 폭·아키텍처 75rem 상한, 공통 좌측 읽기 축,
데이터베이스 문서 table의 keyboard focus도 함께 검사한다.
실제 Pages 배포 후에는 `/`, `/ko/`,
`/en/`, 설치 page 직접 본문, 22개 가이드 route, 데이터베이스 아키텍처 route, legacy 이동,
검색, 좌우 목차, mobile navigation, HTTPS와 custom domain canonical URL을 확인한다.

2026-08-30 구현 commit `d07e4c253e86a09583da7fee054c12f59855b919`의 Pages run
[`33277437435`](https://github.com/HardcoreMonk/purecvisor/actions/runs/33277437435)과 custom
domain에서 27개 page·90개 artifact, 8개 그룹·23개 문서, 데이터베이스 아키텍처 본문·검색,
desktop·mobile overflow와 Axe 계약을 검증했다.

2026-08-30 reader 폭 구현 commit `a82d1f4d9eca2aec6352322a0497872f18700124`의 Pages run
[`33281197096`](https://github.com/HardcoreMonk/purecvisor/actions/runs/33281197096)과 custom
domain에서 일반 본문 50rem, 표·code 68rem, 아키텍처 75rem 상한, 기존 릴리스 세 줄,
desktop·mobile overflow와 Axe 계약을 검증했다.

2026-08-30 공통 읽기 축 재조정 commit
`0968ae90d163d92de5239ec79bc4aadbc962e427`의 Pages run
[`33282754951`](https://github.com/HardcoreMonk/purecvisor/actions/runs/33282754951)과 custom
domain에서 H1~H6·본문·표·code의 좌측축 차이 0, 표·code 50~60rem 적응형 폭,
아키텍처 75rem 독립 확장, 기존 릴리스 세 줄, 1440·1280·390px reflow와 Axe 계약을
검증했다.
