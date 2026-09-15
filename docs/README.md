# 공개 문서 인덱스

> 현행화 기준: 2026-09-16 · PureCVisor Single Edge `2.0.0` · 초기 태그 이후 소스 변경 포함

공개 문서의 현행 기준과 날짜별 검증 기록을 찾는 진입점이다.
공개 소스는 [HardcoreMonk/purecvisor](https://github.com/HardcoreMonk/purecvisor),
문서·기능 영상은 [purecvisor.site](https://purecvisor.site)에서 제공한다.

## 현재 기준

| 문서 | 책임 |
|---|---|
| [저장소 README](../README.md) | 빠른 설치·사용·검증 명령 |
| [통합 운영 가이드](GUIDE.md) | 제품 기능·설정·운영·개발, 22.8절의 최신 공개 검증 범위 |
| [데이터베이스 아키텍처](DATABASE_STRUCTURE.md) | SQLite 9개 파일·26개 테이블의 소유권과 복구 경계 |
| [공개 릴리스 경계](PUBLIC_RELEASE_BOUNDARY.md) | Single Edge 허용·제외 범위와 지원 승격 조건 |
| [공개 소스 정책](PUBLIC_SOURCE_POLICY.md) | 설명 주석·소스맵 제외, 제3자 고지와 공개 기록 경계 |
| [개발 검증 정책](DEVELOPMENT_VERIFICATION_POLICY.md) | 변경 유형별 검증 깊이와 완료 판정 |
| [기능 테스트 시나리오](SERVICE_FUNCTIONAL_TEST_SCENARIOS.md) | 실제 상태·부작용·실패·정리까지 대조하는 기능 시나리오 |
| [ADR 적용 상태](ADR_INDEX.md) | 설계 결정의 현행 Single Edge 적용 여부 |
| [UI 시각 규격](../DESIGN.md) | 제품 Web UI의 시각·상태·컴포넌트 계약 |
| [공개 문서 사이트](PUBLIC_DOCUMENTATION_SITE.md) | Pages 콘텐츠·영상·빌드·배포·검증 |
| [systemd 운영 조건](../packaging/systemd/README.md) | 런타임 패키지·커널·권한·드롭인 설치 |
| [변경 이력](../CHANGELOG.md) | 공개 소스·문서의 날짜별 주요 변경 |
| [제3자 고지](../THIRD_PARTY_NOTICES.md) | 포함 구성요소의 저작권·라이선스 |
| [에이전트 작업 규칙](../AGENTS.md) | 공개 저장소 작업 범위·코드 불변 조건·검증 |

## 작성 정본과 표시 사본

- `GUIDE.md`와 `DATABASE_STRUCTURE.md`를 수정하면 `site/` build가 21개 가이드 장과
  DB 문서로 변환한다. 생성된 `site/src/content/docs/ko/` 본문을 직접 수정하지 않는다.
- 제품 내장 도움말은 [ui/guide-content.md](../ui/guide-content.md)가 소유한다.
  기능·명령·설정 설명을 바꾸면 공개 가이드와 같은 계약인지 함께 대조한다.
- 랜딩의 한국어·영어 소개와 마지막 갱신일은 `site/src/content/docs/index.mdx`와
  `site/src/content/docs/en/index.mdx`에서 관리한다. 현재 reader 본문은 한국어다.
- RPC·설정·테이블·게이트 수의 원본은 실행 코드와 `Makefile`이다. 파일 수와
  테스트 통과 수는 집계 시점·방법을 표시하며 다른 회차의 수치와 합산하지 않는다.

## 기술 조사

- [LXC Btrfs 백엔드 조사](research/2026-09-16-lxc-btrfs-backend.md): 구현 전 조사와 native LXC 실기의 역사 근거, 이후 구현 상태를 구분한다.
- [ADR-0058](adr/0058-lxc-storage-backend-identity.md): 기본 ZFS와 명시적 Btrfs, 객체별 실제 identity, rootfs 복원·안전한 삭제 계약.
- [Btrfs API 검증 기록](operations/2026-09-16-lxc-btrfs-api-validation.md): 지정 Arch/Btrfs 호스트에서 실제 API·owner·복원 중단 상태·정리를 검증한 기록. 초기 `2.0.0` 태그와 Btrfs 구현을 포함한 공개 소스의 차이는 [가이드 4.1절](GUIDE.md#41-컨테이너-생성)을 따른다.

## 기록을 읽는 규칙

- [ADR 원문](adr/)은 결정 당시의 맥락을 보존한다. 현재 적용은 `ADR_INDEX.md`를 먼저 읽는다.
- [공개 운영 인계](operations/), [UI 리뷰](ui-reviews/), [설계·계획](superpowers/)은
  날짜가 있는 회차별 증거다. 옛 브랜치·페이지 수·검증 수치를 오늘의 운영 기준으로 읽지 않는다.
- [에이전트 보조 문서](agents/)는 용어·ADR·이슈 작업 절차를 설명하며 제품 기능 명세가 아니다.
- 지정 테스트 통과, 영상 게시와 문서 갱신은 전체 감사나 모든 환경 인증 완료를 뜻하지 않는다.
  현재 잔여 항목은 `GUIDE.md` 22.8절을 따른다.
- 내부 서버 주소·계정·원본 운영 로그·비공개 인계는 공개 문서에 추가하지 않는다.

## 문서 변경 검증

```bash
npm --prefix site run check
make check-public-comments
python3 scripts/check_design_md.py
bash tests/integration/test_design_md_surface.sh
git diff --check
```

공개 링크·장 제목·소스 경로를 함께 점검한다. 소스 동작까지 바꾸는 경우에는
`DEVELOPMENT_VERIFICATION_POLICY.md`의 해당 제품 검증을 추가한다.
