# 랜딩 기능 검증 원본 영상 교체 인계

## Release Scope

사용자 지시에 따라 기존 편집 영상 6편을 기능 검증 당시 MP4 15편과 촬영 WebM 원본으로
교체한다. Local VPC 7편, generic OVN 4편, VXLAN 4편에 생성·VM 연결·통신·차단·복구·
삭제 결과 설명을 한국어와 영어로 제공한다. MP4는 총 879.24초이며 33,367,621 bytes다.
WebM 153,931,686 bytes와 실제 프레임 포스터를 합친 미디어는 188,229,167 bytes다.

1440×900 전체 화면과 검증 결과 패널을 보존했다. 재편집·crop·배속·합성·재인코딩 없이
검증 때 저장한 MP4를 복사하고 WebM을 별도로 제공한다. MP4가 WebM 원본과 같은
인코딩이라는 뜻은 아니다. `manifest.json`은 각 파일명·hash·치수·길이를 기록한다.
기존 편집 자산은 새 Pages artifact에서 제외했다. 과거 git과 로컬 사본으로 복구 가능하다.

## Verification

- `npm run check`: 26페이지·152개 artifact, 15개 녹화의 hash·faststart·원본 링크·
  정확한 파일 목록·예산·지연 로딩·fallback PASS.
- MP4 15개와 WebM 15개의 전체 프레임 디코딩 및 검증 당시 manifest 대조 PASS.
- Chromium: 9개 route/viewport 조합, 가로 넘침 0, 초기 MP4/WebM 요청 0,
  문서 링크 22개 유지, MP4 15개 실제 재생·seek PASS.
- 키보드 방향키/Home/End, 전환 중지, 미완료 play 중 장면 변경, 실패·재시도,
  종료, dark, reduced motion, 검색·문서·구주소 이동, JavaScript 없는 직접 링크 PASS.
- 실제 HTTP로 미디어 45개 hash·MIME 대조, MP4/WebM 30개 `206 Content-Range`,
  WebM 15개 브라우저 직접 재생 PASS.
- `make check-public-comments`, `python3 scripts/check_design_md.py`, `git diff --check` PASS.
- 공개 content commit `ee19f49705422cbfb1a431a907c55259d3c481e3`,
  [Pages run 34200214552](https://github.com/HardcoreMonk/purecvisor/actions/runs/34200214552)
  SUCCESS, 게시 완료 2026-09-08 16:37:58 KST.
- 실제 `https://purecvisor.site`에서 9개 route/viewport·15편 MP4 재생/seek·오류 재시도·
  키보드·검색·noscript와 WebM 15편 직접 재생 PASS. 예기치 않은 HTTP·JS 오류 0.
- 공개 HTML 3개·manifest·미디어 45개 총 49개 hash가 로컬과 일치한다.
  MP4/WebM 30개 MIME·range 206 PASS, 이전 편집 `vxlan-peer.mp4` 주소는 404다.

## Audit

별도 에이전트가 데이터·컴포넌트·재생 로직·CSS·gate와 spec/plan을 읽기 전용으로
검토하고 MP4/WebM 30개를 원본 경로와 독립 hash 대조했다. 보고된 결함은 없으며
코드 기준 게시 준비 완료 판정이다. 원본 15편의 시각 샘플도 검토했다.
기존 Refero 연구를 계승해 실제 제품 화면과 인접 설명 배치를 유지했다. 이번 세션에는
Refero callable tool이 없어 신규 실시간 조회는 수행하지 않았다.

## Blockers

없음. 공개 Pages 반영과 실제 도메인 검증을 완료했다.

## Warnings

원본에는 시험 당시 계정 표시·시험 주소·검증 도구 패널이 남는다. 검증 패널을 제품의
기본 UI로 설명하지 않는다. 영상 화면은 한국어 무음이며 영어는 페이지 설명으로 제공한다.

## Residual Risk

실제 iOS/Android 기기에서의 검증은 수행하지 않았다. Chromium의 viewport·미디어
조건으로 확인했다. 네트워크 기능 자체를 이번 공개 사이트 작업에서 재실행하지 않았다.
과거 시험 결과를 현재 모든 설치 환경의 인증이나 전체 감사 완료로 확대하지 않는다.

## Current Lifecycle Stage

operate — 공개 content push·Pages 게시·실제 도메인 검증을 완료했다.

## Next Action

없음. 현재 원본 영상과 서비스 설명을 운영한다.

## Follow-Up Tasks

없음. 원복이 필요하면 이 교체 diff만 되돌리는 commit과 Pages 재발행을 사용한다.
