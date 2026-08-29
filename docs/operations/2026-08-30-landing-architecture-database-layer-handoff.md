# 공개 랜딩 서비스 아키텍처 DB 계층 반영 인계

> **일자:** 2026-08-30
> **상태:** LIVE-PASS
> **대상:** PureCVisor 공개 landing `/`, `/ko/`, `/en/`

## 반영 내용

- 서비스 아키텍처 SVG의 범용 SQLite 노드와 단독 Monitoring DB 노드를 책임별 DB 노드
  두 개로 교체했다.
- Core / identity / security / network DB 7개와 Operations DB 3개에 실제 파일명 10개를
  모두 표시했다.
- SQLite 밖 desired state는 별도 노드로 유지해 DB와 전체 서비스 상태의 경계를 보존했다.
- 기존 7계층 palette, white canvas, landing 정보 구조와 native SVG 확대 동작은 유지했다.

## 변경 기준

| 항목 | 값 |
|---|---|
| 제품 정본 commit | `38088967` |
| public 콘텐츠 commit | `3baad6c` |
| Pages run | `33278756764` |
| SVG SHA-256 | `f64b3756dbe546ac65245fa5363d61cbd30e03b1652b53c612ca72e33d685c3b` |
| SVG viewBox | `1849.5234375×2798` |
| live route | `https://purecvisor.site/`, `/ko/`, `/en/` |

## 검증 결과

- GitHub Pages build와 deploy: 성공
- custom domain의 세 landing과 SVG: HTTP 200
- live SVG와 repository 자산: 바이트 단위 일치
- DB 파일명 10개: live SVG에서 모두 확인
- 한국어·영어 HTML: 새 intrinsic width·height와 10개 DB 설명 확인
- 로컬 Pages build: 27개 page, 90개 artifact 통과
- 실브라우저 12개 조합: overflow·console·page·request error·Axe violation 모두 0
- 공개 저장소 필수 검증: C 테스트 1,370개, audit startup 5개, 38개 계약 게이트와 release
  build 통과

## 운영·롤백

- 현재 배포 정본은 public `main`의 `3baad6c`다.
- DB 계층만 롤백해야 하면 직전 public 상태 `b13c4de`의 SVG·landing 계약을 복원한 뒤
  Pages workflow와 custom domain의 asset hash를 다시 확인한다.
- 실제 데이터베이스 내용이나 서버 설정은 변경하지 않았다.
