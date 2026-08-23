# 공개 문서 사이트 운영 기준

> 상태: 운영 중, main push 기반 Pages 자동 배포
> 승인일: 2026-08-21
> 공개 주소: `https://purecvisor.site`
> 저장소: `HardcoreMonk/purecvisor`

## 목적

PureCVisor 2.0.0 공개 문서는 public GitHub 저장소에서 Astro와 Starlight로 빌드하고 GitHub
Pages에 정적 artifact로 배포한다. 공개 서비스 landing은 제품 Web UI runtime과 분리하면서
`docs/GUIDE.md`를 공개 가이드 작성 정본으로 유지하고, 숫자형 22개 장을 언어·분류·문서별
Starlight 정적 page로 분할한다.

## 운영 경계

- 제품 runtime UI는 `ui/`의 Vanilla JavaScript 계약을 유지한다.
- 공개 서비스 landing과 Pages build는 `site/`에서 Astro 7.2.4와 Starlight 0.41.7을 사용한다.
- 공개 운영 가이드 정본은 `/ko/<분류>/<문서>/` directory route다.
- GitHub Actions는 `site/dist/`만 Pages artifact로 업로드한다.
- source map, 비공개 운영 기록, 인증정보와 allowlist 밖 파일은 Pages에 포함하지 않는다.
- `https://purecvisor.site/ko/getting-started/installation/`을 전체 운영 가이드 기본 진입 URL로
  유지한다.
- `/docs.html`은 기존 bookmark와 숫자형 장 hash를 새 정본 route로 보내는 호환 redirect다.
- `guide.html`은 생성하거나 navigation과 landing link에서 사용하지 않는다.
- `site/scripts/guide-routes.mjs`가 8개 그룹·22개 장·legacy hash의 route manifest를 소유한다.
- `site/scripts/prepare-content.mjs`가 `docs/GUIDE.md`의 숫자형 H2 장을 22개 Astro content page로
  분할하고 `site/scripts/publish-product-docs.mjs`가 `/docs.html` 호환 redirect를 생성한다.
- landing source는 `site/src/content/docs/index.mdx`이며 제품 범위·기본 action·현재 Single Edge
  아키텍처를 Hero 하나로 제공한다. 8개 그룹·22개 장의 전체 탐색은 운영 가이드 reader가 소유한다.
- 기본 `/`과 명시적 `/ko/`는 한국어 landing을 제공하고 `/en/`은 영어 landing을 제공한다.
  한국어 콘텐츠 정본은 root `index.mdx`이며 build 준비 단계가 `/ko/`용 source를 복제한다.
- 상단 `서비스`, `시작하기`, `공개 범위`, `문서`는 각각 하위 링크를 가진 disclosure navigation이다.
  `문서 > 전체 운영 가이드`와 영어 `Documentation > Full operations guide`는 모두 한국어 정본
  `/ko/getting-started/installation/`로 이동한다.

## 콘텐츠 동기화 계약

- `docs/GUIDE.md`가 전체 운영 절차와 22개 장의 공개 작성 정본이다.
- `ui/guide-content.md`와 `ui/docs.html`은 제품 Web UI 문서 shell의 정본으로 남으며 공개 Pages
  reader의 runtime dependency가 아니다.
- `site/src/content/docs/index.mdx`는 Hero·상단 disclosure·아키텍처 node에서 대표 정본 route만
  연결하고 전체 22개 장 목록을 복제하지 않는다.
- 분할기는 장 안의 상대 source link를 공개 GitHub 저장소 URL로 정규화하고 H3 이하 heading을
  독립 page의 H2 이하 계층으로 승격한다.
- 카테고리나 장 구성이 바뀌면 제품 포털, route manifest와 `site/scripts/check-site.mjs`의
  reader route·link gate를 같은 릴리스 단위로 갱신한다.

## 시각 기준

- PureCVisor의 흰 canvas, soft gray, ink와 teal token 역할을 유지한다.
- 첫 페이지는 제품 label·단일 노드 운영 범위·기본 action·5계층 아키텍처를 담은 Hero 하나만
  본문으로 제공한다. 서비스 기능, 시작 흐름, 공개 범위와 8개 그룹·22개 장 전체 목록은 첫
  페이지에서 반복하지 않고 Hero·상단 disclosure·아키텍처 node의 운영 가이드 link로 제공한다.
- Hero의 Single Edge 아키텍처 지도는 Access, Control plane, Capability services, Runtime
  adapters, Linux host의 5개 세로 계층으로 구성한다. `purecvisorsd`의 C23 단일 프로세스와
  GMainLoop·GTask, VM/LXC, ZFS/iSCSI, Linux Bridge·OVS/OVN, Local VPC·VXLAN을 실제 host
  adapter와 Linux 자원까지 연결하며 Multi Edge 전용 기능은 표시하지 않는다.
- Hero는 version label 다음에 “하나의 Linux/KVM 노드, 하나의 제어면”을 실제 H1으로 두고 범위
  문장과 action을 이어서 제공한다. 아키텍처 지도는 같은 shell의 전체 폭 하단에 배치하며 범위
  문장은 1024px 이상에서 한 줄, 768px 이하에서 자연 줄바꿈한다.
