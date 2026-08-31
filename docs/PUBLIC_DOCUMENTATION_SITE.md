# 공개 문서 사이트 운영 기준

> 상태: 운영 중, main push 기반 Pages 자동 배포
> 승인일: 2026-08-21
> 현행화 기준: 2026-08-31
> 공개 주소: `https://purecvisor.site`
> 저장소: `HardcoreMonk/purecvisor`

## 목적

PureCVisor 2.0.0 공개 문서는 public GitHub 저장소에서 Astro와 Starlight로 빌드하고 GitHub
Pages에 정적 artifact로 배포한다. 공개 서비스 landing은 제품 Web UI runtime과 분리하면서
`docs/GUIDE.md`의 숫자형 21개 장을 언어·분류·문서별 Starlight 정적 page로 분할한다.
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
- `site/scripts/guide-routes.mjs`가 8개 그룹, 운영 가이드 21개 장, 별도 아키텍처 문서와
  legacy hash의 route manifest를 소유한다.
- `site/scripts/prepare-content.mjs`가 `docs/GUIDE.md`의 숫자형 H2 장을 21개 Astro content page로
  분할하고 `docs/DATABASE_STRUCTURE.md`를 독립 content page로 변환한다.
  `site/scripts/publish-product-docs.mjs`는 `/docs.html` 호환 redirect를 생성한다.
- Pages workflow의 push path에는 `docs/GUIDE.md`, `docs/DATABASE_STRUCTURE.md`,
  `docs/architecture/**`, `site/**`와 workflow 자체를 포함해 작성 정본이나 SVG 입력 중 어느
  하나만 바뀌어도 재배포한다.
- landing source는 `site/src/content/docs/index.mdx`이며 제품 범위·기본 action을 간결한 Hero로
  제공한 뒤 8개 그룹·22개 문서의 `문서 살펴보기` 맵을 이어서 제공한다. 상세 아키텍처와 문서
  본문 탐색은 문서 reader가 소유한다.
- 기본 `/`과 명시적 `/ko/`는 한국어 landing을 제공하고 `/en/`은 영어 landing을 제공한다.
  한국어 콘텐츠 정본은 root `index.mdx`이며 build 준비 단계가 `/ko/`용 source를 복제한다.
- 상단 `서비스`, `시작하기`, `공개 범위`, `문서`는 각각 하위 링크를 가진 disclosure navigation이다.
  `서비스`의 Networking 항목은 `/ko/infrastructure/networking/`으로 직접
  이동한다. Storage는 landing 문서 맵과 reader sidebar에서 계속 제공한다.
  `문서 > 전체 운영 가이드`와 영어 `Documentation > Full operations guide`는 모두 한국어 정본
  `/ko/getting-started/installation/`로 이동한다.

## 콘텐츠 동기화 계약

- `docs/GUIDE.md`가 전체 운영 절차와 21개 장의 공개 작성 정본이다.
- `docs/DATABASE_STRUCTURE.md`가 SQLite 저장소의 책임, schema, 일관성·장애·백업 경계를
  설명하는 공개 작성 정본이다. build 준비 단계는 원문의 H1만 Starlight page title로 치환하고
  나머지 heading·표·code fence와 raw HTML figure를 보존한다. GitHub source에서 사용하는
  `../site/public/assets/` 상대 경로는 Pages의 `/assets/` 경로로 변환한다.
- `ui/guide-content.md`와 `ui/docs.html`은 제품 Web UI 문서 shell의 정본으로 남으며 공개 Pages
  reader의 runtime dependency가 아니다.
- `site/src/components/DocumentationMap.astro`는 `site/scripts/guide-routes.mjs`의
  `landingDocuments`에서 8개 그룹·22개 정본 route를 생성한다. 운영 가이드 21개 장과
  데이터베이스 아키텍처 문서가 landing 목록과 reader navigation을 공유한다.
  `멀티 제어면 참고 기록`은 원문·route·navigation에서 발행하지 않는다. root·`/ko/`·`/en/`
  landing source는 이 컴포넌트를 호출하며 route 목록을 별도로 복제하지 않는다.
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
- 네트워크 page는 생성 전에 `/api/v1/networks/host-baseline`을 확인하는 절차, dispatcher에
  등록된 generic OVN 18개 RPC의 정확한 inventory, subnet 없는 switch create, switch-owned
  DHCP cleanup, 인증 REST ACL/NAT query filter와 canonical `-32602`를 설명한다. 미완성
  OVN/NFV Load Balancer와 VM 자동 포트 내부 helper는 사용자 기능으로 노출하지 않으며,
  generic OVN과 Local VPC OVN backend의 지원 gate를 분리한다.

## 시각 기준

