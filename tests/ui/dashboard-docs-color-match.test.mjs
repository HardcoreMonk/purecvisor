                                                                         
                                                                        
                                         
                                                     
                               
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const DOCS_SOURCE = readFileSync(new URL('../../ui/docs.html', import.meta.url), 'utf8');
const STYLE_SOURCE = readFileSync(new URL('../../ui/style.css', import.meta.url), 'utf8');
const INDEX_SOURCE = readFileSync(new URL('../../ui/index.html', import.meta.url), 'utf8');

const PALETTE = {
  canvas: '#ffffff',
  soft: '#f5f7fa',
  'soft-strong': '#edf1f5',
  ink: '#171c24',
  muted: '#5c6675',
  line: '#dce1e8',
  accent: '#12627a',
  error: '#b42332',
};

function customProperties(source, selector) {
  const clean = source.replace(/\/\*[\s\S]*?\*\//g, '');
  const start = clean.indexOf(`${selector} {`);
  assert.ok(start >= 0, `missing CSS rule: ${selector}`);
  const end = clean.indexOf('}', start);
  assert.ok(end > start, `unterminated CSS rule: ${selector}`);
  const values = {};
  for (const match of clean.slice(start, end).matchAll(/--([\w-]+)\s*:\s*([^;]+);/g)) {
    values[match[1]] = match[2].trim().toLowerCase();
  }
  return values;
}

function relativeLuminance(hex) {
  const channels = [hex.slice(1, 3), hex.slice(3, 5), hex.slice(5, 7)]
    .map(value => Number.parseInt(value, 16) / 255)
    .map(value => value <= 0.04045 ? value / 12.92 : ((value + 0.055) / 1.055) ** 2.4);
  return 0.2126 * channels[0] + 0.7152 * channels[1] + 0.0722 * channels[2];
}

function contrast(first, second) {
  const [lighter, darker] = [relativeLuminance(first), relativeLuminance(second)]
    .sort((a, b) => b - a);
  return (lighter + 0.05) / (darker + 0.05);
}

test('default Supanova maps the documentation palette by role', () => {
  const docs = customProperties(DOCS_SOURCE, ':root');
  const dashboard = customProperties(STYLE_SOURCE, '[data-theme="supanova"]');
  const pairs = [
    ['canvas', 'bg'], ['soft', 'bg2'], ['soft-strong', 'bg3'],
    ['ink', 'fg'], ['muted', 'fg2'], ['line', 'border'],
    ['accent', 'accent'], ['error', 'red'],
  ];
  for (const [docsName, dashboardName] of pairs) {
    assert.equal(docs[`docs-${docsName}`], PALETTE[docsName]);
    assert.equal(dashboard[dashboardName], PALETTE[docsName]);
  }
  assert.match(INDEX_SOURCE, /<meta name="theme-color" content="#ffffff">/);
  assert.match(INDEX_SOURCE, /id="splash"[^>]+background:#ffffff/);
  assert.match(INDEX_SOURCE, /id="splash-bar"[^>]+background:#12627a/);
});

test('light-theme text and semantic lanes meet WCAG AA on the documentation canvas', () => {
  for (const color of [
    PALETTE.ink, PALETTE.muted, PALETTE.accent,
    '#18794e', '#8a6100', PALETTE.error,
  ]) {
    assert.ok(contrast(color, PALETTE.canvas) >= 4.5, `${color} must be AA on white`);
  }
  assert.ok(contrast(PALETTE.canvas, PALETTE.accent) >= 4.5, 'solid accent controls need white text');
});

test('default dashboard surfaces compute to the docs canvas while optional themes remain scoped', async () => {
  await withPage([], async (page) => {
    const result = await page.evaluate(async () => {
      document.documentElement.setAttribute('data-theme', 'supanova');
      document.body.innerHTML = `
        <aside class="shell-sidebar"><button class="vi active">운영 대시보드</button></aside>
        <main class="shell-main">
          <header class="shell-topbar"><button class="tb">검색</button></header>
          <section class="hc"><table><tbody><tr><td>워크로드</td></tr></tbody></table></section>
          <div class="menu-drop">메뉴</div><input type="text" value="검색">
          <button class="btn">승인</button><button class="btn btn-r">거부</button>
        </main>`;
                                                        
                                                             
      await new Promise(resolve => setTimeout(resolve, 360));
      const computed = selector => getComputedStyle(document.querySelector(selector));
      const root = getComputedStyle(document.documentElement);
      const values = {
        tokens: Object.fromEntries(['bg', 'bg2', 'bg3', 'fg', 'fg2', 'accent', 'border']
          .map(name => [name, root.getPropertyValue(`--${name}`).trim()])),
        body: [getComputedStyle(document.body).backgroundColor, getComputedStyle(document.body).color],
        sidebar: [computed('.shell-sidebar').backgroundColor, computed('.shell-sidebar').borderRightColor],
        topbar: [computed('.shell-topbar').backgroundColor, computed('.shell-topbar').borderBottomColor],
        card: [computed('.hc').backgroundColor, computed('.hc').borderColor],
        menu: [computed('.menu-drop').backgroundColor, computed('.menu-drop').borderColor],
        input: [computed('input').backgroundColor, computed('input').borderColor],
        primary: [computed('.btn:not(.btn-r)').backgroundColor, computed('.btn:not(.btn-r)').color],
        destructive: [computed('.btn-r').backgroundColor, computed('.btn-r').color],
      };
      document.documentElement.setAttribute('data-theme', 'supanova-cyan');
      values.cyanBg = getComputedStyle(document.documentElement).getPropertyValue('--bg').trim();
      return values;
    });

    assert.deepEqual(result.tokens, {
      bg: '#ffffff', bg2: '#f5f7fa', bg3: '#edf1f5', fg: '#171c24',
      fg2: '#5c6675', accent: '#12627a', border: '#dce1e8',
    });
    assert.deepEqual(result.body, ['rgb(255, 255, 255)', 'rgb(23, 28, 36)']);
    assert.deepEqual(result.sidebar, ['rgb(245, 247, 250)', 'rgb(220, 225, 232)']);
    assert.deepEqual(result.topbar, ['rgb(255, 255, 255)', 'rgb(220, 225, 232)']);
    assert.deepEqual(result.card, ['rgb(255, 255, 255)', 'rgb(220, 225, 232)']);
    assert.deepEqual(result.menu, ['rgba(255, 255, 255, 0.98)', 'rgb(220, 225, 232)']);
    assert.deepEqual(result.input, ['rgb(255, 255, 255)', 'rgb(220, 225, 232)']);
    assert.deepEqual(result.primary, ['rgb(18, 98, 122)', 'rgb(255, 255, 255)']);
    assert.deepEqual(result.destructive, ['rgb(255, 255, 255)', 'rgb(180, 35, 50)']);
    assert.equal(result.cyanBg, '#07090c');
  });
});
