# 공개 문서 사이트 운영 기준

> 상태: 운영 중, main push 기반 Pages 자동 배포
> 승인일: 2026-08-21
> 공개 주소: `https://purecvisor.site`
> 저장소: `HardcoreMonk/purecvisor`

## 목적

PureCVisor 2.0.0 공개 문서는 public GitHub 저장소에서 Astro와 Starlight로 빌드하고 GitHub
Pages에 정적 artifact로 배포한다. 공개 서비스 landing은 제품 Web UI runtime과 분리하면서
`docs/GUIDE.md`를 공개 가이드 작성 정본으로 유지하고, 전체 운영 가이드는 같은 공개 릴리스의
제품 문서 shell과 reader 콘텐츠를 사용한다.

## 운영 경계

- 제품 runtime UI는 `ui/`의 Vanilla JavaScript 계약을 유지한다.
- 공개 서비스 landing과 Pages build는 `site/`에서 Astro 7.2.4와 Starlight 0.41.7을 사용한다.
- `/docs.html`은 공개 저장소의 `ui/docs.html` 제품 문서 shell을 최종 artifact로 사용한다.
- GitHub Actions는 `site/dist/`만 Pages artifact로 업로드한다.
- source map, 비공개 운영 기록, 인증정보와 allowlist 밖 파일은 Pages에 포함하지 않는다.
- `https://purecvisor.site/docs.html`을 전체 운영 가이드의 정본 URL로 유지한다.
- `guide.html`은 생성하거나 navigation과 landing link에서 사용하지 않는다.
- `site/scripts/prepare-content.mjs`가 `docs/GUIDE.md`를 Astro build와 검색용 content로 생성하고,
  `site/scripts/publish-product-docs.mjs`가 최종 `docs.html`을 제품 문서 shell로 교체한다.
- landing source는 `site/src/content/docs/index.mdx`이며 제품 문서 포털과 같은 8개 작업
  카테고리·22개 장·역할별 추천 경로를 공개 사이트 구조로 제공한다.

## 콘텐츠 동기화 계약

- `docs/GUIDE.md`가 전체 운영 절차와 22개 장의 공개 작성 정본이다.
- `ui/guide-content.md`는 같은 릴리스의 제품·공개 reader 콘텐츠이며 `/docs.html`이 browser에서
  직접 읽는다.
- `site/src/content/docs/index.mdx`는 각 장의 설명을 복제하지 않고 정확한 docs anchor로
  연결한다.
- 최종 publish는 `<base href="/ui/">`와 제품 전용 `/ui/` link를 공개 root 기준으로 바꾸고,
  relative source link를 공개 GitHub 저장소 URL로 정규화한다.
- 카테고리나 장 구성이 바뀌면 제품 포털, 공개 landing과 `site/scripts/check-site.mjs`의
  링크·anchor gate를 같은 릴리스 단위로 갱신한다.

## 시각 기준

- PureCVisor의 흰 canvas, soft gray, ink와 teal token 역할을 유지한다.
- 첫 페이지는 서비스 설명, 시작 흐름, 핵심 기능과 공개 범위를 문서 탐색보다 먼저 제공한다.
- opencodex.me에서 확인한 서비스 소개에서 문서 탐색으로 이어지는 정보 계층, 검색과 3단
  reader 구조만 참고한다.
- `/docs.html`은 제품 포털과 같은 floating header, 좌측 장 navigation, 중앙 본문과 우측 현재
  장 목차를 사용한다.
- 외부 사이트의 logo, 고유 문구, 이미지와 브랜드 자산은 복제하지 않는다.
- 본문 가독성, keyboard focus, mobile navigation과 code overflow를 운영 기준으로 검증한다.
- landing 근거는 `docs/ui-reviews/2026-08-22-public-service-landing.md`, 전체 가이드 reader
  근거는 `docs/ui-reviews/2026-08-22-public-product-docs-layout.md`를 따른다.

## 배포 흐름

```text
docs/GUIDE.md + site landing        ui/docs.html + ui/guide-content.md
        |                                      |
        v                                      v
Astro + Starlight static build      product docs artifact publish
        |                                      |
        +------------------+-------------------+
                           v
               site/dist exact Pages artifact
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

검증은 `index.html`, 제품 `docs.html` shell, `guide-content.md`, local font·icon, `guide.html`
artifact·link 부재, 금지된 내부 주소·private repository 표식, source map 부재와 정적 artifact
생성을 확인한다. 실제 Pages 배포 후에는 root, docs deep link, 검색, 좌우 목차, mobile
navigation, HTTPS와 custom domain canonical URL을 확인한다.
