                                                                                                   
                                                                                    
                                                                  
  
                                                                
  
                  
                                                                               
                                                                                 
                                                                            
  
               
                                                                               
                                                                 
   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = [
  'ui/modules/ui.js',
  'ui/modules/uxlib.js',
  'ui/modules/filter-state.js',
  'ui/modules/endpoints.js',
  'ui/modules/monitor.js',
                                                                    
  'ui/modules/shell.js'
];

const CONFIG = Object.freeze({
  enabled: true,
  cpu_warn: 80,
  cpu_crit: 95,
  mem_warn: 85,
  mem_crit: 95,
  disk_warn: 80,
  disk_crit: 90,
  eval_period: 30,
  webhook_url: '',
  webhook_format: 'generic',
  telegram_chat_id: '',
  dedup_window: 300,
  alert_count: 7,
  composite_rules: [],
  config_revision: 11,
  daemon_config_valid: true,
  daemon_config_error: ''
});

async function boot(page) {
  await page.evaluate(() => {
    window.currentTab = 'mon-alerts';
    window.authToken = 'test-token';
                                                                
                                                                
    window.currentUser = { role: 'ADMIN' };
    window._DEBUG = false;
    window.addEvt = () => {};
    PCV.api = PCV.api || {};
    PCV.api.RPC_ERROR = { CONFLICT: -32002 };
    window.t = key => key;
    window._L = ko => ko;
    window.unwrapData = value => value && value.data !== undefined ? value.data : value;
    window.unwrapList = value => {
      const data = window.unwrapData(value);
      return Array.isArray(data) ? data : [];
    };
    window.fetchGet = url => fetch(url).then(async response => {
      const json = await response.json();
      if (!response.ok) {
        const error = new Error(json.error?.message || `HTTP ${response.status}`);
        error.code = json.error?.code;
        throw error;
      }
      if (json.error) {
        const error = new Error(json.error.message);
        error.code = json.error.code;
        throw error;
      }
      return json;
    });
    window.fetchPut = (url, body) => fetch(url, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    }).then(response => response.json());
  });
  await page.evaluate(() => PCV.monitor.renderAlerts(document.getElementById('cb')));
  await page.waitForSelector(
    '#alert-config-region:not([aria-busy="true"])',
    { timeout: 3000 }
  );
  await page.waitForSelector(
    '#alert-history-region:not([aria-busy="true"])',
    { timeout: 3000 }
  );
}

async function attachActualNavigationApp(page) {
  await boot(page);
  await page.evaluate(() => {
    return fetch('/ui/index.html').then(response => response.text()).then(html => {
      const parsed = new DOMParser().parseFromString(html, 'text/html');
      document.body.innerHTML = parsed.body.innerHTML;
    });
  });
  await page.evaluate(() => {
    window.__navigationScriptErrors = [];
    window.addEventListener('error', event => {
      window.__navigationScriptErrors.push(String(event.error || event.message));
    });
    window._L = ko => ko;
    window.t = key => key;
    PCV.filterEditionItems = items => items;
    window.restoreSession = () => {};
    window.pcvClusterEnabled = true;
    [
      'setCtrSort',
      'snapNameValidate',
      'snapCreateExec',
      'rbValidate',
      'rbExec',
      'snapDeleteAll',
      'sdaPreview',
      'sdaExec',
      'ctrDistChanged',
      'ctrIpModeChanged',
      'ctrLoadBridges',
      'vmPower',
      'render'
    ].forEach(name => {
      window[name] = () => {};
    });
  });
  await page.addScriptTag({ url: '/ui/modules/modal-core.js' });
  await page.addScriptTag({ url: '/ui/modules/nav.js' });
  await page.addScriptTag({ url: '/ui/app.js' });
  await page.evaluate(() => {
    if (!(window.PCV && PCV.shell && typeof PCV.shell.setActive === 'function')) {
      throw new Error(window.__navigationScriptErrors.join('; '));
    }
                                            
                                                                                
    window.__routeSurfaces = () => {
      const crumb = document.getElementById('shell-crumb');
      const bolds = crumb ? crumb.querySelectorAll('b') : [];
      return {
        activeTab: bolds.length ? bolds[bolds.length - 1].textContent : '',
        breadcrumbs: crumb ? crumb.textContent : '',
        activeNav: document.querySelector(
          '#shell-sidebar .shell-navitem[aria-current="page"]'
        )?.dataset.nav || ''
      };
    };
    window.currentTab = 'mon-alerts';
    window.setHashRoute('mon-alerts');
    window.setPageTitle('알림');
    PCV.shell.setActive('mon-alerts');
  });
  await page.evaluate(() => PCV.monitor.renderAlerts(document.getElementById('cb')));
  await page.waitForSelector('#alert-config-region:not([aria-busy="true"])');
}

async function attachActualAlertSocket(page) {
  await page.evaluate(() => {
    class MockWebSocket {
      static OPEN = 1;
      constructor() {
        this.readyState = MockWebSocket.OPEN;
        window.__alertSocket = this;
      }
      send() {}
      close() {}
    }
    window.WebSocket = MockWebSocket;
  });
  await page.addScriptTag({ url: '/ui/modules/api.js' });
  await page.evaluate(() => {
    window.authToken = 'test-token';
    PCV.api.connectWS();
    window.__alertSocket.onopen();
    window.__alertSocket.onmessage({
      data: JSON.stringify({ type: 'auth_ok' })
    });
  });
}

function okRoutes(config = CONFIG, history = []) {
  return {
    '/api/v1/alerts/config': { status: 200, body: { data: config } },
    '/api/v1/alerts': { status: 200, body: history }
  };
}

test('mon-alerts renders exactly one configuration heading and one apply action', async () => {
  await withPage(MODS, async (page) => {
    await boot(page);
    const result = await page.evaluate(() => ({
      headings: Array.from(document.querySelectorAll('h2,h3,h4'))
        .filter(node => node.textContent.includes('알림 설정')).length,
      applies: document.querySelectorAll('[data-alert-apply]').length,
      applyDisabled: document.querySelector('[data-alert-apply]').disabled,
      status: document.getElementById('al-status').textContent,
      dlqTitle: document.getElementById('alert-dlq-title').textContent,
      oldSave: typeof window.alertSave,
      oldMonitorSave: typeof window.saveAlertConfig,
      monitorApply: typeof PCV.monitor.applyAlertConfig,
      monitorManualRefresh: typeof PCV.monitor.refreshAlertHistory,
      windowApply: typeof window.applyAlertConfig,
      windowManualRefresh: typeof window.refreshAlertHistory,
      eventRefresh: typeof PCV.monitor.refreshAlertHistoryFromEvent
    }));
    assert.deepEqual(result, {
      headings: 1,
      applies: 1,
      applyDisabled: true,
      status: '',
      dlqTitle: '웹훅 전송 실패 (DLQ)',
      oldSave: 'undefined',
      oldMonitorSave: 'undefined',
      monitorApply: 'undefined',
      monitorManualRefresh: 'undefined',
      windowApply: 'undefined',
      windowManualRefresh: 'undefined',
      eventRefresh: 'function'
    });
  }, { routes: okRoutes() });
});

test('webhook controls keep explicit label rows without viewport overflow', async () => {
  await withPage(MODS, async (page) => {
    await page.setViewport({ width: 1440, height: 900 });
    await boot(page);

    const inspect = async width => {
      await page.setViewport({ width, height: 900 });
      await page.evaluate(() => new Promise(resolve =>
        requestAnimationFrame(() => requestAnimationFrame(resolve))));
      return page.evaluate(() => {
        const grid = document.querySelector('.alert-webhook-grid');
        const pairs = [
          ['al-webhook_format', '형식'],
          ['al-webhook_url', 'URL'],
          ['al-telegram_chat_id', 'Telegram Chat ID']
        ].map(([id, text]) => {
          const control = document.getElementById(id);
          const label = grid.querySelector(`label[for="${id}"]`);
          const lr = label.getBoundingClientRect();
          const cr = control.getBoundingClientRect();
          return {
            id,
            text: label.textContent,
            sameRow: Math.abs(lr.top - cr.top) < 3,
            labelAbove: lr.bottom <= cr.top + 1,
            ordered: lr.right <= cr.left + 1,
            controlInside: cr.left >= grid.getBoundingClientRect().left &&
              cr.right <= grid.getBoundingClientRect().right + 1
          };
        });
        return {
          pairs,
          columns: getComputedStyle(grid).gridTemplateColumns.split(' ').length,
          overflow: document.documentElement.scrollWidth -
            document.documentElement.clientWidth
        };
      });
    };

    for (const width of [1440, 1024]) {
      const result = await inspect(width);
      assert.equal(result.columns, 2, `${width}px keeps label/control columns`);
      assert.equal(result.overflow, 0, `${width}px document overflow`);
      result.pairs.forEach(pair => {
        assert.equal(pair.text, pair.id === 'al-webhook_format'
          ? '형식'
          : pair.id === 'al-webhook_url' ? 'URL' : 'Telegram Chat ID');
        assert.equal(pair.sameRow, true, `${width}px ${pair.id} row alignment`);
        assert.equal(pair.ordered, true, `${width}px ${pair.id} order`);
        assert.equal(pair.controlInside, true, `${width}px ${pair.id} card bounds`);
      });
    }

    const narrow = await inspect(768);
    assert.equal(narrow.columns, 1);
    assert.equal(narrow.overflow, 0);
    narrow.pairs.forEach(pair => {
      assert.equal(pair.labelAbove, true, `768px ${pair.id} label above control`);
      assert.equal(pair.controlInside, true, `768px ${pair.id} card bounds`);
    });
  }, { routes: okRoutes() });
});

test('config failure disables the form without hiding successful history', async () => {
  await withPage(MODS, async (page) => {
    await boot(page);
    const result = await page.evaluate(() => ({
      configError: document.querySelector('#alert-config-region').textContent,
      inputs: document.querySelectorAll('#alert-config-region input, #alert-config-region select').length,
      apply: document.querySelectorAll('#alert-config-region [data-alert-apply]').length,
      retryDisabled: document.querySelector('[data-alert-config-retry]').disabled,
      historyRows: document.querySelectorAll('#alert-history-region tbody tr').length
    }));
    assert.match(result.configError, /설정을 불러오지 못했습니다/);
    assert.equal(result.inputs, 0);
    assert.equal(result.apply, 0);
    assert.equal(result.retryDisabled, false);
    assert.equal(result.historyRows, 1);
  }, {
    routes: {
      '/api/v1/alerts/config': {
        status: 503,
        body: { error: { message: 'config unavailable' } }
      },
      '/api/v1/alerts': {
        status: 200,
        body: [{
          timestamp: 100,
          severity: 'crit',
          metric: 'cpu',
          value: 99,
          message: 'hot'
        }]
      }
    }
  });
});

