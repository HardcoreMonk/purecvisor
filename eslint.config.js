
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

      'no-undef': 'off',

      'no-unused-vars': ['warn', { args: 'none' }],

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