- PureCVisor의 흰 canvas, soft gray, ink와 teal token 역할을 유지한다.
- 첫 페이지는 제품명·단일 노드 운영 범위·기본 action과 배포 note를 담은 Hero 뒤에
  `문서 살펴보기` 맵을 제공한다. 서비스 기능, 시작 흐름, 공개 범위와 전체 서비스 아키텍처는
  반복하지 않고 Hero action·상단 disclosure·문서 맵에서 정본 운영 가이드로 연결한다.
- landing에는 architecture figure·범례·diagram image·원본 확대 link와 `/assets/diagrams/` request를
  두지 않는다. 전체 구조의 설명과 시각 자료는 시작하기의 `1.2 아키텍처 개요`가 소유한다.
- NGINX 모드의 전체 Single Edge 서비스 아키텍처는
  `site/public/assets/diagrams/purecvisor-single-full-architecture.svg` 원본을 사용한다. SVG는
  클라이언트·설정 입력, TLS 경계, `purecvisorsd` 단일 프로세스, transport·dispatcher,
  동기·비동기 완료, 6개 서비스 도메인, 책임별 7개·2개 저장소로 구분한 로컬 SQLite DB 9개,
  host telemetry·process status, 선택형 DPDK 수명주기, audit-only BPF LSM, 영속 상태와 Linux/KVM host
  연결을 한 화면에 유지하며 Multi Edge 전용 기능은 표시하지 않는다. 두 DB 노드는
  `vm_state.db`, `pcv_audit.db`, `pcv_jobs.db`, `rbac.db`, `pcv_security.db`,
  `security_groups.db`, `vpc.db`, `cloud_jobs.db`, `pcv_webpush.db`를 모두 명시한다.
  공개 DB 개수는 공개 소스에 실제 존재하는 이 9개를 정본으로 하며 다른 내부 배포판의
  저장소 수와 혼용하지 않는다.
- Hero는 별도 version eyebrow 없이 `PURECVISOR 2.0.0`을 실제 H1으로 두고 범위 문장, action과
  배포 note를 이어서 제공한다. 범위 문장은 1024px 이상에서 한 줄, 768px 이하에서 자연
  줄바꿈한다.
- NGINX 모드 SVG의 Mermaid 원본은
  `docs/architecture/purecvisor-single-full-architecture.mmd`가 소유한다. SVG 파일은
  `1699.064208984375×2488` viewBox와 node·edge·label·좌표를 유지한다.
  `<style>`을 제외한 구조·내용 SHA-256은
  `b1a5596b79c43e219cd27216e0606288a47bcecc81fee995b11b518445e72bc6`, 배포 파일 SHA-256은
  `8929be785dce35aa307b1dd299f3dc939f192b95c1f3f339ff66b62bdd31b293`로 고정한다.
  `<script>`, event handler, `<foreignObject>`와 외부 link를 허용하지 않는다.
- SVG 색은 Clients 하늘색, Config 주황색, API Transport 보라색, GMainLoop Control 초록색,
  Domain Modules 청록색, Persistent 살구색, Host 회색의 의미 체계를 사용한다. 상세 overview의
  본문·SVG layer label이 같은 책임을 텍스트로 제공하며 landing에는 별도 범례를 반복하지 않는다.
- SVG palette는 `site/scripts/update-architecture-colors.mjs`로 재현한다. 스크립트는 NGINX와
  직접 HTTPS 두 SVG의 기존 `<style>` 안 semantic color block만 교체하며 node·edge·label과
  좌표는 수정하지 않는다.
- 두 전체 아키텍처 SVG는 Mermaid CLI `11.16.0`, white background와 고정 SVG ID로 생성한 뒤
  semantic palette를 적용한다. 다른 Mermaid 버전이나 font override를 사용하면 geometry와
  hash가 달라진다.

  ```bash
  npx --yes -p @mermaid-js/mermaid-cli@11.16.0 mmdc \
    -i docs/architecture/purecvisor-single-full-architecture.mmd \
    -o site/public/assets/diagrams/purecvisor-single-full-architecture.svg \
    -b white --svgId my-svg
  npx --yes -p @mermaid-js/mermaid-cli@11.16.0 mmdc \
    -i docs/architecture/purecvisor-single-direct-https-architecture.mmd \
    -o site/public/assets/diagrams/purecvisor-single-direct-https-architecture.svg \
    -b white --svgId my-svg
  node site/scripts/update-architecture-colors.mjs
  ```
- Hero는 Starlight의 `data-theme` 계약을 따르며 별도 media shell이나 빈 placeholder를 만들지 않는다.
  landing에는 code-native domain selector, guide node link, SVG 경로 animation과 전용
  JavaScript를 사용하지 않는다.
- `문서 살펴보기`는 soft-gray band 안의 단일 surface에서 8개 그룹·22개 문서를 노출한다.
  각 그룹은 번호·H3·단순 링크 목록으로 구성하고 개별 카드, 그림자, 장식 아이콘과 별도 최종 CTA는
  두지 않는다. 64rem 초과는 4열, 40~64rem은 2열, 40rem 이하는 1열로 재배치한다.
