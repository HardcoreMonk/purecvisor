                                                                                              
                                                                      
                                                                       
  
                                   
  
      
                                                        
               
  
                  
                                                                  
                                                    
                         
  
                                                   
   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

test('withPage serves local SVG sprites with image MIME for visual tests', async () => {
  await withPage([], async (page) => {
    const result = await page.evaluate(async () => {
      const response = await fetch('/ui/vendor/coolicons/coolicons.svg');
      return {
        status: response.status,
        contentType: response.headers.get('content-type'),
        text: (await response.text()).slice(0, 80),
      };
    });
    assert.equal(result.status, 200);
    assert.match(result.contentType, /^image\/svg\+xml/);
    assert.match(result.text, /<svg/);
  });
});

test('withPage serves sequential JSON routes and records request bodies', async () => {
  let calls = 0;
  await withPage([], async (page, ctx) => {
    const first = await page.evaluate(() => fetch('/api/v1/alerts').then(r => r.json()));
    const second = await page.evaluate(() => fetch('/api/v1/alerts').then(r => r.json()));
    await page.evaluate(() => fetch('/api/v1/alerts/config', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ enabled: true })
    }).then(r => r.json()));

    assert.deepEqual(first, [{ severity: 'warn' }]);
    assert.deepEqual(second, [{ severity: 'crit' }]);
    assert.equal(ctx.requests.length, 3);
    assert.equal(ctx.requests[2].method, 'PUT');
    assert.deepEqual(ctx.requests[2].json, { enabled: true });
  }, {
    routes: {
      '/api/v1/alerts': () => ({
        status: 200,
        body: [{ severity: calls++ === 0 ? 'warn' : 'crit' }]
      }),
      '/api/v1/alerts/config': {
        status: 200,
        body: { data: { enabled: true } }
      }
    }
  });
});

test('withPage returns 500 and aggregates an async route error after the callback', async () => {
  const routeError = new Error('route fixture exploded');
  let callbackCompleted = false;

  await assert.rejects(
    withPage([], async (page) => {
      const result = await page.evaluate(async () => {
        const response = await fetch('/api/v1/failing-route', {
          signal: AbortSignal.timeout(2000)
        });
        return { status: response.status, body: await response.json() };
      });

      assert.equal(result.status, 500);
      assert.deepEqual(result.body, {
        error: { message: 'test route fixture failed' }
      });
      callbackCompleted = true;
    }, {
      routes: {
        '/api/v1/failing-route': async () => {
          throw routeError;
        }
      }
    }),
    error => {
      assert.ok(error instanceof AggregateError);
      assert.equal(error.message, 'test route fixture failed');
      assert.equal(error.errors.length, 1);
      assert.strictEqual(error.errors[0], routeError);
      return true;
    }
  );
  assert.equal(callbackCompleted, true);
});

test('withPage returns 500 and aggregates a missing async route reply after the callback', async () => {
  let callbackCompleted = false;

  await assert.rejects(
    withPage([], async (page) => {
      const result = await page.evaluate(async () => {
        const response = await fetch('/api/v1/missing-reply', {
          signal: AbortSignal.timeout(2000)
        });
        return { status: response.status, body: await response.json() };
      });

      assert.equal(result.status, 500);
      assert.deepEqual(result.body, {
        error: { message: 'test route fixture failed' }
      });
      callbackCompleted = true;
    }, {
      routes: {
        '/api/v1/missing-reply': async () => {}
      }
    }),
    error => {
      assert.ok(error instanceof AggregateError);
      assert.equal(error.message, 'test route fixture failed');
      assert.equal(error.errors.length, 1);
      assert.match(error.errors[0].message, /route fixture must return a reply/);
      return true;
    }
  );
  assert.equal(callbackCompleted, true);
});

test('withPage drains a delayed fire-and-forget route failure before resolving', async () => {
  const routeError = new Error('delayed route fixture exploded');
  let markFixtureStarted;
  const fixtureStarted = new Promise(resolve => {
    markFixtureStarted = resolve;
  });
  let markBrowserClosed;
  const browserClosed = new Promise(resolve => {
    markBrowserClosed = resolve;
  });
  let callbackCompleted = false;

  await assert.rejects(
    withPage([], async (page) => {
      const browser = page.browser();
      const closeBrowser = browser.close.bind(browser);
      browser.close = async () => {
        try {
          return await closeBrowser();
        } finally {
          markBrowserClosed();
        }
      };
      await page.evaluate(() => {
        const controller = new AbortController();
        window.delayedRouteController = controller;
        void fetch('/api/v1/delayed-failure', {
          signal: controller.signal
        }).catch(() => {});
      });
      await fixtureStarted;
      await page.evaluate(() => window.delayedRouteController.abort());
      await new Promise(resolve => setTimeout(resolve, 25));
      callbackCompleted = true;
    }, {
      routes: {
        '/api/v1/delayed-failure': async () => {
          markFixtureStarted();
          await browserClosed;
          await new Promise(resolve => setTimeout(resolve, 25));
          throw routeError;
        }
      }
    }),
    error => {
      assert.ok(error instanceof AggregateError);
      assert.equal(error.message, 'test route fixture failed');
      assert.equal(error.errors.length, 1);
      assert.strictEqual(error.errors[0], routeError);
      return true;
    }
  );
  assert.equal(callbackCompleted, true);
});

test('withPage aggregates route and callback errors without discarding either', async () => {
  const routeError = new Error('route fixture exploded before callback');
  const callbackError = new Error('page callback exploded');

  await assert.rejects(
    withPage([], async (page) => {
      const status = await page.evaluate(async () => {
        const response = await fetch('/api/v1/double-failure', {
          signal: AbortSignal.timeout(2000)
        });
        return response.status;
      });
      assert.equal(status, 500);
      throw callbackError;
    }, {
      routes: {
        '/api/v1/double-failure': async () => {
          throw routeError;
        }
      }
    }),
    error => {
      assert.ok(error instanceof AggregateError);
      assert.equal(error.message, 'test route fixture failed');
      assert.equal(error.errors.length, 2);
      assert.strictEqual(error.errors[0], routeError);
      assert.strictEqual(error.errors[1], callbackError);
      return true;
    }
  );
});
