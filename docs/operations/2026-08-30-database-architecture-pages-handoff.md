# 2026-08-30 데이터베이스 아키텍처 Pages 인계

## 1. 변경 목적

소스에 분산된 PureCvisor Single Edge SQLite 저장소를 GitHub source에서만 읽는 상태를 끝내고,
운영자와 개발자가 `purecvisor.site`의 검색·목차·sidebar 안에서 직접 읽을 수 있는 독립 문서로
배포했다.

## 2. 변경 범위

- `docs/DATABASE_STRUCTURE.md`: SQLite 10개의 책임, 33개 운영 table, 데이터 정본,
  cross-DB 일관성, 초기화 실패, lifecycle, WAL 백업·복원과 schema 변경 계약 현행화
- `/ko/development/database-architecture/`: Starlight 독립 정적 route 추가
- `개발·출시` sidebar: 기존 22개 가이드 뒤에 데이터베이스 아키텍처를 추가해
  8개 그룹·23개 문서로 확장
- `site/scripts/prepare-content.mjs`: 별도 Markdown 작성 정본을 결정적으로 변환
- `site/scripts/check-site.mjs`: route·canonical·본문·private 증거·table focus 계약 추가
- Pages workflow: `docs/DATABASE_STRUCTURE.md` 변경도 자동 배포하도록 path trigger 추가

실제 운영 노드 식별자, 비공개 인계 링크와 비공개 commit 식별자는 공개본에서 제거했다.
landing·Header, Starlight shell, 색상·타이포그래피와 기존 22개 가이드 route는 변경하지 않았다.

## 3. 접근성 보강

모바일 Axe 검증에서 Starlight의 가로 scroll table이 keyboard focus를 받지 못하는 위반을
발견했다. build-time rehype 변환으로 Markdown table에 `tabindex=0`을 추가하고 기본 focus
outline을 유지했다. 데이터베이스 본문의 표 52개가 모두 focusable이며, 모바일에서 실제로
overflow하는 29개 표는 방향키로 내부 horizontal scroll할 수 있다.

## 4. 검증

| 검증 | 결과 |
|---|---|
| `cd site && npm ci` | PASS, 취약점 0 |
| `cd site && npm run check` | PASS, 27 pages·90 artifacts·Pagefind 27 HTML |
| site script `node --check` | PASS |
| `make check-npm-lockfile` | PASS, 26/26 |
| `make check-public-comments` | PASS, first-party source·JavaScript comments 0 |
| `python3 scripts/check_design_md.py` | PASS |
| `bash tests/integration/test_design_md_surface.sh` | PASS |
| `git diff --check` | PASS |
| local Chromium 1440×1000·390×844 | H1·canonical·23 links·본문 정상, page overflow 0 |
| local Axe WCAG A/AA | violation 0 |

시각 근거, reference lock과 최초 Axe 발견·수정 기록은
[제품 UI 리뷰](../ui-reviews/2026-08-30-database-architecture-route.md)에 있다.

## 5. Commit·배포 결과

- 사설 작성 정본 commit: `e834818f`
- 공개 구현 commit: `d07e4c253e86a09583da7fee054c12f59855b919`
- GitHub Pages run:
  [`33277437435`](https://github.com/HardcoreMonk/purecvisor/actions/runs/33277437435),
  build 22초·deploy 13초, 모두 성공
- live route: `https://purecvisor.site/ko/development/database-architecture/`, HTTP 200
- live 계약: 정확한 canonical, active navigation, 8개 그룹·23개 문서 link, 표 52개 전부
  focusable, 최신 Audit·Monitoring Evidence·Web Push 본문, private 운영 증거 0
- live Pagefind: `Monitoring Evidence DB`가 새 route와 제목을 반환
- live Chromium 1440×1000·390×844: page overflow 0, Axe 위반 0,
  console·page·request 오류 0
- live desktop·mobile 캡처 SHA-256은 로컬 캡처와 각각
  `6eb08c5cf25f5fd1d9380dfad25a3411bc7a7e372cc4cef56b79846d06ddb21e`,
  `4dc260f7e7bb47370fd9595746673b77c2155c4e2f6b3d456cf15fcc7416d4d4`로 일치

최종 판정은 **LIVE-PASS**다. 공개 `main`, Pages artifact와 custom domain이 같은
데이터베이스 아키텍처 정본을 제공한다.