- opencodex.me에서 확인한 서비스 소개 뒤 전체 문서 맵으로 이어지는 정보 계층과 그룹별 단순 링크
  목록을 참고한다. 외부 색상, logo, 고유 문구, 이미지와 브랜드형 surface는 복제하지 않는다.
- 각 운영 가이드와 데이터베이스 아키텍처 page는 Starlight header, 좌측 8개 그룹·22개 문서
  navigation, 중앙 본문,
  우측 현재 page 목차와 하단 이전·다음 navigation을 사용한다.
- 데이터베이스 아키텍처 page는 공개 소스의 영구 DDL을 기준으로 로컬 SQLite 파일 9개와
  영구 테이블 26개를 설명한다. 서두에는 4개 핵심 수치의 단일 분할 요약 띠를 두고,
  `site/public/assets/diagrams/purecvisor-single-database-architecture.svg`에서 요청·정책 정본·작업
  상태·감사 및 외부 통합·Linux/KVM actual state의 관계를 보여 준다. SVG는 `<title>`·`<desc>`와
  본문 대체 설명, 원본 확대 link를 제공하고 mobile에서는 page가 아니라 figure canvas 안에서만
  가로 scroll한다. `pcv_jobs.db`는 worker 실행 queue가 아닌 선택적 상태 registry로,
  ZFS는 선택형 actual storage로, OVN Local VPC는 후보 backend로 표시한다.
- 시작하기의 `1.2 아키텍처 개요`는 기본 `purecvisorsd` 직접 HTTPS와 선택형 NGINX 외부 TLS
  종료를 접근 가능한 두 탭으로 제공한다. 기본 탭은
  `site/public/assets/diagrams/purecvisor-single-direct-https-architecture.svg`, NGINX 탭은
  `purecvisor-single-full-architecture.svg`를 사용하며 선택한 panel 하나만 표시한다. 직접
  HTTPS SVG의 Mermaid 원본은
  `docs/architecture/purecvisor-single-direct-https-architecture.mmd`가 소유한다. 이 SVG의
  viewBox는 `1817.8671875×2313.699951171875`, 구조·내용 SHA-256은
  `6b2918ca11c217aaf8ac8cec81b78d01ef4b70b50370cfb2c2a39286f10a963c`, 배포 파일 SHA-256은
  `093aadbafec9100e2ed8ec82190d0d0324af66697ef13191f8df910830bce2d3`로 고정한다.
  본문만으로도 두 TLS 경계, 부팅 입력, 4개 transport, 동기·비동기 완료, 6개 서비스 도메인,
  로컬 SQLite DB 9개·desired state, Linux/KVM host와 Single Edge 제외 경계를 읽을 수 있어야
  한다. ASCII 아키텍처를 별도 정본으로 유지하지 않는다.
- TLS 모드 탭은 `button[role=tab]`과 `tabpanel`을 연결하고 `aria-selected`, roving `tabindex`,
  좌우 방향키·Home·End를 지원한다. 선택 상태는 teal 색뿐 아니라 border와 surface로도 구분하며,
  390px에서도 40px 이상 target과 page-level 무가로 overflow를 유지한다.
- overview의 두 SVG는 정적 `<img>`와 확대 link를 fallback으로 먼저 제공한 뒤 same-origin asset을
  안전하게 inline으로 전환한다. Mermaid node·cluster·edge ID에서 컴포넌트와 레이어의 직접 연결을
  계산하고, 정밀 pointer rollover 동안 관련 node·label·화살표만 강조한다. 활성 edge의 dash는
  기존 arrow marker 방향을 보조하며 상시 자동 재생하지 않는다. 두 SVG의 DOM ID와 내부 reference,
  Mermaid keyframe 이름은 instance별 namespace로 분리한다.
- inline 변환은 `<script>`, `<foreignObject>`, event handler, link와 외부 CSS resource를 거부한다.
  fetch·parse·topology 확인이 실패하면 원래 `<img>`를 그대로 유지한다. touch pointer는 정적 map과
  확대 link를 사용하고 `prefers-reduced-motion: reduce`에서는 이동 animation 없이 색·두께·opacity
  강조만 유지한다. hover로만 새로운 필수 설명을 제공하거나 SVG 내부 node를 대량 keyboard tab
  stop으로 만들지 않는다.
