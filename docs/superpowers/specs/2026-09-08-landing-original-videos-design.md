# 랜딩 기능 검증 원본 영상 설계

## 요청과 설계 방향

사용자가 랜딩 편집본을 기능 검증 원본으로 교체하도록 지시했다. 실제 검증 때 저장한
MP4 15개를 그대로 사용하고 대응하는 촬영 WebM 원본을 제공한다. 생성·연결·통신·차단·
복구를 설명하며 기존 3개 기능 탭과 단일 player를 유지한다. 지정된 방향 안의 콘텐츠
시정이며 브랜드 재설계와 제품 runtime 변경은 포함하지 않는다.

## Domain Architecture

`site/src/data/service-videos.mjs`는 기능과 장면 설명, `ServiceShowcase.astro`는 정적
접근성·fallback, `service-showcase.js`는 선택·재생 상태, 미디어 manifest는 파일 identity를
소유한다. 제품 RPC·DB·ADR의 설계 결정은 변경하지 않는다. Local VPC의 Linux backend,
generic OVN, 수동 피어로 연결하는 독립 Single Edge의 경계를 유지한다.

## Plan Grilling

- 원본은 무엇인가: 촬영 WebM과 검증 당시 만든 호환 MP4를 구분하고 둘 다 byte 그대로
  제공한다. MP4를 재인코딩하거나 화면 위에 설명을 합성하지 않는다.
- 생성만 보여도 충분한가: 실제 통신·보안 정책 효과·피어 제거 시 단절·복구를 추가한다.
- 긴 목록에서 설명을 찾을 수 있는가: 현재 장면 제목과 요약을 player 바로 아래로 옮긴다.
- 파일이 커지면 첫 페이지가 느려지는가: MP4/WebM은 사용자 재생·직접 열기 전 요청하지 않는다.
- 기존 편집 파일이 캐시에 남는가: 새 `/assets/service-recordings/` 경로를 사용한다.
- 신규 ADR이 필요한가: 기존 정적 미디어 배포와 재생 계약 내 콘텐츠 시정으로 불필요하다.

## 디자인 검토와 Spec Freeze Snapshot

기존 2026-09-08 서비스 영상 UI 리뷰의 white/teal/Pretendard·기능 탭·실제품 미디어를
유지한다. 이번 세션에는 Refero callable tool이 없어 기존 연구를 대체 근거로 사용한다.
원본 영상 복원은 사용자의 직접 지시를 승인 근거로 삼는다. 새 편집 선택을 요구하지 않는다.

수용 기준: 15쌍 파일 hash 일치, 1440×900 전체 화면, 한국어/영어 결과 설명,
초기 미디어 요청 0, 모든 MP4 재생/seek, 원본 링크, 전환 중지·키보드·오류 재시도,
320~1440px·dark·문서 링크 유지. 파일 경로와 해시는 manifest와 검증 산출물이 소유한다.
공개 서버는 GitHub Pages이며 제품 daemon·VM·호스트 설정은 변경 대상이 아니다.
