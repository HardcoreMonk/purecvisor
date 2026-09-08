# 랜딩 기능 검증 원본 영상 구현 계획

> 실행: 현재 세션에서 순서대로 구현·검증한다.

**목표:** 편집 영상을 기능 검증 당시 파일과 실제 결과 설명으로 교체한다.

**구조:** 기존 데이터·단일 player를 유지하고 미디어 경로, 15개 장면과 현재 장면 설명을 갱신한다.

## 작업

- [x] `site/scripts/check-service-videos.mjs:11`의 6편 계약을 15편·MP4/WebM·1440×900·
  원본 hash·정확한 파일 목록 계약으로 변경한다. 기존 데이터에서 실패를 확인한다.
- [x] 성공 촬영의 MP4와 WebM을 `site/public/assets/service-recordings/`에 복사한다.
  `manifest.json`에 source 파일명·hash·duration·원본 변환 구분을 남기고 실제 포스터를 추출한다.
- [x] `site/src/data/service-videos.mjs:1`의 `media(id)`에 `original` 링크를 추가한다.
  Local VPC 7편, OVN 4편, VXLAN 4편과 각 장면의 한국어/영어 목적·결과를 명시한다.
- [x] `ServiceShowcase.astro:34`의 미디어 치수를 1440×900으로 복원하고 현재 장면
  설명과 원본 링크를 player 아래에 둔다. 장면 수는 데이터에서 계산한다.
- [x] `service-showcase.js:48`의 `selectClip(next)`에서 현재 장면 제목·원본 링크를 갱신한다.
  `custom.css:1183`의 frame ratio와 설명 배치만 조정한다.
- [x] `npm run check`, 공개 주석 gate, 디자인 gate와 `git diff --check`를 실행한다.
  브라우저에서 세 route·반응형·15개 재생/seek·원본 링크·오류·키보드를 확인한다.
- [x] diff와 원본 identity를 리뷰하고 운영 문서·인계에 완료 범위와 게시 상태를 기록한다.

## 엔지니어링 검토

소비 인터페이스는 위 파일의 실제 데이터와 `selectClip(next)`에서 확인했다. 기존 play
generation fencing과 DOM textContent 경로를 유지한다. MP4는 H.264/yuv420p/faststart로
검증 당시 생성된 파일이며 새 encoder가 필요 없다. WebM은 직접 원본 열기용이다.
새 미디어의 총 budget은 250 MiB, 개별 파일은 50 MiB 미만으로 제한한다. Node와 Astro
버전은 lockfile을 따른다. 검증은 Chromium 실제 decode와 filesystem hash를 사용한다.
원복은 이번 변경만 복구하는 commit과 Pages 재발행이며 제품 runtime은 영향받지 않는다.
검토 판정: 구현 가능, 차단 이슈 없음. 영상 내용의 공개 적합성과 실제 재생은 구현 후 확인한다.
