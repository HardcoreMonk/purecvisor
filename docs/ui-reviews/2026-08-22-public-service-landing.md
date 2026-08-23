# 공개 서비스 landing UI 리뷰

> **2026-08-24 승계:** 이 리뷰의 hero·시각 token·문서 directory 결정은 유지한다. 서비스 기능,
> 시작 흐름과 공개 범위를 문서 directory 앞에 두는 결정은
> `docs/ui-reviews/2026-08-24-landing-section-removal.md`가 대체한다.

> 상태: Verified
> 대상: `https://purecvisor.site/`
> 제품 콘텐츠 정본: 제품 Web UI의 `ui/docs.html`, `docs/GUIDE.md`

## 목표와 사용자 작업

첫 방문자가 문서 목차를 읽기 전에 PureCVisor가 무엇인지, 어떤 단위로 동작하는지,
어떤 운영 표면을 한곳에서 다루는지 이해하게 한다. 이어서 설치·운영 가이드, 기능별 문서와
GitHub 저장소로 이동할 수 있어야 한다.

주요 사용자 작업은 다음과 같다.

1. PureCVisor Single Edge의 역할과 공개 범위를 빠르게 이해한다.
2. 설치와 상태 확인 명령으로 첫 실행 경로를 파악한다.
3. VM·컨테이너, 스토리지, 네트워크, 관측·복구와 인터페이스 기능을 비교한다.
4. 8개 작업 카테고리와 22개 장 중 필요한 문서로 이동한다.

## 현재 화면 관측

2026-08-22에 공개 landing, 제품 문서 landing과 외부 레퍼런스를 데스크톱
`1440 × 1000`, 모바일 `390 × 844`에서 캡처했다.

- 현재 공개 landing은 제목 다음에 공개 문서와 문서 카드가 바로 이어진다. 제품의 정체성,
  아키텍처와 핵심 기능을 독립된 서비스 소개 흐름으로 설명하지 않는다.
- 제품 `ui/docs.html`은 어두운 hero, 네 개의 빠른 진입점, 8개 카테고리·22개 장과 역할별
  경로를 제공한다. 이 taxonomy와 Single Edge 문구를 공개 landing의 콘텐츠 정본으로 쓴다.
- 세 화면 모두 데스크톱과 모바일에서 수평 overflow는 관측되지 않았다.

## 레퍼런스 연구

### opencodex.me

공개 사이트의 현재 landing을 직접 확인했다. 정보 계층은 서비스 한 줄 설명과 CTA,
실행 장면, 핵심 기능, 빠른 시작, 전체 문서 탐색 순서다. 문서 프레임 안에서도 첫 페이지는
제품 설명을 충분히 제공하고, 문서는 마지막 탐색 구간으로 연결한다.

### Refero

Refero에서 developer tool, data infrastructure와 documentation landing을 검색하고 다음
스타일을 상세 비교했다.

- OpenAI Developers: 밝은 canvas, 절제된 neutral surface, 작은 radius와 명확한 문서 진입.
- Grafbase: 흰 배경의 기술적 grid, 좌측 설명·우측 제품 구조, teal CTA의 제한적 사용.
- Depot: 어두운 command-center surface, 얇은 border, code와 상태 정보의 높은 판독성.

## 채택 결정

- opencodex.me의 `서비스 설명 → 시작 흐름 → 기능 → 문서` 정보 계층을 채택한다.
- 제품 문서 landing의 어두운 hero와 Single Edge taxonomy를 콘텐츠 기준으로 채택한다.
- 기본 canvas는 PureCVisor의 흰색·soft gray·ink·teal token을 유지한다.
- 첫 화면에는 실제 제품 개념만으로 구성한 Single Edge 제어면 구조를 표시한다.
- 기능 소개는 카드 수를 늘리는 대신 여섯 기능을 하나의 연속 grid로 비교 가능하게 한다.
- 설치·상태 확인 명령은 정본 가이드의 5분 퀵스타트와 같은 내용을 사용한다.
- CTA는 `5분 퀵스타트`, `전체 운영 가이드`, `GitHub` 세 목적을 구분한다.

## 기각 결정

- opencodex.me의 로고, 문구, 이미지, 애니메이션, provider chip과 브랜드 색은 복제하지 않는다.
- 실제 제품 화면이 아닌 가짜 dashboard 수치와 고객·성능 주장은 만들지 않는다.
- 큰 gradient, glow, 장식용 orb와 자동 재생 scroll animation은 사용하지 않는다.
- Multi Edge, 클러스터 자동화, 라이브 마이그레이션과 페더레이션을 현재 기능처럼 소개하지 않는다.
- 제품 `ui/docs.html`을 iframe으로 배포하거나 private endpoint에서 불러오지 않는다. 전체 운영
  가이드 경로는 `2026-08-22-public-product-docs-layout.md`에 따라 공개 저장소의 검증된
  `ui/docs.html`과 `ui/guide-content.md`를 build artifact로 사용한다.

## 정보 구조

1. 상단 navigation: 서비스, 시작하기, 공개 범위, 문서, 검색과 GitHub
2. hero: Single Edge 정의, 핵심 가치, CTA, 제어면 구조
3. 시작 흐름: 설치, 실행, 상태 확인
4. 기능 표면: 워크로드, 스토리지, 네트워크, 관측·복구, 인터페이스, 안전한 운영
5. 공개 범위: 독립 노드 지원과 제외 범위
6. 문서 탐색: 빠른 경로, 8개 카테고리·22개 장, 역할별 추천

## 수용 기준

- 첫 `1440 × 1000` viewport에서 서비스 정의, 주 CTA와 Single Edge 제어면 구조가 보인다.
- `390 × 844`에서 navigation, hero, CTA와 구조 패널이 단일 열로 재배치되고 수평 overflow가 없다.
- landing에 서비스 소개, 시작 흐름, 여섯 기능과 공개 범위가 문서 탐색보다 먼저 나온다.
- 제품 문서와 같은 8개 작업 카테고리·22개 장 링크가 모두 유효한 docs anchor를 가리킨다.
- keyboard focus가 보이고 axe 자동 검사에서 위반이 없다.
- root, docs deep link, 검색과 모바일 navigation이 실제 정적 build에서 동작한다.
- Pages artifact에 source map, 내부 주소와 비공개 저장소 표식이 없다.

## 검증 결과

2026-08-22 정적 build와 로컬 HTTP artifact를 기준으로 검증했다.

- `npm run check`: 36개 Pages artifact, landing 필수 구간, 6개 기능, 8개 카테고리와
  22개 docs anchor 검증 통과
- 데스크톱 `1440 × 1000`: 첫 viewport에서 hero CTA와 제어면 구조 노출, 수평 overflow 0
- 모바일 `390 × 844`: 단일 열 재배치, hero CTA와 제어면 구조 노출, 수평 overflow 0
- axe: 데스크톱·모바일 위반 0
- 브라우저 console, page error와 failed request: 0
- docs `#22-품질-게이트-가이드` deep link, ZFS 검색과 모바일 sidebar 동작 확인
- `scripts/check_design_md.py`, `tests/integration/test_design_md_surface.sh`,
  `make check-public-comments`와 `git diff --check` 통과
