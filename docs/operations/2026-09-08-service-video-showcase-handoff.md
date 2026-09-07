# 서비스 영상 소개 운영 인계

> 상태: Release 준비 — 로컬 구현·브라우저 검증 통과
> 공개 주소: <https://purecvisor.site/#service-demos>
> 설계: [서비스 영상 소개](../superpowers/specs/2026-09-08-service-video-showcase-design.md)
> 계획: [구현 계획](../superpowers/plans/2026-09-08-service-video-showcase.md)

## 변경과 검증

landing의 hero와 문서 맵 사이에 큰 제품 영상·기능 선택·장면 목록을 추가했다. 기존
흰 canvas·청록색·Pretendard와 설치·퀵스타트·문서 22개를 보존한다. 서비스 메뉴와
hero에서 바로 진입한다. root·ko/en의 동일 정보 구조와 두 언어 설명을 제공한다.

| 기능 | 장면 | 길이 |
|---|---|---:|
| Local VPC | VPC와 첫 서브넷 | 39.480초 |
| Local VPC | 두 번째 서브넷 | 42.440초 |
| OVN SDN | 논리 스위치·DHCP | 22.120초 |
| OVN SDN | 라우터·NAT | 26.480초 |
| VXLAN | 네트워크 구성 | 23.433초 |
| VXLAN | 피어 연결 | 48.633초 |

MP4 6개의 합계는 2,939,786 bytes이고 포스터 포함 미디어는 3,170,440 bytes다.
1280×754, H.264/yuv420p, faststart, 무음이며 전체 frame decode를 통과했다.
상단 계정 표시를 crop하고 VXLAN 운영 receipt를 기능 설명으로 교체했다. 원본 녹화의
길이와 제품 동작은 유지했다. 각 장면에는 별도 텍스트 설명을 제공한다.

- 사이트 26페이지·119개 artifact build·기존 문서 gate와 미디어 전용 gate PASS.
- 실제 localhost 9개 route/viewport 조합(320/390/768/1024/1440px)에서 가로 넘침 0,
  초기 MP4 요청 0, 문서 링크 22개 유지. 6개 영상의 실제 재생·길이·해상도 확인.
- 탭 방향키·Home/End, 종료·seek, 미완료 play 중 기능 전환, 네트워크 실패 후 재시도,
  dark·reduced motion·JavaScript 없는 직접 링크, 문서·검색·구주소 redirect PASS.
- 공개 자체 소스 설명 주석 0, JavaScript 101파일 주석 0. 디자인 계약·diff 검증 PASS.
- 같은 에이전트가 별도로 코드·시각을 검토했다. 독립 리뷰는 수행하지 않았다.

## 게시와 복구

게시 전 기준 commit은 `aedde77ed25875a3c0199e5cd2850639dafe932e`다. site 변경을 원복하는
commit을 main에 반영해 Pages를 재발행한다. 제품 daemon·VM·인증 DB는 이번 배포에
포함하지 않는다. 실제 게시 commit·workflow·HTTPS 확인 결과는 배포 후 기록한다.

## 검증 해석

촬영된 시험 환경의 기능 소개이며 모든 지원 환경의 인증이나 전체 감사 완료를 의미하지
않는다. OVN은 CLI 구성·UI 조회, VXLAN은 두 독립 Single Edge의 수동 peer 연결이다.
영상은 무음이고 영어 landing에서도 원본 화면 언어는 한국어다. 설명은 한국어·영어로
제공한다. 실제 iOS/Android 단말 대신 Chromium viewport·미디어 조건으로 검증했다.
