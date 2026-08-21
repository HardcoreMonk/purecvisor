                                                                                               
                                                                                          
                                                              
                                                                       
import { launch } from 'puppeteer';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

async function readRequestBody(req) {
  const chunks = [];
  for await (const chunk of req) chunks.push(chunk);
  const text = Buffer.concat(chunks).toString('utf8');
  if (!text) return { text: '', json: null };
  try {
    return { text, json: JSON.parse(text) };
  } catch {
    return { text, json: null };
  }
}

async function drainRouteHandlers(activeRouteHandlers) {
  while (activeRouteHandlers.size) {
    await Promise.allSettled([...activeRouteHandlers]);
  }
}

async function closeServer(server) {
  if (!server.listening) return;
  await new Promise((resolve, reject) => {
    server.close(error => error ? reject(error) : resolve());
  });
}

  
                  
                                                  
                                                               
                                                    
                                                                   
                                                 
  
                                                             
                                                                    
                           
                                                                
   
export async function withPage(moduleFiles, fn, options = {}) {
  const routes = options.routes || {};
  const requests = [];
  const routeErrors = [];
  const activeRouteHandlers = new Set();
  const server = http.createServer((req, res) => {
    const p = decodeURIComponent(req.url.split('?')[0]);
    if (Object.prototype.hasOwnProperty.call(routes, p)) {
      const handler = (async () => {
        try {
          const parsed = await readRequestBody(req);
          const record = {
            method: req.method,
            path: p,
            search: req.url.includes('?') ? req.url.slice(req.url.indexOf('?')) : '',
            text: parsed.text,
            json: parsed.json
          };
          requests.push(record);

          const configured = routes[p];
          const reply = typeof configured === 'function'
            ? await configured(record, requests.slice())
            : configured;
          if (!reply || typeof reply !== 'object') {
            throw new Error('route fixture must return a reply');
          }

          res.statusCode = reply.status ?? 200;
          res.setHeader('content-type', 'application/json; charset=utf-8');
          res.end(JSON.stringify(reply.body));
        } catch (error) {
          routeErrors.push(error);
          try {
            res.statusCode = 500;
            res.setHeader('content-type', 'application/json; charset=utf-8');
            res.end(JSON.stringify({
              error: { message: 'test route fixture failed' }
            }));
          } catch (responseError) {
            routeErrors.push(responseError);
          }
        }
      })();
      activeRouteHandlers.add(handler);
      void handler.then(
        () => activeRouteHandlers.delete(handler),
        error => {
          activeRouteHandlers.delete(handler);
          routeErrors.push(error);
        }
      );
      return;
    }

    if (p === '/') {
      const scripts = moduleFiles.map(f => `<script src="/${f}"></script>`).join('\n');
      res.setHeader('content-type', 'text/html; charset=utf-8');
      res.end(`<!doctype html><html><head><meta charset="utf-8">`
        + `<link rel="stylesheet" href="/ui/style.css"></head><body>`
        + `<div id="cb"></div><script>window.PCV={};</script>${scripts}</body></html>`);
      return;
    }
    const fp = path.join(ROOT, p.replace(/^\/+/, ''));
    if (fp.startsWith(ROOT) && fs.existsSync(fp) && fs.statSync(fp).isFile()) {
      const ext = path.extname(fp);
                                                      
                                                                     
      const contentTypes = {
        '.css': 'text/css; charset=utf-8',
        '.html': 'text/html; charset=utf-8',
        '.js': 'application/javascript; charset=utf-8',
        '.mjs': 'application/javascript; charset=utf-8',
        '.svg': 'image/svg+xml; charset=utf-8',
        '.png': 'image/png',
        '.woff2': 'font/woff2',
      };
      res.setHeader('content-type', contentTypes[ext] || 'application/octet-stream');
      res.end(fs.readFileSync(fp));
      return;
    }
    res.statusCode = 404;
    res.end('not found');
  });
                                                                         
                                                                 
                                                          
  let port;
  for (let attempt = 0; ; attempt++) {
    await new Promise(r => server.listen(0, '127.0.0.1', r));
    port = server.address().port;
    if (port > 10080 || attempt >= 20) break;
    await closeServer(server);
  }

  let browser;
  let callbackResult;
  let callbackError;
  let callbackFailed = false;
  let setupError;
  let setupFailed = false;
  const cleanupErrors = [];
  try {
                                                     
      
                                                      
                                                        
                                                        
                                                                        
                                                
                                                     
                                                                
    browser = await launch({
      headless: true,
      args: ['--no-sandbox'],
      protocolTimeout: 30_000,
    });
    const page = await browser.newPage();
    const errors = [];
    page.on('pageerror', e => errors.push(String(e)));
    await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'load' });
    if (errors.length) throw new Error('page load errors: ' + errors.join('; '));
    try {
      callbackResult = await fn(page, { requests, port });
    } catch (error) {
      callbackFailed = true;
      callbackError = error;
    }
  } catch (error) {
    setupFailed = true;
    setupError = error;
  } finally {
    if (browser) {
      try {
        await browser.close();
      } catch (error) {
        cleanupErrors.push(error);
      }
    }
    await drainRouteHandlers(activeRouteHandlers);
    try {
      await closeServer(server);
    } catch (error) {
      cleanupErrors.push(error);
    }
  }

  if (routeErrors.length) {
    const errors = [...routeErrors];
    if (callbackFailed) errors.push(callbackError);
    else if (setupFailed) errors.push(setupError);
    errors.push(...cleanupErrors);
    throw new AggregateError(errors, 'test route fixture failed');
  }
  if (callbackFailed) throw callbackError;
  if (setupFailed) throw setupError;
  if (cleanupErrors.length === 1) throw cleanupErrors[0];
  if (cleanupErrors.length > 1) {
    throw new AggregateError(cleanupErrors, 'test harness cleanup failed');
  }
  return callbackResult;
}

                                                                   
export const CORE = ['ui/modules/ui.js', 'ui/modules/uxlib.js'];
