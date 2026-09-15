

import {test} from 'node:test';
import assert from 'node:assert/strict';
import {withPage} from './harness.mjs';
const MODS=['endpoints','api','ui','filter-state','uxlib','modal-core','vm','vm-lifecycle','mobile'].map(n=>'ui/modules/'+n+'.js');
const VMS=[{name:'vm-alpha',uuid:'uuid-alpha',state:'shutoff'}, {name:'vm-beta',uuid:'uuid-beta',state:'running'}];
async function boot(page) {
  await page.evaluate(vms=>{
    Object.assign(window,{_L:(ko,en)=>en,t:k=>k,authToken:'fixture',_DEBUG:false,
      currentUser:{role:'ADMIN'},currentTab:'summary',vmList:vms,selectedVmIndex:0,
      checkedVms:new Set(),sortField:'name',sortDirection:1,navigateTo:()=>{},
      renderSummary:()=>{},renderContent:()=>{},loadAll:()=>{},toast:()=>{},_events:[],_requests:[]});
    window.addEvt=m=>_events.push(m);
    window._serverVms=structuredClone(vms);
    window._reply=(body,status=200)=>new Response(JSON.stringify(body),{status,headers:{'content-type':'application/json'}});
    window._handler=(url,opts)=>_reply({data:opts.method==='DELETE'?'accepted':_serverVms});
    window.fetch=async(url,opts={})=>{_requests.push({url:String(url),method:opts.method||'GET'});return _handler(String(url),opts);};
    window._paint=()=>{document.getElementById('cb').replaceChildren();renderVmScreen(document.getElementById('cb'),vmList[0],'summary');applyRoleVisibility(currentUser.role);};
    _paint();
  },VMS);
}
function scenario(name,fn){test(name,async()=>withPage(MODS,async page=>{await boot(page);await fn(page);}));}
async function confirm(page,phrase){await page.type('#vm-delete-confirm',phrase);await page.click('#vm-delete-confirm-go');await page.waitForFunction(()=>[...document.querySelectorAll('.vm-delete-result')].every(n=>!/Waiting|Checking|Sending/.test(n.textContent)));}

scenario('삭제 진입점은 ADMIN/OPERATOR에 보이고 VIEWER에는 숨으며 0건은 비활성이다',async page=>{
  const result=await page.evaluate(()=>{
    const out=[];
    for(const role of ['ADMIN','OPERATOR','VIEWER']){
      currentUser.role=role;_paint();
      const single=document.querySelector('#vm-delete'),bulk=document.querySelector('#vm-bulk-delete');
      out.push({role,single:!!single.getClientRects().length,bulk:!!bulk.getClientRects().length,disabled:bulk.disabled});
    }return out;
  });
  assert.deepEqual(result,[{role:'ADMIN',single:true,bulk:true,disabled:true},{role:'OPERATOR',single:true,bulk:true,disabled:true},{role:'VIEWER',single:false,bulk:false,disabled:true}]);
});

scenario('목록 재배열에도 선택 UUID가 유지되고 동명 교체는 선택에서 제거된다',async page=>{
  const result=await page.evaluate(()=>{
    toggleChk(0);vmList=[vmList[1],vmList[0]];render(true);
    const stable={indices:[...checkedVms],targets:PCV.vm.getCheckedVmSubjects()};
    vmList[1]={...vmList[1],uuid:'replacement'};render(true);
    return {stable,after:PCV.vm.getCheckedVmSubjects(),disabled:document.querySelector('#vm-bulk-delete').disabled};
  });
  assert.deepEqual(result.stable,{indices:[1],targets:[{name:'vm-alpha',uuid:'uuid-alpha'}]});
  assert.deepEqual(result.after,[]);assert.equal(result.disabled,true);
});

scenario('카드 보기 체크박스도 일괄 삭제 대상을 선택한다',async page=>{
  await page.evaluate(()=>toggleVmView());
  assert.equal(await page.$$eval('#vl input[type=checkbox]',nodes=>nodes.length),2);

  await page.evaluate(()=>{vmList=structuredClone(vmList);render(true);});
  await page.click('#vl input[type=checkbox]');
  assert.equal(await page.$eval('#vm-bulk-delete',b=>b.disabled),false);
  assert.equal(await page.evaluate(()=>PCV.vm.getCheckedVmSubjects()[0].name),'vm-alpha');
});

scenario('단일 삭제 확인은 취소에 포커스를 두고 이름 일치 전 차단하며 취소는 요청 0건이다',async page=>{
  await page.click('#vm-delete');
  assert.equal(await page.evaluate(()=>document.activeElement.textContent),'Cancel');
  await page.type('#vm-delete-confirm','wrong');
  assert.equal(await page.$eval('#vm-delete-confirm-go',b=>b.disabled),true);
  await page.keyboard.press('Escape');
  assert.equal(await page.evaluate(()=>_requests.length),0);
});

scenario('모바일은 자체 목록 identity로 선택을 유지하고 같은 삭제 확인으로 연결한다',async page=>{
  await page.evaluate(()=>{
    const host=document.createElement('div');host.id='mobile-fixture';document.body.appendChild(host);
    window._paintMobile=()=>host.replaceChildren(PCV.mobile.buildPower({vms:_serverVms,containers:[]}));_paintMobile();
  });
  await page.click('#mobile-fixture input[type=checkbox]');
  await page.evaluate(()=>{_serverVms.reverse();_paintMobile();});
  assert.equal(await page.$eval('#mobile-fixture input[aria-label="Select vm-alpha"]',b=>b.checked),true);
  await page.click('#mobile-fixture .m-vm-bulk-delete');
  assert.deepEqual(await page.$$eval('dialog li',nodes=>nodes.map(n=>n.textContent)),['vm-alpha']);
  await page.keyboard.press('Escape');
  await page.evaluate(()=>{_serverVms.find(v=>v.name==='vm-alpha').uuid='replacement';_paintMobile();});
  assert.equal(await page.$eval('#mobile-fixture .m-vm-bulk-delete',b=>b.disabled),true);
  await page.click('#mobile-fixture .m-vm-delete');
  assert.match(await page.$eval('dialog',d=>d.textContent),/vm-beta/);
  await page.keyboard.press('Escape');
  assert.equal(await page.evaluate(()=>_requests.length),0);
});