test('alert WebSocket refreshes only history and preserves the active settings draft', async () => {
  let historyGets = 0;
  let configGets = 0;
  await withPage(MODS, async (page) => {
    await boot(page);
    await page.$eval('#al-cpu_warn', node => {
      node.value = '81';
      node.dispatchEvent(new Event('input', { bubbles: true }));
      node.focus();
    });
    await page.evaluate(() => {
      window.__alertConfigRoot = document.getElementById('alert-config-region');
      window.__renderContentCalls = 0;
      window.renderContent = () => {
        window.__renderContentCalls++;
      };
      class MockWebSocket {
        static OPEN = 1;
        constructor() {
          this.readyState = MockWebSocket.OPEN;
          window.__alertSocket = this;
        }
        send() {}
        close() {}
      }
      window.WebSocket = MockWebSocket;
    });
    await page.addScriptTag({ url: '/ui/modules/api.js' });
    await page.evaluate(() => {
      window.authToken = 'test-token';
      PCV.api.connectWS();
      window.__alertSocket.onopen();
      window.__alertSocket.onmessage({
        data: JSON.stringify({ type: 'auth_ok' })
      });
      window.__alertSocket.onmessage({
        data: JSON.stringify({ type: 'alert' })
      });
    });
    await page.waitForSelector('#alert-history-region tbody tr', {
      timeout: 3000
    });
    const result = await page.evaluate(() => ({
      sameConfigRoot:
        window.__alertConfigRoot === document.getElementById('alert-config-region'),
      value: document.getElementById('al-cpu_warn').value,
      activeId: document.activeElement && document.activeElement.id,
      status: document.getElementById('al-status').textContent,
      historyRows:
        document.querySelectorAll('#alert-history-region tbody tr').length,
      renderContentCalls: window.__renderContentCalls
    }));
    assert.deepEqual(result, {
      sameConfigRoot: true,
      value: '81',
      activeId: 'al-cpu_warn',
      status: '적용되지 않은 변경',
      historyRows: 1,
      renderContentCalls: 0
    });
    assert.equal(configGets, 1);
    assert.equal(historyGets, 2);
  }, {
    routes: {
      '/api/v1/alerts/config': () => {
        configGets++;
        return { status: 200, body: { data: CONFIG } };
      },
      '/api/v1/alerts': () => {
        historyGets++;
        return {
          status: 200,
          body: historyGets === 1
            ? []
            : [{
                timestamp: 101,
                severity: 'warn',
                metric: 'memory',
                value: 91,
                message: 'new alert'
              }]
        };
      }
    }
  });
});

test('alert WebSocket burst commits only the newest history response and preserves the draft', async () => {
  let historyGets = 0;
  let configGets = 0;
  await withPage(MODS, async (page) => {
    await boot(page);
    await page.$eval('#al-cpu_warn', node => {
      node.value = '81';
      node.dispatchEvent(new Event('input', { bubbles: true }));
      node.focus();
    });
    await page.evaluate(() => {
      window.__alertConfigRoot = document.getElementById('alert-config-region');
    });
    await attachActualAlertSocket(page);
    await page.evaluate(() => {
      window.__eventHistoryRequests = [];
      window.fetchGet = () => new Promise(resolve => {
        window.__eventHistoryRequests.push(resolve);
      });
      window.__alertSocket.onmessage({
        data: JSON.stringify({ type: 'alert' })
      });
      window.__alertSocket.onmessage({
        data: JSON.stringify({ type: 'alert' })
      });
    });
    await page.waitForFunction(() => window.__eventHistoryRequests.length === 2);
    await page.evaluate(() => {
      window.__eventHistoryRequests[1]([{
        timestamp: 103,
        severity: 'crit',
        metric: 'cpu',
        value: 96,
        message: 'newest'
      }]);
    });
    await page.waitForFunction(() => (
      document.getElementById('alert-history-region')
        .textContent.includes('newest')
    ));
    await page.evaluate(() => {
      window.__eventHistoryRequests[0]([{
        timestamp: 102,
        severity: 'warn',
        metric: 'memory',
        value: 90,
        message: 'older'
      }]);
    });
    await new Promise(resolve => setTimeout(resolve, 30));
    const result = await page.evaluate(() => ({
      sameConfigRoot:
        window.__alertConfigRoot === document.getElementById('alert-config-region'),
      value: document.getElementById('al-cpu_warn').value,
      activeId: document.activeElement && document.activeElement.id,
      status: document.getElementById('al-status').textContent,
      history: document.getElementById('alert-history-region').textContent
    }));
    assert.equal(result.sameConfigRoot, true);
    assert.equal(result.value, '81');
    assert.equal(result.activeId, 'al-cpu_warn');
    assert.equal(result.status, '적용되지 않은 변경');
    assert.match(result.history, /newest/);
    assert.doesNotMatch(result.history, /older/);
    assert.equal(configGets, 1);
    assert.equal(historyGets, 1);
  }, {
    routes: {
      '/api/v1/alerts/config': () => {
        configGets++;
        return { status: 200, body: { data: CONFIG } };
      },
      '/api/v1/alerts': () => {
        historyGets++;
        return { status: 200, body: [] };
      }
    }
  });
});

test('a late failed event request cannot replace the newest successful history', async () => {
  let historyGets = 0;
  await withPage(MODS, async (page) => {
    await boot(page);
    await attachActualAlertSocket(page);
    await page.evaluate(() => {
      window.__eventHistoryRequests = [];
      window.fetchGet = () => new Promise(resolve => {
        window.__eventHistoryRequests.push(resolve);
      });
      window.__alertSocket.onmessage({
        data: JSON.stringify({ type: 'alert' })
      });
      window.__alertSocket.onmessage({
        data: JSON.stringify({ type: 'alert' })
      });
    });
    await page.waitForFunction(() => window.__eventHistoryRequests.length === 2);
    await page.evaluate(() => {
      window.__eventHistoryRequests[1]([{
        timestamp: 203,
        severity: 'crit',
        metric: 'disk',
        value: 97,
        message: 'latest success'
      }]);
    });
    await page.waitForFunction(() => (
      document.getElementById('alert-history-region')
        .textContent.includes('latest success')
    ));
    await page.evaluate(() => {
      window.__eventHistoryRequests[0]({
        error: { message: 'older failed' }
      });
    });
    await new Promise(resolve => setTimeout(resolve, 30));
    const result = await page.evaluate(() => ({
      history: document.getElementById('alert-history-region').textContent,
      refreshError: Boolean(
        document.querySelector('.alert-history-refresh-error')
      ),
      fullError: Boolean(
        document.querySelector('[data-alert-history-retry]')
      )
    }));
    assert.match(result.history, /latest success/);
    assert.equal(result.refreshError, false);
    assert.equal(result.fullError, false);
  }, {
    routes: {
      '/api/v1/alerts/config': { status: 200, body: { data: CONFIG } },
      '/api/v1/alerts': () => {
        historyGets++;
        return { status: 200, body: [] };
      }
    }
  });
});

test('failed newest event during initial load becomes a full history error', async () => {
  await withPage(MODS, async (page) => {
    await page.evaluate(config => {
      window.currentTab = 'mon-alerts';
      window.authToken = 'test-token';
      window._DEBUG = false;
      window.addEvt = () => {};
      PCV.api = PCV.api || {};
      PCV.api.RPC_ERROR = { CONFLICT: -32002 };
      window.t = key => key;
      window._L = ko => ko;
      window.unwrapData = value => (
        value && value.data !== undefined ? value.data : value
      );
      window.unwrapList = value => {
        const data = window.unwrapData(value);
        return Array.isArray(data) ? data : [];
      };
      window.fetchPut = () => Promise.reject(new Error('unexpected PUT'));
      window.__initialRaceHistory = [];
      window.fetchGet = url => {
        if (url.includes('/alerts/config')) {
          return Promise.resolve({ data: config });
        }
        return new Promise(resolve => {
          window.__initialRaceHistory.push(resolve);
        });
      };
      window.__initialRaceRender = PCV.monitor.renderAlerts(
        document.getElementById('cb')
      );
    }, CONFIG);
    await page.waitForFunction(() => window.__initialRaceHistory.length === 1);

    await page.evaluate(() => {
      window.__initialRaceRefresh =
        PCV.monitor.refreshAlertHistoryFromEvent();
    });
    await page.waitForFunction(() => window.__initialRaceHistory.length === 2);
    await page.evaluate(async () => {
      window.__initialRaceHistory[1]({
        error: { message: 'newest event unavailable' }
      });
      await window.__initialRaceRefresh;
    });

    await page.evaluate(async () => {
      window.__initialRaceHistory[0]([{
        timestamp: 100,
        severity: 'warn',
        metric: 'cpu',
        value: 81,
        message: 'stale initial success'
      }]);
      await window.__initialRaceRender;
    });
    const result = await page.evaluate(() => ({
      text: document.getElementById('alert-history-region').textContent,
      retry: Boolean(document.querySelector('[data-alert-history-retry]')),
      inline: Boolean(document.querySelector('.alert-history-refresh-error')),
      rows: document.querySelectorAll('#alert-history-region tbody tr').length,
      stamp: document.getElementById('alert-last-updated').textContent
    }));
    assert.match(result.text, /알림 이력을 불러오지 못했습니다/);
    assert.doesNotMatch(result.text, /알림이 아직 없습니다/);
    assert.doesNotMatch(result.text, /새로고침에 실패했습니다/);
    assert.equal(result.retry, true);
    assert.equal(result.inline, false);
    assert.equal(result.rows, 0);
    assert.match(result.stamp, /—/);
  }, { routes: okRoutes() });
});

test('direct alert rerender invalidates history requests without generation reuse', async () => {
  await withPage(MODS, async (page) => {
    await boot(page);
    await page.evaluate(config => {
      window.__rerenderHistoryRequests = [];
      window.fetchGet = url => {
        if (url.includes('/alerts/config')) {
          return Promise.resolve({ data: config });
        }
        return new Promise(resolve => {
          window.__rerenderHistoryRequests.push(resolve);
        });
      };
      window.__alertRenders = [
        PCV.monitor.renderAlerts(document.getElementById('cb')),
        PCV.monitor.renderAlerts(document.getElementById('cb'))
      ];
    }, CONFIG);
    await page.waitForFunction(() => (
      window.__rerenderHistoryRequests.length === 2
    ));
    await page.evaluate(() => {
      window.__rerenderHistoryRequests[1]([{
        timestamp: 403,
        severity: 'crit',
        metric: 'cpu',
        value: 98,
        message: 'new render'
      }]);
    });
    await page.waitForFunction(() => (
      document.getElementById('alert-history-region')
        .textContent.includes('new render')
    ));
    await page.evaluate(async () => {
      window.__rerenderHistoryRequests[0]([{
        timestamp: 402,
        severity: 'warn',
        metric: 'memory',
        value: 89,
        message: 'old render'
      }]);
      await Promise.allSettled(window.__alertRenders);
    });
    const history = await page.$eval(
      '#alert-history-region',
      node => node.textContent
    );
    assert.match(history, /new render/);
    assert.doesNotMatch(history, /old render/);
  }, { routes: okRoutes() });
});

