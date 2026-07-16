/* ═══════════════════════════════════════════════════════════════
   PureCVisor — modules/vm.js
   VM List, Summary, Console, Snapshots, Performance, Power,
   Create Wizard, Settings, NIC Manager, Clone, Export
   ADR-0013: IIFE 모듈 스코프 전환 — window.PCV.vm 네임스페이스
   ═══════════════════════════════════════════════════════════════ */

/*
 * ===== vm.js 모듈 개요 (주니어 개발자 필독) =====
 *
 * [역할]
 *   VM 관련 모든 UI 로직. 사이드바 VM 목록 렌더링, 상세 정보(summary),
 *   VNC 콘솔, 스냅샷 관리, 성능 차트, 전원 제어, 생성 위자드, NIC 관리 등.
 *   이 파일이 가장 크다 (~1500 LOC). 기능별로 섹션 구분자(═══)를 따라가면 된다.
 *
 * [PCV 네임스페이스 (ADR-0013)]
 *   IIFE 안에서 정의 후 PCV.vm = { ... }로 공개 API를 노출.
 *   window.render, window.vmPower 등 하위 호환 심은 전환기 코드.
 *   HTML onclick에서 직접 호출하는 함수는 window에 등록이 필수이다.
 *
 * [주요 함수]
 *   - render(skipContent): 사이드바 VM 목록 렌더링. skipContent=true면
 *     _lastVmListHash와 비교하여 변경 없으면 건너뛴다 (깜박임 방지).
 *   - renderSummary(b, v): VM 상세 정보 카드 (CPU/MEM/Disk/Net + 액션 버튼).
 *   - renderConsole(b, v): VNC 콘솔 연결 UI.
 *   - renderSnapshots(b, v): 스냅샷 목록 + 생성/롤백/삭제.
 *   - renderPerformance(b, v): 60초 히스토리 차트 (CPU/MEM).
 *   - vmPower(action): 전원 제어 (start/stop/suspend/resume) + 상태 폴링.
 *   - showCreate(): 3단계 VM 생성 위자드.
 *
 * [VM 목록 렌더링 성능]
 *   _lastVmListHash는 "name+state+cpu" 를 | 로 연결한 문자열이다.
 *   해시가 같으면 DOM 업데이트를 건너뛴다. 이렇게 하는 이유:
 *   10초 폴링마다 innerHTML을 교체하면 스크롤 위치가 초기화되고,
 *   사용자가 클릭 중인 요소가 사라져 UX가 나빠진다.
 *   WS 이벤트로 VM 상태가 바뀌면 해시가 달라져서 자동 갱신된다.
 *
 * [스파크라인 캔버스 관리]
 *   각 VM 사이드바 항목에 <canvas id="spark-{name}"> 이 있다.
 *   render()가 innerHTML을 교체할 때 기존 캔버스가 파괴되고 새로 생성된다.
 *   setTimeout(50ms) 후에 getContext('2d')로 그리는 이유:
 *   innerHTML 할당 직후에는 브라우저가 아직 레이아웃을 계산하지 않아
 *   캔버스 크기가 0일 수 있다.
 *   _vmCpuHist[name]에 30개의 CPU% 이력을 유지하여 미니 차트를 그린다.
 *
 * [VNC 콘솔 연결 흐름]
 *   1. fetchGet(EP.VNC(name))으로 VNC 포트 번호를 조회.
 *   2. noVNC 라이브러리를 로컬 vendor ESM에서 동적 로딩.
 *   3. WebSocket URL: ws(s)://host/api/v1/ws/vnc?port=XXXX
 *      (백엔드 ws_server.c가 WS↔VNC TCP를 프록시).
 *   4. RFB 객체가 connect/disconnect/securityfailure 이벤트를 발생.
 *   5. 팝업 창에서도 동일 흐름 (openNoVNCPopup).
 *
 * [스냅샷 트리 렌더링]
 *   백엔드는 "pool/vm@snapname\tdate" 문자열 배열을 반환한다.
 *   파싱하여 {name, full_path, time} 객체로 변환 후 테이블로 표시.
 *   롤백은 파괴적 작업이므로 VM 이름 타이핑 확인(destroyConfirm 패턴)을 적용.
 *   일괄 삭제는 prefix 필터 + keep_recent 옵션으로 미리보기 후 실행.
 *
 * [흔한 실수]
 *   - vmList와 selectedVmIndex는 app.js의 var 전역이다.
 *     이 모듈 안에서는 window.vmList로 참조되지만, var 선언이 같은 스코프가
 *     아니므로 IIFE 안에서 vmList를 직접 쓰면 클로저 밖의 전역을 참조한다.
 *   - render() 안에서 vmList를 변경하지 마라. loadAll()만 vmList를 갱신해야 한다.
 *   - onclick 문자열 안에서 VM 이름을 넣을 때 escapeAttr()를 사용하라.
 *     VM 이름에 - 이외의 특수문자는 없지만, 방어적 코딩 습관을 위해.
 *   - showCreate() 호출 시 wizData를 항상 초기화한다. 이전 값이 남으면 혼란.
 */