scenario('일괄 확인에 고정한 이름·UUID를 전송하며 중복 클릭과 접수 완료 오표시를 막는다',async page=>{
  await page.evaluate(()=>{toggleChk(0);toggleChk(1);PCV.vm.bulkDelete();vmList.reverse();selectedVmIndex=1;});
  assert.match(await page.$eval('dialog',d=>d.textContent),/vm-alpha[\s\S]*vm-beta/);
  await page.type('#vm-delete-confirm','DELETE 2');
  await page.evaluate(()=>{const b=document.querySelector('#vm-delete-confirm-go');b.click();b.click();});
  await page.waitForFunction(()=>document.querySelectorAll('.vm-delete-result').length===2&&[...document.querySelectorAll('.vm-delete-result')].every(n=>n.textContent.includes('accepted')));
  const result=await page.evaluate(()=>({deletes:_requests.filter(r=>r.method==='DELETE').map(r=>r.url),text:document.querySelector('dialog').textContent,events:_events}));
  assert.deepEqual(result.deletes,['/api/v1/vms/vm-alpha','/api/v1/vms/vm-beta']);
  assert.deepEqual(result.events,[]);assert.doesNotMatch(result.text,/✅/);assert.match(result.text,/unconfirmed/);
});

for(const kind of ['missing','replaced','unreadable']) scenario('전송 직전 서버 대상 검증 차단: '+kind,async page=>{
  await page.evaluate(kind=>{
    PCV.vm.vmDel(vmList[0]);
    if(kind==='missing')_serverVms=[];
    if(kind==='replaced')_serverVms[0].uuid='replacement';
    if(kind==='unreadable')_handler=()=>_reply({error:{message:'CONTROLLED_READ_FAILURE'}},503);
  },kind);
  await confirm(page,'vm-alpha');
  assert.equal(await page.evaluate(()=>_requests.filter(r=>r.method==='DELETE').length),0);
  assert.match(await page.$eval('.vm-delete-result',n=>n.textContent),/failed/i);
});

scenario('일괄 부분 실패는 개별 오류를 남기고 다른 대상을 계속 처리한다',async page=>{
  await page.evaluate(()=>{
    _handler=(url,opts)=>opts.method==='DELETE'&&url.endsWith('vm-alpha')?_reply({error:{message:'CONTROLLED_DENIED'}},403):_reply({data:opts.method==='DELETE'?'accepted':_serverVms});
    toggleChk(0);toggleChk(1);PCV.vm.bulkDelete();
  });
  await confirm(page,'DELETE 2');
  const rows=await page.$$eval('.vm-delete-result',nodes=>nodes.map(n=>({text:n.textContent,role:n.getAttribute('role')})));
  assert.match(rows[0].text,/CONTROLLED_DENIED/);assert.equal(rows[0].role,'alert');assert.match(rows[1].text,/accepted/);
  assert.equal(await page.evaluate(()=>_requests.filter(r=>r.method==='DELETE').length),2);
});

scenario('전송 오류는 자동 재시도하지 않고 결과 미확인으로 유지한다',async page=>{
  await page.evaluate(()=>{_handler=(url,opts)=>opts.method==='DELETE'?Promise.reject(new TypeError('CONTROLLED_NETWORK')):_reply({data:_serverVms});PCV.vm.vmDel(vmList[0]);});
  await confirm(page,'vm-alpha');
  assert.match(await page.$eval('.vm-delete-result',n=>n.textContent),/Acceptance unconfirmed.*CONTROLLED_NETWORK/);
  assert.equal(await page.evaluate(()=>_requests.filter(r=>r.method==='DELETE').length),1);
});

scenario('결과 창 닫힘은 조회 중인 다음 DELETE 전송을 중단한다',async page=>{
  await page.evaluate(()=>{_handler=()=>new Promise(resolve=>{window._release=()=>resolve(_reply({data:_serverVms}));});PCV.vm.vmDel(vmList[0]);});
  await page.type('#vm-delete-confirm','vm-alpha');await page.click('#vm-delete-confirm-go');
  await page.waitForFunction(()=>typeof _release==='function');
  await page.keyboard.press('Escape');await page.evaluate(()=>_release());
  await page.waitForFunction(()=>!document.querySelector('dialog'));
  assert.equal(await page.evaluate(()=>_requests.filter(r=>r.method==='DELETE').length),0);
});

for(const state of ['completed','pending','failed']) scenario('Job 응답은 실제 상태로 구분한다: '+state,async page=>{
  await page.evaluate(state=>{
    PCV.api.waitForJob=async()=>{if(state==='failed')throw new Error('CONTROLLED_JOB_FAILURE');return {status:state};};
    _handler=(url,opts)=>_reply({data:opts.method==='DELETE'?{job_id:'fixture'}:_serverVms});PCV.vm.vmDel(vmList[0]);
  },state);
  await confirm(page,'vm-alpha');
  const result=await page.evaluate(()=>({text:document.querySelector('.vm-delete-result').textContent,events:_events.length}));
  assert.equal(result.events,state==='completed'?1:0);
  assert.match(result.text,state==='completed'?/✅ Deleted/:state==='pending'?/unconfirmed/:/CONTROLLED_JOB_FAILURE/);
});