test('detached alert generations discard delayed config and history responses together', async () => {
  await withPage(MODS, async (page) => {
    await boot(page);
    await page.evaluate(config => {
      window.__staleConfig = config;
      window.__staleAlertResolvers = [];
      window.__deferStaleAlerts = true;
      window.fetchGet = url => {
        if (window.__deferStaleAlerts) {
          return new Promise(resolve => {
            window.__staleAlertResolvers.push({ url, resolve });
          });
        }
        if (url.includes('/alerts/config')) {
          return Promise.resolve({
            data: {
              ...config,
              cpu_warn: 72,
              config_revision: 12
            }
          });
        }
        return Promise.resolve([{
          timestamp: 500,
          severity: 'crit',
          metric: 'disk',
          value: 98,
          message: 'current generation'
        }]);
      };
      window.__staleRender = PCV.monitor.renderAlerts(
        document.getElementById('cb')
      );
    }, CONFIG);
    await page.waitForFunction(() => window.__staleAlertResolvers.length === 2);
    await page.evaluate(async () => {
      window.__detachedConfigRoot =
        document.getElementById('alert-config-region');
      window.__detachedHistoryRoot =
        document.getElementById('alert-history-region');
      window.__deferStaleAlerts = false;
      window._navGeneration = PCV.ui.navGen() + 1;
      await PCV.monitor.renderAlerts(document.getElementById('cb'));
      window.__staleAlertResolvers.forEach(request => {
        if (request.url.includes('/alerts/config')) {
          request.resolve({
            data: {
              ...window.__staleConfig,
              cpu_warn: 65,
              config_revision: 99
            }
          });
        } else {
          request.resolve([{
            timestamp: 999,
            severity: 'warn',
            metric: 'memory',
            value: 90,
            message: 'obsolete generation'
          }]);
        }
      });
      await window.__staleRender;
    });
    const result = await page.evaluate(() => ({
      cpuWarn: document.getElementById('al-cpu_warn').value,
      history: document.getElementById('alert-history-region').textContent,
      detachedConfig: window.__detachedConfigRoot.isConnected,
      detachedHistory: window.__detachedHistoryRoot.isConnected,
      detachedConfigText: window.__detachedConfigRoot.textContent,
      detachedHistoryText: window.__detachedHistoryRoot.textContent
    }));
    assert.equal(result.cpuWarn, '72');
    assert.match(result.history, /current generation/);
    assert.doesNotMatch(result.history, /obsolete generation/);
    assert.equal(result.detachedConfig, false);
    assert.equal(result.detachedHistory, false);
    assert.doesNotMatch(result.detachedConfigText, /65/);
    assert.doesNotMatch(result.detachedHistoryText, /obsolete generation/);
  }, { routes: okRoutes() });
});

test('event refresh failure preserves successful rows and reports an inline live error', async () => {
  let historyGets = 0;
  const initialHistory = [{
    timestamp: 301,
    severity: 'warn',
    metric: 'memory',
    value: 90,
    message: 'preserved row'
  }];
  await withPage(MODS, async (page) => {
    await boot(page);
    await page.$eval('#al-cpu_warn', node => {
      node.value = '81';
      node.dispatchEvent(new Event('input', { bubbles: true }));
      node.focus();
    });
    await page.evaluate(() => {
      window.__alertConfigRoot = document.getElementById('alert-config-region');
    });
    await attachActualAlertSocket(page);
    await page.evaluate(() => {
      window.fetchGet = () => Promise.resolve({
        error: { message: 'refresh unavailable' }
      });
      window.__alertSocket.onmessage({
        data: JSON.stringify({ type: 'alert' })
      });
    });
    await page.waitForSelector(
      '.alert-history-refresh-error[aria-live="polite"]',
      { timeout: 3000 }
    );
    const result = await page.evaluate(() => ({
      sameConfigRoot:
        window.__alertConfigRoot === document.getElementById('alert-config-region'),
      value: document.getElementById('al-cpu_warn').value,
      activeId: document.activeElement && document.activeElement.id,
      status: document.getElementById('al-status').textContent,
      history: document.getElementById('alert-history-region').textContent,
      rows: document.querySelectorAll('#alert-history-region tbody tr').length,
      fullError: Boolean(
        document.querySelector('[data-alert-history-retry]')
      )
    }));
    assert.equal(result.sameConfigRoot, true);
    assert.equal(result.value, '81');
    assert.equal(result.activeId, 'al-cpu_warn');
    assert.equal(result.status, '적용되지 않은 변경');
    assert.match(result.history, /preserved row/);
    assert.match(result.history, /새 알림 이력을 갱신하지 못했습니다/);
    assert.equal(result.rows, 1);
    assert.equal(result.fullError, false);
    assert.equal(historyGets, 1);
  }, {
    routes: {
      '/api/v1/alerts/config': { status: 200, body: { data: CONFIG } },
      '/api/v1/alerts': () => {
        historyGets++;
        return { status: 200, body: initialHistory };
      }
    }
  });
});

test('initial history failure keeps the independent full error and retry action', async () => {
  await withPage(MODS, async (page) => {
    await boot(page);
    const result = await page.evaluate(() => ({
      configInput: Boolean(document.getElementById('al-cpu_warn')),
      history: document.getElementById('alert-history-region').textContent,
      retry: Boolean(document.querySelector('[data-alert-history-retry]'))
    }));
    assert.equal(result.configInput, true);
    assert.match(result.history, /알림 이력을 불러오지 못했습니다/);
    assert.equal(result.retry, true);
  }, {
    routes: {
      '/api/v1/alerts/config': { status: 200, body: { data: CONFIG } },
      '/api/v1/alerts': {
        status: 503,
        body: { error: { message: 'initial unavailable' } }
      }
    }
  });
});

test('partial successful config payload is rejected instead of filled with defaults', async () => {
  await withPage(MODS, async (page) => {
    await boot(page);
    const result = await page.evaluate(() => ({
      text: document.getElementById('alert-config-region').textContent,
      inputs: document.querySelectorAll('#alert-config-region input').length
    }));
    assert.match(result.text, /설정을 불러오지 못했습니다/);
    assert.equal(result.inputs, 0);
  }, { routes: okRoutes({ enabled: true }) });
});

test('Enable stays local and apply sends only editable fields plus expected revision', async () => {
  await withPage(MODS, async (page, ctx) => {
    await boot(page);
    await page.click('#al-enabled');
    assert.equal(ctx.requests.filter(request => request.method === 'PUT').length, 0);
    assert.match(
      await page.$eval('#al-status', node => node.textContent),
      /적용되지 않은 변경/
    );
    await page.click('[data-alert-apply]');
    await page.waitForFunction(() => (
      /현재 실행 환경에 설정을 적용했습니다/.test(
        document.querySelector('#al-status').textContent
      )
    ));
    const put = ctx.requests.find(request => request.method === 'PUT');
    assert.deepEqual(put.json, {
      enabled: false,
      cpu_warn: 80,
      cpu_crit: 95,
      mem_warn: 85,
      mem_crit: 95,
      disk_warn: 80,
      disk_crit: 90,
      eval_period: 30,
      webhook_url: '',
      webhook_format: 'generic',
      telegram_chat_id: '',
      expected_revision: 11
    });
    assert.equal(
      await page.$eval('[data-alert-apply]', node => node.disabled),
      true
    );
  }, {
    routes: {
      '/api/v1/alerts/config': request => {
        if (request.method !== 'PUT') {
          return { status: 200, body: { data: CONFIG } };
        }
        const applied = {
          ...CONFIG,
          ...request.json,
          config_revision: 12
        };
        delete applied.expected_revision;
        return { status: 200, body: { data: applied } };
      },
      '/api/v1/alerts': { status: 200, body: [] }
    }
  });
});

test('failed apply preserves draft and dirty state', async () => {
  await withPage(MODS, async (page, ctx) => {
    await boot(page);
    await page.$eval('#al-cpu_warn', node => {
      node.value = '81';
      node.dispatchEvent(new Event('input', { bubbles: true }));
    });
    await page.click('#al-enabled');
    await page.click('[data-alert-apply]');
    await page.waitForFunction(() => (
      /설정을 적용하지 못했습니다/.test(document.querySelector('#al-status').textContent)
    ));
    const result = await page.evaluate(() => ({
      cpuWarn: document.getElementById('al-cpu_warn').value,
      enabled: document.getElementById('al-enabled').checked,
      status: document.getElementById('al-status').textContent,
      applyDisabled: document.querySelector('[data-alert-apply]').disabled
    }));
    assert.deepEqual(result, {
      cpuWarn: '81',
      enabled: false,
      status: '설정을 적용하지 못했습니다: unavailable',
      applyDisabled: false
    });
    assert.equal(ctx.requests.filter(request => request.method === 'PUT').length, 1);
  }, {
    routes: {
      '/api/v1/alerts/config': request => request.method === 'PUT'
        ? { status: 500, body: { error: { message: 'unavailable' } } }
        : { status: 200, body: { data: CONFIG } },
      '/api/v1/alerts': { status: 200, body: [] }
    }
  });
});

test('invalid numeric and webhook drafts never send a PUT request', async () => {
  await withPage(MODS, async (page, ctx) => {
    await boot(page);
    const cases = [
      ['#al-cpu_warn', '95'],
      ['#al-cpu_warn', '-1'],
      ['#al-cpu_warn', '101'],
      ['#al-cpu_warn', '80.5'],
      ['#al-cpu_warn', ''],
      ['#al-mem_warn', '95'],
      ['#al-disk_crit', '80'],
      ['#al-eval_period', '4'],
      ['#al-eval_period', '601'],
      ['#al-webhook_url', 'ftp://example.test/hook'],
      ['#al-webhook_url', 'https://'],
      ['#al-webhook_url', 'é'.repeat(256)],
      ['#al-telegram_chat_id', '한'.repeat(22)]
    ];
    for (const [selector, value] of cases) {
      await page.$eval(selector, (node, next) => {
        node.value = next;
        node.dispatchEvent(new Event('input', { bubbles: true }));
      }, value);
      assert.equal(
        await page.$eval(selector, node => node.getAttribute('aria-invalid')),
        'true'
      );
      assert.equal(
        await page.$eval('[data-alert-apply]', node => node.disabled),
        true
      );
      assert.match(
        await page.$eval('#al-status', node => node.textContent),
        /입력 범위·길이/
      );
      await page.$eval(selector, node => {
        const defaults = {
          'al-cpu_warn': '80',
          'al-mem_warn': '85',
          'al-disk_crit': '90',
          'al-eval_period': '30',
          'al-webhook_url': '',
          'al-telegram_chat_id': ''
        };
        node.value = defaults[node.id];
        node.dispatchEvent(new Event('input', { bubbles: true }));
      });
    }
    await page.$eval('#al-cpu_warn', node => {
      node.value = '81';
      node.dispatchEvent(new Event('input', { bubbles: true }));
    });
    assert.equal(
      await page.$eval('#al-cpu_warn', node => node.hasAttribute('aria-invalid')),
      false
    );
    assert.equal(
      await page.$eval('[data-alert-apply]', node => node.disabled),
      false
    );
    assert.equal(
      await page.$eval('#al-status', node => node.textContent),
      '적용되지 않은 변경'
    );
    assert.equal(ctx.requests.filter(request => request.method === 'PUT').length, 0);
  }, { routes: okRoutes() });
});

