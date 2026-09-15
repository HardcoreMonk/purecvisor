# 공개 소스 정책

> **현행화 기준:** 2026-09-15

PureCVisor 공개 저장소는 비공개 개발 저장소의 이력과 설명 주석을 포함하지 않는 릴리스 스냅샷이다.

## 공개 원칙

- 자체 작성한 C, 헤더, JavaScript, Python, Shell, HTML, CSS, 빌드 및 패키징 소스에는 설명 주석과 문서화 문자열을 포함하지 않는다.
- 실행에 필요한 shebang과 AppArmor 전처리 지시문은 주석으로 취급하지 않는다.
- `ui/vendor/`의 제3자 저작권 및 라이선스 고지는 변경하거나 삭제하지 않는다.
- 공개 저장소에는 비공개 Git 이력, 비공개 운영 인계 문서, 실제 서버 주소, 인증정보, 로컬 분석 도구 산출물을 포함하지 않는다.
- 공개 commit·Pages 배포·검증 범위를 설명하는 인계 문서는 민감한 주소·계정·원본 운영 로그를 제외하고 `docs/operations/`에 보존한다. 과거 기록과 현행 기준은 `docs/README.md`에서 구분한다.
- UI 소스맵은 생성하거나 배포하지 않는다.
- 공개 문서 작성 정본은 `docs/GUIDE.md`, `docs/DATABASE_STRUCTURE.md`와 공개 경계·검증
  문서에 두고, `site/`의 Astro/Starlight build가 이를 route별 콘텐츠로 변환한다. Pages에는
  `site/dist/` artifact만 배포한다.
- Astro·Starlight source와 Pages workflow도 설명 주석과 source map을 포함하지 않는다.

## 검증

```bash
python3 scripts/strip_source_comments.py --check
node scripts/check_javascript_comments.mjs
bash tests/integration/test_public_comment_policy.sh
make check-public-comments
```

설명과 설계 근거는 코드 주석이 아니라 `docs/`, ADR, 테스트 이름과 공개 계약 파일에 유지한다.

`Makefile`의 `@#` 등 recipe 주석과 shell 연속 명령에 끼워 넣은 주석 전용 backtick
치환도 제거 대상이다. 실행 명령·인용 문자열·줄 연결은 보존하고 변환 회귀로 확인한다.

의도적인 테스트 전용 export, 기존 C API와 헤더 inline·매크로의 사용처 보정은
`contracts/dead_export_waivers.json`에 심볼별 사유와 공개 코드 근거를 기록한다.
`check-dead-exports`는 근거 파일의 실제 심볼 참조와 현재 후보 상태를 확인하며, 없는
파일·주석이나 문자열만의 참조·삭제되거나 이미 배선된 심볼은 예외로 인정하지 않는다.
이 계약은 `scripts/dead_exports_baseline.txt`의 기존 미배선 부채와 별도로 유지한다.

공개 변환으로 제거되는 설명 주석을 검사의 필수 anchor로 사용하지 않는다. 배포 단계는
실행 코드 경계로, 운영 계약은 공개 `GUIDE.md`·검증 정책·ADR로 확인한다. 설정 기본값,
ABI 사전검사, 실패 시 cleanup과 잘못된 계약을 거부하는 반사실 검증은 계속 유지한다.

`make check-single-ui-surface`는 현재 이벤트·명령 버튼 연결과 소스맵 부재를 검사한다.
소스맵 파일·깨진 링크, manifest 항목, 번들 참조가 있으면 실패한다. 임시 사본에서
각 위반을 만들어 거부 여부를 확인하며 `check-all`과 `dev-check`에도 포함한다.
