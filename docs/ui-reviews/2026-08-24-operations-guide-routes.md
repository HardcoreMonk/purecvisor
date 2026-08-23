# 운영 가이드 독립 문서 URL·본문 UI 리뷰

> 상태: PASS, 로컬 구현·자동·실브라우저 검증 완료, Pages 배포 전
> 대상: 공개 운영 가이드 22개 장과 landing·상단 navigation의 모든 가이드 링크
> 관련 결정: ADR-0046, ADR-0047

## 목표와 사용자 작업

운영자가 landing이나 상단 메뉴에서 작업을 선택하면 단일 `/docs.html` shell을 거치지 않고
`/ko/<분류>/<문서>/` 주소에서 해당 장의 제목과 본문을 즉시 읽어야 한다. 현재 문서 위치는
URL, 좌측 전체 문서 navigation의 active item, 우측 현재 페이지 목차로 함께 드러나야 하며,
인접 장은 이전·다음 이동으로 이어져야 한다.

## 현재 상태 증거

- 관측 시각: 2026-08-24 Asia/Seoul
- 현재 운영 진입은 `/docs.html#<장-anchor>` 한 파일에 22개 장을 동적으로 표시한다.
- desktop `1440 × 1000` 캡처:
  `.scratch/ui-reviews/2026-08-24-operations-guide-routes/before-purecvisor-docs.png`
  — `2a8a92d25b3547e5ad447bacf6ec5d13f61ee705c218733949df0e872fbb71c6`
- 현재 URL만으로 분류와 문서를 읽을 수 없고, landing·상단 메뉴 링크가 모두 `/docs.html`의
  hash 계약에 묶여 있다.

## 연구 근거

