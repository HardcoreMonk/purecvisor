# 공개 소스 정책

> **현행화 기준:** 2026-08-31

PureCVisor 공개 저장소는 비공개 개발 저장소의 이력과 설명 주석을 포함하지 않는 릴리스 스냅샷이다.

## 공개 원칙

- 자체 작성한 C, 헤더, JavaScript, Python, Shell, HTML, CSS, 빌드 및 패키징 소스에는 설명 주석과 문서화 문자열을 포함하지 않는다.
- 실행에 필요한 shebang과 AppArmor 전처리 지시문은 주석으로 취급하지 않는다.
- `ui/vendor/`의 제3자 저작권 및 라이선스 고지는 변경하거나 삭제하지 않는다.
- 공개 저장소에는 비공개 Git 이력, 운영 인계 문서, 실제 서버 주소, 인증정보, 로컬 분석 도구 산출물을 포함하지 않는다.
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
