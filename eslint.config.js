// PureCVisor UI — ESLint flat config (프론트 #4-B)
//
// 목적: 버그 검출 기준선 확보 (포맷터 아님 — 세미콜론/따옴표/들여쓰기 룰 도입 안 함).
// vm.js 분할 등 대형 리팩토링 전 실수(중복 키, 도달 불가 코드, self-assign 등) 조기 발견.
//
// 이 코드베이스는 의도된 전역 네임스페이스 설계다 (ADR-0013):
//   각 모듈이 (function(PCV){ ... })(window.PCV) IIFE 안에서 window.* /
//   PCV.* 에 등록하고, 다른 모듈·index.html 인라인 스크립트·onclick 속성에서
//   bare 참조로 호출한다. no-undef 를 켜면 이 설계 자체와 충돌해 수천 건
//   오탐이 발생하므로 off — 대신 no-unused-vars 등 실질적 버그 검출 룰에
//   집중한다.
const js = require('@eslint/js');
const globals = require('globals');

module.exports = [
  {
    ignores: ['ui/vendor/**', 'ui/bundle.js', 'ui/app.bundle.js', 'ui/app.bundle.js.map', 'node_modules/**'],
  },
  js.configs.recommended,
  {
    files: ['ui/app.js', 'ui/i18n.js', 'ui/modules/*.js'],
    languageOptions: {
      ecmaVersion: 2020,
      sourceType: 'script',
      globals: {
        ...globals.browser,
      },
    },
    rules: {
      // 전역 네임스페이스 설계(window.*, PCV.*, HTML onclick bare 참조)와
      // 양립 불가 — recommended 기본은 off 지만 명시적으로 문서화.
      'no-undef': 'off',
      // 미사용 변수는 실수 신호로 유효 — 다만 함수 인자는 콜백 시그니처
      // 고정(예: addEventListener 핸들러)이 흔해 검사하지 않는다.
      'no-unused-vars': ['warn', { args: 'none' }],
      // 레거시 코드에 의도적 빈 catch(무시 가능한 실패) 다수 존재.
      'no-empty': ['error', { allowEmptyCatch: true }],
    },
  },
  {
    files: ['ui/sw.js'],
    languageOptions: {
      ecmaVersion: 2020,
      sourceType: 'script',
      globals: {
        ...globals.serviceworker,
      },
    },
    rules: {
      'no-undef': 'off',
      'no-unused-vars': ['warn', { args: 'none' }],
      'no-empty': ['error', { allowEmptyCatch: true }],
    },
  },
];