| 출처 | ID/URL | 관찰 | 적용 |
|---|---|---|---|
| opencodex.me 설치 | [한국어 설치 문서](https://opencodex.me/ko/getting-started/installation/) | `언어/분류/문서` URL, 직접 노출되는 H1·본문, 그룹형 좌측 navigation, 우측 페이지 목차, 이전·다음 문서 | URL·문서 reader 정보 구조의 직접 근거 |
| opencodex.me 캡처 | `.scratch/ui-reviews/2026-08-24-operations-guide-routes/reference-opencodex-installation.png` — `039214a8a4c7e6e7628ac4198c5ea1a2e572bd20aa6201767d20b38f71aa6b37` | 장식보다 읽기 폭과 현재 위치를 우선한 Starlight 3열 문서 구조 | PureCVisor Starlight 기본 reader를 유지하는 근거 |
| Refero Raycast Docs | [`079d9e56-e565-40e4-8509-d5f93435de2b`](https://refero.design/pages/079d9e56-e565-40e4-8509-d5f93435de2b) | 좌측 전체 navigation, 680–820px 본문, 우측 contextual 목차, code block과 이전·다음 이동 | 긴 기술 문서의 3열과 선형 탐색 근거 |
| Refero 1Password Docs | [`640b11d4-8629-4178-9211-e2ada11425a2`](https://refero.design/pages/640b11d4-8629-4178-9211-e2ada11425a2) | 설치·업데이트 문서를 독립 페이지로 제공하고 active sidebar, code, 이전·다음 제어를 결합 | 설치 문서의 작업 단위 분리 근거 |
| Refero Fingerprint Docs flow | [`11143`](https://refero.design/flows/11143) | 검색 결과 선택 후 고유 URL의 전체 문서 article과 code가 즉시 열림 | landing/search에서 직접 article로 이동하는 흐름 근거 |

`refero-design` 보조 스킬은 설치되어 있지 않아 새 설치를 수행하지 않았고, Refero MCP의
screen·flow 검색과 상세 조회를 직접 사용했다. 외부 화면의 브랜드·색상·문구는 복제하지 않는다.

## 라우트 매핑

| 장 | 새 정본 경로 |
|---|---|
| 1. 시작하기 | `/ko/getting-started/overview/` |
| 2. 설치 및 환경 구성 | `/ko/getting-started/installation/` |
| 3. VM 관리 | `/ko/workloads/virtual-machines/` |
| 4. 컨테이너 관리 | `/ko/workloads/containers/` |
| 5. 스토리지 | `/ko/infrastructure/storage/` |
| 6. 네트워크 | `/ko/infrastructure/networking/` |
| 7. 멀티 제어면 참고 기록 | `/ko/infrastructure/multi-control-plane-notes/` |
| 8. 모니터링 & 알림 | `/ko/operations/monitoring-alerts/` |
| 9. 백업 & 복원 | `/ko/operations/backup-restore/` |
| 10. 보안 | `/ko/security/security/` |
| 11. 클라우드 마이그레이션 | `/ko/security/cloud-migration/` |
| 12. AI & 자가치유 | `/ko/security/ai-self-healing/` |
| 13. Web UI | `/ko/interfaces/web-ui/` |
| 14. REST API | `/ko/interfaces/rest-api/` |
| 15. CLI 레퍼런스 | `/ko/interfaces/cli/` |
| 16. 설정 레퍼런스 | `/ko/reference/configuration/` |
| 17. 트러블슈팅 | `/ko/reference/troubleshooting/` |
| 18. 부록 | `/ko/reference/appendix/` |
| 19. 개발자 & 엔지니어 가이드 | `/ko/development/engineering/` |
| 20. 영업 & 마케팅 가이드 | `/ko/development/sales-marketing/` |
| 21. 아키텍처 리팩토링 가이드 | `/ko/development/architecture-refactoring/` |
| 22. 품질 게이트 가이드 | `/ko/development/quality-gates/` |

## 채택 결정

- `docs/GUIDE.md` 정본은 유지하고 build 준비 단계에서 숫자형 H2 장 22개를 독립 Markdown
  page로 결정적으로 분할한다.
- `/ko/getting-started/installation/`을 `전체 운영 가이드`의 기본 진입으로 사용한다.
- 각 page는 Starlight의 H1, 좌측 8개 그룹·22개 링크, 오른쪽 현재 페이지 목차, 검색,
  code copy와 이전·다음 page navigation을 사용한다.
- landing, 상단 disclosure, 빠른 진입, 카테고리 22개, 역할별 추천 경로의 `/docs.html#...`
  링크를 모두 새 정본 route로 교체한다.
- 기존 `/docs.html`은 bookmark를 깨지 않도록 legacy hash를 해당 새 route로 보내는 호환
  redirect만 유지하고 신규 navigation에는 노출하지 않는다.
- 공개 운영 본문의 현재 정본 언어는 한국어이므로 영어 landing에서도 한국어 정본 route로
  연결한다. 전체 가이드가 영어로 번역된 것처럼 `/en/...` 복제본을 만들지 않는다.

## 기각 결정

- 22개 장을 수작업 복제해 `docs/GUIDE.md`와 이중 정본으로 만들지 않는다.
- 한 page에서 JavaScript로 Markdown 전체를 fetch한 뒤 hash에 따라 장을 바꾸는 reader를
  신규 정본으로 유지하지 않는다.
- opencodex.me의 로고, 고유 문구, 브랜드 색, 페이지 분류명을 복제하지 않는다.
- 한 장을 수십 개의 작은 page로 과도하게 쪼개거나 한국어 본문을 자동 번역해 영어 정본으로
  표시하지 않는다.

## 수용 기준

- 22개 route가 directory-format HTML로 생성되고 모두 HTTP 200에 해당하는 정적 artifact를 가진다.
- `/ko/getting-started/installation/`을 직접 열면 `설치 및 환경 구성` H1과 본문 첫 section이
  별도 click이나 fetch 대기 없이 HTML에 존재한다.
- 좌측 sidebar는 8개 그룹과 22개 장을 노출하고 현재 page에 `aria-current=page`를 제공한다.
- 오른쪽 목차와 이전·다음 navigation이 현재 장의 heading·인접 장에 맞는다.
- landing·Header의 운영 가이드 링크에 `/docs.html`이 남지 않고 22개 directory link가 정확하다.
- `/docs.html`과 기존 숫자형 장 hash는 대응하는 새 route로 호환 이동한다.
- 1440·1280px에서 3열 reader와 code overflow가 정상이고, 390px에서 sidebar가 drawer로
  축소되며 수평 overflow가 없다.
- build link gate, Pagefind, axe, browser console·page·request 오류가 모두 통과한다.

## 구현 후 검증

- `npm run check`: 26개 HTML page, 22개 운영 가이드 route와 총 86개 Pages artifact 통과
- 설치 page HTML에 `설치 및 환경 구성` H1과 `2.1 시스템 요구사항` 본문이 정적으로 포함되고
  `guide-content.md` fetch가 발생하지 않음
- Chromium `1440 × 1000`: sidebar 8개 그룹·22개 link, 현재 설치 page, 우측 목차 22개,
  이전 `시작하기`·다음 `VM 관리`, overflow 0 확인
- Chromium `1280 × 900`: 3열 reader와 overflow 0 확인
- Chromium에서 22개 route 전체의 H1, active sidebar item, 22개 sidebar link, 본문과 overflow 0을
  순회 검증하고 page·request 오류 0을 확인함
- Chromium `390 × 844`: mobile menu, H1·본문 즉시 노출과 overflow 0 확인
- Pagefind에서 `트러블슈팅` 검색 결과가 `/ko/reference/troubleshooting/`로 연결됨
- `/docs.html#3-vm-관리`가 `/ko/workloads/virtual-machines/`로 호환 이동함
- 26개 HTML의 실제 anchor 기준 내부 link 무결성 통과
- axe 위반, browser console·page·request 오류 0
- `python3 scripts/check_design_md.py`, `tests/integration/test_design_md_surface.sh`,
  `make check-public-comments`, `python3 scripts/strip_source_comments.py --check`, `git diff --check` 통과
- UI workflow의 `scripts/check_spec_lifecycle.py`와 `scripts/tests/test_spec_lifecycle.py`는 공개
  저장소에 존재하지 않아 실행 대상이 아니며, 공개 사이트 계약은 `npm run check`가 검증함
- desktop 캡처:
  `.scratch/ui-reviews/2026-08-24-operations-guide-routes/after-installation-desktop.png`
  — `fcfd1ce81236fdc378de8b09c199330572b9cab78058962fc8c66b8e8dc36d38`
- mobile 캡처:
  `.scratch/ui-reviews/2026-08-24-operations-guide-routes/after-installation-mobile.png`
  — `3ed3a79f7b23cb2eb06525e2371a65f5b73673bf89b53ce303c1c3b6a38806ea`
- Pages push·배포와 custom domain 재검증은 수행하지 않았으므로 ADR-0047 상태는 `Implemented`로
  유지한다.