window.PCV = window.PCV || {};
(function(PCV) {

/* ═══ SORT / FILTER / RENDER ═══ */
/* _lastVmListHash: "name+state+cpu|name+state+cpu|..." 형태의 문자열.
 *   render(skipContent=true) 시 이전 해시와 비교하여 변경이 없으면
 *   DOM 업데이트를 건너뛴다. 폴링으로 인한 불필요한 리렌더링 방지.
 * vmViewMode: 'list' 또는 'card'. localStorage에 영속 저장된다.
 *   카드 뷰에서는 드래그&드롭 마이그레이션 영역도 표시된다. */
var _lastVmListHash = '';
var vmViewMode = localStorage.getItem('pcv-vm-view') || 'list';

function toggleVmView() {
  vmViewMode = vmViewMode === 'list' ? 'card' : 'list';
  localStorage.setItem('pcv-vm-view', vmViewMode);
  render();
}
function setSort(k) {
  if (sortField === k) sortDirection *= -1;
  else { sortField = k; sortDirection = 1; }
  render();
}

function getFiltered() {
  const f = (document.getElementById('vf') || {}).value || '';
  let l = [...vmList];
  if (f) l = l.filter(v => typeof window.fuzzyMatch === 'function' ? window.fuzzyMatch(v.name, f) : v.name.toLowerCase().includes(f.toLowerCase()));
  l.sort((a, b) => {
    let va, vb;
    if (sortField === 'cpu') { va = a.live_cpu_pct || 0; vb = b.live_cpu_pct || 0; }
    else if (sortField === 'mem') { va = a.mem_percent || 0; vb = b.mem_percent || 0; }
    else if (sortField === 'state') { va = a.state; vb = b.state; }
    else { va = a.name; vb = b.name; }
    return va < vb ? -sortDirection : va > vb ? sortDirection : 0;
  });
  /* G-4: favorites sort to top */
  const favs = getFavorites();
  l.sort((a, b) => {
    const af = favs.includes(a.name) ? 0 : 1;
    const bf = favs.includes(b.name) ? 0 : 1;
    return af - bf;
  });
  return l;
}

var _renderInFlight = false;
function render(skipContent) {
  if (_renderInFlight) return;
  _renderInFlight = true;
  try { _renderCore(skipContent); } finally { _renderInFlight = false; }
}
function _renderCore(skipContent) {
  var newHash = vmList.map(function(v){return v.name+v.state+(v.live_cpu_pct||0);}).join('|');
  if (skipContent && newHash === _lastVmListHash) return;
  _lastVmListHash = newHash;
  const l = getFiltered();
  const favs = getFavorites();
  /* D2: Update sparkline history */
  vmList.forEach(function(v) {
    if (!_vmCpuHist[v.name]) _vmCpuHist[v.name] = [];
    _vmCpuHist[v.name].push(v.live_cpu_pct || 0);
    if (_vmCpuHist[v.name].length > 30) _vmCpuHist[v.name].shift();
  });
  /* #16 빈 상태 — VM이 0개일 때 CTA 표시 */
  if (l.length === 0 && typeof emptyStatePro === 'function') {
    var _vl = document.getElementById('vl');
    PCV.uxlib.clearEl(_vl);
    _vl.appendChild(emptyStatePro({
      icon: '&#128187;',
      title: _L('VM이 없습니다', 'No virtual machines'),
      desc: _L('첫 VM을 만들어 시작하세요. 몇 초 안에 부팅 가능합니다.', 'Create your first VM. Boots in seconds.'),
      ctaLabel: _L('+ VM 만들기', '+ Create VM'),
      ctaAction: 'showCreate()'
    }));
    return;
  }
  var el = PCV.uxlib.el;
  var parts = [];
  parts.push(el('div', { class: 'flex gap-4 mb-8 justify-end' },
    el('button', { class: 'btn ' + (vmViewMode === 'list' ? 'btn-g' : '') + ' btn-xs', onclick: 'toggleVmView()' }, '☰ ' + _L('목록', 'List')),
    el('button', { class: 'btn ' + (vmViewMode === 'card' ? 'btn-g' : '') + ' btn-xs', onclick: 'toggleVmView()' }, '▦ ' + _L('카드', 'Card')),
    el('button', { class: 'btn', onclick: 'showVmCompare()' }, _L('비교', 'Compare')),
    el('button', { class: 'btn', onclick: 'showBulkActions()', 'data-role': 'OPERATOR,ADMIN' }, _L('일괄 작업', 'Bulk'))));
  if (vmViewMode === 'card') {
    var cardGrid = el('div', { class: 'sg grid-3' });
    l.forEach(function(v, ri) {
      var on = v.state === 'running';
      var cp = v.live_cpu_pct || 0;
      var mp = v.mem_percent || 0;
      cardGrid.appendChild(el('div', { class: 'hc', draggable: 'true', ondragstart: "event.dataTransfer.setData('text/plain','" + v.name + "')", style: 'cursor:grab;border-left:3px solid ' + (on ? 'var(--green)' : 'var(--red)'), onclick: 'selectedVmIndex=' + vmList.indexOf(v) + ";currentTab='summary';switchSbTab('vms');render()" },
        el('div', { class: 'flex items-center gap-6 mb-6' },
          el('span', { style: 'font-size:8px;color:' + (on ? 'var(--green)' : 'var(--red)') }, '●'),
          el('b', null, v.name)),
        el('div', { class: 'flex gap-8 text-11' },
          el('div', { class: 'flex-1' }, el('div', { class: 'color-muted' }, 'CPU'), _vmProgressBar(cp)),
          el('div', { class: 'flex-1' }, el('div', { class: 'color-muted' }, 'MEM'), _vmProgressBar(mp))),
        el('div', { class: 'flex gap-8 mt-6 text-xs color-muted' },
          el('span', null, (v.vcpu || '?') + ' vCPU'),
          el('span', null, (v.memory_mb || '?') + ' MB'),
          el('span', null, HN.badge(v.state || '?', on ? 'g' : 'r')))));
    });
    parts.push(cardGrid);
    /* D3: Migration drop zone for cluster nodes */
    parts.push(el('h3', { style: 'margin:16px 0 8px' }, _L('마이그레이션 대상 노드', 'Migration Target Nodes')));
    var nodeGrid = el('div', { class: 'sg grid-3' });
    var nodes = (typeof MON_NODES !== 'undefined' && MON_NODES) ? MON_NODES : [{name:'Node1',ip:'localhost'}];
    nodes.forEach(function(nd) {
      nodeGrid.appendChild(el('div', { class: 'hc', style: 'text-align:center;padding:20px;border:2px dashed var(--border);transition:border-color 0.2s', ondragover: "event.preventDefault();this.style.borderColor='var(--accent)'", ondragleave: "this.style.borderColor='var(--border)'", ondrop: "event.preventDefault();this.style.borderColor='var(--border)';vmMigrateDrop(event.dataTransfer.getData('text/plain'),'" + nd.ip + "','" + nd.name + "')" },
        el('div', { style: 'font-size:24px;margin-bottom:6px' }, '🖥'),
        el('div', { class: 'text-13 font-600' }, nd.name),
        el('div', { class: 'color-muted text-xs' }, nd.ip)));
    });
    parts.push(nodeGrid);
  } else {
    l.forEach((v, i) => {
      const ri = vmList.indexOf(v);
      const on = v.state === 'running';
      const cp = v.live_cpu_pct || 0;
      const c = cp > 85 ? 'var(--red)' : cp > 60 ? 'var(--yellow)' : 'var(--green)';
      const star = favs.includes(v.name) ? '★' : '☆';
      parts.push(el('div', { class: 'vi ' + (ri === selectedVmIndex ? 'active' : ''), onclick: 'selectedVmIndex=' + ri + ";currentTab=localStorage.getItem('pcv-last-vm-tab')||'summary';switchSbTab('vms');document.querySelectorAll('#ct button').forEach(b=>b.classList.toggle('active',b.dataset.t==='summary'));render()", oncontextmenu: 'showCtx(event,' + ri + ')' },
        el('input', { type: 'checkbox', checked: checkedVms.has(ri) ? '' : null, 'aria-label': 'Select ' + v.name, onclick: 'event.stopPropagation();toggleChk(' + ri + ')' }),
        el('span', { class: 'fav-star', onclick: "event.stopPropagation();toggleFavorite('" + escapeAttr(v.name) + "')", title: 'Favorite' }, star),
        el('span', { class: 'dot ' + (on ? 'on' : 'off') }),
        el('span', { class: 'nm' }, v.name),
        el('span', { class: 'mini-bar' }, el('span', { class: 'mini-fill pcv-bar-fill-inline', style: '--bw:' + cp + '%;--bc:' + c })),
        el('span', { class: 'st' }, cp.toFixed(0) + '%'),
        el('canvas', { class: 'vm-spark', id: 'spark-' + v.name, width: '40', height: '14', style: 'vertical-align:middle;margin-left:4px' })));
    });
  }
  var vl = document.getElementById('vl');
  PCV.uxlib.clearEl(vl);
  vl.appendChild(PCV.uxlib.frag(parts));
  /* D2: Draw sparklines */
  setTimeout(function() {
    vmList.forEach(function(v) {
      var canvas = document.getElementById('spark-' + v.name);
      if (!canvas) return;
      var hist = _vmCpuHist[v.name] || [];
      if (hist.length < 2) return;
      var ctx = canvas.getContext('2d');
      var w = canvas.width, ht = canvas.height;
      ctx.clearRect(0, 0, w, ht);
      ctx.strokeStyle = 'rgba(0,240,255,0.6)';
      ctx.lineWidth = 1;
      ctx.beginPath();
      for (var si = 0; si < hist.length; si++) {
        var x = (si / (hist.length - 1)) * w;
        var y = ht - (hist[si] / 100) * ht;
        if (si === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      }
      ctx.stroke();
    });
  }, 50);
  document.getElementById('vc').textContent = vmList.length;
  document.getElementById('sb2').textContent = vmList[selectedVmIndex] ? vmList[selectedVmIndex].name : t('no_vm');
  document.getElementById('bbtn').style.display = checkedVms.size > 0 ? 'inline' : 'none';
  /* G-4: auto-refresh indicator */
  const elapsed = Math.round((Date.now() - lastLoadTime) / 1000);
  const sb3 = document.getElementById('sb3');
  if (sb3) sb3.textContent = 'Updated ' + elapsed + 's ago';
  /* VM 탭에서는 항상 콘텐츠 탭바(요약/콘솔/스냅샷/성능) 표시 */
  const ctBar = document.getElementById('ct');
  if (ctBar && ['summary','console','snapshots','performance','timeline'].includes(currentTab)) {
    ctBar.style.display = 'flex';
  }
  if (!skipContent) renderContent();
}

function toggleChk(i) {
  checkedVms.has(i) ? checkedVms.delete(i) : checkedVms.add(i);
  render();
}

async function bulkStop() {
  if (!await customConfirm(t('btn.stop_selected'), 'Stop ' + checkedVms.size + ' VMs?')) return;
  var total = checkedVms.size;
  var failed = [];
  for (const i of checkedVms) {
    const r = await fetchPost(EP.VM_STOP(vmList[i].name), {});
    if (r && r.error) { failed.push(vmList[i].name + ': ' + (r.error.message || '')); continue; }
    addEvt('VM Bulk stop — ' + vmList[i].name);
  }
  checkedVms.clear();
  if (failed.length) toast(failed.length + ' / ' + total + ' stop failed', false);
  setTimeout(loadAll, 1500);
}

/* ═══ F1: KEYBOARD NAVIGATION ═══
 * #4-C1: 진입부에 위젯 가드 2종 추가 — ①e.defaultPrevented(다른 핸들러가
 * 이미 이 키를 소비했음)면 return, ②e.target이 메뉴바/role=button|link|
 * menuitem 위(인터랙티브 위젯 위의 Enter는 위젯 몫)면 return. #3 실측에서
 * 확인된 충돌(메뉴 Enter가 VM Summary로 화면을 가로채는 회귀 — roving
 * tabindex로 .mi가 포커스 가능해지는 순간 재현됨, `clickedAfterEnter: false`)
 * 을 여기서 근본 해소한다. j/k/화살표 등 VM 목록 탐색 동작 자체는 불변. */
document.addEventListener('keydown', function(e) {
  if (e.defaultPrevented) return;
  if (e.target.closest && e.target.closest('.menubar, [role="button"], [role="link"], [role="menuitem"]')) return;
  if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA' || e.target.tagName === 'SELECT') return;
  if (currentTab !== 'summary' && currentTab !== 'dashboard' && currentTab !== 'console' && currentTab !== 'snapshots' && currentTab !== 'performance') return;
  if (e.key === 'ArrowDown' || e.key === 'j') {
    e.preventDefault();
    if (selectedVmIndex < vmList.length - 1) { selectedVmIndex++; render(); }
  } else if (e.key === 'ArrowUp' || e.key === 'k') {
    e.preventDefault();
    if (selectedVmIndex > 0) { selectedVmIndex--; render(); }
  } else if (e.key === 'Enter') {
    e.preventDefault();
    currentTab = 'summary'; switchSbTab('vms'); render();
  }
});

/* ═══ D2: VM SPARKLINE MINI CHART ═══
 *  _vmCpuHist[vmName] = [cpu%, cpu%, ...] (최대 30개)
 *  render()에서 vmList.forEach로 최신 CPU%를 push하고, 30개 초과 시 shift.
 *  setTimeout(50ms) 후 각 <canvas id="spark-{name}">에 꺾은선을 그린다.
 *  캔버스가 innerHTML 교체 시 파괴되므로 매번 새로 그려야 한다. */
var _vmCpuHist = {};

/* ═══ CONTEXT MENU ═══ */
function showCtx(e, i) {
  e.preventDefault();
  selectedVmIndex = i;
  const m = document.getElementById('ctx');
  const ri = i;
  /* ADR-013 DOM-safe: 컨텍스트 메뉴를 el/frag 로 조립. 인라인 onclick 문자열을
   * 동일 의미의 클로저로 이전 — 대상 함수는 모듈 경계 밖 전역이라 window.fn 참조.
   * 아이콘 HTML 엔티티(&#9654; 등)는 동일 코드포인트의 리터럴 글리프로 대체. */
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var sep = function() { return el('div', { class: 'sep' }); };
  var ci = function(label, onClick) { return el('div', { class: 'ci', onClick: onClick }, label); };
  clearEl(m);
  m.appendChild(frag(
    ci('▶ ' + t('power.start'), function() { window.vmPower('start'); }),
    ci('■ ' + t('power.stop'), function() { window.vmPower('stop'); }),
    sep(),
    ci('📷 ' + t('vm.snapshot'), function() { window.showSnap(); }),
    ci('⚙ ' + t('vm.settings'), function() { window.showSettings(); }),
    ci('✎ ' + _L('이름 변경', 'Rename'), function() { window.showRenameVm(); }),
    ci('🖨 VNC', function() { window.showVnc(); }),
    sep(),
    ci('📌 Memory Stats', function() { window.showMemStats(); }),
    ci('⚙ CPU Stats', function() { window.showCpuStats(); }),
    ci('💾 Disk Resize', function() { window.showDiskLiveResize(); }),
    ci('💬 Guest Agent', function() { window.showGuestAgent(); }),
    sep(),
    ci('🌐 NIC', function() { window.showNicMgr(); }),
    ci('📋 Clone', function() { window.vmClone(ri); }),
    ci('📦 Export OVA', function() { window.vmExportOva(ri); }),
    sep(),
    ci('❌ ' + t('btn.delete'), function() { window.vmDel(); })
  ));
  m.style.display = 'block';
  m.style.left = e.pageX + 'px';
  m.style.top = e.pageY + 'px';
  render();
}

function _vmStripCidr(v) {
  return String(v || '').split('/')[0];
}

function _vmNetSource(nic) {
  return nic.bridge || nic.source || nic.network || nic.type || '-';
}

function _vmNetworkMap(networks) {
  var map = {};
  (networks || []).forEach(function(n) {
    if (n && n.name) map[n.name] = n;
  });
  return map;
}

function _vmNicDns(nic, netMap) {
  var raw = String((nic && nic.dns) || '').trim();
  if (raw && raw !== 'off') return raw;
  if (raw === 'off') return PCV.uxlib.el('span', { class: 'color-muted' }, 'OFF');

  var source = _vmNetSource(nic);
  var meta = netMap[source];
  if (meta && meta.dhcp && meta.ip_cidr)
    return _vmStripCidr(meta.ip_cidr);
  return '-';
}

function _vmRenderNicDetails(nics, networks, v) {
  var el = PCV.uxlib.el;
  var netMap = _vmNetworkMap(networks);
  if (!Array.isArray(nics) || nics.length === 0) {
    var count = v && v.network_count ? String(v.network_count) : '0';
    return el('div', { class: 'color-muted text-xs', style: 'margin-top:8px' },
      count === '0' ? _L('할당된 NIC 없음', 'No assigned NICs')
                    : _L('NIC 상세 조회 불가', 'NIC details unavailable'));
  }

  var wrap = el('div', { style: 'margin-top:8px;border-top:1px solid var(--border);padding-top:6px' });
  nics.forEach(function(nic, idx) {
    var source = _vmNetSource(nic);
    var ip = nic.ip || '';
    var model = nic.model || 'virtio';
    var mac = nic.mac || '-';
    var target = nic.target ? ' / ' + nic.target : '';
    wrap.appendChild(el('div', { style: 'padding:5px 0;border-bottom:1px solid rgba(255,255,255,.06)' },
      HN.row('NIC ' + (idx + 1), [el('span', { class: 'color-accent' }, source), ' ', el('span', { class: 'color-muted text-xs' }, model + target)]),
      HN.row('MAC', el('span', { class: 'text-xs' }, mac)),
      HN.row('IP', ip ? el('span', { class: 'color-green' }, ip) : el('span', { class: 'color-muted' }, '-')),
      HN.row('DNS', _vmNicDns(nic, netMap))));
  });
  return wrap;
}

function _vmPrimaryNicValue(nics, field) {
  if (!Array.isArray(nics)) return '';
  for (var i = 0; i < nics.length; i++) {
    if (nics[i] && nics[i][field]) return nics[i][field];
  }
  return '';
}

/* renderProgressBar(ui.js 문자열 헬퍼, 수정 금지) 의 노드 등가물 — class/구조 동형. */
function _vmProgressBar(p, c) {
  var el = PCV.uxlib.el;
  var cl = p > 85 ? 'var(--red)' : p > 60 ? 'var(--yellow)' : 'var(--green)';
  var anim = p > 85 ? ' pulse-anim' : '';
  return el('div', { class: 'pb' + anim },
    el('div', { class: 'pb-f scan-anim', style: 'width:' + p + '%;background:' + (c || cl) }),
    el('div', { class: 'pb-t' }, p.toFixed(1) + '%'));
}

/* ═══ VM SUMMARY ═══ */
async function renderSummary(b, v) {
  if (!v) { PCV.uxlib.clearEl(b); b.appendChild(PCV.uxlib.el('p', { class: 'color-muted' }, t('vm.select'))); return; }
  const on = v.state === 'running';

  /* 실시간 메트릭 + NIC 상세 조회 */
  var metrics = {};
  var nics = [];
  var networks = [];
  var summaryReqs = [
    on ? fetchGet(EP.VM_DETAIL(v.name)) : Promise.resolve({}),
    fetchGet(EP.VM_NICS(v.name)),
    fetchGet(EP.NET_LIST())
  ];
  var summaryResults = await Promise.allSettled(summaryReqs);
  if (summaryResults[0].status === 'fulfilled')
    metrics = unwrapData(summaryResults[0].value) || summaryResults[0].value || {};
  if (summaryResults[1].status === 'fulfilled')
    nics = unwrapList(summaryResults[1].value);
  if (summaryResults[2].status === 'fulfilled')
    networks = unwrapList(summaryResults[2].value);

  var cpuPct = metrics.cpu || v.live_cpu_pct || 0;
  var memPct = metrics.mem || v.mem_percent || 0;
  var vcpu = metrics.vcpu || v.vcpu || '-';
  var memMb = metrics.memory_mb || v.memory_mb || '-';
  var diskRd = metrics.disk_rd || v.disk_rd || 0;
  var diskWr = metrics.disk_wr || v.disk_wr || 0;
  var netRx = metrics.net_rx || v.net_rx || 0;
  var netTx = metrics.net_tx || v.net_tx || 0;
  var primaryIp = _vmPrimaryNicValue(nics, 'ip') || v.ip || '-';
  var primaryDns = nics.length ? _vmNicDns(nics[0], _vmNetworkMap(networks)) : '-';
  var diskUsageAction = on
    ? PCV.uxlib.el('button', { class: 'btn btn-xs', onclick: 'showVmDiskUsage()' }, '📊 ' + _L('디스크 사용량', 'Disk Usage'))
    : PCV.uxlib.el('span', { class: 'color-muted text-xs' }, _L('실행 중인 VM에서 확인 가능', 'Available while running'));

  /* live 데이터를 vmList에도 반영 (사이드바 프로그레스바용) */
  v.live_cpu_pct = cpuPct;
  v.mem_percent = memPct;
  v.vcpu = vcpu;
  v.memory_mb = memMb;
  v.ip = primaryIp;

  const cpuHi = cpuPct > 85;
  var el = PCV.uxlib.el;
  var header = el('div', { class: 'flex gap-10 items-center mb-14' },
    el('span', { class: 'neon-blink color-accent' }, '>>'),
    el('h2', { style: 'font-family:var(--font-display);font-size:16px;letter-spacing:.05em' }, v.name),
    HN.badge(v.state + (cpuHi ? ' [HIGH_LOAD]' : ''), on ? 'g' : 'r'));
  var sg = el('div', { class: 'sg' },
    HN.card('💻 System', [
      HN.row('Guest OS', 'Linux (KVM)'),
      HN.row('UUID', el('span', { class: 'text-xs' }, v.uuid || '-')),
      HN.row(_L('부트', 'Boot'), (v.boot_mode || 'bios').toUpperCase()),
      HN.row(_L('자동시작', 'Auto Start'), v.auto_start ? el('span', { class: 'color-green' }, 'ON') : el('span', { class: 'color-muted' }, 'OFF'))
    ]),
    HN.card('⚙ CPU', [
      HN.row('vCPU', String(vcpu)),
      HN.row(_L('사용률', 'Usage'), el('span', { class: cpuHi ? 'color-red' : 'color-green' }, cpuPct.toFixed(1) + '%')),
      _vmProgressBar(cpuPct)
    ]),
    HN.card('📌 ' + _L('메모리', 'Memory'), [
      HN.row(_L('할당', 'Allocated'), String(memMb) + ' MB'),
      HN.row(_L('사용률', 'Usage'), memPct.toFixed(1) + '%'),
      _vmProgressBar(memPct)
    ]),
    HN.card('💾 ' + _L('스토리지', 'Storage'), [
      HN.row(_L('타입', 'Type'), HN.badge(escapeHtml(v.storage_type || '-'), v.storage_type === 'zvol' ? 'g' : 'y')),
      HN.row(_L('포맷', 'Format'), v.disk_format || '-'),
      HN.row(_L('경로', 'Path'), el('span', { class: 'text-xs' }, v.disk_path || '-')),
      HN.row(_L('게스트 사용량', 'Guest Usage'), diskUsageAction),
      HN.row(_L('스냅샷', 'Snapshots'), String(v.snapshot_count || 0)),
      HN.row('NIC', String(v.network_count || 0))
    ]),
    HN.card('💾 Disk I/O', [
      HN.row(_L('읽기', 'Read'), el('span', { class: 'color-cyan' }, formatBytes(diskRd))),
      HN.row(_L('쓰기', 'Write'), el('span', { class: 'color-peach' }, formatBytes(diskWr))),
      HN.row('IOPS R', el('span', { class: 'color-cyan' }, (metrics.disk_rd_req || 0).toLocaleString())),
      HN.row('IOPS W', el('span', { class: 'color-peach' }, (metrics.disk_wr_req || 0).toLocaleString()))
    ]),
    el('div', { class: 'hc glitch-panel' },
      el('h4', null, '🌐 ' + _L('네트워크', 'Network')),
      HN.row('RX', el('span', { class: 'color-yellow' }, formatBytes(netRx))),
      HN.row('TX', el('span', { class: 'color-yellow' }, formatBytes(netTx))),
      HN.row('IP', primaryIp && primaryIp !== '-' ? el('span', { class: 'color-green' }, primaryIp) : el('span', { class: 'color-muted' }, '-')),
      HN.row('DNS', primaryDns),
      HN.row('RX pps', el('span', { class: 'color-muted' }, (metrics.net_rx_pkts || 0).toLocaleString())),
      HN.row('TX pps', el('span', { class: 'color-muted' }, (metrics.net_tx_pkts || 0).toLocaleString())),
      _vmRenderNicDetails(nics, networks, v)),
    el('div', { class: 'hc', style: 'grid-column:1/-1' },
      el('h4', null, _L('작업', 'Actions')),
      el('div', { class: 'flex gap-4 flex-wrap mb-8' },
        el('button', { class: 'btn btn-g', onclick: "vmPower('start')" }, '▶ ' + t('power.start')),
        el('button', { class: 'btn', onclick: "vmPower('suspend')" }, '❚❚ ' + t('power.pause')),
        el('button', { class: 'btn', onclick: "vmPower('resume')" }, '▶▶ ' + t('power.resume')),
        el('button', { class: 'btn btn-r', onclick: "vmPower('stop')" }, '■ ' + t('power.stop'))),
      el('div', { style: 'display:grid;grid-template-columns:repeat(auto-fill,minmax(100px,1fr));gap:4px' },
        el('button', { class: 'btn', onclick: 'showSnap()' }, t('vm.snapshot')),
        el('button', { class: 'btn', onclick: 'showSettings()' }, t('vm.settings')),
        el('button', { class: 'btn', onclick: 'showRenameVm()' }, '✎ ' + _L('이름', 'Rename')),
        el('button', { class: 'btn', onclick: 'showNicMgr()' }, 'NIC'),
        el('button', { class: 'btn', onclick: 'vmClone(' + selectedVmIndex + ')' }, '📋 Clone'),
        el('button', { class: 'btn', onclick: 'vmExportOva(' + selectedVmIndex + ')' }, '📦 Export'),
        el('button', { class: 'btn', onclick: 'showImportOva()' }, '📥 Import'),
        el('button', { class: 'btn', onclick: 'showMemStats()' }, '📌 Mem'),
        el('button', { class: 'btn', onclick: 'showCpuStats()' }, '⚙ CPU'),
        el('button', { class: 'btn', onclick: 'showVmDiskUsage()' }, '📊 ' + _L('디스크 사용량', 'Disk Usage')),
        el('button', { class: 'btn', onclick: 'showDiskLiveResize()' }, '💾 Disk'),
        el('button', { class: 'btn', onclick: 'showBlkioEditor()' }, '⚙ I/O'),
        el('button', { class: 'btn', onclick: 'showGuestAgent()' }, '💬 Agent'))));
  PCV.uxlib.clearEl(b);
  b.appendChild(header);
  b.appendChild(sg);
}

/* ═══ EXPORT TO PCV NAMESPACE (ADR-0013) ═══
 *  PCV.vm에 등록되는 함수가 이 모듈의 공식 인터페이스.
 *  아래 BACKWARD COMPAT SHIMS는 HTML onclick과 다른 모듈의
 *  window.render() 등 직접 참조를 위한 전환기 코드.
 *  신규 코드에서는 PCV.vm.render() 사용을 권장. */
PCV.vm = Object.assign(PCV.vm || {}, {
  render: render,
  setSort: setSort,
  getFiltered: getFiltered,
  toggleVmView: toggleVmView,
  renderSummary: renderSummary,
});

/* ═══ BACKWARD COMPAT SHIMS (ADR-0013: remove after full transition) ═══ */
window.render = render;
window.setSort = setSort;
window.getFiltered = getFiltered;
window.toggleChk = toggleChk;
window.bulkStop = bulkStop;
window.showCtx = showCtx;
window.renderSummary = renderSummary;
window.toggleVmView = toggleVmView;

})(window.PCV);
