                                                                                    
                                                                           
                                                                
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage, CORE } from './harness.mjs';

const APP_SOURCE = readFileSync(new URL('../../ui/app.js', import.meta.url), 'utf8');
const MODS = [...CORE, 'ui/modules/modal-core.js', 'ui/modules/help.js'];

test('? shortcut has one registry owner and no legacy modal owner', () => {
  assert.equal((APP_SOURCE.match(/registerShortcut\('\?'/g) || []).length, 1);
  assert.equal(APP_SOURCE.includes('showShortcutsHelp'), false);
});

test('? opens exactly one help dialog and is ignored by editable controls', async () => {
  await withPage(MODS, async (page) => {
    const result = await page.evaluate(async () => {
      registerShortcut('?', () => toggleKbdHelp(), '단축키 도움말');
      document.body.dispatchEvent(new KeyboardEvent('keydown', {
        key: '?', shiftKey: true, bubbles: true
      }));
      await Promise.resolve();
      const firstCount = document.querySelectorAll('dialog.kbd-help[open]').length;
      closeKbdHelp();

      const input = document.createElement('input');
      document.body.appendChild(input);
      input.focus();
      input.dispatchEvent(new KeyboardEvent('keydown', {
        key: '?', shiftKey: true, bubbles: true
      }));
      await Promise.resolve();
      const inputCount = document.querySelectorAll('dialog.kbd-help[open]').length;

      const select = document.createElement('select');
      document.body.appendChild(select);
      select.focus();
      select.dispatchEvent(new KeyboardEvent('keydown', {
        key: '?', shiftKey: true, bubbles: true
      }));
      await Promise.resolve();
      return {
        firstCount,
        inputCount,
        selectCount: document.querySelectorAll('dialog.kbd-help[open]').length
      };
    });
    assert.deepEqual(result, { firstCount: 1, inputCount: 0, selectCount: 0 });
  });
});
