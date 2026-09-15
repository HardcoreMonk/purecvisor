  
                           
                                                                   
                                                                         
                                                              
  
                       
                                                  

   
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';
import axe from 'axe-core';
import { withPage } from './harness.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const DOCS_SOURCE = fs.readFileSync(path.join(ROOT, 'ui/docs.html'), 'utf8');
const GUIDE_REDIRECT_SOURCE = fs.readFileSync(path.join(ROOT, 'ui/guide.html'), 'utf8');
const GUIDE_SOURCE = fs.readFileSync(path.join(ROOT, 'ui/guide-content.md'), 'utf8');
const REST_CHAPTER_SOURCE = GUIDE_SOURCE.match(/^## 14\. REST API[\s\S]*?(?=^## 15\. CLI 레퍼런스)/m)?.[0] || '';

function slugify(text) {
  return String(text || '').toLowerCase()
    .replace(/[`*\[\]()]/g, '')
    .replace(/[^a-z0-9가-힣\s-]/g, '')
    .replace(/\s+/g, '-')
    .replace(/-+/g, '-')
    .replace(/^-+|-+$/g, '');
}

function guideHeadingSlugs() {
  return new Set(GUIDE_SOURCE.split('\n').flatMap(line => {
    const match = line.match(/^#{1,5}\s+(.+)$/);
    return match ? [slugify(match[1])] : [];
  }));
}

function guideChapterSlugs() {
  return new Set(GUIDE_SOURCE.split('\n').flatMap(line => {
    const match = line.match(/^##\s+\d+\.\s+(.+)$/);
    return match ? [slugify(line.slice(3))] : [];
  }));
}

function renderedHeadingCount() {
  let inCode = false;
  let count = 0;
  for (const line of GUIDE_SOURCE.split('\n')) {
    if (/^```/.test(line)) {
      inCode = !inCode;
      continue;
    }
    if (!inCode && /^#{1,5}\s+/.test(line)) count++;
  }
  return count;
}

function assertNear(actual, expected, message, tolerance = 1.1) {
  assert.ok(Math.abs(actual - expected) <= tolerance,
    `${message}: expected ${expected}±${tolerance}, got ${actual}`);
}

function canonicalBlockCounts() {
  const counts = { codeBlocks: 0, tables: 0, quotes: 0, listItems: 0, links: 0 };
  let inCode = false;
  let inTable = false;
  let inQuote = false;
  for (const line of GUIDE_SOURCE.split('\n')) {
    if (/^```/.test(line)) {
      if (!inCode) counts.codeBlocks++;
      inCode = !inCode;
      inTable = false;
      inQuote = false;
      continue;
    }
    if (inCode) continue;

    const tableLine = /^\|/.test(line);
    const quoteLine = /^>/.test(line);
    if (tableLine && !inTable) counts.tables++;
    if (quoteLine && !inQuote) counts.quotes++;
    inTable = tableLine;
    inQuote = quoteLine;
    if (/^\s*(?:[-*]|\d+\.)\s+/.test(line)) counts.listItems++;
    counts.links += [...line.matchAll(/\[[^\]]+\]\([^)]+\)/g)].length;
  }
  return counts;
}

test('docs portal: REST 전체 계약을 canonical 14장에 직접 제공한다', () => {
  assert.ok(REST_CHAPTER_SOURCE, 'canonical guide must expose chapter 14');
  const fenceCount = (REST_CHAPTER_SOURCE.match(/^```/gm) || []).length;
  assert.equal(fenceCount % 2, 0, 'REST chapter fences must remain paired');
  assert.equal(fenceCount / 2, 25,
    'REST chapter must embed the 3 auth, 6 curl, 12 push, and 4 RPC-only examples');

  for (const heading of [
    '### 14.2 인증과 토큰',
    '### 14.3 RBAC 역할',
    '### 14.4 보안',
    '### 14.5 엔드포인트와 응답',
    '### 14.6 curl 예제',
    '### 14.7 브라우저 푸시 (SP2b)',
    '### 14.8 2.0 RPC 예제 (RPC 전용)',
  ]) {
    assert.ok(REST_CHAPTER_SOURCE.includes(heading), `${heading} must remain in the canonical reader`);
  }
  for (const contract of [
    'PURECVISOR_ADMIN_PASSWORD',
    'libvirt domain metadata',
    '600 IP / 1200 유저 / 60 인증',
    'X-Total-Count',
    '/api/v1/vms/web-prod/start',
    '/api/v1/vms/web/import-ec2',
    'PushSubscription.toJSON()',
    '/api/v1/push/vapid/rotate',
    'tenant_overlay.create',
    'qos.tenant.set',
    'suricata.ips.enable',
    'debug.trace.start',
  ]) {
    assert.ok(REST_CHAPTER_SOURCE.includes(contract), `${contract} must remain in chapter 14`);
  }
});

test('docs portal: 정적 링크·배포·안전 DOM 계약이 정본과 일치한다', () => {
  const slugs = guideHeadingSlugs();
  const primaryNav = DOCS_SOURCE.match(/<nav class="primary-nav"[\s\S]*?<\/nav>/)?.[0] || '';
  const navLinks = [...primaryNav.matchAll(/<a href="docs\.html#([^"]+)"/g)].map(match => match[1]);
  const chapterSlugs = guideChapterSlugs();
  const chapterLinks = [...DOCS_SOURCE.matchAll(/class="chapter-card" href="docs\.html#([^"]+)"/g)]
    .map(match => match[1]);

  assert.equal(navLinks.length, 16, 'four public-site-equivalent groups must expose sixteen deep links');
  assert.equal(new Set(navLinks).size, 16, 'header deep links must not replace coverage with duplicates');
  for (const slug of navLinks) assert.ok(slugs.has(slug), `guide heading must exist for ${slug}`);
  assert.equal(chapterLinks.length, 21, 'reader metadata must retain all 21 public guide chapters');
  assert.equal(new Set(chapterLinks).size, 21, 'reader chapter metadata must not contain duplicates');
  assert.doesNotMatch(GUIDE_SOURCE, /^## 7\./m, 'non-public multi-control-plane chapter must stay excluded');
  assert.deepEqual(new Set(chapterLinks), chapterSlugs,
    'reader metadata must exactly cover the canonical numbered H2 headings');
  assert.match(DOCS_SOURCE, /PCV\.docsPortal/, 'inline runtime must stay under PCV namespace');
  assert.match(DOCS_SOURCE, /class="site-header-inner"/, 'docs shell must expose the target header');
  assert.equal((primaryNav.match(/data-nav-group/g) || []).length, 4,
    'the header must preserve four navigation groups');
  assert.equal((DOCS_SOURCE.match(/class="pcv-layer-key is-(?:clients|config|transport|control|domain|persistent|host)"/g) || []).length, 7,
    'the architecture frame must preserve all seven layer labels');
  assert.match(DOCS_SOURCE, /id="reader-mobile-toc"/, 'reader must expose the current-section mobile strip');
  assert.match(DOCS_SOURCE, /reader-code-toolbar/, 'rendered code must expose the approved toolbar frame');
  assert.doesNotMatch(DOCS_SOURCE, /\.innerHTML\s*=/, 'markdown search results must not use innerHTML');
  assert.match(DOCS_SOURCE, /replaceChildren\(/, 'search results must use DOM replacement');
  assert.doesNotMatch(DOCS_SOURCE, /(?:href|location\.href)\s*=\s*["']guide\.html(?:#|["'])/,
    'directory, search, recommendations and reader must not reopen the legacy guide surface');
  assert.match(GUIDE_REDIRECT_SOURCE, /location\.replace\('\/ui\/docs\.html' \+ window\.location\.hash\)/,
    'legacy guide bookmarks must preserve their heading hash in the unified reader');

  const deploy = fs.readFileSync(path.join(ROOT, 'scripts/deploy.sh'), 'utf8');
  const assetManifest = fs.readFileSync(path.join(ROOT, 'packaging/ui-assets.manifest'), 'utf8');
  assert.match(deploy, /UI_ASSET_MANIFEST=.*packaging\/ui-assets\.manifest/,
    'remote and local deploy must consume the shared UI asset manifest');
  for (const file of ['docs.html', 'guide.html', 'guide-content.md']) {
    assert.ok(assetManifest.includes(`ui/${file} ui/${file} replace`),
      `${file} must be a required manifest target`);
  }
  assert.match(deploy, /pcv_ui_assets/, 'remote deployment must stage the recursive UI assets directory');
  const sw = fs.readFileSync(path.join(ROOT, 'ui/sw.js'), 'utf8');
  for (const asset of [
    "'/ui/docs.html'",
    "'/ui/guide.html'",
    "'/ui/guide-content.md'",
    "'/ui/assets/diagrams/purecvisor-single-full-architecture.svg'",
  ]) {
    assert.ok(sw.includes(asset), `${asset} must be precached`);
  }
  const canonicalArchitecture = fs.readFileSync(
    path.join(ROOT, 'site/public/assets/diagrams/purecvisor-single-full-architecture.svg'));
  const productArchitecture = fs.readFileSync(
    path.join(ROOT, 'ui/assets/diagrams/purecvisor-single-full-architecture.svg'));
  assert.deepEqual(productArchitecture, canonicalArchitecture,
    'the deployed architecture must remain byte-identical to the current repository source');
  assert.match(fs.readFileSync(path.join(ROOT, 'Makefile'), 'utf8'),
    /\$\(UI_DIR\)\/assets\/diagrams\/purecvisor-single-full-architecture\.svg/,
    'architecture changes must invalidate the service worker cache identity');
  assert.match(fs.readFileSync(path.join(ROOT, 'packaging/deb/build-deb.sh'), 'utf8'),
    /\[ -d ui\/assets \].*cp -a ui\/assets/,
    'the DEB must include the recursive UI assets directory');
});

test('docs portal: 공개 사이트와 같은 landing·dropdown·theme 진입점을 제공한다', async () => {
  for (const content of [
    'PURECVISOR 2.0.0 · SINGLE EDGE',
    '하나의 Linux/KVM 노드, 하나의 제어면.',
    'VM, 컨테이너, ZFS 스토리지와 네트워크 가상화를 한곳에서 운영합니다.',
    '독립 노드 배포 · C23 단일 프로세스 · Web UI, REST API, CLI',
    'Single Edge 서비스 아키텍처',
    'SVG 원본 구조',
    '확대해서 보기',
  ]) {
    assert.ok(DOCS_SOURCE.includes(content), `${content} must remain in the product docs landing`);
  }
  assert.match(DOCS_SOURCE,
    /href="docs\.html#purecvisor-single-edge-운영-가이드"/,
    'full guide CTA must enter the canonical integrated reader');
  assert.match(DOCS_SOURCE,
    /href="https:\/\/github\.com\/HardcoreMonk\/purecvisor" target="_blank" rel="noopener"/,
    'public repository CTA must isolate the new browsing context');

  await withPage([], async (page, { port }) => {
    await page.setViewport({ width: 1440, height: 1000, deviceScaleFactor: 1 });
    await page.goto(`http://127.0.0.1:${port}/ui/docs.html`, { waitUntil: 'networkidle0' });
    const landingState = await page.evaluate(() => ({
      title: document.getElementById('pcv-hero-title').textContent.trim(),
      groups: document.querySelectorAll('[data-nav-group]').length,
      links: document.querySelectorAll('.nav-menu a').length,
      legend: document.querySelectorAll('.pcv-architecture-legend li').length,
      image: document.querySelector('.pcv-architecture-source-image').getAttribute('src'),
      quickstart: document.querySelector('.pcv-button-primary').getAttribute('href'),
      guide: document.querySelector('.pcv-button-ghost').getAttribute('href'),
      externalTarget: document.querySelector('.header-icon-link').target,
      externalRel: document.querySelector('.header-icon-link').rel,
      visibleLegacyCards: [...document.querySelectorAll('.chapter-card')]
        .filter(node => node.getBoundingClientRect().height > 0).length,
    }));
    assert.deepEqual(landingState, {
      title: '하나의 Linux/KVM 노드, 하나의 제어면.',
      groups: 4,
      links: 16,
      legend: 7,
      image: 'assets/diagrams/purecvisor-single-full-architecture.svg',
      quickstart: 'docs.html#14-5분-퀵스타트',
      guide: 'docs.html#purecvisor-single-edge-운영-가이드',
      externalTarget: '_blank',
      externalRel: 'noopener',
      visibleLegacyCards: 0,
    });

    await page.focus('.nav-trigger');
    await page.keyboard.press('ArrowDown');
    assert.equal(await page.$eval('.nav-trigger', node => node.getAttribute('aria-expanded')), 'true');
    assert.equal(await page.$eval('#nav-service', node => node.hidden), false);
    assert.equal(await page.evaluate(() => document.activeElement?.closest('#nav-service') !== null), true);
    await page.keyboard.press('Escape');
    assert.equal(await page.$eval('#nav-service', node => node.hidden), true);

    await page.select('#docs-theme-select', 'dark');
    assert.deepEqual(await page.evaluate(() => ({
      theme: document.documentElement.getAttribute('data-theme'),
      mode: document.documentElement.getAttribute('data-theme-mode'),
      stored: localStorage.getItem('pcv-docs-theme'),
    })), { theme: 'dark', mode: 'dark', stored: 'dark' });
    await page.select('#docs-theme-select', 'auto');

    await page.click('.pcv-button-ghost');
    await page.waitForFunction(() => !document.getElementById('docs-reader').hidden &&
      decodeURIComponent(location.hash) === '#purecvisor-single-edge-운영-가이드');
    assert.equal(await page.$eval('#docs-landing', node => node.hidden), true);
  });
});

test('docs portal: landing shell에 axe 접근성 위반이 없다', async () => {
  await withPage([], async (page, { port }) => {
    await page.goto(`http://127.0.0.1:${port}/ui/docs.html`, { waitUntil: 'networkidle0' });
    await page.evaluate(axe.source);
    const violations = await page.evaluate(async () => {
      const result = await window.axe.run({ include: [['.site-header'], ['#docs-landing']] });
      return result.violations.map(rule => ({
        id: rule.id,
        impact: rule.impact,
        nodes: rule.nodes.map(node => ({ target: node.target, summary: node.failureSummary })),
      }));
    });
    assert.deepEqual(violations, []);
  });
});

test('docs portal: 검색 결과·empty state·keyboard selection이 동작한다', async () => {
  await withPage([], async (page, { port }) => {
    await page.goto(`http://127.0.0.1:${port}/ui/docs.html`, { waitUntil: 'networkidle0' });
    assert.equal(await page.$$eval('[data-nav-group]', nodes => nodes.length), 4);
    assert.equal(await page.$$eval('.nav-menu a', nodes => nodes.length), 16);

    await page.focus('#doc-search-input');
    await page.type('#doc-search-input', '스냅샷');
    await page.waitForFunction(() => document.querySelectorAll('#doc-search-results [role="option"]').length > 0);
    const searchState = await page.evaluate(() => ({
      expanded: document.getElementById('doc-search-input').getAttribute('aria-expanded'),
      count: document.querySelectorAll('#doc-search-results [role="option"]').length,
      firstHref: document.querySelector('#doc-search-results [role="option"]').getAttribute('href'),
      firstText: document.querySelector('#doc-search-results [role="option"]').textContent,
    }));
    assert.equal(searchState.expanded, 'true');
    assert.ok(searchState.count <= 8 && searchState.count > 0);
    assert.match(searchState.firstHref, /^docs\.html#/);
    assert.match(searchState.firstText, /스냅샷/);

    await page.keyboard.press('ArrowDown');
    assert.match(await page.$eval('#doc-search-input', node => node.getAttribute('aria-activedescendant') || ''),
      /^doc-search-option-/);
    await page.keyboard.press('Escape');
    assert.equal(await page.$eval('#doc-search-popover', node => node.hidden), true);

    await page.$eval('#doc-search-input', node => { node.value = 'zzzz-no-result'; });
    await page.$eval('#doc-search-input', node => node.dispatchEvent(new Event('input', { bubbles: true })));
    await page.waitForFunction(() => document.getElementById('doc-search-status').textContent.includes('일치하는 문서가 없습니다'));
    assert.equal(await page.$$eval('#doc-search-results [role="option"]', nodes => nodes.length), 0);
  });
});

test('docs portal: 검색 색인 실패에도 landing deep link와 원문 fallback을 보존한다', async () => {
  await withPage([], async (page, { port }) => {
    await page.setRequestInterception(true);
    page.on('request', request => {
      if (request.url().endsWith('/ui/guide-content.md')) request.abort();
      else request.continue();
    });
    await page.goto(`http://127.0.0.1:${port}/ui/docs.html`, { waitUntil: 'networkidle0' });
    await page.focus('#doc-search-input');
    await page.waitForFunction(() => document.getElementById('doc-search-status').textContent.includes('검색 색인을 불러오지 못했습니다'));

    assert.equal(await page.$$eval('[data-nav-group]', nodes => nodes.length), 4,
      'the four static navigation groups must remain available');
    assert.equal(await page.$$eval('.nav-menu a', nodes => nodes.length), 16,
      'all static landing deep links must remain available');
    assert.equal(await page.$eval('.pcv-architecture-source-image', node => node.complete), true,
      'the current architecture remains available without the search index');
    assert.equal(await page.$eval('#doc-search-status a', node => node.getAttribute('href')), 'guide-content.md');
    assert.equal(await page.$eval('#doc-search-input', node => node.getAttribute('aria-expanded')), 'true');
  });
});

test('docs portal: 하위 장에서도 새 shell 안에 guide-content 전체와 좌우 목차를 렌더링한다', async () => {
  await withPage([], async (page, { port }) => {
    await page.goto(`http://127.0.0.1:${port}/ui/docs.html#6-네트워크`, { waitUntil: 'networkidle0' });
    await page.waitForFunction(() => !document.getElementById('docs-reader').hidden &&
      !document.getElementById('reader-content').hidden && document.getElementById('6-네트워크'));

    const expectedHeadings = renderedHeadingCount();
    const expectedChapters = GUIDE_SOURCE.split('\n').filter(line => /^##\s+\d+\.\s+/.test(line)).length;
    const expectedBlocks = canonicalBlockCounts();
    const readerState = await page.evaluate(() => ({
      pathname: location.pathname,
      hash: decodeURIComponent(location.hash),
      headerVisible: document.querySelector('.site-header').getBoundingClientRect().height > 0,
      landingHidden: document.getElementById('docs-landing').hidden,
      readerHidden: document.getElementById('docs-reader').hidden,
      headings: document.querySelectorAll('#reader-content h1, #reader-content h2, #reader-content h3, #reader-content h4, #reader-content h5').length,
      chapters: document.querySelectorAll('#reader-chapter-nav [data-slug]').length,
      tocLinks: document.querySelectorAll('#reader-toc [data-slug]').length,
      tables: document.querySelectorAll('#reader-content table').length,
      codeBlocks: document.querySelectorAll('#reader-content pre code').length,
      focusableCodeBlocks: document.querySelectorAll('#reader-content pre[tabindex="0"]').length,
      copyButtons: document.querySelectorAll('#reader-content .reader-copy-code').length,
      namedCopyButtons: document.querySelectorAll('#reader-content .reader-copy-code[aria-label="코드 복사"]').length,
      quotes: document.querySelectorAll('#reader-content blockquote').length,
      listItems: document.querySelectorAll('#reader-content li').length,
      links: document.querySelectorAll('#reader-content a:not(.reader-heading-anchor)').length,
      hasFirstChapter: document.getElementById('1-시작하기') !== null,
      hasLastChapter: document.getElementById('22-품질-게이트-가이드') !== null,
      legacyTopbar: document.querySelector('.topbar') !== null,
      activeChapter: document.querySelector('#reader-chapter-nav [aria-current="page"]')?.getAttribute('data-slug'),
    }));
    assert.equal(readerState.pathname, '/ui/docs.html');
    assert.equal(readerState.hash, '#6-네트워크');
    assert.equal(readerState.headerVisible, true);
    assert.equal(readerState.landingHidden, true);
    assert.equal(readerState.readerHidden, false);
    assert.equal(readerState.headings, expectedHeadings, 'all H1-H5 headings must be reflected in the new reader');
    assert.equal(readerState.chapters, expectedChapters, 'left navigation must expose every numbered chapter');
    assert.ok(readerState.tocLinks > 0, 'current chapter must expose its lower-level headings');
    assert.equal(readerState.tables, expectedBlocks.tables, 'every canonical Markdown table must be rendered');
    assert.equal(readerState.codeBlocks, expectedBlocks.codeBlocks,
      'every canonical fenced code block must be rendered');
    assert.equal(readerState.focusableCodeBlocks, expectedBlocks.codeBlocks,
      'every horizontally scrollable code block must be keyboard focusable');
    assert.equal(readerState.copyButtons, expectedBlocks.codeBlocks,
      'every canonical code block must expose a native copy button');
    assert.equal(readerState.namedCopyButtons, expectedBlocks.codeBlocks,
      'every copy button must expose an accessible name');
    assert.equal(readerState.quotes, expectedBlocks.quotes, 'every canonical blockquote must be rendered');
    assert.equal(readerState.listItems, expectedBlocks.listItems, 'every canonical list item must be rendered');
    assert.equal(readerState.links, expectedBlocks.links, 'every canonical inline link must be rendered');
    assert.equal(readerState.hasFirstChapter, true);
    assert.equal(readerState.hasLastChapter, true);
    assert.equal(readerState.legacyTopbar, false);
    assert.equal(readerState.activeChapter, '6-네트워크');

    await page.goto(`http://127.0.0.1:${port}/ui/guide.html#14-rest-api`, { waitUntil: 'networkidle0' });
    await page.waitForFunction(() => location.pathname.endsWith('/ui/docs.html') &&
      decodeURIComponent(location.hash) === '#14-rest-api' &&
      !document.getElementById('docs-reader').hidden);
    const restChapterState = await page.evaluate(() => {
      const start = document.getElementById('14-rest-api');
      const end = document.getElementById('15-cli-레퍼런스');
      let node = start?.nextElementSibling;
      let codeBlocks = 0;
      let text = start?.textContent || '';
      while (node && node !== end) {
        if (node.matches('.reader-code')) codeBlocks += 1;
        text += ' ' + node.textContent;
        node = node.nextElementSibling;
      }
      return {
        codeBlocks,
        hasPush: text.includes('PushSubscription.toJSON()') && text.includes('/api/v1/push/vapid/rotate'),
        hasRpc: text.includes('tenant_overlay.create') && text.includes('debug.trace.start'),
        activeChapter: document.querySelector('#reader-chapter-nav [aria-current="page"]')?.getAttribute('data-slug'),
      };
    });
    assert.deepEqual(restChapterState, {
      codeBlocks: 25,
      hasPush: true,
      hasRpc: true,
      activeChapter: '14-rest-api',
    });
  });
});

test('docs portal: 상세 reader shell에 axe 접근성 위반이 없다', async () => {
  await withPage([], async (page, { port }) => {
    await page.goto(`http://127.0.0.1:${port}/ui/docs.html#6-네트워크`, { waitUntil: 'networkidle0' });
    await page.waitForFunction(() => !document.getElementById('docs-reader').hidden &&
      !document.getElementById('reader-content').hidden);
    await page.evaluate(axe.source);
    const violations = await page.evaluate(async () => {
      const result = await window.axe.run({
        include: [['#docs-reader']],
        exclude: [['#reader-content']],
      });
      return result.violations.map(rule => ({
        id: rule.id,
        impact: rule.impact,
        nodes: rule.nodes.map(node => ({
          target: node.target,
          summary: node.failureSummary,
          html: node.html,
        })),
      }));
    });
    assert.deepEqual(violations, []);
  });
});

test('docs portal: 1440·1280·1024·768·480·390px에서 live landing과 reader 계약을 지킨다', async () => {
  await withPage([], async (page, { port }) => {
    for (const width of [1440, 1280, 1024, 768, 480, 390]) {
      await page.setViewport({ width, height: 1000, deviceScaleFactor: 1 });
      await page.goto(`http://127.0.0.1:${port}/ui/docs.html`, { waitUntil: 'networkidle0' });
      const layout = await page.evaluate(() => ({
        overflow: document.documentElement.scrollWidth - document.documentElement.clientWidth,
        headerHeight: document.querySelector('.site-header').getBoundingClientRect().height,
        headerPosition: getComputedStyle(document.querySelector('.site-header')).position,
        headerInnerRadius: getComputedStyle(document.querySelector('.site-header-inner')).borderRadius,
        primaryNavDisplay: getComputedStyle(document.querySelector('.primary-nav')).display,
        utilitiesDisplay: getComputedStyle(document.querySelector('.header-utilities')).display,
        mobileLanguageDisplay: getComputedStyle(document.querySelector('.mobile-language-switch')).display,
        shellWidth: document.querySelector('.pcv-shell').getBoundingClientRect().width,
        shellX: document.querySelector('.pcv-shell').getBoundingClientRect().x,
        shellY: document.querySelector('.pcv-shell').getBoundingClientRect().y,
        eyebrowY: document.querySelector('.pcv-eyebrow').getBoundingClientRect().y,
        titleSize: parseFloat(getComputedStyle(document.getElementById('pcv-hero-title')).fontSize),
        actionDisplay: getComputedStyle(document.querySelector('.pcv-actions')).display,
        actionHeight: document.querySelector('.pcv-button-primary').getBoundingClientRect().height,
        actionWidth: document.querySelector('.pcv-button-primary').getBoundingClientRect().width,
        actionContainerWidth: document.querySelector('.pcv-actions').getBoundingClientRect().width,
        legendColumns: getComputedStyle(document.querySelector('.pcv-architecture-legend'))
          .gridTemplateColumns.split(' ').length,
        mapRight: document.querySelector('.pcv-control-map').getBoundingClientRect().right,
        mapY: document.querySelector('.pcv-control-map').getBoundingClientRect().y,
        mapBarHeight: document.querySelector('.pcv-map-bar').getBoundingClientRect().height,
        legendHeight: document.querySelector('.pcv-architecture-legend').getBoundingClientRect().height,
        searchX: document.querySelector('.doc-search').getBoundingClientRect().x,
        mobileLanguageX: document.querySelector('.mobile-language-switch').getBoundingClientRect().x,
        architectureLoaded: document.querySelector('.pcv-architecture-source-image').naturalWidth > 0,
        visibleLegacyChapters: [...document.querySelectorAll('.chapter-card')]
          .filter(node => getComputedStyle(node).display !== 'none' && node.getBoundingClientRect().height > 0).length,
      }));
      assert.ok(layout.overflow <= 1, `${width}px document must not overflow horizontally`);
      assert.equal(layout.headerHeight, width < 800 ? 56 : 64);
      assert.equal(layout.headerPosition, 'fixed');
      assert.equal(layout.headerInnerRadius, '0px');
      assert.equal(layout.primaryNavDisplay === 'none', width <= 1152,
        `${width}px primary navigation breakpoint must match the target`);
      assert.equal(layout.utilitiesDisplay === 'none', width < 800,
        `${width}px desktop utilities must collapse only on mobile`);
      assert.equal(layout.mobileLanguageDisplay !== 'none', width < 800,
        `${width}px mobile language pill must replace desktop utilities`);
      assert.ok(layout.shellWidth <= Math.min(1200, width - (width < 800 ? 32 : 48)) + 1,
        `${width}px hero shell must respect the target max width and gutter`);
      assert.ok(layout.titleSize >= (width < 800 ? 35 : 47),
        `${width}px hero headline must preserve the target scale`);
      assert.ok(layout.actionHeight >= 48, `${width}px hero action must remain 48px or taller`);
      assert.equal(layout.legendColumns, width > 1152 ? 7 : (width >= 800 ? 4 : 2));
      assert.ok(layout.mapRight <= width + 1, `${width}px architecture frame must stay in the viewport`);
      assert.equal(layout.architectureLoaded, true, `${width}px current architecture SVG must load`);
      assert.equal(layout.visibleLegacyChapters, 0, `${width}px legacy category cards must stay off the landing`);
      if (width < 800) {
        assert.equal(layout.actionDisplay, 'grid');
        assert.ok(Math.abs(layout.actionWidth - layout.actionContainerWidth) <= 1,
          `${width}px landing actions must stack at full width`);
      } else {
        assert.equal(layout.actionDisplay, 'flex');
      }
      if (width === 1440) {
        assertNear(layout.shellX, 120, '1440px shell x');
        assertNear(layout.shellY, 65, '1440px shell y');
        assertNear(layout.eyebrowY, 153, '1440px eyebrow y');
        assertNear(layout.titleSize, 65.6, '1440px title size', 0.1);
        assertNear(layout.mapY, 579.69, '1440px map y');
        assertNear(layout.mapBarHeight, 56, '1440px map bar height');
        assertNear(layout.legendHeight, 58.39, '1440px legend height');
        assertNear(layout.searchX, 855.94, '1440px search x');
      } else if (width === 390) {
        assertNear(layout.shellX, 16, '390px shell x');
        assertNear(layout.shellY, 57, '390px shell y');
        assertNear(layout.eyebrowY, 129, '390px eyebrow y');
        assertNear(layout.titleSize, 35.2, '390px title size', 0.1);
        assertNear(layout.mapY, 594.98, '390px map y');
        assertNear(layout.mapBarHeight, 90.67, '390px map bar height');
        assertNear(layout.legendHeight, 182.56, '390px legend height');
        assertNear(layout.searchX, 342, '390px search x');
        assertNear(layout.mobileLanguageX, 130.83, '390px language switch x');
      }

      await page.focus('#doc-search-input');
      const searchBounds = await page.$eval('#doc-search-popover', node => {
        const rect = node.getBoundingClientRect();
        return { left: rect.left, right: rect.right, width: rect.width };
      });
      assert.ok(searchBounds.left >= 0 && searchBounds.right <= width + 1,
        `${width}px search popover must stay inside viewport`);

      await page.goto(`http://127.0.0.1:${port}/ui/docs.html#6-네트워크`, { waitUntil: 'networkidle0' });
      await page.waitForFunction(() => !document.getElementById('docs-reader').hidden &&
        !document.getElementById('reader-content').hidden);
      await page.waitForFunction(() => {
        const targetTop = document.getElementById('6-네트워크').getBoundingClientRect().top;
        const mobileToc = document.getElementById('reader-mobile-toc');
        const stickyBottom = getComputedStyle(mobileToc).display === 'none'
          ? document.querySelector('.site-header').getBoundingClientRect().bottom
          : mobileToc.getBoundingClientRect().bottom;
        return targetTop >= stickyBottom && targetTop <= stickyBottom + 140;
      });
      const readerLayout = await page.evaluate(() => ({
        overflow: document.documentElement.scrollWidth - document.documentElement.clientWidth,
        headerBackdrop: getComputedStyle(document.querySelector('.site-header')).backdropFilter,
        headerRadius: getComputedStyle(document.querySelector('.site-header-inner')).borderRadius,
        headerHeight: document.querySelector('.site-header').getBoundingClientRect().height,
        toggleDisplay: getComputedStyle(document.getElementById('reader-index-toggle')).display,
        indexPosition: getComputedStyle(document.getElementById('reader-index')).position,
        rightTocDisplay: getComputedStyle(document.querySelector('.reader-on-page')).display,
        mobileTocDisplay: getComputedStyle(document.getElementById('reader-mobile-toc')).display,
        mobileSection: document.getElementById('reader-mobile-section').textContent.trim(),
        targetTop: document.getElementById('6-네트워크').getBoundingClientRect().top,
        stickyBottom: getComputedStyle(document.getElementById('reader-mobile-toc')).display === 'none'
          ? document.querySelector('.site-header').getBoundingClientRect().bottom
          : document.getElementById('reader-mobile-toc').getBoundingClientRect().bottom,
        activeBackground: getComputedStyle(document.querySelector('#reader-chapter-nav .is-active')).backgroundColor,
        activeColor: getComputedStyle(document.querySelector('#reader-chapter-nav .is-active')).color,
        copyHeight: document.querySelector('.reader-copy-code').getBoundingClientRect().height,
        navFontSize: parseFloat(getComputedStyle(document.querySelector('#reader-chapter-nav a')).fontSize),
        navFontWeight: Number(getComputedStyle(document.querySelector('#reader-chapter-nav a')).fontWeight),
        navLineHeight: parseFloat(getComputedStyle(document.querySelector('#reader-chapter-nav a')).lineHeight),
        navHeight: document.querySelector('#reader-chapter-nav a').getBoundingClientRect().height,
        groupFontSize: parseFloat(getComputedStyle(document.querySelector('.reader-nav-group-title')).fontSize),
        groupFontFamily: getComputedStyle(document.querySelector('.reader-nav-group-title')).fontFamily,
        tableOverflow: [...document.querySelectorAll('.reader-table-wrap')]
          .every(node => node.scrollWidth >= node.clientWidth && node.getBoundingClientRect().width <= innerWidth),
      }));
      assert.ok(readerLayout.overflow <= 1, `${width}px reader must not overflow horizontally`);
      assert.match(readerLayout.headerBackdrop, /blur\(16px\)/,
        `${width}px docs header must retain the live-site fixed-header blur`);
      assert.equal(readerLayout.headerRadius, '0px');
      assert.equal(readerLayout.headerHeight, width < 800 ? 56 : 64);
      assert.equal(readerLayout.activeBackground, 'rgb(13, 13, 13)');
      assert.equal(readerLayout.activeColor, 'rgb(255, 255, 255)');
      assert.ok(readerLayout.copyHeight >= 40, `${width}px copy target must remain at least 40px`);
      assert.equal(readerLayout.mobileSection, '6. 네트워크',
        `${width}px deep link must retain the requested chapter name`);
      assert.ok(readerLayout.targetTop >= readerLayout.stickyBottom &&
        readerLayout.targetTop <= readerLayout.stickyBottom + 140,
      `${width}px deep link must align below the fixed header and current-section strip ` +
        `(target=${readerLayout.targetTop}, sticky=${readerLayout.stickyBottom})`);
      assert.equal(readerLayout.tableOverflow, true, `${width}px wide tables must stay in their scroll containers`);
      assert.ok(readerLayout.navFontSize >= 14, `${width}px chapter navigation must remain at least 14px`);
      assert.ok(readerLayout.navFontWeight >= 500, `${width}px chapter navigation must keep medium weight`);
      assert.ok(readerLayout.navLineHeight / readerLayout.navFontSize >= 1.4,
        `${width}px chapter navigation must keep readable leading`);
      assert.ok(readerLayout.navHeight >= 40, `${width}px chapter navigation rows must remain at least 40px`);
      assert.ok(readerLayout.groupFontSize >= 12, `${width}px navigation group labels must remain at least 12px`);
      assert.match(readerLayout.groupFontFamily, /Pretendard/,
        `${width}px Korean navigation group labels must use the sans family`);
      if (width <= 1100) {
        assert.notEqual(readerLayout.toggleDisplay, 'none');
        assert.equal(readerLayout.indexPosition, 'fixed');
        assert.equal(readerLayout.mobileTocDisplay, 'flex');
        await page.click('#reader-index-toggle');
        assert.equal(await page.$eval('body', node => node.classList.contains('reader-nav-open')), true);
        assert.equal(await page.$eval('#reader-overlay', node => node.hidden), false);
        await page.click('#reader-overlay');
        assert.equal(await page.$eval('body', node => node.classList.contains('reader-nav-open')), false);
      } else {
        assert.equal(readerLayout.toggleDisplay, 'none');
        assert.equal(readerLayout.indexPosition, 'sticky');
        assert.equal(readerLayout.mobileTocDisplay, 'none');
      }
      assert.equal(readerLayout.rightTocDisplay === 'none', width <= 1320);
    }
  });
});