- reader의 폭은 자료 역할에 따라 구분한다. 일반 문장·heading·pagination은 최대 56rem,
  표와 code block은 기본 56rem에서 콘텐츠가 요구하는 만큼 최대 72rem까지 적응형으로
  확장한다. H1~H6, 본문, 표와 code는 공통 좌측 읽기 축을 유지하고 아키텍처 figure는 최대
  88rem canvas를 사용한다. 좌·우 reader sidebar 기준 폭은 16rem이다. 이 계약은 절대 고정
  폭이 아니며 좁은 화면에서는 reader의 가용 폭을 사용한다.
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
  `docs/ui-reviews/2026-08-30-public-site-current-state-refresh.md`를 따르며, 같은 리뷰의
  2026-08-31 후속 기록이 generic OVN 콘텐츠와 Networking 직접 링크 정합화를 승계한다.
  데이터베이스
  아키텍처 route 추가는 `docs/ui-reviews/2026-08-30-database-architecture-route.md`, landing
  SVG의 DB 계층 보강은 `docs/ui-reviews/2026-08-30-landing-architecture-database-layer.md`,
  reader의 의미 기반 폭 체계는
  `docs/ui-reviews/2026-08-30-public-documentation-content-widths.md`, 공통 읽기 축 재조정은
  `docs/ui-reviews/2026-08-30-public-documentation-reading-axis.md`, 넓은 화면 공간 활용 확장은
  `docs/ui-reviews/2026-09-01-public-documentation-wide-layout.md`, 시작하기의 전체 아키텍처 보강은
  `docs/ui-reviews/2026-08-30-overview-architecture-completeness.md`, TLS 모드별 SVG와 탭은
  `docs/ui-reviews/2026-08-30-overview-architecture-tls-mode-tabs.md`, node·레이어 연결 흐름 롤오버는
  `docs/ui-reviews/2026-08-30-overview-architecture-rollover-flow.md`, landing SVG 노출 제거는
  `docs/ui-reviews/2026-08-30-landing-architecture-removal.md`, landing 문서 맵 재도입은
  `docs/ui-reviews/2026-08-30-landing-documentation-map.md`, 데이터베이스 SVG 흐름과 문서 계층은
  `docs/ui-reviews/2026-08-31-database-architecture-visual-flow.md`를 따른다.

## 배포 흐름

```text
docs/GUIDE.md + docs/DATABASE_STRUCTURE.md + route manifest
                            |
                            v
       21개 가이드 + 1개 DB 아키텍처 page 생성
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

검증은 `index.html`, `ko/index.html`, `en/index.html`, 21개 가이드와 1개 데이터베이스
아키텍처 directory page, 8개 sidebar group, 현재 page, 이전·다음 navigation,
`/docs.html` legacy mapping, 네 disclosure menu와 언어 route,
landing `문서 살펴보기`의 8개 그룹·22개 정본 link, 폐기된 항목·route 부재, 최종 CTA 부재, Hero action,
내부 link 무결성,
`guide.html`·`guide-content.md` artifact 부재, 금지된 내부 주소·private repository 표식과 source
map 부재를 확인한다. Hero 한 열, 문서 맵 4→2→1열, 44px link target, desktop 범위 문장 한 줄,
mobile 줄바꿈, light/dark surface token,
landing architecture figure·범례·diagram image·diagram link·diagram request 부재, SVG 파일·구조 hash,
overview의 progressive inline fallback·ID namespace·node/cluster/edge mapping·reduced-motion,
일반 본문 56rem·표/code 56~72rem 적응형 폭·아키텍처 88rem 상한·16rem sidebar,
공통 좌측 읽기 축,
데이터베이스 문서 table의 keyboard focus도 함께 검사한다.
네트워크 page에서는 host baseline endpoint, subnet 없는 정확한 switch create, generic OVN
18개 RPC set, DHCP ownership cleanup, REST ACL/NAT filter와 `-32602`를 positive contract로
검사한다. `vm_port`, subnet을 포함한 switch create, OVN/NFV Load Balancer 사용자 호출이
다시 나타나면 실패다. Header의 Networking은 6장으로 직접 연결돼야 하며,
Storage는 landing 문서 맵과 reader sidebar에서 접근 가능해야 한다.
실제 Pages 배포 후에는 `/`, `/ko/`,
`/en/`, 설치 page 직접 본문, 21개 가이드 route, 데이터베이스 아키텍처 route, legacy 이동,
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

2026-08-31 generic OVN·host baseline 문서와 Networking 직접 링크 배포 receipt는
[공개 운영 인계](operations/2026-08-31-ovn-documentation-source-pages-sync-handoff.md)에서
별도로 관리한다.

2026-09-01 넓은 화면 reader 확장 commit `fe7ce8c9f35fd1b70493efe2a0a7c9e29f5dc1ff`의 Pages run
[`33425021822`](https://github.com/HardcoreMonk/purecvisor/actions/runs/33425021822)과 custom
domain에서 16rem sidebar, 일반 본문 56rem, 표·code 56~72rem, 88rem architecture canvas,
1920·390px reflow와 Axe·browser 오류 0 계약을 검증했다.