test('conflict preserves draft until confirmed server reload', async () => {
  let getCount = 0;
  await withPage(MODS, async (page, ctx) => {
    await page.evaluate(() => {
      window.__confirmDecisions = [];
    });
    await boot(page);
    await page.evaluate(() => {
      PCV.ui.customConfirm = () => Promise.resolve(window.__confirmDecisions.shift());
    });
    await page.$eval('#al-cpu_warn', node => {
      node.value = '82';
      node.dispatchEvent(new Event('input', { bubbles: true }));
    });
    await page.click('[data-alert-apply]');
    await page.waitForSelector('[data-alert-conflict-reload]:not([hidden])');
    assert.equal(await page.$eval('#al-cpu_warn', node => node.value), '82');
    assert.match(
      await page.$eval('#al-status', node => node.textContent),
      /다른 작업자가 설정을 변경했습니다/
    );

    await page.evaluate(() => window.__confirmDecisions.push(false));
    await page.click('[data-alert-conflict-reload]');
    await new Promise(resolve => setTimeout(resolve, 20));
    assert.equal(getCount, 1);
    assert.equal(await page.$eval('#al-cpu_warn', node => node.value), '82');

    await page.evaluate(() => window.__confirmDecisions.push(true));
    await page.click('[data-alert-conflict-reload]');
    await page.waitForFunction(() => (
      document.getElementById('al-cpu_warn')?.value === '83'
    ));
    assert.equal(getCount, 2);
    assert.equal(
      ctx.requests.filter(request => request.method === 'PUT').length,
      1
    );
    assert.equal(
      await page.$eval('[data-alert-apply]', node => node.disabled),
      true
    );
  }, {
    routes: {
      '/api/v1/alerts/config': request => {
        if (request.method === 'PUT') {
          return {
            status: 409,
            body: { error: { code: -32002, message: 'revision conflict' } }
          };
        }
        getCount++;
        return {
          status: 200,
          body: {
            data: getCount === 1
              ? CONFIG
              : { ...CONFIG, cpu_warn: 83, config_revision: 12 }
          }
        };
      },
      '/api/v1/alerts': { status: 200, body: [] }
    }
  });
});

test('dirty settings navigation asks once and cancel preserves the draft', async () => {
  await withPage(MODS, async (page) => {
    await boot(page);
    const result = await page.evaluate(async () => {
      let confirms = 0;
      let resolveConfirm;
      PCV.ui.customConfirm = () => {
        confirms++;
        return new Promise(resolve => {
          resolveConfirm = resolve;
        });
      };
      const input = document.getElementById('al-cpu_warn');
      input.value = '82';
      input.dispatchEvent(new Event('input', { bubbles: true }));
      const commits = [];
      const first = PCV.ui.requestNavigation('dashboard', () => commits.push('dashboard'));
      const duplicate = PCV.ui.requestNavigation('storage', () => commits.push('storage'));
      resolveConfirm(false);
      await Promise.resolve();
      await Promise.resolve();
      return {
        first,
        duplicate,
        confirms,
        commits,
        value: input.value,
        status: document.getElementById('al-status').textContent
      };
    });
    assert.deepEqual(result, {
      first: false,
      duplicate: false,
      confirms: 1,
      commits: [],
      value: '82',
      status: '적용되지 않은 변경'
    });
  }, { routes: okRoutes() });
});

test('actual app navigation keeps every route surface unchanged on cancel and commits once on approval', async () => {
  await withPage(MODS, async (page) => {
    await attachActualNavigationApp(page);
    const result = await page.evaluate(async () => {
      const input = document.getElementById('al-cpu_warn');
      input.value = '81';
      input.dispatchEvent(new Event('input', { bubbles: true }));
      input.focus();

      let prompts = 0;
      let resolveDecision;
      PCV.ui.customConfirm = () => {
        prompts++;
        return new Promise(resolve => {
          resolveDecision = resolve;
        });
      };
      const snapshot = () => ({
        tab: window.currentTab,
        generation: window._navGeneration,
        hash: location.hash,
        title: document.title,
        activeTab: window.__routeSurfaces().activeTab,
        breadcrumbs: window.__routeSurfaces().breadcrumbs,
        value: document.getElementById('al-cpu_warn')?.value || ''
      });
      const before = snapshot();
      const first = window.navigateTo('test-route');
      const duplicate = window.navigateTo('storage');
      const pending = snapshot();
      resolveDecision(false);
      await Promise.resolve();
      await Promise.resolve();
      const cancelled = snapshot();
      const promptsAfterCancel = prompts;

      PCV.ui.customConfirm = () => {
        prompts++;
        return Promise.resolve(true);
      };
      const approvedReturn = window.navigateTo('test-route');
      await Promise.resolve();
      await Promise.resolve();
      const approved = snapshot();
      return {
        first,
        duplicate,
        approvedReturn,
        before,
        pending,
        cancelled,
        approved,
        promptsAfterCancel,
        prompts
      };
    });
    assert.equal(result.first, false);
    assert.equal(result.duplicate, false);
    assert.equal(result.approvedReturn, false);
    assert.deepEqual(result.pending, result.before);
    assert.deepEqual(result.cancelled, result.before);
    assert.equal(result.promptsAfterCancel, 1);
    assert.equal(result.prompts, 2);
    assert.equal(result.approved.tab, 'test-route');
    assert.equal(
      result.approved.generation,
      result.before.generation + 1
    );
    assert.equal(result.approved.hash, '#/test-route');
    assert.equal(result.approved.title, 'PureCVisor — Test Route');
                                                             
    assert.equal(result.approved.activeTab, 'test-route');
    assert.match(result.approved.breadcrumbs, /test-route/);
  }, { routes: okRoutes() });
});

test('actual app hash approval preserves a resource id and opens it once', async () => {
  await withPage(MODS, async (page) => {
    await attachActualNavigationApp(page);
    const result = await page.evaluate(async () => {
      const input = document.getElementById('al-cpu_warn');
      input.value = '81';
      input.dispatchEvent(new Event('input', { bubbles: true }));

      let resolveDecision;
      PCV.ui.customConfirm = () => new Promise(resolve => {
        resolveDecision = resolve;
      });
      const opened = [];
      window.openResourceById = (target, id) => opened.push([target, id]);

      history.replaceState(null, '', '#/test-route/resource-42');
      window.dispatchEvent(new HashChangeEvent('hashchange'));
      const pendingHash = location.hash;
      resolveDecision(true);
      await new Promise(resolve => setTimeout(resolve, 250));
      return {
        pendingHash,
        finalHash: location.hash,
        tab: window.currentTab,
        opened
      };
    });
    assert.deepEqual(result, {
      pendingHash: '#/mon-alerts',
      finalHash: '#/test-route/resource-42',
      tab: 'test-route',
      opened: [['test-route', 'resource-42']]
    });
  }, { routes: okRoutes() });
});

test('actual app navigation is synchronous and prompt-free after apply succeeds', async () => {
  await withPage(MODS, async (page) => {
    await attachActualNavigationApp(page);
    await page.$eval('#al-cpu_warn', node => {
      node.value = '81';
      node.dispatchEvent(new Event('input', { bubbles: true }));
    });
    await page.$eval('[data-alert-apply]', node => node.click());
    await page.waitForFunction(() => (
      /현재 실행 환경에 설정을 적용했습니다/.test(
        document.getElementById('al-status').textContent
      )
    ));
    const result = await page.evaluate(() => {
      let prompts = 0;
      PCV.ui.customConfirm = () => {
        prompts++;
        return Promise.resolve(false);
      };
      const beforeGeneration = window._navGeneration;
      const accepted = window.navigateTo('test-route');
      return {
        accepted,
        prompts,
        tab: window.currentTab,
        generation: window._navGeneration,
        beforeGeneration,
        hash: location.hash,
        title: document.title,
        activeTab: window.__routeSurfaces().activeTab
      };
    });
    assert.deepEqual(result, {
      accepted: true,
      prompts: 0,
      tab: 'test-route',
      generation: result.beforeGeneration + 1,
      beforeGeneration: result.beforeGeneration,
      hash: '#/test-route',
      title: 'PureCVisor — Test Route',
      activeTab: 'test-route'
    });
  }, {
    routes: {
      '/api/v1/alerts/config': request => {
        if (request.method !== 'PUT') {
          return { status: 200, body: { data: CONFIG } };
        }
        const applied = {
          ...CONFIG,
          ...request.json,
          config_revision: 12
        };
        delete applied.expected_revision;
        return { status: 200, body: { data: applied } };
      },
      '/api/v1/alerts': { status: 200, body: [] }
    }
  });
});

test('shell chrome waits for approval and then follows the committed route', async () => {
  await withPage(MODS, async (page) => {
    await attachActualNavigationApp(page);
    const result = await page.evaluate(async () => {
      const input = document.getElementById('al-cpu_warn');
      input.value = '81';
      input.dispatchEvent(new Event('input', { bubbles: true }));

      const snapshot = () => ({
        tab: window.currentTab,
        activeNav: window.__routeSurfaces().activeNav,
        hash: location.hash,
        title: document.title,
        activeTab: window.__routeSurfaces().activeTab,
        breadcrumbs: window.__routeSurfaces().breadcrumbs
      });
      const before = snapshot();

      const navItem = (id) => document.querySelector(
        '#shell-sidebar .shell-navitem[data-nav="' + id + '"]'
      );
      navItem('containers').click();
      const pending = snapshot();
      const cancelDialog = PCV.modalCore.currentDialog();
      cancelDialog?.querySelector('[data-confirm-cancel]')?.click();
      await Promise.resolve();
      await Promise.resolve();
      const cancelled = snapshot();

      navItem('mon-overview').click();
      const approveDialog = PCV.modalCore.currentDialog();
      const approvalOpened = Boolean(approveDialog);
      approveDialog?.querySelector('[data-confirm-accept]')?.click();
      await Promise.resolve();
      await Promise.resolve();
      const approved = snapshot();
      return {
        before,
        pending,
        cancelled,
        approvalOpened,
        approved
      };
    });
    assert.deepEqual(result.pending, result.before);
    assert.deepEqual(result.cancelled, result.before);
    assert.equal(result.approvalOpened, true);
    assert.equal(result.approved.tab, 'mon-overview');
    assert.equal(result.approved.activeNav, 'mon-overview');
    assert.equal(result.approved.hash, '#/mon-overview');
    assert.equal(result.approved.title, 'PureCVisor — 운영 개요');
    assert.equal(result.approved.activeTab, '운영 개요');
    assert.match(result.approved.breadcrumbs, /운영 개요/);
  }, { routes: okRoutes() });
});

