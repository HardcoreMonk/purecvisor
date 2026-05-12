# ADR-0014: CSRF 토큰 코드 제거 — JWT Bearer 전용 아키텍처

날짜: 2026-04-10
상태: accepted

## 맥락
PureCVisor REST API는 JWT Bearer 토큰을 `Authorization: Bearer xxx` 헤더로
전송한다. 쿠키 기반 세션을 사용하지 않는다.

CSRF(Cross-Site Request Forgery)는 **브라우저가 쿠키를 자동 첨부**하는 특성을
악용하는 공격이다. Authorization 헤더는 브라우저가 자동 첨부하지 않으므로,
JWT Bearer 방식에서는 CSRF 공격이 **원천적으로 불가능**하다.

현재 상태:
- rest_middleware.c에 pcv_csrf_generate/validate 구현 존재
- rest_server.c에서 비차단 모드로 호출 (실패해도 경고만)
- 토큰 생성에 g_random_int_range() 사용 (비암호학적 PRNG)
- 멀티노드 환경에서 토큰 불일치로 빈번히 경고 발생

코드 리뷰(2026-04-10)에서 #5(약한 RNG) + N11(정책 결정)으로 식별.

## 결정
CSRF 관련 코드를 완전히 제거한다:
1. rest_middleware.c: pcv_csrf_generate(), pcv_csrf_validate(), g_csrf_tokens 해시테이블 삭제
2. rest_server.c: X-CSRF-Token 헤더 검증 블록 삭제
3. handler_auth.c: 로그인 응답의 csrf_token 필드 삭제
4. ui/modules/api.js: X-CSRF-Token 헤더 전송 삭제

## 결과
- 좋음: 불필요한 코드 ~80줄 제거, 요청당 해시테이블 조회 제거
- 좋음: 멀티노드 환경 CSRF 경고 노이즈 제거
- 좋음: 보안 감사 시 "약한 RNG" 지적 원천 제거
- 나쁨: 향후 쿠키 기반 인증 도입 시 CSRF를 새로 구현해야 함
- 포기한 것: CSRF를 올바르게 구현 (getrandom + etcd 공유) — 비용 대비 이득 없음

## 하지 않기로 한 것
- 쿠키 기반 세션 인증을 도입하지 않는다. JWT Bearer가 API 아키텍처에 적합.
- CSRF 코드를 "혹시 모르니" 남겨두지 않는다. 비차단 모드의 죽은 코드는 보안 착각만 유발.
