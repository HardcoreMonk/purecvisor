# ADR-0013: 프론트엔드 IIFE 모듈 스코프 도입

날짜: 2026-04-10
상태: accepted

## 맥락
Web UI는 26개 JS 파일, ~24,000 LOC의 Vanilla JS SPA이다. 모든 모듈이
`window` 객체에 함수/변수를 직접 등록하여 200+ 전역 속성이 존재한다.
이로 인해:
- 모듈 간 암묵적 의존 (import/export 없음)
- 이름 충돌 위험 (vmList, selectedVmIndex 등 일반적 이름)
- 디버깅 시 어느 모듈이 전역을 오염했는지 추적 어려움
- 테스트 격리 불가능

기존 방침(feedback_vanilla_js.md): "프레임워크 전환 금지, 번들러 없는 임베디드 UI".

코드 리뷰(2026-04-10)에서 Architecture #20으로 식별.

## 결정
IIFE(Immediately Invoked Function Expression) 클로저 패턴으로 전환한다:

```js
// 기존: window.fetchGet = function(...) { ... }
// 신규:
window.PCV = window.PCV || {};
(function(PCV) {
    function fetchGet(...) { ... }
    PCV.api = { fetchGet, fetchPost, ... };
})(window.PCV);
```

1. 전역 네임스페이스를 `window.PCV` 1개로 통합
2. 각 모듈은 `PCV.api`, `PCV.vm`, `PCV.modal` 등 서브 네임스페이스에 등록
3. 모듈 내부 함수는 클로저에 은닉
4. 기존 `window.xxx` 참조는 `PCV.xxx` + 호환 shim(`window.fetchGet = PCV.api.fetchGet`)
5. 점진 전환: 모듈 1개씩 IIFE 래핑, 호환 shim 유지, 전체 전환 후 shim 제거

## 결과
- 좋음: 전역 오염 200+ → 1개 (`window.PCV`)
- 좋음: 번들러 불필요 (Vanilla JS 방침 유지)
- 좋음: ES modules와 달리 구형 브라우저/WebView 호환
- 나쁨: 점진 전환 기간 동안 `PCV.xxx`와 `window.xxx` 이중 참조 혼재
- 나쁨: 모듈 간 의존 순서는 여전히 `<script>` 태그 순서에 의존
- 포기한 것: ES modules — `<script type="module">`은 CORS 제약 + 로컬 파일 서빙 시 문제

## 하지 않기로 한 것
- ES modules로 전환하지 않는다. 임베디드 환경에서 CORS/로컬 파일 이슈.
- React/Vue/Svelte 프레임워크를 도입하지 않는다. 기존 방침 유지.
- Webpack/Vite 번들러를 도입하지 않는다. 빌드 의존성 최소화 방침.