test('programmatic navigateTo mutates route surfaces only after navigation approval', async () => {
  await withPage(MODS, async (page) => {
    await attachActualNavigationApp(page);
    const result = await page.evaluate(async () => {
                                                                        
      const input = document.getElementById('al-cpu_warn');
      input.value = '81';
      input.dispatchEvent(new Event('input', { bubbles: true }));

      const snapshot = () => ({
        tab: window.currentTab,
        activeTab: window.__routeSurfaces().activeTab,
        activeNav: window.__routeSurfaces().activeNav,
        hash: location.hash,
        title: document.title,
        breadcrumbs: window.__routeSurfaces().breadcrumbs
      });
      const before = snapshot();

      window.navigateTo('storage');
      const pending = snapshot();
      const cancelDialog = PCV.modalCore.currentDialog();
      cancelDialog?.querySelector('[data-confirm-cancel]')?.click();
      await Promise.resolve();
      await Promise.resolve();
      const cancelled = snapshot();

      window.navigateTo('storage');
      const approveDialog = PCV.modalCore.currentDialog();
      const approvalOpened = Boolean(approveDialog);
      approveDialog?.querySelector('[data-confirm-accept]')?.click();
      await Promise.resolve();
      await Promise.resolve();
      const approved = snapshot();
      return {
        before,
        pending,
        cancelled,
        approvalOpened,
        approved
      };
    });
    assert.deepEqual(result.pending, result.before);
    assert.deepEqual(result.cancelled, result.before);
    assert.equal(result.approvalOpened, true);
    assert.equal(result.approved.activeTab, '스토리지');
    assert.equal(result.approved.activeNav, 'storage');
    assert.equal(result.approved.tab, 'storage');
    assert.equal(result.approved.hash, '#/storage');
    assert.equal(result.approved.title, 'PureCVisor — 스토리지');
    assert.match(result.approved.breadcrumbs, /스토리지/);
  }, { routes: okRoutes() });
});

test('leaving the dirty alert screen falls back to dashboard after approval', async () => {
  await withPage(MODS, async (page) => {
    await attachActualNavigationApp(page);
    const result = await page.evaluate(async () => {
      const input = document.getElementById('al-cpu_warn');
      input.value = '81';
      input.dispatchEvent(new Event('input', { bubbles: true }));
      input.focus();

      const snapshot = () => ({
        tab: window.currentTab,
        activeTab: window.__routeSurfaces().activeTab,
        activeNav: window.__routeSurfaces().activeNav,
        hash: location.hash,
        title: document.title,
        breadcrumbs: window.__routeSurfaces().breadcrumbs,
        draftConnected: input.isConnected,
        draftValue: input.value,
        draftStatus: document.getElementById('al-status')?.textContent || ''
      });
      const before = snapshot();

      window.navigateTo('dashboard');
      const pending = snapshot();
      PCV.modalCore.currentDialog()
        ?.querySelector('[data-confirm-cancel]')
        ?.click();
      await Promise.resolve();
      await Promise.resolve();
      const cancelled = snapshot();

      window.navigateTo('dashboard');
      const approveDialog = PCV.modalCore.currentDialog();
      const approvalOpened = Boolean(approveDialog);
      approveDialog?.querySelector('[data-confirm-accept]')?.click();
      await Promise.resolve();
      await Promise.resolve();
      const approved = {
        tab: window.currentTab,
        activeTab: window.__routeSurfaces().activeTab,
        activeNav: window.__routeSurfaces().activeNav,
        hash: location.hash,
        title: document.title,
        breadcrumbs: window.__routeSurfaces().breadcrumbs
      };
      await new Promise((resolve, reject) => {
        const deadline = Date.now() + 3000;
        function waitForDraftDetach() {
          if (!input.isConnected) {
            resolve();
          } else if (Date.now() >= deadline) {
            reject(new Error('alert draft stayed mounted after dashboard navigation'));
          } else {
            setTimeout(waitForDraftDetach, 10);
          }
        }
        waitForDraftDetach();
      });
      approved.draftConnected = input.isConnected;
      approved.draftPresent = Boolean(document.getElementById('al-cpu_warn'));

      const followupReturn = window.navigateTo('storage');
      const followup = {
        tab: window.currentTab,
        hash: location.hash,
        title: document.title,
        activeTab: window.__routeSurfaces().activeTab,
        breadcrumbs: window.__routeSurfaces().breadcrumbs,
        dialogs: document.querySelectorAll('dialog').length
      };
      return {
        before,
        pending,
        cancelled,
        approvalOpened,
        approved,
        followupReturn,
        followup
      };
    });
    assert.deepEqual(result.pending, result.before);
    assert.deepEqual(result.cancelled, result.before);
    assert.equal(result.approvalOpened, true);
                                                       
    assert.equal(result.approved.activeTab, '운영 대시보드');
    assert.equal(result.approved.activeNav, 'dashboard');
    assert.equal(result.approved.tab, 'dashboard');
    assert.equal(result.approved.hash, '#/dashboard');
    assert.equal(result.approved.title, 'PureCVisor — 대시보드');
    assert.match(result.approved.breadcrumbs, /대시보드/);
    assert.equal(result.approved.draftConnected, false);
    assert.equal(result.approved.draftPresent, false);
    assert.equal(result.followupReturn, true);
    assert.equal(result.followup.tab, 'storage');
    assert.equal(result.followup.hash, '#/storage');
    assert.equal(result.followup.title, 'PureCVisor — 스토리지');
    assert.equal(result.followup.activeTab, '스토리지');
    assert.match(result.followup.breadcrumbs, /스토리지/);
    assert.equal(result.followup.dialogs, 0);
  }, { routes: okRoutes() });
});

test('command actions use the app navigation entry and update every route surface', async () => {
  await withPage(MODS, async (page) => {
    await attachActualNavigationApp(page);
    const result = await page.evaluate(() => {
      const action = window.CMD_ACTIONS.find(item => item.label === '스토리지');
      const accepted = action.action();
      return {
        accepted,
        tab: window.currentTab,
        activeNav: window.__routeSurfaces().activeNav,
        hash: location.hash,
        title: document.title,
        activeTab: window.__routeSurfaces().activeTab,
        breadcrumbs: window.__routeSurfaces().breadcrumbs
      };
    });
    assert.deepEqual(result, {
      accepted: true,
      tab: 'storage',
      activeNav: 'storage',
      hash: '#/storage',
      title: 'PureCVisor — 스토리지',
      activeTab: '스토리지',
      breadcrumbs: result.breadcrumbs
    });
    assert.match(result.breadcrumbs, /스토리지/);
  }, { routes: okRoutes() });
});

test('apply disables every setting control and rejects duplicate submission', async () => {
  let releasePut;
  await withPage(MODS, async (page, ctx) => {
    await boot(page);
    await page.$eval('#al-cpu_warn', node => {
      node.value = '81';
      node.dispatchEvent(new Event('input', { bubbles: true }));
    });
    await page.click('[data-alert-apply]');
    await page.waitForFunction(() => (
      document.querySelector('[data-alert-apply]').textContent.includes('적용 중')
    ));
    const locked = await page.evaluate(() => ({
      disabled: Array.from(document.querySelectorAll(
        '#alert-config-region input, #alert-config-region select, #alert-config-region button'
      )).every(node => node.disabled),
      label: document.querySelector('[data-alert-apply]').textContent
    }));
    assert.deepEqual(locked, { disabled: true, label: '적용 중…' });
    await page.$eval('[data-alert-apply]', node => node.click());
    assert.equal(ctx.requests.filter(request => request.method === 'PUT').length, 1);

    releasePut();
    await page.waitForFunction(() => (
      /현재 실행 환경에 설정을 적용했습니다/.test(
        document.getElementById('al-status').textContent
      )
    ));
  }, {
    routes: {
      '/api/v1/alerts/config': request => {
        if (request.method !== 'PUT') {
          return { status: 200, body: { data: CONFIG } };
        }
        return new Promise(resolve => {
          releasePut = () => resolve({
            status: 200,
            body: {
              data: {
                ...CONFIG,
                ...request.json,
                config_revision: 12,
                expected_revision: undefined
              }
            }
          });
        });
      },
      '/api/v1/alerts': { status: 200, body: [] }
    }
  });
});

test('invalid daemon source warning survives runtime apply and clears after a valid reload', async () => {
  let getCount = 0;
  const invalidSource = {
    ...CONFIG,
    enabled: false,
    cpu_warn: 70,
    cpu_crit: 90,
    daemon_config_valid: false,
    daemon_config_error: 'invalid_alert_config'
  };
  await withPage(MODS, async (page) => {
    await boot(page);
    assert.match(
      await page.$eval('.alert-daemon-config-warning', node => node.textContent),
      /안전 기본값/
    );
    assert.equal(await page.$eval('#al-enabled', node => node.checked), false);
    assert.equal(await page.$eval('#al-cpu_warn', node => node.value), '70');

    await page.$eval('#al-cpu_warn', node => {
      node.value = '71';
      node.dispatchEvent(new Event('input', { bubbles: true }));
    });
    await page.click('[data-alert-apply]');
    await page.waitForFunction(() => (
      /현재 실행 환경에 설정을 적용했습니다/.test(
        document.getElementById('al-status').textContent
      )
    ));
    assert.equal(
      await page.$eval('.alert-daemon-config-warning', node => node.hidden),
      false
    );

    await page.evaluate(() => PCV.monitor.renderAlerts(document.getElementById('cb')));
    await page.waitForSelector(
      '#alert-config-region:not([aria-busy="true"])'
    );
    assert.equal(
      await page.$('.alert-daemon-config-warning'),
      null
    );
    assert.equal(await page.$eval('#al-cpu_warn', node => node.value), '72');
  }, {
    routes: {
      '/api/v1/alerts/config': request => {
        if (request.method === 'PUT') {
          const applied = {
            ...invalidSource,
            ...request.json,
            config_revision: 12
          };
          delete applied.expected_revision;
          return { status: 200, body: { data: applied } };
        }
        getCount++;
        return {
          status: 200,
          body: {
            data: getCount === 1
              ? invalidSource
              : {
                  ...CONFIG,
                  cpu_warn: 72,
                  config_revision: 13,
                  daemon_config_valid: true,
                  daemon_config_error: ''
                }
          }
        };
      },
      '/api/v1/alerts': { status: 200, body: [] }
    }
  });
});

