                                                                                          
                                                                                  
                                                                 
  
                             
  
                                                              
                                                                            
  
      
                                                                                   
                                                      
                                                     
                                                                 
  
                                                              
                                                  
                         
  
                                                                   
                                                                   
                                                     
                                                                 
                                                                                 
                                                       
                                                                   
                                                            
                                                           
                                                 
                                                               
                                                                                 
                                                           
                                              
                                                                 
                                                          
  
                                             
   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage, CORE } from './harness.mjs';

const MODS_HN = CORE;
const MODS_VM = [
  'ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/endpoints.js',
  'ui/modules/filter-state.js', 'ui/modules/vm.js'
];

test('HN.pagehead: title+desc+actions 전체 경로 구조', async () => {
  await withPage(MODS_HN, async page => {
    const shape = await page.evaluate(() => {
      const el = window.PCV.uxlib.el;
      const node = window.HN.pagehead({
        title: '가상 머신',
        desc: '노드의 VM 자산과 전원 상태를 관리합니다.',
        actions: [
          el('button', { class: 'btn btn-primary' }, '+ 새 VM'),
          el('button', { class: 'btn' }, '스냅샷')
        ]
      });
      document.getElementById('cb').appendChild(node);
      const root = document.querySelector('#cb > .pagehead');
      const copy = root.querySelector(':scope > .pagehead-copy');
      const title = copy.querySelector(':scope > h2.pagehead-title');
      const desc = copy.querySelector(':scope > p.pagehead-desc');
      const actions = root.querySelector(':scope > .pagehead-actions');
      return {
        tag: root.tagName,
        childOrder: Array.from(root.children).map(c => c.className),
        titleTag: title.tagName,
        titleText: title.textContent,
        descText: desc.textContent,
        actionLabels: Array.from(actions.querySelectorAll('button')).map(btn => btn.textContent),
        h1Count: document.querySelectorAll('#cb h1').length
      };
    });
    assert.equal(shape.tag, 'DIV');
    assert.deepEqual(shape.childOrder, ['pagehead-copy', 'pagehead-actions']);
    assert.equal(shape.titleTag, 'H2');
    assert.equal(shape.titleText, '가상 머신');
    assert.equal(shape.descText, '노드의 VM 자산과 전원 상태를 관리합니다.');
    assert.deepEqual(shape.actionLabels, ['+ 새 VM', '스냅샷']);
    assert.equal(shape.h1Count, 0, 'pagehead는 h1을 만들지 않는다');
  });
});

test('HN.pagehead: desc/actions 생략 시 해당 노드 자체가 없다', async () => {
  await withPage(MODS_HN, async page => {
    const shape = await page.evaluate(() => {
      const node = window.HN.pagehead({ title: '컨테이너' });
      document.getElementById('cb').appendChild(node);
      const root = document.querySelector('#cb > .pagehead');
      return {
        childOrder: Array.from(root.children).map(c => c.className),
        hasDesc: root.querySelector('.pagehead-desc') !== null,
        hasActions: root.querySelector('.pagehead-actions') !== null,
        titleText: root.querySelector('h2.pagehead-title').textContent
      };
    });
    assert.deepEqual(shape.childOrder, ['pagehead-copy']);
    assert.equal(shape.hasDesc, false);
    assert.equal(shape.hasActions, false);
    assert.equal(shape.titleText, '컨테이너');
  });
});

                                                                  
const MANY_VMS = Array.from({ length: 42 }, (_, i) => ({
  name: 'vm-' + String(i).padStart(2, '0'),
  state: i % 5 === 0 ? 'shutoff' : 'running',
  running: i % 5 === 0 ? 0 : 1,
  vcpu: 2 + (i % 4),
  mem: 1024 * (1 + (i % 8)),
}));

test('renderVmScreen: 실셸 체인(.app→.shell-main→.content.shell-content→.cb)에서도 뷰포트 오버플로가 없다 (M1 flex 컬럼 + S-F1 하니스 개선)', async () => {
  await withPage(MODS_VM, async page => {
    await page.setViewport({ width: 1280, height: 800 });
                                                                    
                                                                            
                                                                     
    await page.evaluate(async () => {
      const html = await fetch('/ui/index.html').then(r => r.text());
      const parsed = new DOMParser().parseFromString(html, 'text/html');
      document.body.innerHTML = parsed.body.innerHTML;
    });
    await page.evaluate(vms => {
      window._DEBUG = true;
      window._L = (ko) => ko;
      window.t = key => key;
      window.currentTab = 'vm';
                                                       
                                                     
      window.vmList = vms;
      window.selectedVmIndex = 0;
      window.sortField = 'name';
      window.sortDirection = 1;
      window.checkedVms = new Set();
      window.navigateTo = () => {};
      window.renderSummary = () => {};
      window.showCreate = () => {};
      window.showSnap = () => {};
    }, MANY_VMS);
    await page.evaluate(() => window.renderVmScreen(document.getElementById('cb'), window.vmList[0], 'summary'));

    const metrics = await page.evaluate(() => {
      const cb = document.getElementById('cb');
      const shellMain = document.querySelector('.shell-main');
      return {
        pageheadTitle: cb.querySelector('.pagehead .pagehead-title')?.textContent,
        pageheadBeforeScreen: cb.querySelector('.pagehead')?.compareDocumentPosition(cb.querySelector('.vm-screen')) === Node.DOCUMENT_POSITION_FOLLOWING,
        cbScrollHeight: cb.scrollHeight,
        cbClientHeight: cb.clientHeight,
        shellMainClientHeight: shellMain.clientHeight,
        innerHeight: window.innerHeight,
      };
    });
    assert.equal(metrics.pageheadTitle, '가상 머신');
    assert.equal(metrics.pageheadBeforeScreen, true, 'pagehead 는 2열 래퍼 위에 온다');
                                                                     
    assert.ok(
      metrics.cbScrollHeight <= metrics.cbClientHeight + 1,
      `#cb 내부 무스크롤: scrollHeight=${metrics.cbScrollHeight} clientHeight=${metrics.cbClientHeight}`
    );
                                                         
                                                                    
    assert.ok(
      metrics.shellMainClientHeight <= metrics.innerHeight,
      `.shell-main 뷰포트 이내: clientHeight=${metrics.shellMainClientHeight} innerHeight=${metrics.innerHeight}`
    );
  }, { routes: {} });
});
