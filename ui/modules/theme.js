/* ═══════════════════════════════════════════════════════════════
   PureCVisor — modules/theme.js
   Theme management: previews, editor, auto-theme, custom themes
   ═══════════════════════════════════════════════════════════════ */

window.PCV = window.PCV || {};
(function(PCV) {

/* Supanova 변형만 유지 — 나머지 테마는 전부 삭제 */
var THEME_PREVIEWS = [
  { id: 'supanova',         name: 'SUPANOVA (Teal)',  colors: ['#07090c','#14b8a6','#34d399','#f43f5e'] },
  { id: 'supanova-cyan',    name: 'SUPANOVA CYAN',    colors: ['#07090c','#0891b2','#34d399','#f43f5e'] },
  { id: 'supanova-hicontrast', name: 'SUPANOVA HI-CONTRAST', colors: ['#030405','#facc15','#ffffff','#ff4d6d'] },
];

/* 레거시 테마 id → supanova 마이그레이션 */
var SUPANOVA_THEMES = ['supanova', 'supanova-cyan', 'supanova-hicontrast'];
function sanitizeTheme(t) {
  return SUPANOVA_THEMES.indexOf(t) >= 0 ? t : 'supanova';
}

function changeTheme(t) {
  t = sanitizeTheme(t);
  document.documentElement.setAttribute('data-theme', t);
  localStorage.setItem('pcv-theme', t);
  /* T-2/B: 테마 전환 시 Chart.js 색상 즉시 반영 */
  destroyAllCharts();
  /* #12: uxlib에 등록된 리스너에게 알림 (pcvCharts 추가 정리) */
  try { window.dispatchEvent(new Event('pcv-theme-change')); } catch (_) {}
  if (typeof renderContent === 'function') {
    try { renderContent(); } catch(e) {}
  }
}

function toggleTheme() {
  const themes = SUPANOVA_THEMES;
  const cur = document.documentElement.getAttribute('data-theme') || 'supanova';
  const idx = (themes.indexOf(cur) + 1) % themes.length;
  changeTheme(themes[idx]);
  const s = document.getElementById('theme-select');
  if (s) s.value = themes[idx];
}

/* Time-based auto theme · prefers-color-scheme listener 제거
   (pure-light/pure-dark 테마 삭제에 따라 더 이상 무의미) */

/* Custom Theme Editor */
var THEME_VARS = ['bg','bg2','bg3','fg','fg2','accent','green','red','yellow','cyan','peach','magenta','border'];

function openThemeEditor() {
  var el = PCV.uxlib.el;
  var originalTheme = document.documentElement.getAttribute('data-theme') || '';
  const style = getComputedStyle(document.documentElement);
  var items = THEME_VARS.map(v => {
    const cur = style.getPropertyValue('--' + v).trim();
    const hex = cssColorToHex(cur);
    return el('div', { class: 'theme-editor-item' },
      el('label', { for: 'cv-' + v }, '--' + v),
      el('input', { type: 'color', id: 'cv-' + v, value: hex, 'data-var': v, onchange: 'previewThemeVar(this)' }),
      el('span', { style: 'font-size:9px;color:var(--fg2)', id: 'te-val-' + v }, hex));
  });
  showModal([
    el('h2', null, 'Theme Editor'),
    el('div', { class: 'theme-editor' },
      el('div', { class: 'theme-editor-grid' }, items)),
    el('div', { class: 'theme-editor-actions' },
      el('button', { class: 'btn btn-g', onclick: 'saveCustomTheme()' }, 'Save as Custom'),
      el('button', { class: 'btn', onclick: 'exportTheme()' }, 'Export JSON'),
      el('button', { class: 'btn', onclick: 'importTheme()' }, 'Import'),
      el('button', { class: 'btn btn-r', onclick: "changeTheme('" + originalTheme + "');closeModal()", style: 'margin-left:8px' }, _L ? _L('원래 테마로', 'Reset to Original') : 'Reset to Original'),
      el('button', { class: 'btn btn-r', onclick: 'closeModal()' }, 'Cancel'))
  ]);
}

function cssColorToHex(c) {
  if (!c) return '#000000';
  c = c.trim();
  if (c.startsWith('#')) return c.length === 4 ? '#' + c[1]+c[1]+c[2]+c[2]+c[3]+c[3] : c;
  const m = c.match(/rgba?\((\d+),\s*(\d+),\s*(\d+)/);
  if (m) return '#' + [m[1],m[2],m[3]].map(x => parseInt(x).toString(16).padStart(2,'0')).join('');
  return '#000000';
}

function previewThemeVar(el) {
  const v = el.dataset.var;
  document.documentElement.style.setProperty('--' + v, el.value);
  const span = document.getElementById('te-val-' + v);
  if (span) span.textContent = el.value;
}

function saveCustomTheme() {
  const custom = {};
  THEME_VARS.forEach(v => {
    const el = document.querySelector('.theme-editor-item input[data-var="' + v + '"]');
    if (el) custom[v] = el.value;
  });
  localStorage.setItem('pcv-custom-theme', JSON.stringify(custom));
  applyCustomTheme(custom);
  toast('Custom theme saved');
  closeModal();
}

function applyCustomTheme(vars) {
  Object.entries(vars).forEach(([k, v]) => {
    document.documentElement.style.setProperty('--' + k, v);
  });
  document.documentElement.setAttribute('data-theme', 'custom');
  localStorage.setItem('pcv-theme', 'custom');
  const s = document.getElementById('theme-select');
  if (s) s.value = 'custom';
}

function exportTheme() {
  const custom = {};
  THEME_VARS.forEach(v => {
    const el = document.querySelector('.theme-editor-item input[data-var="' + v + '"]');
    if (el) custom[v] = el.value;
  });
  const blob = new Blob([JSON.stringify(custom, null, 2)], { type: 'application/json' });
  const a = document.createElement('a'); a.href = URL.createObjectURL(blob);
  a.download = 'pcv-theme.json'; a.click();
  toast(t('msg.theme_exported'));
}

function importTheme() {
  const input = document.createElement('input'); input.type = 'file'; input.accept = '.json';
  input.onchange = e => {
    const file = e.target.files[0]; if (!file) return;
    const reader = new FileReader();
    reader.onload = ev => {
      try {
        const vars = JSON.parse(ev.target.result);
        localStorage.setItem('pcv-custom-theme', JSON.stringify(vars));
        applyCustomTheme(vars);
        toast(t('msg.theme_imported'));
        closeModal();
      } catch (err) { toast(t('msg.invalid_theme'), false); }
    };
    reader.readAsText(file);
  };
  input.click();
}

/* ═══ UI SETTINGS EXPORT/IMPORT ═══ */
function exportUiSettings() {
  var settings = {
    theme: document.documentElement.getAttribute('data-theme') || '',
    sidebarWidth: localStorage.getItem('pcv-sb-width') || '',
    vmViewMode: localStorage.getItem('pcv-vm-view') || 'list',
    language: typeof I18N !== 'undefined' ? I18N.getLang() : 'ko',
    autoTheme: localStorage.getItem('pcv-auto-theme') || 'false',
    favorites: localStorage.getItem('pcv-favorites') || '[]',
    customTheme: localStorage.getItem('pcv-custom-theme') || '',
  };
  var blob = new Blob([JSON.stringify(settings, null, 2)], { type: 'application/json' });
  var a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'purecvisor-settings.json';
  a.click();
  toast(_L ? _L('설정 내보내기 완료', 'Settings exported') : 'Settings exported');
}

function importUiSettings() {
  var input = document.createElement('input');
  input.type = 'file'; input.accept = '.json';
  input.onchange = function() {
    var file = input.files[0]; if (!file) return;
    var reader = new FileReader();
    reader.onload = function(e) {
      try {
        var s = JSON.parse(e.target.result);
        if (s.theme !== undefined) { changeTheme(s.theme); }
        if (s.sidebarWidth) localStorage.setItem('pcv-sb-width', s.sidebarWidth);
        if (s.vmViewMode) localStorage.setItem('pcv-vm-view', s.vmViewMode);
        if (s.language && typeof I18N !== 'undefined') { I18N.setLang(s.language); if (typeof applyI18n === 'function') applyI18n(); if (typeof renderContent === 'function') { try { renderContent(); } catch (e) {} } }
        if (s.autoTheme) localStorage.setItem('pcv-auto-theme', s.autoTheme);
        if (s.favorites) localStorage.setItem('pcv-favorites', s.favorites);
        if (s.customTheme) localStorage.setItem('pcv-custom-theme', s.customTheme);
        toast(_L ? _L('설정 가져오기 완료 — 새로고침 권장', 'Settings imported — refresh recommended') : 'Settings imported');
      } catch (err) {
        toast(_L ? _L('잘못된 설정 파일', 'Invalid settings file') : 'Invalid', false);
      }
    };
    reader.readAsText(file);
  };
  input.click();
}
window.exportUiSettings = exportUiSettings;
window.importUiSettings = importUiSettings;

/* ── PCV.theme namespace export ────────────────────── */
PCV.theme = {
  PREVIEWS: THEME_PREVIEWS,
  VARS: THEME_VARS,
  change: changeTheme,
  toggle: toggleTheme,
  sanitize: sanitizeTheme,
  openEditor: openThemeEditor,
  cssColorToHex: cssColorToHex,
  previewVar: previewThemeVar,
  saveCustom: saveCustomTheme,
  applyCustom: applyCustomTheme,
  exportTheme: exportTheme,
  importTheme: importTheme,
  exportUiSettings: exportUiSettings,
  importUiSettings: importUiSettings
};

/* ═══ BACKWARD-COMPAT WINDOW REGISTRATIONS ═══ */
window.THEME_PREVIEWS = THEME_PREVIEWS;
window.changeTheme = changeTheme;
window.toggleTheme = toggleTheme;
window.sanitizeTheme = sanitizeTheme;
window.THEME_VARS = THEME_VARS;
window.openThemeEditor = openThemeEditor;
window.cssColorToHex = cssColorToHex;
window.previewThemeVar = previewThemeVar;
window.saveCustomTheme = saveCustomTheme;
window.applyCustomTheme = applyCustomTheme;
window.exportTheme = exportTheme;
window.importTheme = importTheme;
})(window.PCV);
