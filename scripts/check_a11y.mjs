#!/usr/bin/env node









import { spawn } from 'node:child_process';
import net from 'node:net';
import { setTimeout as wait } from 'node:timers/promises';
import puppeteer from 'puppeteer';
import axe from 'axe-core';
const axeSource = axe.source;

const configuredPort = parseInt(process.env.AXE_PORT || '0', 10) || 0;
const PAGES = (process.env.AXE_PAGES || 'guide.html,index.html,offline.html')
  .split(',')
  .map(normalizePage)
  .filter(Boolean);

const THEMES = process.env.AXE_THEMES === 'all'
  ? [null, 'supanova', 'supanova-cyan', 'supanova-hicontrast']
  : (process.env.AXE_THEMES ? process.env.AXE_THEMES.split(',') : [null]);
const MAX = parseInt(process.env.AXE_MAX_VIOLATIONS || '0', 10);

function normalizePage(pageName) {
  const p = pageName.trim().replace(/^\/+/, '');
  if (!p) return '';
  if (p.startsWith('ui/') || p.startsWith('docs/')) return p;
  return 'ui/' + p;
}

function reservePort(preferredPort) {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.unref();
    server.on('error', reject);
    server.listen(preferredPort, '127.0.0.1', () => {
      const address = server.address();
      const port = typeof address === 'object' && address ? address.port : preferredPort;
      server.close(() => resolve(port));
    });
  });
}

async function waitForHttpServer(port, srv, getStderr) {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    if (srv.exitCode !== null) {
      throw new Error('static server exited early: ' + (getStderr() || 'no stderr'));
    }
    try {
      const res = await fetch(`http://127.0.0.1:${port}/`);
      if (res.ok) return;
    } catch (_) {

    }
    await wait(100);
  }
  throw new Error('static server did not become ready');
}

const PORT = await reservePort(configuredPort);
let serverStderr = '';
const srv = spawn('python3', ['-m', 'http.server', String(PORT), '--bind', '127.0.0.1'],
  { cwd: '.', stdio: ['ignore', 'ignore', 'pipe'] });
srv.stderr.on('data', chunk => {
  serverStderr += chunk.toString();
});

await waitForHttpServer(PORT, srv, () => serverStderr.trim());

let exitCode = 0;
let grandTotal = 0;
try {
  const browser = await puppeteer.launch({
    headless: 'new',
    args: ['--no-sandbox', '--disable-setuid-sandbox'],
  });

  for (const theme of THEMES) {
    for (const pageName of PAGES) {
      const url = `http://127.0.0.1:${PORT}/${pageName}`;
      const page = await browser.newPage();
      try {
        await page.goto(url, { waitUntil: 'networkidle2', timeout: 30000 });
        if (theme) {
          await page.evaluate((t) => {
            document.documentElement.setAttribute('data-theme', t);
          }, theme);
          await wait(150);
        }
        await page.evaluate(axeSource);
        const result = await page.evaluate(async () => await window.axe.run());
        const v = result.violations || [];
        const total = v.reduce((s, x) => s + x.nodes.length, 0);
        grandTotal += total;
        const tag = theme ? `[${theme}]` : '[default]';
        console.log(`\n=== ${tag} ${pageName} === ${v.length} rules / ${total} nodes`);
        for (const rule of v) {
          console.log(`  [${rule.impact}] ${rule.id} — ${rule.help} (${rule.nodes.length} nodes)`);
          if (process.env.AXE_VERBOSE) {
            for (const node of rule.nodes) {
              console.log(`    target: ${node.target.join(' ')}`);
              if (node.failureSummary) console.log(`      ${node.failureSummary.split('\n').join(' | ')}`);
            }
          }
        }
      } catch (e) {
        console.error(`  ERROR ${pageName} ${theme || 'default'}: ${e.message}`);
        exitCode = 2;
      } finally {
        await page.close();
      }
    }
  }
  await browser.close();

  console.log(`\nTOTAL: ${grandTotal} violations across ${PAGES.length} pages × ${THEMES.length} themes`);
  if (grandTotal > MAX) {
    console.error(`FAIL: ${grandTotal} > ${MAX} threshold`);
    exitCode = 1;
  } else {
    console.log(`OK: ${grandTotal} ≤ ${MAX}`);
  }
} catch (e) {
  console.error('axe runner error:', e.message);
  exitCode = 2;
} finally {
  srv.kill();
}

process.exit(exitCode);
