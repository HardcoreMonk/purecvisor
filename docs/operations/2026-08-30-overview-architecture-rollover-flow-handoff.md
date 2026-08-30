# 시작하기 아키텍처 연결 흐름 롤오버 운영 인계

> **일자:** 2026-08-30
> **상태:** LIVE-PASS
> **대상:** `/ko/getting-started/overview/#12-아키텍처-개요`

## 반영 내용

- 두 TLS 모드 SVG에서 서비스 레이어 또는 컴포넌트에 정밀 pointer를 올리면 직접 연결된
  node·화살표·label을 강조하고 dash 흐름으로 방향을 표시한다.
- 기본 direct HTTPS SVG는 page 진입 시, 숨겨진 NGINX SVG는 해당 탭을 처음 선택할 때 inline한다.
- touch와 JavaScript 비활성 환경은 기존 정적 SVG·확대 link를 사용한다.
- reduced-motion에서는 이동 animation을 제거하고 정적 색·두께·opacity 강조를 유지한다.
- 원본 SVG node·edge·label·좌표와 checksum은 변경하지 않았다.

## 변경·배포 기준

| 항목 | 값 |
|---|---|
| 기능 commit | `1e38eaf051508db41236921e4009b767438997bc` |
| 직전 public commit | `51dadfc` |
| Pages run | `33291113150` |
| build / deploy | PASS, 21초 / 10초 |
| direct SVG SHA-256 | `f728f3460a50d44ccf388f3daf56d48882323b005de58838cbfa3385e52431b7` |
| NGINX 포함 full SVG SHA-256 | `f64b3756dbe546ac65245fa5363d61cbd30e03b1652b53c612ca72e33d685c3b` |
| live route | `https://purecvisor.site/ko/getting-started/overview/#12-아키텍처-개요` |

## 검증 결과

- local·live direct topology: node 31, edge 40, layer 8
- local·live NGINX topology: node 32, edge 41, layer 9
- live component dash offset: 120ms 사이 `5.7172` 이동
- live layer·NGINX 직접 연결 수와 node 수: 로컬 계약과 일치
- 1920·1280·390px: 가로 overflow 0, 탭·SVG 폭·touch fallback 정상
- reduced-motion animation: `none`, 정적 3px 강조 유지
- JavaScript 비활성: inline SVG 0, 원본 image 2개 정상 로드
- Axe WCAG A/AA 위반과 console·page·request 오류: 0
- 저장소 검증: Pages 27 page·92 artifact, C test 1,370개, audit startup 5/5,
  공개 계약 gate 38개, release build 모두 PASS

상세 설계 근거와 local·live 캡처 SHA-256은
[UI 리뷰](../ui-reviews/2026-08-30-overview-architecture-rollover-flow.md)에 기록했다.

## 운영·롤백

- inline fetch·parse·topology 검증이 실패하면 사용자에게 오류 UI를 노출하지 않고 원본 `<img>`를
  유지하므로 확대 link와 전체 구조 읽기는 계속 가능하다.
- hover 동작 회귀 시 브라우저의 SVG asset 응답 `Content-Type: image/svg+xml`, console 오류,
  `data-pcv-architecture-state`의 `ready` 또는 `fallback` 상태를 우선 확인한다.
- 기능만 롤백해야 하면 `51dadfc`의 overview interaction·CSS·검증 계약을 복원한 뒤 Pages workflow와
  custom domain의 정적 SVG fallback을 다시 확인한다.
- 서버 네트워크, 데이터베이스, `purecvisorsd`, NGINX 운영 설정은 변경하지 않았다.