test('reentry during an in-flight apply stays locked and resyncs from the server', async () => {
  let releasePut;
  let getCount = 0;
  const appliedConfig = {
    ...CONFIG,
    cpu_warn: 81,
    config_revision: 12
  };
  await withPage(MODS, async (page, ctx) => {
    await boot(page);
    await page.$eval('#al-cpu_warn', node => {
      node.value = '81';
      node.dispatchEvent(new Event('input', { bubbles: true }));
    });
    await page.click('[data-alert-apply]');
    await page.waitForFunction(() => (
      document.querySelector('[data-alert-apply]').textContent.includes('적용 중')
    ));

    await page.evaluate(async () => {
      window._navGeneration = PCV.ui.navGen() + 1;
      await PCV.monitor.renderAlerts(document.getElementById('cb'));
    });
    assert.equal(
      await page.$eval('[data-alert-apply]', node => node.disabled),
      true
    );
    await page.$eval('[data-alert-apply]', node => node.click());
    assert.equal(ctx.requests.filter(request => request.method === 'PUT').length, 1);

    releasePut();
    await page.waitForFunction(() => (
      document.getElementById('al-cpu_warn')?.value === '81' &&
      document.querySelector('[data-alert-apply]')?.textContent === '적용'
    ));
    await new Promise(resolve => setTimeout(resolve, 20));
    assert.equal(getCount, 3);
    assert.equal(
      await page.$eval('[data-alert-apply]', node => node.disabled),
      true
    );
  }, {
    routes: {
      '/api/v1/alerts/config': request => {
        if (request.method === 'PUT') {
          return new Promise(resolve => {
            releasePut = () => resolve({
              status: 200,
              body: { data: appliedConfig }
            });
          });
        }
        getCount++;
        return {
          status: 200,
          body: {
            data: getCount < 3 ? CONFIG : appliedConfig
          }
        };
      },
      '/api/v1/alerts': { status: 200, body: [] }
    }
  });
});

test('severity filtering rerenders only history and preserves the settings draft', async () => {
  const history = [
    { timestamp: 100, severity: 'warn', metric: 'cpu', value: 81, message: 'warn row' },
    { timestamp: 200, severity: 'crit', metric: 'disk', value: 97, message: 'crit row' }
  ];
  await withPage(MODS, async (page, ctx) => {
    await boot(page);
    await page.$eval('#al-cpu_warn', node => {
      node.value = '77';
      node.dispatchEvent(new Event('input', { bubbles: true }));
      node.focus();
      window.__alertConfigInput = node;
    });
    await page.evaluate(() => PCV.ui.filterState.apply({ severity: ['crit'] }));
    const filtered = await page.evaluate(() => ({
      sameInput: window.__alertConfigInput === document.getElementById('al-cpu_warn'),
      value: document.getElementById('al-cpu_warn').value,
      dirty: document.getElementById('al-status').textContent,
      active: document.activeElement.id,
      rows: document.querySelectorAll('#alert-history-region tbody tr').length,
      text: document.getElementById('alert-history-region').textContent,
      search: location.search,
      counts: Array.from(document.querySelectorAll(
        '#alert-history-region [data-facet="severity"] .chip-c'
      )).map(node => node.textContent)
    }));
    assert.equal(filtered.sameInput, true);
    assert.equal(filtered.value, '77');
    assert.match(filtered.dirty, /적용되지 않은 변경/);
    assert.equal(filtered.active, 'al-cpu_warn');
    assert.equal(filtered.rows, 1);
    assert.match(filtered.text, /crit row/);
    assert.doesNotMatch(filtered.text, /warn row/);
    assert.equal(filtered.search, '?severity=crit');
    assert.deepEqual(filtered.counts, ['2', '1', '1']);
    assert.equal(ctx.requests.filter(r => r.path === '/api/v1/alerts').length, 1);

    await page.click('[data-facet="severity"][data-val="warn"]');
    assert.equal(
      await page.evaluate(() => document.activeElement?.dataset.val),
      'warn'
    );
    await page.click('[data-facet="severity"][data-val="__all__"]');
    assert.equal(
      await page.evaluate(() => document.activeElement?.dataset.val),
      '__all__'
    );
    assert.equal(
      await page.evaluate(() => (
        document.querySelectorAll('#alert-history-region tbody tr').length
      )),
      2
    );
    assert.equal(ctx.requests.filter(r => r.path === '/api/v1/alerts').length, 1);
  }, { routes: okRoutes(CONFIG, history) });
});

test('cold alert entry restores valid severity from the URL and normalizes invalid values', async () => {
  const history = [
    { timestamp: 100, severity: 'warn', metric: 'cpu', value: 81, message: 'warn row' },
    { timestamp: 200, severity: 'crit', metric: 'disk', value: 97, message: 'crit row' }
  ];
  await withPage(MODS, async (page) => {
    await page.evaluate(() => {
      history.replaceState(null, '', '/?severity=crit');
    });
    await boot(page);
    const restored = await page.evaluate(() => ({
      selected: PCV.ui.filterState.current().severity || [],
      search: location.search,
      rows: document.querySelectorAll('#alert-history-region tbody tr').length,
      text: document.getElementById('alert-history-region').textContent,
      pressed: document.querySelector(
        '[data-facet="severity"][data-val="crit"]'
      ).getAttribute('aria-pressed')
    }));
    assert.deepEqual(restored.selected, ['crit']);
    assert.equal(restored.search, '?severity=crit');
    assert.equal(restored.rows, 1);
    assert.match(restored.text, /crit row/);
    assert.doesNotMatch(restored.text, /warn row/);
    assert.equal(restored.pressed, 'true');
  }, { routes: okRoutes(CONFIG, history) });

  await withPage(MODS, async (page) => {
    await page.evaluate(() => {
      history.replaceState(null, '', '/?severity=crit,warn');
    });
    await boot(page);
    const normalized = await page.evaluate(() => ({
      selected: PCV.ui.filterState.current().severity || [],
      search: location.search,
      rows: document.querySelectorAll('#alert-history-region tbody tr').length,
      allPressed: document.querySelector(
        '[data-facet="severity"][data-val="__all__"]'
      ).getAttribute('aria-pressed')
    }));
    assert.deepEqual(normalized.selected, []);
    assert.equal(normalized.search, '');
    assert.equal(normalized.rows, 2);
    assert.equal(normalized.allPressed, 'true');
  }, { routes: okRoutes(CONFIG, history) });
});

test('successful enabled changes immediately synchronize the cached empty-history copy', async () => {
  let revision = CONFIG.config_revision;
  await withPage(MODS, async (page, ctx) => {
    await boot(page);
    const historyRoot = await page.$('#alert-history-region');
    assert.match(
      await page.$eval('#alert-history-region', node => node.textContent),
      /임계값을 초과한 알림이 아직 없습니다/
    );

    await page.click('#al-enabled');
    await page.click('[data-alert-apply]');
    await page.waitForFunction(() => (
      document.getElementById('alert-history-region').textContent
        .includes('임계값 알림 평가가 꺼져 있습니다')
    ));
    assert.equal(
      await page.evaluate(node => node === document.getElementById(
        'alert-history-region'
      ), historyRoot),
      true
    );

    await page.click('#al-enabled');
    await page.click('[data-alert-apply]');
    await page.waitForFunction(() => (
      document.getElementById('alert-history-region').textContent
        .includes('임계값을 초과한 알림이 아직 없습니다')
    ));
    assert.equal(
      ctx.requests.filter(request => request.path === '/api/v1/alerts').length,
      1
    );
  }, {
    routes: {
      '/api/v1/alerts/config': request => {
        if (request.method === 'GET') {
          return { status: 200, body: { data: CONFIG } };
        }
        revision++;
        const applied = {
          ...CONFIG,
          ...request.json,
          config_revision: revision
        };
        delete applied.expected_revision;
        return { status: 200, body: { data: applied } };
      },
      '/api/v1/alerts': { status: 200, body: [] }
    }
  });
});

test('only finite number timestamps render and sort ahead of non-number values', async () => {
  await withPage(MODS, async (page) => {
    await boot(page);
    const rows = await page.$$eval('#alert-history-region tbody tr', nodes => (
      nodes.map(node => ({
        metric: node.children[2].textContent,
        time: node.children[0].textContent
      }))
    ));
    assert.deepEqual(
      rows.map(row => row.metric),
      [
        'later-finite',
        'negative-finite',
        'null-time',
        'empty-time',
        'false-time',
        'true-time',
        'numeric-string-time',
        'object-time'
      ]
    );
    assert.match(rows[0].time, /^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$/);
    assert.match(rows[1].time, /^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$/);
    assert.deepEqual(
      rows.slice(2).map(row => row.time),
      ['—', '—', '—', '—', '—', '—']
    );
  }, {
    routes: okRoutes(CONFIG, [
      {
        timestamp: null,
        severity: 'warn',
        metric: 'null-time',
        value: 81,
        message: 'null timestamp'
      },
      {
        timestamp: '',
        severity: 'warn',
        metric: 'empty-time',
        value: 82,
        message: 'empty timestamp'
      },
      {
        timestamp: false,
        severity: 'warn',
        metric: 'false-time',
        value: 83,
        message: 'false timestamp'
      },
      {
        timestamp: true,
        severity: 'warn',
        metric: 'true-time',
        value: 84,
        message: 'true timestamp'
      },
      {
        timestamp: '123',
        severity: 'warn',
        metric: 'numeric-string-time',
        value: 85,
        message: 'numeric string timestamp'
      },
      {
        timestamp: {},
        severity: 'warn',
        metric: 'object-time',
        value: 86,
        message: 'object timestamp'
      },
      {
        timestamp: -1,
        severity: 'crit',
        metric: 'negative-finite',
        value: 97,
        message: 'negative finite timestamp'
      },
      {
        timestamp: 5,
        severity: 'crit',
        metric: 'later-finite',
        value: 98,
        message: 'later finite timestamp'
      }
    ])
  });
});

test('manual refresh updates only history and preserves the last successful stamp on failure', async () => {
  let historyGets = 0;
  await withPage(MODS, async (page, ctx) => {
    await boot(page);
    await page.$eval('#al-cpu_warn', node => {
      node.value = '77';
      node.dispatchEvent(new Event('input', { bubbles: true }));
      node.focus();
      window.__alertConfigInput = node;
    });
    const initialStamp = await page.$eval('#alert-last-updated', node => node.textContent);
    assert.match(initialStamp, /\d{2}:\d{2}:\d{2}/);

    await page.$eval('[data-alert-refresh]', node => node.click());
    await page.waitForFunction(() => (
      document.getElementById('alert-history-region')
        .textContent.includes('crit after refresh')
    ));
    const successfulStamp = await page.$eval(
      '#alert-last-updated',
      node => node.textContent
    );
    assert.match(successfulStamp, /\d{2}:\d{2}:\d{2}/);

    await page.$eval('[data-alert-refresh]', node => node.click());
    await page.waitForFunction(() => (
      /새로고침에 실패했습니다/.test(
        document.getElementById('alert-history-message').textContent
      )
    ));
    const result = await page.evaluate(() => ({
      stamp: document.getElementById('alert-last-updated').textContent,
      text: document.getElementById('alert-history-region').textContent,
      sameInput: window.__alertConfigInput === document.getElementById('al-cpu_warn'),
      value: document.getElementById('al-cpu_warn').value,
      dirty: document.getElementById('al-status').textContent,
      active: document.activeElement.id
    }));
    assert.equal(result.stamp, successfulStamp);
    assert.match(result.text, /crit after refresh/);
    assert.equal(result.sameInput, true);
    assert.equal(result.value, '77');
    assert.match(result.dirty, /적용되지 않은 변경/);
    assert.equal(result.active, 'al-cpu_warn');
    assert.equal(ctx.requests.filter(r => r.path === '/api/v1/alerts').length, 3);
  }, {
    routes: {
      '/api/v1/alerts/config': { status: 200, body: { data: CONFIG } },
      '/api/v1/alerts': () => {
        historyGets++;
        if (historyGets === 1) {
          return {
            status: 200,
            body: [{
              timestamp: 100,
              severity: 'warn',
              metric: 'cpu',
              value: 81,
              message: 'initial warn'
            }]
          };
        }
        if (historyGets === 2) {
          return {
            status: 200,
            body: [{
              timestamp: 200,
              severity: 'crit',
              metric: 'disk',
              value: 97,
              message: 'crit after refresh'
            }]
          };
        }
        return {
          status: 503,
          body: { error: { message: 'refresh unavailable' } }
        };
      }
    }
  });
});

