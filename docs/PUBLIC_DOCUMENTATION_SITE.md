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
- landing source는 `site/src/content/docs/index.mdx`이며 제품 문서 포털과 같은 8개 작업
  카테고리·22개 장·역할별 추천 경로를 공개 사이트 구조로 제공한다.
- 기본 `/`과 명시적 `/ko/`는 한국어 landing을 제공하고 `/en/`은 영어 landing을 제공한다.
  한국어 콘텐츠 정본은 root `index.mdx`이며 build 준비 단계가 `/ko/`용 source를 복제한다.
- 상단 `서비스`, `시작하기`, `공개 범위`, `문서`는 각각 하위 링크를 가진 disclosure navigation이다.
  `문서 > 전체 운영 가이드`와 영어 `Documentation > Full operations guide`는 모두 한국어 정본
  `/ko/getting-started/installation/`로 이동한다.

## 콘텐츠 동기화 계약

- `docs/GUIDE.md`가 전체 운영 절차와 22개 장의 공개 작성 정본이다.
- `ui/guide-content.md`와 `ui/docs.html`은 제품 Web UI 문서 shell의 정본으로 남으며 공개 Pages
  reader의 runtime dependency가 아니다.
- `site/src/content/docs/index.mdx`는 각 장의 설명을 복제하지 않고 정확한 directory route로
  연결한다.
- 분할기는 장 안의 상대 source link를 공개 GitHub 저장소 URL로 정규화하고 H3 이하 heading을
  독립 page의 H2 이하 계층으로 승격한다.
- 카테고리나 장 구성이 바뀌면 제품 포털, 공개 landing과 `site/scripts/check-site.mjs`의
  route·link gate를 같은 릴리스 단위로 갱신한다.

## 시각 기준

- PureCVisor의 흰 canvas, soft gray, ink와 teal token 역할을 유지한다.
- 첫 페이지는 서비스 설명, 시작 흐름, 핵심 기능과 공개 범위를 문서 탐색보다 먼저 제공한다.
- opencodex.me에서 확인한 서비스 소개에서 문서 탐색으로 이어지는 정보 계층, 검색과 3단
  reader 구조, 그룹 제목과 단순 링크 목록으로 구성한 상단 disclosure navigation만 참고한다.
- 각 운영 가이드 page는 Starlight header, 좌측 8개 그룹·22개 장 navigation, 중앙 본문,
  우측 현재 page 목차와 하단 이전·다음 navigation을 사용한다.
- 외부 사이트의 logo, 고유 문구, 이미지와 브랜드 자산은 복제하지 않는다.
- 본문 가독성, keyboard focus, mobile navigation과 code overflow를 운영 기준으로 검증한다.
- landing 근거는 `docs/ui-reviews/2026-08-22-public-service-landing.md`, 전체 가이드 reader
  근거는 `docs/ui-reviews/2026-08-22-public-product-docs-layout.md`, 언어·상단 navigation 근거는
  `docs/ui-reviews/2026-08-23-public-site-i18n-navigation.md`를 따른다.

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
내부 link 무결성, `guide.html`·`guide-content.md` artifact 부재, 금지된 내부 주소·private repository
표식과 source map 부재를 확인한다. 실제 Pages 배포 후에는 `/`, `/ko/`, `/en/`, 설치 page 직접
본문, 22개 route, legacy 이동, 검색, 좌우 목차, mobile navigation, HTTPS와 custom domain
canonical URL을 확인한다.