- 01~05 계층은 neutral surface와 hairline으로 후퇴시키고 `purecvisorsd`와 선택 capability 경로를
  우선한다. Workloads·Storage·Network Fabric·Virtual Network는 diagram 전용 blue·chartreuse·
  fuchsia·lavender edge token을 사용하되 status와 CTA 색을 대체하지 않는다.
- Hero와 아키텍처 지도는 Starlight의 `data-theme` 계약을 따르며 light/white에서는 흰 Hero,
  cool-gray map과 흰 node, dark에서는 near-black Hero·map과 짙은 node를 사용한다. map title,
  layer index·meta, node primary·secondary는 viewport와 무관하게 최소 12px을 유지하고 service
  primary는 14px을 사용한다. 390px에서는 Capability service를 2열 선택기로 유지하고 path
  이름을 우선하며, 세부 기술 범위는 선택 결과의 runtime·host node와 desktop 보기에 유지한다.
- Access의 Web UI·REST API·`pcvctl` 3개는 정본 운영 가이드 link를 제공한다. Capability services의
  워크로드·스토리지·네트워크 패브릭·가상 네트워크 4개는 code-native 선형 icon, `aria-pressed`
  선택 control과 별도 정본 guide link를 제공한다. 선택한 capability에 대응하는 runtime·host node만
  기본 표시하며 arrow key·Home·End로 경로를 바꿀 수 있다.
- architecture figure 내부는 mono typography를 일관되게 사용한다. 최상위 figure는 설명 media
  역할의 32px radius, 선택 route는 16px radius와 단 하나의 `6px 6px 0` offset shadow를 사용한다.
  이 radius·shadow·neon edge는 public landing architecture artifact 전용 예외이며 제품 runtime card,
  CTA, 상태 surface와 다른 문서 component로 확산하지 않는다. reduced motion에서는 animation과
  transform을 제거한다.
- 첫 페이지에는 문서 directory, 역할별 추천 경로와 별도 최종 CTA 구역을 두지 않는다. 전체
  문서 탐색은 Hero의 `전체 운영 가이드`와 Header의 `문서` disclosure에서 reader로 이동해 수행한다.
- opencodex.me에서 확인한 서비스 소개에서 문서 탐색으로 이어지는 정보 계층, 검색과 3단
  reader 구조, 그룹 제목과 단순 링크 목록으로 구성한 상단 disclosure navigation만 참고한다.
- 각 운영 가이드 page는 Starlight header, 좌측 8개 그룹·22개 장 navigation, 중앙 본문,
  우측 현재 page 목차와 하단 이전·다음 navigation을 사용한다.
- 외부 사이트의 logo, 고유 문구, 이미지와 브랜드 자산은 복제하지 않는다.
- 본문 가독성, keyboard focus, mobile navigation과 code overflow를 운영 기준으로 검증한다.
- landing 근거는 `docs/ui-reviews/2026-08-22-public-service-landing.md`, 전체 가이드 reader
  근거는 `docs/ui-reviews/2026-08-22-public-product-docs-layout.md`, 언어·상단 navigation 근거는
  `docs/ui-reviews/2026-08-23-public-site-i18n-navigation.md`, 아키텍처 지도 근거는
  `docs/ui-reviews/2026-08-24-landing-single-edge-architecture-layers.md`와
  `docs/ui-reviews/2026-08-24-landing-architecture-bottom-layer-colors.md`, 판독성·테마 근거는
  `docs/ui-reviews/2026-08-24-landing-architecture-readability-themes.md`, 선택 경로·Refero 합성 근거는
  `docs/ui-reviews/2026-08-24-landing-architecture-route-explorer.md`, 문서 디렉터리 제거 근거는
  `docs/ui-reviews/2026-08-24-landing-documentation-section-removal.md`를 따른다.

## 배포 흐름

```text
docs/GUIDE.md + guide-routes.mjs + site landing
                         |
                         v
       22개 한국어 content page 생성 + Astro/Starlight build
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

검증은 `index.html`, `ko/index.html`, `en/index.html`, 22개 directory page, 8개 sidebar group,
현재 page, 이전·다음 navigation, `/docs.html` legacy mapping, 네 disclosure menu와 언어 route,
landing 문서 directory·역할별 경로·최종 CTA 부재, Hero action, 내부 link 무결성,
`guide.html`·`guide-content.md` artifact 부재, 금지된 내부 주소·private repository 표식과 source
map 부재, 5개 아키텍처 계층·7개 정본 link·4개 path control·motion 계약을 확인한다. Hero 한 열,
desktop 범위 문장 한 줄, mobile 줄바꿈, light/dark surface token, map 최소 12px 글꼴, 32px figure,
16px active route, offset shadow와 mobile 2열 path selector 계약도 함께 검사한다.
실제 Pages 배포 후에는 `/`, `/ko/`,
`/en/`, 설치 page 직접 본문, 22개 route, legacy 이동, 검색, 좌우 목차, mobile navigation, HTTPS와
custom domain canonical URL을 확인한다.
