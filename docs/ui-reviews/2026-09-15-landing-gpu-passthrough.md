# 랜딩 GPU Passthrough 영상 추가 리뷰

## 목표와 구현 진입

공개 랜딩의 기존 기능 영상 선택기에 RTX 3070 Ti 테스트를 추가한다. 대상은 Single Edge를
검토하는 운영자이며, Windows 장치 인식과 실제 Vulkan 렌더링을 확인하는 것이 목적이다.
기존 white/soft-gray/ink/teal, Pretendard, 단일 플레이어와 기능 탭을 시각 기준으로 고정한다.
구현 진입 판정은 **PASS**이며 실제 재생·반응형·배포 확인 후 완료를 판단한다.

## 근거와 결정

- 현재 공개 `https://purecvisor.site/#service-demos`의 3개 기능 탭과 단일 플레이어를 기준으로 한다.
  변경 전 캡처는 로컬 Git 메타데이터의 `gpu-landing-20260915/before-desktop.png`에 보존한다.
- Refero Prisma 스타일 `1a0712ef-d58e-4432-b7ed-0110480e2424` (`https://prisma.io`):
  기술 콘텐츠를 담은 얇은 경계와 선택 상태 중심 accent를 채택한다. 외부 폰트·팔레트는 기각한다.
- Refero Vimeo 스타일 `260ed304-25ca-401f-a0a8-c40f63f9e4fd` (`https://vimeo.com`):
  미디어를 크게 보여 주고 설명을 가까이 배치하는 원칙만 채택한다. 배경 이미지 hero는 기각한다.
- Refero Riverside 화면 `92d89694-6c27-4932-8bee-a67cf18714fe`의 player·장면 선택 패턴과
  기존 영상 리뷰의 기능 선택→사용자 재생→설명/문서 흐름을 유지한다. 신규 다단계 흐름은 없다.
- 2026-09-14 촬영 전달본 `purecvisor-rtx3070ti-gpu-passthrough-ko.mp4`를 그대로 게시한다.
  124초·1920×1080·30fps·무음이며 SHA-256은
  `e02e2d32d9a74a04d5eb1f490ab25de15f986ac63bc72b5394af6d3b46b396db`다.
  GPU 할당/VM 시작은 감사 기록 설명, Windows 장치 확인/렌더링은 실제 녹화임을 구분한다.

| 우선순위 | 결정 | 수용 기준 |
|---|---|---|
| P0 | GPU Passthrough를 4번째 탭으로 추가 | 한국어·영어 3개 랜딩 route에서 총 16편 노출 |
| P0 | 검수된 한국어 자막 완성본을 재편집 없이 게시 | MP4 hash·124초·1920×1080·전체 decode 일치 |
| P0 | GPU의 설명과 VM 가이드 링크를 별도 제공 | 네트워크 원본 안내와 GPU 자막/설명 영상 안내가 혼용되지 않음 |
| P0 | GPU 별도 촬영 원본은 게시하지 않음 | 존재하지 않는 WebM 링크를 만들지 않음; 기존 15편 원본 링크 유지 |
| P1 | desktop 4열, 작은 화면 2열 탭 | 390/768/1440px에서 overflow 없음, 명확한 선택·focus |
| P1 | 선택 영상의 가로세로 비율 사용 | GPU 16:9·기존 녹화 16:10, crop 없음 |
| P1 | 기존 지연 재생과 오류/재시도 유지 | 재생 전 미디어 요청 0, 전환 시 중지, seek·native controls 정상 |

## 구현·검증 순서

1. 영상·실제 프레임 포스터·manifest를 추가하고 기존 15편 hash를 보존한다.
2. 데이터, 텍스트 범위, 선택기, 영상별 비율/원본 링크와 가이드 연결을 갱신한다.
3. 기존 미디어 gate의 네트워크 원본 검사를 유지하면서 GPU 완성본 계약을 추가한다.
4. site check·공개 소스 주석 gate·문법·diff와 실제 브라우저에서 선택·재생·seek·전환을 확인한다.
5. 변경 파일만 공개 main에 반영하고 Pages 배포와 실제 도메인의 파일·재생을 확인한다.

## 구현 후 로컬 판정

**로컬 PASS.** site check가 26페이지·154개 배포 파일, 4개 기능·16편 영상의 계약을
통과했다. 미디어는 225,189,881 bytes로 기존 250 MiB 예산 안이며, GPU MP4의
SHA-256·124초·1920×1080·30fps·H.264/yuv420p·전체 decode 오류 0을 확인했다.
기존 네트워크 15편의 manifest와 미디어 identity는 동일하다.

실제 Chromium에서 `/`·`/ko/`·`/en/`와 390·768·1440px 조합, 초기 미디어 요청 0,
GPU 선택·16:9 표시·원본 링크 숨김·가이드 전환, GPU 재생·60초 seek,
VPC 복귀·16:10 표시·원본 링크 복원·재생 중지, 키보드 End·방향키 순환,
미디어 오류·재시도, dark mobile overflow까지 27개 검사를 통과했다.
변경 후 desktop·mobile·영어·dark·렌더링 프레임을 직접 확인했다.
공개 소스 주석 gate, 디자인 계약과 diff 검사도 통과했다.

캡처·검사 결과는 로컬 Git 메타데이터의 `gpu-landing-20260915/`에 보존한다.
공개 배포 결과는 같은 위치의 후행 운영 인계와 publication receipt로 기록한다.

## 공개 배포 확인 — 2026-09-15

[`52587a5`](https://github.com/HardcoreMonk/purecvisor/commit/52587a5f4cc637d34a6a55a48c0cd76e93c800c9)의
[Pages 실행](https://github.com/HardcoreMonk/purecvisor/actions/runs/34974574546)이 성공했다.
실제 `purecvisor.site`의 `/`·`/ko/`·`/en/`에서 로컬과 같은 27개 브라우저 검사를 통과했고,
GPU 파일 identity·Range 응답·재생·seek와 기존 VPC 복귀를 확인했다.
공개 영상은 지정 GPU의 짧은 기능 검증 기록이며 장시간 안정성 인증을 의미하지 않는다.