test('malformed history shapes fail safely while malformed records stay isolated', async () => {
  await withPage(MODS, async (page) => {
    await boot(page);
    const result = await page.evaluate(() => ({
      history: document.getElementById('alert-history-region').textContent,
      retry: Boolean(document.querySelector('[data-alert-history-retry]')),
      rows: document.querySelectorAll('#alert-history-region tbody tr').length
    }));
    assert.match(result.history, /알림 이력을 불러오지 못했습니다/);
    assert.equal(result.retry, true);
    assert.equal(result.rows, 0);
  }, {
    routes: {
      '/api/v1/alerts/config': { status: 200, body: { data: CONFIG } },
      '/api/v1/alerts': { status: 200, body: { data: { items: [] } } }
    }
  });

  await withPage(MODS, async (page) => {
    await boot(page);
    const result = await page.evaluate(() => ({
      rows: document.querySelectorAll('#alert-history-region tbody tr').length,
      unknown: Array.from(document.querySelectorAll(
        '#alert-history-region tbody tr'
      )).filter(row => row.textContent.includes('UNKNOWN')).length,
      fallbacks: document.getElementById('alert-history-region').textContent,
      caption: document.querySelector('#alert-history-region caption')?.textContent,
      scopes: Array.from(document.querySelectorAll('#alert-history-region th'))
        .every(node => node.getAttribute('scope') === 'col'),
      metricClass: document.querySelector(
        '#alert-history-region tbody td:nth-child(3)'
      )?.className,
      valueClass: document.querySelector(
        '#alert-history-region tbody td:nth-child(4)'
      )?.className
    }));
    assert.equal(result.rows, 5);
    assert.equal(result.unknown, 4);
    assert.match(result.fallbacks, /—/);
    assert.equal(result.caption, '최근 알림 이력');
    assert.equal(result.scopes, true);
    assert.match(result.metricClass, /font-mono/);
    assert.match(result.valueClass, /font-mono/);
    assert.match(result.valueClass, /text-right/);
  }, {
    routes: okRoutes(CONFIG, [
      null,
      'broken',
      7,
      [],
      {
        timestamp: 100,
        severity: 'warn',
        metric: 'cpu',
        value: 81,
        message: 'valid'
      }
    ])
  });
});

test('alert history exposes semantic severity lanes and accessible table state', async () => {
  await withPage(MODS, async (page) => {
    await boot(page);
    const visual = await page.evaluate(() => {
      const crit = document.querySelector('.alert-row-crit');
      const warn = document.querySelector('.alert-row-warn');
      const unknown = document.querySelector('.alert-row-idle');
      const table = document.querySelector('#alert-history-region table');
      const lane = row => {
        const style = getComputedStyle(row);
        const pillStyle = getComputedStyle(row.querySelector('.pill'));
        return {
          width: style.borderInlineStartWidth,
          color: style.borderInlineStartColor,
          pillColor: pillStyle.color,
          background: style.backgroundColor,
          shadow: style.boxShadow
        };
      };
      return {
        critPill: crit.querySelector('.pill').textContent,
        warnPill: warn.querySelector('.pill').textContent,
        unknownPill: unknown.querySelector('.pill').textContent,
        critLane: lane(crit),
        warnLane: lane(warn),
        unknownLane: lane(unknown),
        caption: table.querySelector('caption').textContent,
        scopes: Array.from(table.querySelectorAll('th'))
          .map(node => node.getAttribute('scope')),
        filterLabel: document.querySelector('.filterbar')
          .getAttribute('aria-label'),
        live: document.getElementById('alert-result-count')
          .getAttribute('aria-live')
      };
    });
    assert.equal(visual.critPill, 'CRIT');
    assert.equal(visual.warnPill, 'WARN');
    assert.equal(visual.unknownPill, 'UNKNOWN');
    for (const lane of [
      visual.critLane,
      visual.warnLane,
      visual.unknownLane
    ]) {
      assert.equal(lane.width, '3px');
      assert.equal(lane.color, lane.pillColor);
      assert.equal(lane.background, 'rgba(0, 0, 0, 0)');
      assert.equal(lane.shadow, 'none');
    }
    assert.equal(visual.caption, '최근 알림 이력');
    assert.ok(visual.scopes.every(value => value === 'col'));
    assert.equal(visual.filterLabel, '알림 심각도 필터');
    assert.equal(visual.live, 'polite');

    const beforeHover = visual.critLane.color;
    await page.hover('.alert-row-crit');
    const hovered = await page.$eval('.alert-row-crit', row => ({
      color: getComputedStyle(row).borderInlineStartColor,
      shadow: getComputedStyle(row).boxShadow
    }));
    assert.equal(hovered.color, beforeHover);
    assert.equal(hovered.shadow, 'none');
  }, {
    routes: okRoutes(CONFIG, [
      {
        timestamp: 300,
        severity: 'crit',
        metric: 'disk',
        value: 97,
        message: 'critical'
      },
      {
        timestamp: 200,
        severity: 'warn',
        metric: 'cpu',
        value: 82,
        message: 'warning'
      },
      {
        timestamp: 100,
        severity: 'other',
        metric: 'custom',
        value: 1,
        message: 'unknown'
      }
    ])
  });
});

test('alert settings and history honor desktop responsive and touch targets', async () => {
  const history = Array.from({ length: 101 }, (_, index) => ({
    timestamp: 1000 + index,
    severity: index % 2 ? 'crit' : 'warn',
    metric: 'cpu',
    value: 80 + (index % 20),
    message: 'R'.repeat(512) + index
  }));
  await withPage(MODS, async (page) => {
    await page.setViewport({ width: 1440, height: 900 });
    await page.$eval('#cb', node => node.classList.add('cb'));
    await boot(page);

    async function layoutAt(width) {
      await page.setViewport({ width, height: 900 });
      return page.evaluate(() => {
        const columns = selector => {
          const value = getComputedStyle(
            document.querySelector(selector)
          ).gridTemplateColumns;
          return value === 'none' ? 0 : value.trim().split(/\s+/).length;
        };
        const target = selector => {
          const rect = document.querySelector(selector)
            .getBoundingClientRect();
          return { width: rect.width, height: rect.height };
        };
        const historyTable = document.querySelector(
          '#alert-history-region table'
        );
        return {
          width: innerWidth,
          thresholdColumns: columns('.alert-threshold-grid'),
          secondaryColumns: columns('.alert-secondary-grid'),
          chip: target('[data-facet="severity"][data-val="crit"]'),
          refresh: target('[data-alert-refresh]'),
          page: target('[data-alert-page="next"]'),
          documentFits: document.documentElement.scrollWidth <= innerWidth,
          historyHeadWrap: getComputedStyle(
            document.querySelector('.alert-history-head')
          ).flexWrap,
          filterWrap: getComputedStyle(
            document.querySelector('.filterbar')
          ).flexWrap,
          historyOverflowX: getComputedStyle(historyTable).overflowX,
          historyScrolls:
            historyTable.scrollWidth > historyTable.clientWidth
        };
      });
    }

    const wide = await layoutAt(1440);
    assert.equal(wide.thresholdColumns, 3);
    assert.equal(wide.secondaryColumns, 2);
    assert.equal(wide.documentFits, true);
    assert.equal(wide.historyOverflowX, 'auto');
    assert.equal(wide.historyScrolls, true);

    const medium = await layoutAt(1024);
    assert.equal(medium.thresholdColumns, 2);
    assert.equal(medium.secondaryColumns, 2);
    assert.equal(medium.documentFits, true);
    assert.equal(medium.historyOverflowX, 'auto');
    assert.equal(medium.historyScrolls, true);

    for (const width of [768, 601]) {
      const narrow = await layoutAt(width);
      assert.equal(narrow.thresholdColumns, 1);
      assert.equal(narrow.secondaryColumns, 1);
      assert.equal(narrow.documentFits, true);
      assert.equal(narrow.historyHeadWrap, 'wrap');
      assert.equal(narrow.filterWrap, 'wrap');
      assert.equal(narrow.historyOverflowX, 'auto');
      assert.equal(narrow.historyScrolls, true);
      for (const target of [narrow.chip, narrow.refresh, narrow.page]) {
        assert.ok(target.width >= 44);
        assert.ok(target.height >= 44);
      }
    }
  }, { routes: okRoutes(CONFIG, history) });
});

test('malformed refresh preserves cached rows, page, and successful timestamp', async () => {
  let historyGets = 0;
  const history = Array.from({ length: 250 }, (_, index) => ({
    timestamp: 1000 + index,
    severity: index % 2 ? 'crit' : 'warn',
    metric: 'cpu',
    value: 80 + (index % 20),
    message: `cached-${index}`
  }));
  await withPage(MODS, async (page) => {
    await boot(page);
    await page.click('[data-alert-page="next"]');
    const before = await page.evaluate(() => ({
      stamp: document.getElementById('alert-last-updated').textContent,
      range: document.getElementById('alert-result-count').textContent,
      text: document.getElementById('alert-history-region').textContent
    }));
    assert.match(before.range, /101–200/);

    await page.click('[data-alert-refresh]');
    await page.waitForFunction(() => (
      document.getElementById('alert-result-count').textContent
        .includes('1–100 / 250')
    ));
    const successfulStamp = await page.$eval(
      '#alert-last-updated',
      node => node.textContent
    );
    await page.click('[data-alert-page="next"]');
    assert.match(
      await page.$eval('#alert-result-count', node => node.textContent),
      /101–200/
    );

    await page.click('[data-alert-refresh]');
    await page.waitForFunction(() => (
      /새로고침에 실패했습니다/.test(
        document.getElementById('alert-history-message').textContent
      )
    ));
    const after = await page.evaluate(() => ({
      stamp: document.getElementById('alert-last-updated').textContent,
      range: document.getElementById('alert-result-count').textContent,
      text: document.getElementById('alert-history-region').textContent,
      rows: document.querySelectorAll('#alert-history-region tbody tr').length
    }));
    assert.equal(after.stamp, successfulStamp);
    assert.equal(after.range, before.range);
    assert.equal(after.rows, 100);
    assert.match(after.text, /cached-/);
  }, {
    routes: {
      '/api/v1/alerts/config': { status: 200, body: { data: CONFIG } },
      '/api/v1/alerts': () => {
        historyGets++;
        return historyGets <= 2
          ? { status: 200, body: history }
          : { status: 200, body: { data: { items: [] } } };
      }
    }
  });
});

