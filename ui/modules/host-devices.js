                                                                  
                                       
                                                          
                                                          
                                                                     
   
                                                                                        
                                                                                           
                                                                                          
                           
  
                           
                                                                        
                                                                                                 
                                                                                             
                                                                                                          
                                                                        
                                                                                                
                                                                                               
                                                                                     
                                                                          
                                                                                     
                                                                          
                                                                                   
                                                                                                  
                                                                                                                
                                                                                       
                                                                                       
  
                       
                                                                             
                                                                                
                                                                            
   
window.PCV = window.PCV || {};
(function (PCV) {

  var FILTER_KEY = 'hdassign';                                                                         
  var GROUPS = ['gpu', 'usb', 'bluetooth'];
  var ROLE_RANK = { viewer: 0, operator: 1, admin: 2 };

                                                                        
  var T = {
    title: function () { return _L('호스트 장치', 'Host Devices'); },
    gone: function () { return _L('호스트에서 빠짐 · 다시 꽂아도 자동으로 안 붙음(해제 뒤 다시 할당)', 'Unplugged from host · will not reattach on replug (release, then assign again)'); },
    viewOnly: function () { return _L('보기 권한만', 'View-only role'); },
    adminOnly: function () { return _L('관리자 권한 필요', 'Administrator role required'); },
    otherVm: function () { return _L('다른 VM이 사용 중', 'In use by another VM'); },
    dup: function () { return _L('같은 ID 장치가 둘 이상 꽂혀 있습니다 — 하나를 뺀 뒤 할당', 'Two or more devices with the same ID are plugged in — unplug one, then assign'); },
    stopFirst: function () { return _L('VM을 끈 뒤 가능', 'Stop the VM first'); },
    noVms: function (admin) {
      return admin ? _L('할당할 VM이 없습니다', 'No VMs to assign to') : _L('할당할 수 있는 내 VM이 없습니다', 'You own no VMs to assign to');
    },
  };

                     

  function _roleRank(role) {
    var key = String(role || '').toLowerCase();
    return Object.prototype.hasOwnProperty.call(ROLE_RANK, key) ? ROLE_RANK[key] : -1;
  }

     
                                                                                          
                                                                                          
     
  function describe(row, a) {
    var present = !!(row && row.present);
    var live = !!a.live, persistent = !!a.persistent, missing = !!a.missing;
    var stale = !!a.stale_live_address, ambiguous = !!a.ambiguous, active = !!a.vm_active;
    var run = null, runWarn = false, persist = null, persistWarn = false;
    if (live && missing) { run = _L('자리만 있음 · 장치 없이 시작됨', 'Placeholder only · started without the device'); runWarn = true; }
    else if (live && stale && !present) { run = T.gone(); runWarn = true; }
    else if (live && stale && present) { run = _L('옛 장치 번호 · 다시 연결 필요(해제 뒤 다시 할당)', 'Old device number · reconnect needed (release, then assign again)'); runWarn = true; }
    else if (live && !present) { run = T.gone(); runWarn = true; }                                        
    else if (live && ambiguous) { run = _L('같은 ID 장치 여럿 · 어느 것인지 모름', 'Several devices share this ID · cannot tell which'); runWarn = true; }
    else if (live && !missing && present && !stale) { run = _L('실행 중', 'Running'); }

    if (persistent && live) persist = _L('영구', 'Persistent');
    else if (live && !persistent) persist = _L('지금만(VM을 끄면 빠짐)', 'Live only (removed when the VM stops)');
    else if (persistent && !live && !present) { persist = _L('영구만 · 호스트에 안 꽂힘', 'Persistent only · not plugged into the host'); persistWarn = true; }
    else if (persistent && !live && present && active) { persist = _L('영구만 · 지금은 안 붙음(다음 시작 때 붙음)', 'Persistent only · not attached now (attaches at next start)'); persistWarn = true; }
    else if (persistent && !live && !active) persist = _L('영구(VM 꺼짐 — 다음 시작 때 붙음)', 'Persistent (VM off — attaches at next start)');
    return { run: run, persist: persist, warn: runWarn || persistWarn, runWarn: runWarn, persistWarn: persistWarn };
  }

                                                                              
  function _needsAttachNow(row, a) {
    return row.kind === 'usb' && !!row.present && !!a.vm_visible && !!a.vm && !!a.persistent && !a.live && !!a.vm_active;
  }

                                               
  function _deviceWords(row) {
    var out = [];
    var asg = row.assignments || [];
    if (row.kind === 'pci') {
      if (!row.present) return [{ text: _L('PCI 장치 · 호스트에 없음(정의만 있음)', 'PCI device · not on host (definition only)'), warn: true }];
      out.push({ text: row.driver ? String(row.driver) : _L('드라이버 없음', 'No driver'), warn: false });
      if (row.host_display) out.push({ text: _L('호스트 화면 사용 중', 'Used by the host display'), warn: false });
      if (row.partial_assignment) out.push({ text: _L('일부만 할당됨 · 정의 복구 필요', 'Partially assigned · definition needs repair'), warn: true });
      if (asg.some(function (a) { return a.managed === 'yes'; })) {
        out.push({ text: _L('옛 정의(managed=yes) · 해제 뒤 다시 할당', 'Old definition (managed=yes) · release, then assign again'), warn: true });
      }
    }
    if (!asg.length && row.present) {
      out.push({ text: row.group === 'bluetooth' ? _L('미할당 · USB 장치', 'Unassigned · USB device') : _L('미할당', 'Unassigned'), warn: false });
    }
    return out;
  }

                                                                    
  function _natural(row, vms, rank) {
    var asg = row.assignments || [];
    var vis = asg.filter(function (a) { return a.vm_visible && a.vm; });
    if (asg.length && !vis.length) return { buttons: [], text: T.otherVm(), reason: null, hiddenOnly: true };          
    return row.kind === 'pci' ? _naturalGpu(row, asg, vis, vms) : _naturalUsb(row, asg, vis, vms, rank);
  }

  function _naturalUsb(row, asg, vis, vms, rank) {
    var dup = !!row.duplicate_on_host || row.blocked_reason === 'duplicate_on_host';
    if (!asg.length) {
      if (row.blocked_reason === 'hub') return { buttons: [], text: _L('할당 불가 · 허브', 'Cannot assign · hub'), reason: null };
      var reason = null;
      if (dup) reason = T.dup();
      else if (!vms.length) reason = T.noVms(rank >= 2);
      else if (row.assignable === false) reason = _L('이 장치는 지금 할당할 수 없습니다', 'This device cannot be assigned now');
      return { buttons: [{ kind: 'assign', disabled: !!reason }], text: null, reason: reason };
    }
    var buttons = [];
    var why = null;
    var now = vis.filter(function (a) { return _needsAttachNow(row, a); })[0];
    if (now) {
      buttons.push({ kind: 'attach-now', disabled: dup, vm: now.vm });
      if (dup) why = T.dup();
    }
                                                                                
    buttons.push(asg.length > 1 ? { kind: 'release-multi', disabled: false } : { kind: 'release', disabled: false, vm: vis[0].vm });
    return { buttons: buttons, text: null, reason: why };
  }

  function _naturalGpu(row, asg, vis, vms) {
    if (!row.present) {                                                                                   
      return { buttons: [], text: _L('해제 불가 — 호스트에 장치가 없음(정의는 운영자가 정리)', 'Cannot release — device not on host (an operator must clean up the definition)'), reason: null };
    }
    var reason = null;
    if (!asg.length) {
      if (row.host_display || row.blocked_reason === 'host_display') return { buttons: [], text: _L('할당 불가', 'Cannot assign'), reason: null };
      if (row.blocked_reason === 'iommu_group_unsafe' || row.group_safe === false) reason = _L('IOMMU 그룹에 다른 장치가 섞여 있어 할당할 수 없습니다', 'The IOMMU group mixes in other devices, so it cannot be assigned');
      else if (row.blocked_reason === 'not_vfio_bound' || row.group_vfio_ready === false) reason = _L('호스트에서 GPU 그룹 전체를 vfio-pci에 먼저 묶어야 합니다(ADR-0047)', 'Bind the whole GPU group to vfio-pci on the host first (ADR-0047)');
      else if (!vms.length) reason = T.noVms(true);
      else if (!vms.some(function (v) { return !v.active; })) reason = T.stopFirst();
      else if (row.assignable === false) reason = _L('이 장치는 지금 할당할 수 없습니다', 'This device cannot be assigned now');
      return { buttons: [{ kind: 'assign', disabled: !!reason }], text: null, reason: reason };
    }
    var multi = asg.length > 1;
    if (row.group_safe === false) reason = _L('IOMMU 그룹에 다른 장치가 섞여 있어 화면에서 해제할 수 없습니다(정의는 운영자가 정리)', 'The IOMMU group mixes in other devices, so it cannot be released here (an operator must clean up the definition)');
    else if (multi ? vis.every(function (a) { return a.vm_active; }) : vis[0].vm_active) reason = T.stopFirst();
    var button = multi ? { kind: 'release-multi', disabled: !!reason } : { kind: 'release', disabled: !!reason, vm: vis[0].vm };
    return { buttons: [button], text: null, reason: reason };
  }

     
                                                                                                            
                                                                                          
                                                                                         
     
  function actionState(row, ctx) {
    ctx = ctx || {};
    var rank = _roleRank(ctx.role);
    var nat = _natural(row, Array.isArray(ctx.vms) ? ctx.vms : [], rank);
    var off = function (b) { var c = Object.assign({}, b); c.disabled = true; return c; };
    if (rank < 1) {                                                                                
      if (nat.hiddenOnly) return { buttons: [], text: null, reason: T.viewOnly() };
      return { buttons: nat.buttons.map(off), text: nat.text || null, reason: nat.buttons.length ? T.viewOnly() : null };
    }
    if (row.kind === 'pci' && rank < 2 && nat.buttons.length) {                     
      return { buttons: nat.buttons.map(off), text: nat.text || null, reason: T.adminOnly() };
    }
    return { buttons: nat.buttons, text: nat.text || null, reason: nat.reason || null };
  }

                                                                                
  function counts(rows, showHidden) {
    var out = { all: 0, assigned: 0, unassigned: 0, hidden: 0 };
    (rows || []).forEach(function (r) {
      if (r.hidden_by_default) { out.hidden++; if (!showHidden) return; }
      out.all++;
      if ((r.assignments || []).length) out.assigned++; else out.unassigned++;
    });
    return out;
  }

                      

  function _short(addr) { return String(addr || '').replace(/^0000:/, ''); }
  function _idText(row) { return row.kind === 'pci' ? _short(row.pci_addr || row.id) : String(row.id); }
                                                            
  function _name(row) {
    if (row.name) return String(row.name);
    var generic = row.kind === 'pci' ? _L('PCI 장치', 'PCI device') : _L('USB 장치', 'USB device');
    return row.vendor_name ? String(row.vendor_name) + ' ' + generic : generic;
  }
  function _traitHint(traits) {
    var words = { keyboard: _L('키보드', 'keyboard'), mouse: _L('마우스', 'mouse'), audio: _L('오디오', 'audio'),
      storage: _L('저장 장치', 'storage'), bluetooth: _L('블루투스', 'Bluetooth'), hub: _L('허브', 'hub') };
    var list = ['keyboard', 'mouse', 'audio', 'storage', 'bluetooth', 'hub']
      .filter(function (k) { return (traits || []).indexOf(k) >= 0; }).map(function (k) { return words[k]; });
    return list.length ? '(' + list.join('·') + ')' : null;
  }
  function _classWord(code) {
    return /^0403/.test(String(code || '')) ? _L('오디오', 'Audio') : _L('PCI 장치', 'PCI device');
  }
  function _currentRole() {
    if (typeof pcvRoleAllows !== 'function') return null;
    if (pcvRoleAllows('admin')) return 'ADMIN';
    if (pcvRoleAllows('operator')) return 'OPERATOR';
    if (pcvRoleAllows('viewer')) return 'VIEWER';
    return null;
  }
                                               
  function _prefix(code) {
    switch (Number(code)) {
      case -32602: return _L('요청 값이 맞지 않습니다', 'The request values are invalid');
      case -32006: return _L('이 계정 테넌트에 허용되지 않은 장치입니다', 'This device is not allowed for your tenant');
      case -32001: return _L('VM을 찾을 수 없습니다 — 목록을 새로고침하세요', 'VM not found — refresh the list');
      case -32004: return _L('다른 작업이 진행 중입니다 — 잠시 뒤 다시 하세요', 'Another operation is in progress — try again shortly');
      case -32002: return _L("지금 상태와 맞지 않습니다(정책이 다름·자리표시자·같은 장치 여럿·꺼진 VM에 '지금만') — 먼저 해제하거나 '영구로 저장'을 켜세요",
        "Conflicts with the current state (different policy, placeholder, duplicate device, or live-only on a stopped VM) — release first or turn on 'Save persistently'");
      default: return _L('동작하지 못했습니다', 'The action failed');
    }
  }
  function _errMessage(error) {
    return String((error && (error.message || error.code)) || _L('알 수 없는 오류', 'Unknown error'));
  }

                                                                                                  
                                                                                     
                                        

  async function _fetchList() {
    try {
      var r = await fetchPost(EP.RPC(), { jsonrpc: '2.0', method: 'host.device.list', params: {}, id: 'hd-list' });
      if (r && r.error) return { error: r.error };
      return { result: unwrapData(r) };
    } catch (e) { return { error: { message: e && e.message ? e.message : String(e) } }; }
  }
                                                               
  async function _usbAttach(vm, row, persist) {
    try {
      var r = await fetchPost(EP.RPC(), { jsonrpc: '2.0', method: 'vm.usb.attach', params: { vm_id: vm, vendor_id: row.vendor_id, product_id: row.product_id, persistent: persist }, id: 'hd-usb-attach' });
      if (r && r.error) return { error: r.error };
      return { result: unwrapData(r) };
    } catch (e) { return { error: { message: e && e.message ? e.message : String(e) } }; }
  }
                                                                                                      
  async function _usbAttachNow(vm, row, policy) {
    try {
      var r = await fetchPost(EP.RPC(), { jsonrpc: '2.0', method: 'vm.usb.attach', params: { vm_id: vm, vendor_id: row.vendor_id, product_id: row.product_id, startup_policy: policy }, id: 'hd-usb-attach-now' });
      if (r && r.error) return { error: r.error };
      return { result: unwrapData(r) };
    } catch (e) { return { error: { message: e && e.message ? e.message : String(e) } }; }
  }
  async function _usbDetach(vm, row) {
    try {
      var r = await fetchPost(EP.RPC(), { jsonrpc: '2.0', method: 'vm.usb.detach', params: { vm_id: vm, vendor_id: row.vendor_id, product_id: row.product_id }, id: 'hd-usb-detach' });
      if (r && r.error) return { error: r.error };
      return { result: unwrapData(r) };
    } catch (e) { return { error: { message: e && e.message ? e.message : String(e) } }; }
  }
                                                                                  
  async function _gpuAttach(vm, row) {
    try {
      var r = await fetchPost(EP.RPC(), { jsonrpc: '2.0', method: 'device.gpu.attach', params: { vm_name: vm, pci_addr: row.pci_addr }, id: 'hd-gpu-attach' });
      if (r && r.error) return { error: r.error };
      return { result: unwrapData(r) };
    } catch (e) { return { error: { message: e && e.message ? e.message : String(e) } }; }
  }
  async function _gpuDetach(vm, row) {
    try {
      var r = await fetchPost(EP.RPC(), { jsonrpc: '2.0', method: 'device.gpu.detach', params: { vm_name: vm, pci_addr: row.pci_addr }, id: 'hd-gpu-detach' });
      if (r && r.error) return { error: r.error };
      return { result: unwrapData(r) };
    } catch (e) { return { error: { message: e && e.message ? e.message : String(e) } }; }
  }

                  
                                                                              
  var _cur = null;
  var _seq = 0;
  var _reasonSeq = 0;
  var _unsub = null;

  async function renderHostDevices(b) {
    if (!b) return;
    var el = PCV.uxlib.el;
    var st = {
      token: ++_seq, target: b, data: null, showHidden: false, refreshing: false,
      root: el('div', { class: 'hd-screen' }),
      result: el('div', { class: 'hd-result', 'aria-live': 'polite' })                              
    };
    _cur = st;
    _ensureSub();
    PCV.uxlib.clearEl(b);
    b.appendChild(st.root);
    st.root.appendChild(HN.pagehead({ title: T.title() }));
    st.root.appendChild(PCV.uxlib.msg('loading', { tag: 'p', cls: 'hd-loading' }, _L('불러오는 중', 'Loading')));
    await _load(st, null);
  }

                                                                       
  function _ensureSub() {
    if (_unsub || !PCV.ui || !PCV.ui.filterState || !PCV.ui.filterState.subscribe) return;
    _unsub = PCV.ui.filterState.subscribe(function () {
      var st = _cur;
      if (!st || !st.root.isConnected) { if (_unsub) { _unsub(); _unsub = null; } return; }
      if (st.data) _paint(st, null);
    });
  }

  function _filterValue() {
    var fs = PCV.ui && PCV.ui.filterState;
    var v = fs ? (fs.current()[FILTER_KEY] || []) : [];
    return v.length === 1 && (v[0] === 'assigned' || v[0] === 'unassigned') ? v[0] : null;
  }

  async function _load(st, focus) {
    var token = st.token;
    var res = await _fetchList();
    if (_cur !== st || token !== st.token) return;                                               
    st.refreshing = false;
    var data = res.result;
    var valid = !res.error && data && Array.isArray(data.devices) && Array.isArray(data.vms);
    if (!valid) {
                                                                                        
      st.data = null;
      var message = res.error ? _errMessage(res.error) : _L('응답 형식이 올바르지 않습니다', 'Unexpected response shape');
      var box = PCV.uxlib.renderLoadError(st.root, {
        title: T.title(),
        message: _L('호스트 장치 목록을 불러오지 못했습니다: ', 'Could not load host devices: ') + message,
        retry: function () { renderHostDevices(st.target); }
      });
      var retry = box && box.querySelector('button');
      if (retry && focus) retry.focus();
      return;
    }
    st.data = data;
    _paint(st, focus);
  }

  function _reload(st, focus) {
    if (_cur !== st) return;
    _load(st, focus);
  }

  async function _refresh(st, button) {
    if (st.refreshing) return;
    st.refreshing = true;
    button.style.minWidth = button.getBoundingClientRect().width + 'px';                    
    button.disabled = true;
    button.setAttribute('aria-busy', 'true');
    await _load(st, { ctl: 'refresh' });                                                                              
  }

                   

  function _paint(st, focus) {
    var el = PCV.uxlib.el, clearEl = PCV.uxlib.clearEl;
    if (!focus) focus = _focusSnapshot(st.root);
    var data = st.data, rows = data.devices;
    var role = _currentRole();
    var ctx = { role: role, vms: data.vms };
    clearEl(st.root);
    var head = HN.pagehead({ title: T.title() });
    var title = head.querySelector('.pagehead-title');
    if (title) title.setAttribute('tabindex', '-1');                                                              
    var refresh = el('button', { type: 'button', class: 'btn hd-refresh', 'data-hd-ctl': 'refresh',
      disabled: st.refreshing ? 'disabled' : null, onClick: function () { _refresh(st, refresh); } }, _L('새로고침', 'Refresh'));
    st.root.appendChild(head);

    if (!rows.length) {
      var admin = _roleRank(role) >= 2;
      st.root.appendChild(el('div', { class: 'hd-toolbar' },
        PCV.uxlib.msg('muted', { tag: 'p', cls: 'hd-empty' }, admin
          ? _L('표시할 호스트 장치가 없습니다', 'No host devices to show')
          : _L('이 계정 테넌트에 허용된 호스트 장치가 없습니다(관리자가 resource.grant로 허용)', 'No host devices are allowed for this account’s tenant (an administrator grants them with resource.grant)')),
        refresh));
      _notes(st, data);
      st.root.appendChild(st.result);
      _restoreFocus(st, focus);
      return;
    }

    var c = counts(rows, st.showHidden);
    var bar = HN.filterBar([{
      key: FILTER_KEY, label: _L('할당', 'Assignment'), singleSelect: true, showAll: true,
      allLabel: _L('전체', 'All'), allCount: c.all,
      options: [
        { value: 'assigned', label: _L('할당됨', 'Assigned'), count: c.assigned },
        { value: 'unassigned', label: _L('미할당', 'Unassigned'), count: c.unassigned }
      ]
    }]);
    var toolbar = el('div', { class: 'hd-toolbar' }, bar);
    if (c.hidden) {
      var toggle = el('button', { type: 'button', class: 'btn btn-soft', 'data-hd-ctl': 'hidden',
        'aria-pressed': st.showHidden ? 'true' : 'false',
        onClick: function () { st.showHidden = !st.showHidden; _paint(st, { ctl: 'hidden' }); }
      }, st.showHidden ? _L('숨기기', 'Hide') : _L('보기', 'Show'));
      toolbar.appendChild(el('div', { class: 'hd-hidden-note' },
        el('span', null, st.showHidden
          ? _L('허브·저장 장치 ' + c.hidden + '개 표시 중', c.hidden + ' hubs/storage devices shown')
          : _L('허브·저장 장치 ' + c.hidden + '개 숨김', c.hidden + ' hubs/storage devices hidden')),
        ' ', toggle));
    }
    toolbar.appendChild(refresh);
    st.root.appendChild(toolbar);
    _notes(st, data);
    st.root.appendChild(st.result);

    var sel = _filterValue();
    var shown = rows.filter(function (r) {
      if (r.hidden_by_default && !st.showHidden) return false;
      var assigned = (r.assignments || []).length > 0;
      return sel === 'assigned' ? assigned : sel === 'unassigned' ? !assigned : true;
    });
    if (!shown.length) {
      st.root.appendChild(PCV.uxlib.msg('muted', { tag: 'p', cls: 'hd-empty' }, _L('조건에 맞는 장치가 없습니다', 'No devices match the filter')));
      _restoreFocus(st, focus);
      return;
    }
    var heads = [_L('장치', 'Device'), 'ID', _L('할당', 'Assigned to'), _L('상태', 'State'), _L('동작', 'Action')];
    var table = el('table', { class: 'card-mobile hd-table' },
      el('thead', null, el('tr', null, heads.map(function (h) { return el('th', { scope: 'col' }, h); }))));
    GROUPS.forEach(function (g) {
      var list = shown.filter(function (r) { return r.group === g; });
      if (!list.length) return;
      var label = g === 'gpu' ? 'GPU' : g === 'usb' ? 'USB' : _L('블루투스', 'Bluetooth');
      table.appendChild(el('tbody', { 'data-hd-group': g },
        el('tr', { class: 'hd-grp' }, el('th', { scope: 'rowgroup', colspan: '5' }, label)),
        list.map(function (r) { return _rowNode(st, r, ctx, heads); })));
    });
    st.root.appendChild(table);
    _restoreFocus(st, focus);
  }

                                                                                        
  function _notes(st, data) {
    var lines = [];
    var n = data.devices.length;
    if (data.truncated) {
      lines.push(_L('장치가 많아 일부만 보입니다(상한 ' + n + '개). pcvctl --format=json host devices --all로 전체를 보세요',
        'Showing only the first ' + n + ' devices. Run pcvctl --format=json host devices --all for the full list'));
    }
    var src = data.sources || {};
    if (src.usb === 'absent') lines.push(_L('이 호스트에서 USB 장치 목록을 읽을 수 없습니다(정의만 있는 장치는 호스트에 없는 행으로 보입니다)', 'Host USB device list not available on this host'));
    if (src.pci === 'absent') lines.push(_L('이 호스트에서 PCI 장치 목록을 읽을 수 없습니다', 'Host PCI device list not available on this host'));
    if (src.names === 'unavailable') lines.push(_L('장치 이름 사전이 없어 ID만 보입니다', 'Device name database unavailable — IDs only'));
    lines.forEach(function (text) { st.root.appendChild(PCV.uxlib.msg('warn', { tag: 'p', cls: 'hd-note' }, text)); });
  }

  function _segments(segs) {
    var el = PCV.uxlib.el;
    var out = [];
    segs.forEach(function (s, i) {
      if (i) out.push(' · ');
      out.push(s.warn ? el('span', { class: 'hd-warn' }, s.text) : s.text);
    });
    return el('div', { class: 'hd-line' }, out);
  }

  function _rowNode(st, row, ctx, heads) {
    var el = PCV.uxlib.el;
    var name = _name(row);
    var asg = row.assignments || [];
                                                                                                
                                                                                                      
                                                              
    var hint = row.kind === 'usb' ? _traitHint(row.traits) : null;
    var compWord = row.assignable === true ? _L('함께 이동', 'moves with it') : _L('같은 IOMMU 그룹', 'same IOMMU group');
    var device = el('div', { class: 'hd-cell' },
      el('div', { class: 'hd-name' }, el('span', null, name), hint ? ' ' : null, hint ? el('span', { class: 'hd-sub' }, hint) : null),
      (row.companions || []).map(function (cmp) {
        return el('div', { class: 'hd-sub' }, '+ ' + (cmp.name || _classWord(cmp.class_code)) + '(' + _short(cmp.pci_addr) + ') ' + compWord);
      }));
                                                                        
    var assign = asg.length
      ? asg.map(function (a) {
        return el('div', { class: 'hd-line' }, el('span', { class: 'hd-vm' + (a.vm_visible && a.vm ? '' : ' hd-vm-other') },
          a.vm_visible && a.vm ? String(a.vm) : _L('다른 VM', 'Other VM'),
          a.vm_active ? null : el('span', { class: 'hd-vm-off' }, ' · ' + _L('꺼짐', 'off'))));
      })
      : el('span', { class: 'hd-host' }, _L('호스트', 'Host'));
                                                                        
    var lines = asg.map(function (a) {
      var d = describe(row, a);
      var segs = [];
      if (d.run) segs.push({ text: d.run, warn: d.runWarn });
      if (d.persist) segs.push({ text: d.persist, warn: d.persistWarn });
      return segs;
    });
    var words = _deviceWords(row);
    if (lines.length <= 1) lines = [(lines[0] || []).concat(words)];
    else if (words.length) lines.push(words);
    var status = lines.filter(function (l) { return l.length; }).map(_segments);
    return el('tr', { 'data-hd-row': row.id, 'data-hd-group': row.group },
      el('td', { 'data-label': heads[0] }, device),
      el('td', { 'data-label': heads[1], class: 'font-mono nowrap', title: row.kind === 'pci' ? String(row.pci_addr || row.id) : null }, _idText(row)),
      el('td', { 'data-label': heads[2] }, el('div', { class: 'hd-cell' }, assign)),
      el('td', { 'data-label': heads[3] }, el('div', { class: 'hd-cell' }, status)),
      el('td', { 'data-label': heads[4] }, _actionsNode(st, row, actionState(row, ctx))));
  }

  function _actionsNode(st, row, state) {
    var el = PCV.uxlib.el;
    var name = _name(row);
    var reasonId = state.reason ? 'hd-reason-' + (++_reasonSeq) : null;
    var labels = {
      assign: [_L('할당…', 'Assign…'), _L(name + ' 할당…', 'Assign ' + name + '…')],
      release: [_L('해제', 'Release'), _L(name + ' 해제', 'Release ' + name)],
      'release-multi': [_L('해제…', 'Release…'), _L(name + ' 해제…', 'Release ' + name + '…')],
      'attach-now': [_L('지금 붙이기', 'Attach now'), _L(name + ' 지금 붙이기', 'Attach ' + name + ' now')]
    };
    var wrap = el('div', { class: 'hd-actions' });
    state.buttons.forEach(function (b) {
      var vm = b.vm || (b.kind === 'release-multi' ? '*' : '');
      var button = el('button', {
        type: 'button', class: 'btn', 'aria-label': labels[b.kind][1],
        'data-hd-key': row.id + '|' + vm, 'data-hd-kind': b.kind,
        disabled: b.disabled ? 'disabled' : null,
        'aria-describedby': b.disabled && reasonId ? reasonId : null
      }, labels[b.kind][0]);
      button.addEventListener('click', function () { _onAction(st, row, b, button); });
      wrap.appendChild(button);
    });
    if (state.text) wrap.appendChild(el('span', { class: 'hd-text' }, state.text));
    if (state.reason) wrap.appendChild(el('span', { class: 'hd-reason', id: reasonId }, state.reason));
    return wrap;
  }

  function _onAction(st, row, b, button) {
    var focus = { id: row.id, vm: b.vm || (b.kind === 'release-multi' ? '*' : ''), kind: b.kind };
    if (b.kind === 'assign') _openAssign(st, row, button, focus);
    else if (b.kind === 'release') _openRelease(st, row, b.vm, button, focus);
    else if (b.kind === 'release-multi') _openRelease(st, row, null, button, focus);
    else if (b.kind === 'attach-now') _attachNow(st, row, b.vm, button, focus);
  }

                                 

  function _focusSnapshot(root) {
    var a = document.activeElement;
    if (!a || !root.contains(a) || !a.getAttribute) return null;
    if (a.getAttribute('data-hd-ctl')) return { ctl: a.getAttribute('data-hd-ctl') };
    if (a.getAttribute('data-facet')) return { facet: a.getAttribute('data-facet'), val: a.getAttribute('data-val') };
    var key = a.getAttribute('data-hd-key');
    if (key) {
      var cut = key.indexOf('|');
      return { id: key.slice(0, cut), vm: key.slice(cut + 1), kind: a.getAttribute('data-hd-kind') };
    }
    return null;
  }

  function _restoreFocus(st, f) {
    if (!f) return;
    var root = st.root;
    var target = null;
    var all = function (sel) { return Array.prototype.slice.call(root.querySelectorAll(sel)); };
    if (f.ctl) {
      target = all('[data-hd-ctl]').filter(function (n) { return n.getAttribute('data-hd-ctl') === f.ctl; })[0];
    } else if (f.facet) {
      target = all('.chip').filter(function (n) {
        return n.getAttribute('data-facet') === f.facet && n.getAttribute('data-val') === f.val;
      })[0];
    } else if (f.id !== undefined) {
      var key = f.id + '|' + (f.vm || '');
      var buttons = all('button[data-hd-key]').filter(function (n) { return !n.disabled; });
      target = buttons.filter(function (n) { return n.getAttribute('data-hd-key') === key && n.getAttribute('data-hd-kind') === f.kind; })[0]
        || buttons.filter(function (n) { return n.getAttribute('data-hd-key') === key; })[0];
      if (!target) {
        var tr = all('tr[data-hd-row]').filter(function (n) { return n.getAttribute('data-hd-row') === f.id; })[0];
        if (tr) target = Array.prototype.slice.call(tr.querySelectorAll('button')).filter(function (n) { return !n.disabled; })[0];
      }
      if (!target) target = root.querySelector('.pagehead-title');
    }
    if (target) target.focus();
  }

                                                           
  function _focusTrigger(st, focus, trigger) {
    if (trigger && trigger.isConnected && !trigger.disabled) trigger.focus();
    else _restoreFocus(st, focus);
  }

                                                           
  function _result(st, ok, text) {
    var node = PCV.uxlib.setMsg(st.result, ok ? 'ok' : 'err', { tag: 'p', cls: 'hd-result-line' }, text);
    if (node && !ok) node.setAttribute('role', 'alert');
  }
  function _failText(error) { return _prefix(error && error.code) + ': ' + _errMessage(error); }
  function _usbOutcome(res) {
    var parts = [];
    if (res && res.live) parts.push(_L('실행 중', 'running'));
    if (res && res.config) parts.push(_L('영구', 'persistent'));
    return parts.length ? '(' + parts.join(' · ') + ')' : _L('(이미 붙어 있음 — 바뀐 것 없음)', '(already attached — nothing changed)');
  }

                               
                                                                                                                  
                                                       
  function _actionDialog(opts) {
    var st = opts.st;
    var busy = false, closed = false, changed = false;
    var dlg = PCV.modalCore.open(opts.body, {
      onClose: function () {
        closed = true;
        if (busy) return;                                                                   
        if (changed) _reload(st, opts.focus);                                                  
        else _focusTrigger(st, opts.focus, opts.trigger);
      }
    });
    opts.cancel.addEventListener('click', function () { PCV.modalCore.closeDialog(dlg); });
    opts.cancel.focus();                                                                  
    opts.ok.addEventListener('click', async function () {
      if (busy || opts.ok.disabled) return;
      busy = true;
      opts.ok.style.minWidth = opts.ok.getBoundingClientRect().width + 'px';
      opts.ok.disabled = true;                                                                   
      opts.ok.setAttribute('aria-busy', 'true');
      PCV.uxlib.clearEl(opts.alertBox);
      var res = await opts.send();
      busy = false;
      changed = true;
      opts.ok.removeAttribute('aria-busy');
      if (res.error) {
        if (closed) { _result(st, false, _failText(res.error)); _reload(st, opts.focus); return; }
        opts.alertBox.appendChild(PCV.uxlib.el('strong', null, _prefix(res.error.code)));
        opts.alertBox.appendChild(PCV.uxlib.el('div', { class: 'hd-alert-msg' }, _errMessage(res.error)));
        opts.sync();                                                                     
        return;
      }
      _result(st, true, opts.done(res.result));
      if (closed) { _reload(st, opts.focus); return; }
      PCV.modalCore.closeDialog(dlg);                                                
    });
    return dlg;
  }

  function _footer(cancel, ok) {
    return PCV.uxlib.el('div', { class: 'hd-foot' }, cancel, ok);
  }
  function _vmOptions(vms, gpu) {
    var el = PCV.uxlib.el;
    return [el('option', { value: '' }, _L('VM 선택', 'Select VM'))].concat(vms.map(function (v) {
      var blocked = gpu && v.active;
      return el('option', { value: String(v.name), disabled: blocked ? 'disabled' : null },
        String(v.name) + ' (' + (v.active ? _L('실행 중', 'running') : _L('꺼짐', 'off')) + ')' + (blocked ? ' ' + _L('(끈 뒤 가능)', '(stop it first)') : ''));
    }));
  }

                              

  function _openAssign(st, row, trigger, focus) {
    var el = PCV.uxlib.el;
    var gpu = row.kind === 'pci';
    var vms = (st.data && st.data.vms) || [];
    var name = _name(row);
    var traits = row.traits || [];
    var has = function (t) { return traits.indexOf(t) >= 0; };
    var select = el('select', { id: 'hd-assign-vm', class: 'input' }, _vmOptions(vms, gpu));
    var persist = el('input', { type: 'checkbox', id: 'hd-assign-persist' });
    persist.checked = true;
    var hint = el('p', { class: 'hd-hint' });
    var guide = el('p', { class: 'hd-guide' });
                                                                           
    var inputRisk = !gpu && (has('keyboard') || has('mouse')) && !!row.host_driver_bound;
    var storageRisk = !gpu && has('storage');
    var risks = [];
    var confirm = null;
    if (inputRisk) risks.push(el('p', { class: 'hd-risk' }, _L('이 장치는 지금 호스트의 키보드·마우스로 쓰이고 있습니다. VM에 넘기면 호스트에서 이 장치로 입력할 수 없습니다.', 'The host is using this device as a keyboard/mouse right now. Once given to a VM, the host cannot take input from it.')));
    if (storageRisk) risks.push(el('p', { class: 'hd-risk' }, _L('호스트가 이 저장 장치를 쓰고 있으면 데이터가 손상될 수 있습니다.', 'If the host is using this storage device, data may be corrupted.')));
    if (inputRisk || storageRisk) {
      confirm = el('input', { type: 'checkbox', id: 'hd-assign-confirm' });
      risks.push(el('label', { class: 'hd-check' }, confirm, ' ', _L('호스트에서 이 장치를 쓰지 않는다는 것을 확인했습니다', 'I confirmed the host does not use this device')));
    }
    if (!gpu && has('bluetooth')) risks.push(el('p', { class: 'hd-risk' }, _L('호스트 블루투스가 끊깁니다.', 'Host Bluetooth will disconnect.')));
    var alertBox = el('div', { class: 'hd-alert', role: 'alert' });
    var cancel = el('button', { type: 'button', class: 'btn' }, _L('취소', 'Cancel'));
    var ok = el('button', { type: 'button', class: 'btn btn-primary', disabled: 'disabled' }, _L('할당', 'Assign'));
    var userPersist = true;                                                                  

    function chosen() {
      return vms.filter(function (v) { return String(v.name) === select.value; })[0] || null;
    }
    function sync() {
      var vm = chosen();
      var hintText = '';
      if (gpu) { persist.checked = true; persist.disabled = true; hintText = _L('GPU는 정지 VM의 영구 정의에만 할당되고 다음 시작 때 붙습니다', 'A GPU is assigned only to a stopped VM’s persistent definition and attaches at the next start'); }
      else if (vm && !vm.active) { persist.checked = true; persist.disabled = true; hintText = _L('꺼진 VM은 영구 정의에만 붙일 수 있습니다', 'A stopped VM can only take it in its persistent definition'); }
      else if (vm && !vm.persistent) { persist.checked = false; persist.disabled = true; hintText = _L('이 VM은 영구 정의가 없어 지금만 붙습니다', 'This VM has no persistent definition, so it attaches only for now'); }
      else { persist.disabled = false; persist.checked = userPersist; }
      hint.textContent = hintText;
      guide.textContent = gpu ? '' : (vm && vm.active && !persist.checked)
        ? _L('지금만 붙고 VM을 끄면 빠집니다', 'Attaches now only and is removed when the VM stops')
        : _L('실행 중인 VM이면 지금 바로 붙고, 정지된 VM이면 다음 시작 때 붙습니다.', 'A running VM gets it right away; a stopped VM gets it at the next start.');
      ok.disabled = !vm || (gpu && vm.active) || (confirm !== null && !confirm.checked);
    }
    select.addEventListener('change', sync);
    persist.addEventListener('change', function () { if (!persist.disabled) userPersist = persist.checked; sync(); });
    if (confirm) confirm.addEventListener('change', sync);
    sync();

    _actionDialog({
      st: st, trigger: trigger, focus: focus, ok: ok, cancel: cancel, alertBox: alertBox, sync: sync,
      body: [
        el('h2', null, _L(name + ' ' + _idText(row) + '를 어느 VM에?', 'Assign ' + name + ' ' + _idText(row) + ' to which VM?')),
        el('div', { class: 'fr' }, el('label', { for: 'hd-assign-vm' }, 'VM'), select),
        el('label', { class: 'hd-check' }, persist, ' ', _L('영구로 저장 — VM을 다시 켜도 붙어 있음', 'Save persistently — stays attached after the VM restarts')),
        hint, guide, risks, alertBox, _footer(cancel, ok)
      ],
      send: function () {
        var vm = select.value;
        return gpu ? _gpuAttach(vm, row) : _usbAttach(vm, row, persist.checked);
      },
      done: function (res) {
        var vm = select.value;
        return gpu
          ? _L(vm + '에 GPU 그룹을 할당했습니다(다음 시작 때 붙음)', 'Assigned the GPU group to ' + vm + ' (attaches at next start)')
          : _L(vm + '에 붙였습니다', 'Attached to ' + vm) + _usbOutcome(res);
      }
    });
  }

                                                 

  function _openRelease(st, row, vm, trigger, focus) {
    var el = PCV.uxlib.el;
    var gpu = row.kind === 'pci';
    var name = _name(row);
    var vis = (row.assignments || []).filter(function (a) { return a.vm_visible && a.vm; });
    var text = el('p', { class: 'hd-confirm' });
    var picker = null;
    if (!vm) {                                                                              
      picker = el('select', { id: 'hd-release-vm', class: 'input' }, _vmOptions(vis.map(function (a) {
        return { name: a.vm, active: a.vm_active };
      }), gpu));
    }
    var alertBox = el('div', { class: 'hd-alert', role: 'alert' });
    var cancel = el('button', { type: 'button', class: 'btn' }, _L('취소', 'Cancel'));
    var ok = el('button', { type: 'button', class: 'btn btn-r', disabled: 'disabled' }, _L('해제', 'Release'));
    var addrs = gpu ? [_short(row.pci_addr)].concat((row.companions || []).map(function (c) { return _short(c.pci_addr); })).join(', ') : '';
    function target() { return vm || (picker ? picker.value : ''); }
    function sync() {
      var v = target();
      text.textContent = !v
        ? _L('뗄 VM을 고르세요.', 'Choose the VM to release it from.')
        : gpu
          ? _L('정지된 ' + v + '의 영구 정의에서 GPU 그룹(' + addrs + ')을 뺍니다. 호스트 드라이버 바인딩은 바꾸지 않습니다.', 'Removes the GPU group (' + addrs + ') from the persistent definition of the stopped VM ' + v + '. Host driver binding is not changed.')
          : _L(v + '에서 ' + name + '(' + row.id + ')를 뗍니다. 실행 중이면 게스트에서 바로 사라지고, 영구 정의에서도 빠집니다.', 'Detaches ' + name + ' (' + row.id + ') from ' + v + '. If it is running, the guest loses it immediately, and it is removed from the persistent definition too.');
      ok.disabled = !v;
    }
    if (picker) picker.addEventListener('change', sync);
    sync();
    _actionDialog({
      st: st, trigger: trigger, focus: focus, ok: ok, cancel: cancel, alertBox: alertBox, sync: sync,
      body: [
        el('h2', null, _L('장치 해제', 'Release device')),
        picker ? el('div', { class: 'fr' }, el('label', { for: 'hd-release-vm' }, 'VM'), picker) : null,
        text, alertBox, _footer(cancel, ok)
      ],
      send: function () { return gpu ? _gpuDetach(target(), row) : _usbDetach(target(), row); },
      done: function () {
        return gpu
          ? _L(target() + '의 영구 정의에서 GPU 그룹을 뺐습니다', 'Removed the GPU group from ' + target() + '’s persistent definition')
          : _L(target() + '에서 ' + name + '을(를) 뗐습니다', 'Detached ' + name + ' from ' + target());
      }
    });
  }

                                                  

  async function _attachNow(st, row, vm, button, focus) {
    if (button.disabled) return;
    button.style.minWidth = button.getBoundingClientRect().width + 'px';
    button.disabled = true;                                               
    button.setAttribute('aria-busy', 'true');
    var a = (row.assignments || []).filter(function (x) { return x.vm === vm; })[0] || {};
    var res = await _usbAttachNow(vm, row, a.startup_policy);
    if (res.error) _result(st, false, _failText(res.error));
    else _result(st, true, _L(vm + '에 지금 붙였습니다', 'Attached to ' + vm + ' now') + _usbOutcome(res.result));
    _reload(st, focus);                                                                 
  }

  window.renderHostDevices = renderHostDevices;
  PCV.hostDevices = {
    render: renderHostDevices,
    describe: describe,
    actionState: actionState,
    counts: counts
  };
})(window.PCV);
