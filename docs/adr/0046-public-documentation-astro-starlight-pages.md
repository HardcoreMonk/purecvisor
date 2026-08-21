# ADR-0046: 공개 문서는 Astro·Starlight GitHub Pages로 운영한다

- **상태:** Approved
- **일자:** 2026-08-21
- **승인:** 2026-08-21 사용자 명시 승인
- **변경:** 2026-08-22 전체 운영 가이드 정본을 `/docs.html`로 전환하도록 사용자 명시 승인
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
3. 문서 build는 Astro 7.2.4와 Starlight 0.41.7의 정적 output을 사용한다.
4. 제품 `ui/`는 Vanilla JavaScript 계약을 유지하고 문서 framework는 `site/` 밖으로 확장하지
   않는다.
5. `docs/GUIDE.md`를 전체 운영 가이드의 단일 진실로 유지하며 build 단계에서 Starlight content로
   생성한다.
6. `/docs.html`을 전체 가이드의 안정적인 공개 URL로 유지하고 `guide.html`은 생성하거나 사용하지
   않는다.
7. GitHub Actions는 `site/dist/`만 Pages artifact로 업로드하고 action dependency는 commit SHA로
   고정한다.
8. DNS cutover는 Pages artifact, domain verification와 HTTPS를 확인한 뒤 별도 운영 단계로
   수행한다.

## Consequences

- 공개 문서 source와 변경 이력을 외부 사용자가 검토하고 기여할 수 있다.
- 검색, sidebar, 접근성 기반과 향후 다국어 확장을 Starlight가 제공한다.
- Node.js와 npm dependency update, Pages workflow와 custom domain이 새 운영 책임이 된다.
- private 원본 저장소의 파일은 Pages artifact에 포함되지 않지만 공개 가이드 내용은 별도
  민감정보 검사를 계속 통과해야 한다.
- `purecvisor.site`를 사용하던 기존 제품 endpoint는 DNS cutover 뒤 별도 hostname이 필요하다.

## Rejected alternatives

- private `purecvisor-single` Pages: GitHub Pro에 종속되고 private 자료의 artifact 혼입 위험이
  커 public 공개 저장소 방식보다 불리하다.
- 기존 단일 HTML 영구 유지: 최초 공개에는 충분하지만 중장기 문서 분할과 기여 흐름의 유지
  비용이 커진다.
- Docusaurus: 제품 버전별 문서 동시 유지가 현재 핵심 요구가 아니므로 더 큰 versioning·React
  운영 표면을 채택하지 않는다.

## Verification

- clean install에서 `npm run check`가 정적 artifact를 재현해야 한다.
- `dist/index.html`과 `dist/docs.html`이 생성되고 전체 guide의 마지막 장까지 포함해야 한다.
- `dist/guide.html`과 `/guide.html` navigation·landing link가 없어야 한다.
- source map과 내부 운영 주소, private repository 표식이 artifact에 없어야 한다.
- GitHub Pages 기본 주소와 custom domain에서 root, docs, 검색과 HTTPS가 동작해야 한다.
- DNS cutover 전후 rollback 경로와 기존 endpoint 분리를 기록해야 한다.
