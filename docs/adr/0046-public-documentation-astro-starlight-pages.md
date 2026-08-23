# ADR-0046: 공개 문서는 Astro·Starlight GitHub Pages로 운영한다

- **상태:** Verified
- **일자:** 2026-08-21
- **승인:** 2026-08-21 사용자 명시 승인
- **변경:** 2026-08-22 전체 운영 가이드 정본을 `/docs.html`로 전환하도록 사용자 명시 승인
- **변경:** 2026-08-22 `/docs.html`을 제품 `ui/docs.html` 레이아웃으로 전환하도록 사용자 명시 승인
- **변경:** 2026-08-23 한국어 기본·`/ko/`·`/en/` route와 그룹형 상단 navigation 사용자 명시 승인
- **검증:** 2026-08-23 commit `ca3a399`, Pages run `32645170384`, custom domain 운영 브라우저 검증
- **Single Edge 적용 상태:** 제품 runtime과 분리된 공개 문서 build·hosting 계약
- **관련:** ADR-0013, ADR-0016, ADR-0037

## Context

PureCVisor 공개 문서는 제품 Web UI와 별도로 장기간 유지해야 한다. 기존 정적 `guide.html`은
기능과 접근성 검증을 통과했지만 문서 분할, 다국어, 검색 index와 외부 기여가 늘어나면 직접
유지 비용이 커진다. 원본 개발 저장소는 private이고 공개판은 깨끗한 이력의
`HardcoreMonk/purecvisor` 저장소에서 운영한다.

## Decision

1. `https://purecvisor.site`는 public GitHub Pages 문서 사이트가 소유한다.
2. Pages source repository는 public `HardcoreMonk/purecvisor`다.
3. 공개 landing과 Pages build는 Astro 7.2.4와 Starlight 0.41.7의 정적 output을 사용한다.
4. 제품 `ui/`는 Vanilla JavaScript 계약을 유지하고 문서 framework는 `site/` 밖으로 확장하지
   않는다.
5. `docs/GUIDE.md`를 전체 운영 가이드의 공개 작성 정본으로 유지한다. 최종 reader는 같은 공개
   릴리스의 `ui/docs.html`과 `ui/guide-content.md`를 사용한다.
6. Astro build 뒤 `/docs.html`을 제품 문서 shell로 교체하고 base·제품 전용 link와 relative
   source link만 공개 domain 경계에 맞게 정규화한다.
7. `/docs.html`을 전체 가이드의 안정적인 공개 URL로 유지하고 `guide.html`은 생성하거나 사용하지
   않는다.
8. GitHub Actions는 `site/dist/`만 Pages artifact로 업로드하고 action dependency는 commit SHA로
   고정한다.
9. DNS cutover는 Pages artifact, domain verification와 HTTPS를 확인한 뒤 별도 운영 단계로
   수행한다.
10. `/`은 한국어 기본 landing이고 `/ko/`는 명시적 한국어, `/en/`은 영어 landing이다. 상단
    `서비스`, `시작하기`, `공개 범위`, `문서`는 하위 링크 disclosure이며 전체 운영 가이드
    action은 `/docs.html` 안정 URL을 사용한다.

## Consequences

- 공개 문서 source와 변경 이력을 외부 사용자가 검토하고 기여할 수 있다.
- 공개 landing과 build 기반은 Starlight가, 전체 가이드 검색·좌우 navigation과 모바일 drawer는
  제품 문서 shell이 제공한다.
- Node.js와 npm dependency update, Pages workflow와 custom domain이 새 운영 책임이 된다.
- private 원본 저장소의 파일은 Pages artifact에 포함되지 않지만 공개 가이드 내용은 별도
  민감정보 검사를 계속 통과해야 한다.
- `purecvisor.site`를 사용하던 기존 제품 endpoint는 DNS cutover 뒤 별도 hostname이 필요하다.

## Rejected alternatives

- private `purecvisor-single` Pages: GitHub Pro에 종속되고 private 자료의 artifact 혼입 위험이
  커 public 공개 저장소 방식보다 불리하다.
- Starlight 기본 reader 유지: 제품 `ui/docs.html`과 정보 구조·검색·좌우 목차가 달라 공개와 제품
  문서 경험이 계속 어긋난다.
- Docusaurus: 제품 버전별 문서 동시 유지가 현재 핵심 요구가 아니므로 더 큰 versioning·React
  운영 표면을 채택하지 않는다.

## Verification

- clean install에서 `npm run check`가 정적 artifact를 재현해야 한다.
- `dist/index.html`, 제품 shell 기반 `dist/docs.html`과 `dist/guide-content.md`가 생성되고 전체
  guide의 마지막 장까지 포함해야 한다.
- `dist/ko/index.html`과 `dist/en/index.html`이 각각 `lang=ko`, `lang=en`으로 생성되고 root는
  한국어를 유지해야 한다.
- 세 landing은 네 disclosure group, KO·EN 전환과 `/docs.html` 전체 운영 가이드 link를
  제공해야 한다.
- `dist/guide.html`과 `/guide.html` navigation·landing link가 없어야 한다.
- 제품 전용 `/ui/` base·link가 공개 artifact에 없어야 하며 font와 icon을 same-origin에서
  제공해야 한다.
- source map과 내부 운영 주소, private repository 표식이 artifact에 없어야 한다.
- GitHub Pages 기본 주소와 custom domain에서 root, docs, 검색과 HTTPS가 동작해야 한다.
- DNS cutover 전후 rollback 경로와 기존 endpoint 분리를 기록해야 한다.
