# 공개 소스 개발 main 정합화 인계

> 기준일: 2026-10-02 KST
> 범위: PureCVisor Single Edge 공개 소스와 기능·설치·검증 문서
> 게시 기준: 공개 `main`의 `db78d15`
> 상태: 공개 게시용 로컬 검증·검토 완료

## 요청과 입력

사용자가 공개 저장소를 개발 main과 동일하게 처리하고 소스코드 주석을 제거하도록
요청했다. 개발 main 작업 트리의 공개 허용 제품 로직을 기준으로 하며, 이미 공개한
첫 알림 쿨다운·검사 시정과 NVRAM 삭제·롤백 보호를 보존한다. 개발 Git 이력과
비공개 운영 문서는 전달하지 않는다.

미병합 후속 브랜치의 서버 ISO 탐색·검색, 정지 VM ISO 저장·재조회, Windows 설치
프로필·ISO 판독, VM 전원 응답 유실 처리와 후속 UI 변경은 이번 입력에 포함하지 않는다.
개발 main 동기화가 이 기능의 신규 출시를 뜻하지 않는다.

## 변경 결과

- 공개에만 존재했던 LXC Btrfs, 저장소 identity·복원 journal과 관련 Job·시험 확장을
  제외했다. LXC driver·handler·UI·빌드·설정·회귀 계약은 개발 main과 같은 ZFS 전용
  구성을 사용한다. 기존 Btrfs 운영 설치본이나 컨테이너 데이터를 변환하지 않았다.
- 과거 Btrfs commit·조사·실기 기록은 보존하고 ADR-0058은 현재 적용에서 제외해
  Archived로 기록했다. 현재 설치 안내와 과거 검증 회차를 구분했다.
- 자체 소스 주석·Python 문서화 문자열·UI 소스맵 제외를 재적용했다. 실행 shebang,
  AppArmor 지시문과 `ui/vendor/`의 제3자 라이선스 고지는 보존했다.
- 내부 Monitoring 제외와 공개용 검사·문서 예제 정규화는 기존 공개 정책을 유지했다.
  문자열·실행 지시문을 주석으로 삭제하지 않았다.
- Omarchy 소스 빌드 안내는 보존하되 현재 main의 file disk VM·ZFS LXC 전제에 맞췄다.
  과거 지정 Arch/Btrfs 실기를 현재 main의 기능 인증으로 사용하지 않는다.

## 로직 정합 검증

개발 main의 C/헤더 318개를 대조했다. 공개 제외 대상인 내부 전용 10개는 부재이며,
나머지 308개는 기존 공개 투영을 적용한 후 문자열을 포함한 코드 토큰이 일치한다.
공개에만 존재하는 코어 C/헤더는 없다. 생성 번들·SW와 vendor를 제외한 자체 UI
JavaScript 32개도 parser 토큰으로 비교해 내부 제외와 예제 주소 정규화 후 일치했다. 공개 정책에 필요한
빌드·검사 적응과 생성물은 원본 파일의 바이트 복사 대상에서 구분한다.

## 검증과 검토

| 검사 | 결과 |
|---|---|
| 일반 `make single`·`make test` | C 1,479 PASS·14 환경 의존 SKIP, audit startup 5 PASS, compiler warning 0 |
| 공개 `make -j1 check-all` | 40개 계약 게이트 PASS |
| clean release·release C 시험 | `make clean` 후 `make -j4 release`, `-O2 -DNDEBUG -flto=auto`, compiler warning 0. release C 1,479 PASS·14 SKIP, audit startup 5 PASS |
| UI 전체 | 59개 시험 파일·512 PASS, FAIL·SKIP·취소 0 |
| 자체 주석 | C/H·Python·Shell·HTML/CSS·빌드/패키징 설명 주석·문서화 문자열 0, JavaScript 110개 파일의 설명 주석 0, 변환 회귀 13 PASS |
| UI 생성물 | 공식 bundle·SW 캐시 갱신, source hash `07017a72`, bundle 신선도·JS syntax·디자인 연결 PASS, 소스맵 부재 |
| 사이트 | 26페이지·154 artifact, 기존 영상 4그룹·16편의 내용 hash와 게시 자산 검사 PASS |
| 내부 제외 | 공개 파일과 clean release daemon·CLI·test runner의 내부 식별자·symbol 부재, 기존 공개 관측 RPC 보존 PASS |
| 정보·자산 경계 | 비공개 저장소·키·운영망·도메인·금지 파일 신호 0. 기존 site·workflow·vendor의 추적 파일 byte identity 보존 |

Chrome 기본 캐시 버전이 없어 설치된 Chrome 154로 UI 시험을 재실행했다. 설치 도구나
시험 skip으로 우회하지 않았다. 문서 정합화 과정의 REST 브라우저 푸시 설명 누락은 기존
공개 정본에서 복구해 디자인·전체 UI 검사를 통과했다. 내부 제외 검사의 고정 역사
비교점은 이번 변경 전 공개 main으로 설정하고 보호 파일의 byte 보존을 별도로 대조했다.

같은 에이전트가 전체 diff, main 코드 토큰 정합, 주석 제거·검사 적응, 기능·문서·ADR
경계와 자산 보존을 검토했다. 별도 독립 검토를 수행한 것으로 계상하지 않는다.
소스·주석 제거 검증은 전체 수동 감사나 실환경 인증의 완료 판정이 아니다.
전체 감사·지원 환경·장시간 안정성의 기존 미완료 상태를 유지한다.

## Release와 Operate

이번 범위는 공개 소스 게시다. 제품 버전 `2.0.0`을 유지하며 새 태그를 발급하지 않는다.
실행 중 하이퍼바이저·서비스·운영 컨테이너·VM·데이터와 설치 UI를 변경하지 않는다.
과거 Btrfs 설치본은 현재 ZFS 전용 main을 동일 기능 업그레이드로 취급하지 않는다.
공개 사이트의 설명은 문서 build와 기존 main workflow로 갱신한다.