test('oversized alert cache caps, paginates, and filters locally within the paint budget', async () => {
  const history = Array.from({ length: 1100 }, (_, index) => ({
    timestamp: 1000 + index,
    severity: index % 2 ? 'crit' : 'warn',
    metric: `metric-${index}`,
    value: 80 + (index % 20),
    message: `row-${index}`
  }));
  await withPage(MODS, async (page, ctx) => {
    await boot(page);
    const first = await page.evaluate(() => ({
      rows: document.querySelectorAll('#alert-history-region tbody tr').length,
      range: document.getElementById('alert-result-count').textContent,
      prevDisabled: document.querySelector('[data-alert-page="prev"]').disabled,
      nextDisabled: document.querySelector('[data-alert-page="next"]').disabled,
      firstText: document.querySelector('#alert-history-region tbody tr').textContent,
      firstTime: document.querySelector(
        '#alert-history-region tbody tr td'
      ).textContent
    }));
    assert.equal(first.rows, 100);
    assert.match(first.range, /1–100 \/ 1000/);
    assert.equal(first.prevDisabled, true);
    assert.equal(first.nextDisabled, false);
    assert.match(first.firstText, /row-1099/);
    assert.match(first.firstTime, /^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$/);

    await page.$eval('#al-cpu_warn', node => {
      node.value = '77';
      node.dispatchEvent(new Event('input', { bubbles: true }));
      node.focus();
      window.__alertConfigInput = node;
    });
    await page.$eval('[data-alert-page="next"]', node => node.click());
    const second = await page.evaluate(() => ({
      range: document.getElementById('alert-result-count').textContent,
      sameInput: window.__alertConfigInput === document.getElementById('al-cpu_warn'),
      value: document.getElementById('al-cpu_warn').value,
      active: document.activeElement.id,
      rows: document.querySelectorAll('#alert-history-region tbody tr').length
    }));
    assert.match(second.range, /101–200 \/ 1000/);
    assert.equal(second.sameInput, true);
    assert.equal(second.value, '77');
    assert.equal(second.active, 'al-cpu_warn');
    assert.equal(second.rows, 100);

    const performanceResult = await page.evaluate(async () => {
      const durations = [];
      let maxRows = 0;
      for (const severity of ['crit', 'warn', 'crit', 'warn', 'crit']) {
        const started = performance.now();
        PCV.ui.filterState.apply({ severity: [severity] });
                                                        
                                                             
                                                        
                                                         
        await Promise.race([
          new Promise(resolve => {
            requestAnimationFrame(() => requestAnimationFrame(resolve));
          }),
          new Promise(resolve => setTimeout(resolve, 2000)),
        ]);
        durations.push(performance.now() - started);
        maxRows = Math.max(
          maxRows,
          document.querySelectorAll('#alert-history-region tbody tr').length
        );
      }
      durations.sort((a, b) => a - b);
      const crit = Array.from(document.querySelectorAll(
        '#alert-history-region [data-facet="severity"] .chip-c'
      )).map(node => node.textContent);
      return {
        median: durations[2],
        maxRows,
        range: document.getElementById('alert-result-count').textContent,
        counts: crit
      };
    });
    assert.ok(
      performanceResult.median <= 250,
      `filter paint median ${performanceResult.median}ms exceeds 250ms`
    );
    assert.equal(performanceResult.maxRows, 100);
    assert.match(performanceResult.range, /1–100 \/ 500/);
    assert.deepEqual(performanceResult.counts, ['1000', '500', '500']);

    await page.evaluate(() => PCV.ui.filterState.apply({ severity: [] }));
    await page.focus('[data-alert-page="next"]');
    await page.$eval('[data-alert-page="next"]', node => node.click());
    assert.equal(
      await page.evaluate(() => document.activeElement?.dataset.alertPage),
      'next'
    );
    await page.$eval('[data-alert-page="next"]', node => node.click());
    await page.focus('[data-alert-page="prev"]');
    await page.$eval('[data-alert-page="prev"]', node => node.click());
    assert.equal(
      await page.evaluate(() => document.activeElement?.dataset.alertPage),
      'prev'
    );
    for (let index = 0; index < 8; index++) {
      await page.$eval('[data-alert-page="next"]', node => node.click());
    }
    const last = await page.evaluate(() => ({
      range: document.getElementById('alert-result-count').textContent,
      rows: document.querySelectorAll('#alert-history-region tbody tr').length,
      nextDisabled: document.querySelector('[data-alert-page="next"]').disabled
    }));
    assert.match(last.range, /901–1000 \/ 1000/);
    assert.equal(last.rows, 100);
    assert.equal(last.nextDisabled, true);
    assert.equal(ctx.requests.filter(r => r.path === '/api/v1/alerts').length, 1);
  }, { routes: okRoutes(CONFIG, history) });
});

test('alert history empty states explain disabled evaluation and zero filter results', async () => {
  await withPage(MODS, async (page) => {
    await boot(page);
    const disabled = await page.evaluate(() => ({
      text: document.getElementById('alert-history-region').textContent,
      action: Boolean(document.querySelector(
        '#alert-history-region .alert-history-empty button'
      ))
    }));
    assert.match(
      disabled.text,
      /임계값 알림 평가가 꺼져 있습니다. 보안·운영 이벤트는 계속 기록됩니다/
    );
    assert.equal(disabled.action, true);
    await page.click('#alert-history-region .alert-history-empty button');
    assert.equal(
      await page.evaluate(() => document.activeElement.id),
      'alert-config-title'
    );
  }, {
    routes: okRoutes({ ...CONFIG, enabled: false }, [])
  });

  await withPage(MODS, async (page) => {
    await boot(page);
    await page.evaluate(() => {
      PCV.ui.filterState.apply({ severity: ['crit'] });
    });
    assert.match(
      await page.$eval('#alert-history-region', node => node.textContent),
      /Critical 알림이 없습니다/
    );
    await page.click('#alert-history-region .alert-history-empty button');
    const cleared = await page.evaluate(() => ({
      rows: document.querySelectorAll('#alert-history-region tbody tr').length,
      search: location.search,
      selected: PCV.ui.filterState.current().severity || []
    }));
    assert.deepEqual(cleared, { rows: 1, search: '', selected: [] });
  }, {
    routes: okRoutes(CONFIG, [{
      timestamp: 100,
      severity: 'warn',
      metric: 'cpu',
      value: 81,
      message: 'warn only'
    }])
  });
});

test('history refresh blocks duplicate clicks and disables severity chips while pending', async () => {
  let historyGets = 0;
  let releaseRefresh;
  await withPage(MODS, async (page, ctx) => {
    await boot(page);
    await page.click('[data-alert-refresh]');
    await page.waitForFunction(() => (
      document.querySelector('[data-alert-refresh]').disabled
    ));
    const pending = await page.evaluate(() => ({
      refreshBusy: document.querySelector('[data-alert-refresh]')
        .getAttribute('aria-busy'),
      chipDisabled: document.querySelector('[data-facet="severity"]')
        .getAttribute('aria-disabled'),
      chipTabIndex: document.querySelector('[data-facet="severity"]')
        .getAttribute('tabindex')
    }));
    assert.equal(pending.refreshBusy, 'true');
    assert.equal(pending.chipDisabled, 'true');
    assert.equal(pending.chipTabIndex, '-1');
    await page.$eval('[data-alert-refresh]', node => node.click());
    assert.equal(ctx.requests.filter(r => r.path === '/api/v1/alerts').length, 2);

    releaseRefresh();
    await page.waitForFunction(() => (
      !document.querySelector('[data-alert-refresh]').disabled
    ));
  }, {
    routes: {
      '/api/v1/alerts/config': { status: 200, body: { data: CONFIG } },
      '/api/v1/alerts': () => {
        historyGets++;
        if (historyGets === 1) {
          return { status: 200, body: [] };
        }
        return new Promise(resolve => {
          releaseRefresh = () => resolve({ status: 200, body: [] });
        });
      }
    }
  });
});

test('alert filter subscription normalizes invalid state and self-unsubscribes off-route', async () => {
  await withPage(MODS, async (page) => {
    await boot(page);
    await page.evaluate(() => {
      const originalSubscribe = PCV.ui.filterState.subscribe;
      window.__alertFilterSubscriptions = {
        active: 0,
        subscribed: 0,
        callbacks: 0
      };
      PCV.ui.filterState.subscribe = callback => {
        window.__alertFilterSubscriptions.active++;
        window.__alertFilterSubscriptions.subscribed++;
        const wrapped = state => {
          window.__alertFilterSubscriptions.callbacks++;
          callback(state);
        };
        const unsubscribe = originalSubscribe(wrapped);
        return () => {
          window.__alertFilterSubscriptions.active--;
          unsubscribe();
        };
      };
      return PCV.monitor.renderAlerts(document.getElementById('cb'));
    });
    assert.equal(
      await page.evaluate(() => window.__alertFilterSubscriptions.active),
      1
    );

    await page.evaluate(() => {
      PCV.ui.filterState.apply({ severity: ['crit', 'warn'] });
    });
    const normalized = await page.evaluate(() => ({
      state: PCV.ui.filterState.current().severity || [],
      search: location.search,
      active: window.__alertFilterSubscriptions.active
    }));
    assert.deepEqual(normalized.state, []);
    assert.equal(normalized.search, '');
    assert.equal(normalized.active, 1);

    await page.evaluate(() => {
      window.currentTab = 'mon-vms';
      PCV.ui.filterState.apply({ severity: ['crit'] });
    });
    assert.equal(
      await page.evaluate(() => window.__alertFilterSubscriptions.active),
      0
    );

    await page.evaluate(() => {
      window.currentTab = 'mon-alerts';
      return PCV.monitor.renderAlerts(document.getElementById('cb'));
    });
    const reentry = await page.evaluate(() => {
      const beforeCallbacks = window.__alertFilterSubscriptions.callbacks;
      PCV.ui.filterState.apply({ severity: ['warn'] });
      return {
        active: window.__alertFilterSubscriptions.active,
        subscribed: window.__alertFilterSubscriptions.subscribed,
        callbackDelta:
          window.__alertFilterSubscriptions.callbacks - beforeCallbacks
      };
    });
    assert.deepEqual(reentry, {
      active: 1,
      subscribed: 2,
      callbackDelta: 1
    });
  }, { routes: okRoutes(CONFIG, []) });
});
