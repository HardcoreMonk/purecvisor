# 공개 문서 사이트 운영 기준

> 상태: Approved, 로컬 구현·검증 완료, 최초 commit·push·Pages 활성화 대기
> 승인일: 2026-08-21
> 공개 주소: `https://purecvisor.site`
> 저장소: `HardcoreMonk/purecvisor`

## 목적

PureCVisor 2.0.0 공개 문서는 public GitHub 저장소에서 Astro와 Starlight로 빌드하고 GitHub
Pages에 정적 artifact로 배포한다. 제품 Web UI와 문서 사이트의 source, build와 runtime을
분리하면서 `docs/GUIDE.md`를 공개 운영 가이드의 단일 진실로 유지한다.

## 운영 경계

- 제품 runtime UI는 `ui/`의 Vanilla JavaScript 계약을 유지한다.
- 공개 문서 site만 `site/`에서 Astro 7.2.4와 Starlight 0.41.7을 사용한다.
- GitHub Actions는 `site/dist/`만 Pages artifact로 업로드한다.
- source map, 비공개 운영 기록, 인증정보와 allowlist 밖 파일은 Pages에 포함하지 않는다.
- `https://purecvisor.site/guide.html`은 전체 운영 가이드의 호환·정본 URL로 유지한다.
- `site/scripts/prepare-content.mjs`가 `docs/GUIDE.md`를 Starlight content로 생성한다.

## 시각 기준

- PureCVisor의 흰 canvas, soft gray, ink와 teal token 역할을 유지한다.
- opencodex.me에서 확인한 문서 중심 landing, 검색과 3단 reader 구조만 참고한다.
- 외부 사이트의 logo, 고유 문구, 이미지와 브랜드 자산은 복제하지 않는다.
- 본문 가독성, keyboard focus, mobile navigation과 code overflow를 운영 기준으로 검증한다.

## 배포 흐름

```text
docs/GUIDE.md + site source
        |
        v
Astro + Starlight static build
        |
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

검증은 `index.html`, `guide.html`, 정본 guide 내용, 금지된 내부 주소·private repository 표식,
source map 부재와 정적 artifact 생성을 확인한다. 실제 Pages 배포 후에는 root, guide deep link,
검색, mobile navigation, HTTPS와 custom domain canonical URL을 확인한다.
