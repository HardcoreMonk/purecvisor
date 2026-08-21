                                                                  
                                   
                                        
                                                       
                                                                     
  
                                                         
  
                           
                                                                                                   
  
                                                                       
                                                            
                                                     
                                            
  
                                                                                      
                                                                                   
                                                                                    
  
                                                                                 
                                                                            
                                                                                      
                                                         
                                                    
                               
  
                                                             
                                                                                  
                                                                          
                                                                              
                     
  
                                                             
                                                      
                                                              
   
window.PCV = window.PCV || {};
(function(PCV) {
  'use strict';

  var API = PCV.api || {};
  var currentEventId = '';
  var lastEvents = [];
  var suricataBusy = false;

    
                                                                                
                                                                           
                                 
     
                                                                     
                                                                 
                                                        
                                
    
                                                                     
                                                               
                                                                  
                                              
    
                                                       
                                   

     
                                                                         
    
                                                                  
                                                 
                                                                  
                                           
                                                                
                                          
                                                                      
                                                                        
     
  async function rpc(method, params) {
    var body = {
      jsonrpc: '2.0',
      method: method,
      params: params || {},
      id: 'sec-' + Date.now()
    };
    var json = API.fetchPost
      ? await API.fetchPost(EP.RPC(), body)
      : await fetchPost(EP.RPC(), body);
    if (json && json.error) {
      throw new Error(json.error.message || 'RPC failed');
    }
    return API.unwrapData ? API.unwrapData(json) : (json.result || json.data || json);
  }

     
                                                      
                                                              
                                                       
                                                     
     
  async function rest(method, url, body) {
    var name = method === 'GET' ? 'fetchGet'
      : method === 'PUT' ? 'fetchPut'
      : method === 'DELETE' ? 'fetchDelete' : 'fetchPost';
    var fn = API[name] || window[name];
    if (typeof fn !== 'function') throw new Error('API transport unavailable');
    var json = method === 'GET' ? await fn(url) : await fn(url, body || {});
    if (json && json.error) {
      throw new Error(json.error.message || 'REST request failed');
    }
    var unwrap = API.unwrapData || window.unwrapData;
    return typeof unwrap === 'function'
      ? unwrap(json) : (json && (json.data !== undefined ? json.data : json.result)) || json;
  }

  async function loadSuricataRuntime() {
    var loaded = await Promise.all([
      rest('GET', EP.SURICATA_STATUS()),
      rest('GET', EP.SURICATA_POLICY()),
      rest('GET', EP.SURICATA_IPS_STATUS()),
      rest('GET', EP.SURICATA_IPS_DROP())
    ]);
    return {
      status: loaded[0] || {},
      policy: loaded[1] || {},
      ips: loaded[2] || {},
      drop: loaded[3] || {}
    };
  }

     
                                                              
    
                                                                                   
                                                                      
                                                        
                                                          
    
                                                        
                                                    
    
                   
                                                        
                                                             
                                                             
                                                       
                                            
     
  function canRole(role) {
    return typeof pcvRoleAllows === 'function' ? pcvRoleAllows(role) : false;
  }

  function notify(msg, ok) {
                                                     
                                                        
                                 
    if (typeof toast === 'function') {
      try { toast(msg, ok !== false); } catch (e) {                                     }
    }
  }

  async function askConfirm(msg) {
    if (typeof customConfirm === 'function') {
      return customConfirm(_L('Suricata 변경 확인', 'Confirm Suricata change'), msg);
    }
    return window.confirm(msg);
  }

  function asArray(value, key) {
    if (Array.isArray(value)) return value;
    if (value && Array.isArray(value[key])) return value[key];
    return [];
  }

  function formatTs(ts) {
    var n = Number(ts || 0);
    if (!n) return '-';
    return new Date(n * 1000).toLocaleString();
  }

  function upper(v) {
    return String(v || '').toUpperCase();
  }

                                                                  
                                                                                           
                                                                    
                                                          
                                         
  function badgeSeverity(sev) {
    if (sev === 'crit') return HN.statusPill('crit', 'CRIT');
    if (sev === 'warn') return HN.statusPill('warn', 'WARN');
    return HN.statusPill('idle', 'INFO');                                    
  }

  function badgeStatus(status) {
    if (status === 'resolved') return HN.statusPill('ok', 'RESOLVED');
    if (status === 'suppressed') return HN.statusPill('idle', 'SUPPRESSED');
    if (status === 'action_pending') return HN.statusPill('warn', 'PENDING');
    if (status === 'open') return HN.statusPill('warn', 'OPEN');
    return HN.statusPill('idle', upper(status || '-'));
  }

  function statusCard(title, value, hint, type) {
    var el = PCV.uxlib.el;
    var cls = type === 'r' ? 'color-red' : type === 'y' ? 'color-yellow' : 'color-green';
    return HN.card(title, [
      el('div', { class: 'stat-md ' + cls }, value),
      el('div', { class: 'stat-label mt-4' }, hint || '')
    ], 'text-center');
  }

  function renderStatusBar(status, pendingCount) {
      
                                                                                
                                                                                    
       
    var el = PCV.uxlib.el;
    var guard = status && status.enabled ? 'enabled' : 'disabled';
    var baseline = (status && status.baseline_status) || 'unknown';
    var degraded = status && status.degraded;
    var guardHint = guard === 'enabled'
      ? _L('탐지 경로 활성', 'Detection path active')
      : _L('admin opt-in 필요', 'admin opt-in required');
    var baselineHint = baseline === 'trusted'
      ? _L('관리자 refresh 완료', 'admin refresh completed')
      : _L('자동 trusted 전환 없음', 'never auto-trusted');
    var risk = String((status && status.open_risk) || 0);
    var pending = String((status && status.pending_actions) || pendingCount || 0);

    var grid = HN.grid(4,
      statusCard('Guard', PCV.uxlib.frag(HN.statusDot(guard === 'enabled' ? 'ok' : 'crit'), ' ' + upper(guard)), guardHint, guard === 'enabled' ? 'g' : 'r'),
      statusCard('Baseline', PCV.uxlib.frag(HN.statusDot(baseline === 'trusted' ? 'ok' : 'warn'), ' ' + upper(baseline)), baselineHint, baseline === 'trusted' ? 'g' : 'y'),
      statusCard(_L('Open Risk', 'Open Risk'), risk, degraded ? _L('store degraded', 'store degraded') : _L('CRIT/WARN 가중치', 'CRIT/WARN weighted'), Number(risk) > 0 ? 'r' : 'g'),
      statusCard(_L('Pending Actions', 'Pending Actions'), pending, _L('수동 승인 전용', 'manual approval only'), Number(pending) > 0 ? 'y' : 'g'));

    var controlsRow = canRole('admin')
      ? el('label', { class: 'text-xs', style: 'display:flex;align-items:center;gap:6px' },
          el('input', { type: 'checkbox', 'data-sec-enabled': '1', checked: guard === 'enabled' ? '' : null }),
          ' ' + _L('Security Guard 활성', 'Enable Security Guard'))
      : el('span', { class: 'stat-label' }, _L('Guard 변경은 admin 권한이 필요합니다.', 'Changing Guard state requires admin.'));

    var bar = el('div', { class: 'hc mb-12' },
      el('div', { class: 'flex gap-8 flex-wrap items-center' },
        el('button', { class: 'btn', type: 'button', 'data-sec-refresh': '1' }, '⟳ ' + _L('새로고침', 'Refresh')),
        controlsRow));

    return [grid, bar];
  }

  function suricataStatePill(state, degraded) {
    var value = String(state || 'unknown').toLowerCase();
    if (degraded) return HN.statusPill('crit', 'DEGRADED');
    if (value === 'active' || value === 'running' || value === 'enabled')
      return HN.statusPill('ok', upper(value));
    if (value === 'inactive' || value === 'failed' || value === 'disabled')
      return HN.statusPill(value === 'failed' ? 'crit' : 'idle', upper(value));
    return HN.statusPill('warn', upper(value));
  }

  function renderSuricataRuntime(view) {
    var el = PCV.uxlib.el;
    var section = el('section', { id: 'suricata-runtime', class: 'mb-12', 'aria-live': 'polite' });
    var heading = el('div', { class: 'flex items-center gap-8 mb-8 flex-wrap' },
      el('h3', { style: 'margin:0' }, 'Suricata Runtime'),
      el('button', { class: 'btn', type: 'button', 'data-suri-refresh': '1',
        'aria-label': _L('Suricata 상태 새로고침', 'Refresh Suricata state') }, '⟳ ' + _L('새로고침', 'Refresh')));
    section.appendChild(heading);

    if (view && view.restricted) {
      section.appendChild(el('div', { class: 'hc' },
        el('div', { class: 'flex gap-8 items-center flex-wrap' },
          HN.statusPill('idle', 'ADMIN ONLY'),
          el('span', { class: 'color-muted text-12' },
            _L('Suricata 정책·상태는 전 테넌트 트래픽 관찰 정보라 ADMIN에게만 공개됩니다.',
               'Suricata policy and state expose cross-tenant traffic data and are ADMIN-only.')))));
      return section;
    }
    if (!view || view.error) {
      section.appendChild(el('div', { class: 'hc' },
        el('h4', { class: 'color-red' }, _L('Suricata 상태 로드 실패', 'Failed to load Suricata state')),
        el('p', { class: 'color-muted text-12' }, (view && view.error) || _L('상태를 알 수 없습니다.', 'State unavailable.'))));
      return section;
    }

    var status = view.status || {};
    var policy = view.policy || {};
    var ips = view.ips || {};
    var dropSids = asArray(view.drop, 'sids');
    var engine = status.engine || {};
    var rules = status.rules || {};
    var update = rules.update || {};
    var ipsEngine = ips.engine || {};
    var lastToggle = ips.last_toggle || {};
    var pending = Boolean(update.running || lastToggle.running);
    section.setAttribute('data-suri-pending', pending ? '1' : '0');
    var engineState = engine.binary_present === false ? 'unavailable' : (engine.state || 'unknown');
    var ipsState = ips.degraded ? 'degraded'
      : ips.enabled ? (ipsEngine.state || 'enabled') : 'disabled';

    section.appendChild(HN.grid(4,
      statusCard('IDS Engine', suricataStatePill(engineState, false),
        engine.binary_present === false ? _L('바이너리 미설치', 'binary absent') : _L('eve 인제스트 상태', 'EVE ingest state'),
        engineState === 'active' ? 'g' : 'y'),
      statusCard(_L('탐지 룰', 'Detection Rules'), String(rules.count || 0),
        update.running ? _L('갱신 진행 중', 'update in progress')
          : update.last_result ? _L('마지막 결과: ', 'last result: ') + update.last_result
          : _L('갱신 이력 없음', 'no update history'), update.running ? 'y' : 'g'),
      statusCard(_L('정책 모드', 'Policy Mode'),
        HN.statusPill(policy.auto_isolate === 'enforce' ? 'warn' : 'idle', upper(policy.auto_isolate || 'dry_run')),
        _L('등록 테넌트 ', 'registered tenants ') + String(Object.keys(policy.tenants || {}).length),
        policy.auto_isolate === 'enforce' ? 'y' : 'g'),
      statusCard('IPS', suricataStatePill(ipsState, Boolean(ips.degraded)),
        lastToggle.running ? _L('변경 진행 중', 'change in progress')
          : 'NFQUEUE ' + String(ips.queue_num == null ? '-' : ips.queue_num) + ' · ' + String(ips.mode || '-'),
        ips.degraded ? 'r' : ips.enabled ? 'g' : 'y')));

    var policyControls = canRole('admin')
      ? el('div', { class: 'flex gap-8 items-center flex-wrap mt-10' },
          el('label', { class: 'text-xs' }, _L('자동 격리', 'Auto isolation'), ' ',
            el('select', { id: 'suri-policy-mode', class: 'input-pcv', disabled: pending ? 'disabled' : null },
              el('option', { value: 'dry_run', selected: policy.auto_isolate !== 'enforce' ? 'selected' : null }, 'DRY RUN'),
              el('option', { value: 'enforce', selected: policy.auto_isolate === 'enforce' ? 'selected' : null }, 'ENFORCE'))),
          el('button', { class: 'btn btn-primary', type: 'button', 'data-suri-policy-save': '1',
            'data-suri-action': '1', disabled: pending ? 'disabled' : null }, _L('정책 저장', 'Save policy')))
      : el('p', { class: 'color-muted text-12' }, _L('ADMIN 전용 제어입니다.', 'Controls require ADMIN.'));
    var rulesControls = canRole('admin')
      ? el('div', { class: 'mt-12' },
          el('label', { class: 'text-xs', for: 'suri-rules-url' }, _L('룰 갱신 URL', 'Rules update URL')),
          el('div', { class: 'flex gap-8 items-center flex-wrap mt-4' },
            el('input', { id: 'suri-rules-url', class: 'input-pcv', type: 'url',
              value: update.last_url || '', placeholder: 'https://example/rules.tar.gz',
              style: 'flex:1;min-width:220px', disabled: pending ? 'disabled' : null }),
            el('button', { class: 'btn', type: 'button', 'data-suri-rules-update': '1',
              'data-suri-action': '1', disabled: pending ? 'disabled' : null }, _L('룰 갱신', 'Update rules'))))
      : null;

    var ipsAction = ips.enabled ? 'disable' : 'enable';
    var ipsControls = canRole('admin')
      ? el('button', { class: ips.enabled ? 'btn btn-r' : 'btn btn-primary', type: 'button',
          'data-suri-ips': ipsAction, 'data-suri-action': '1', disabled: pending ? 'disabled' : null },
          ips.enabled ? _L('IPS 비활성화', 'Disable IPS') : _L('IPS 활성화', 'Enable IPS'))
      : el('span', { class: 'color-muted text-12' }, _L('ADMIN 전용', 'ADMIN only'));

    var dropRows = dropSids.map(function(sid) {
      return el('tr', null,
        el('td', { class: 'nowrap' }, el('code', null, String(sid))),
        el('td', { class: 'text-right' }, canRole('admin')
          ? el('button', { class: 'btn', type: 'button', 'data-suri-drop-remove': String(sid),
              'data-suri-action': '1', disabled: pending ? 'disabled' : null }, _L('제거', 'Remove'))
          : ''));
    });
    var dropBody = dropRows.length
      ? el('div', { style: 'overflow-x:auto;max-width:100%' },
          el('table', { class: 'data-table text-11', style: 'min-width:320px' },
            el('thead', null, el('tr', null, el('th', null, 'SID'), el('th', { class: 'text-right' }, _L('조작', 'Action')))),
            el('tbody', null, dropRows)))
      : el('div', { class: 'empty-state p-20 text-center' },
          el('div', { class: 'empty-title' }, _L('차단 SID 없음', 'No drop SIDs')),
          el('div', { class: 'empty-desc' }, _L('기본은 탐지만 하며 자동 차단하지 않습니다.', 'Detection is the default; no rules are blocked automatically.')));
    var dropAdd = canRole('admin')
      ? el('div', { class: 'flex gap-8 items-center flex-wrap mb-8' },
          el('label', { class: 'text-xs', for: 'suri-drop-sids' }, _L('추가할 SID', 'SIDs to add')),
          el('input', { id: 'suri-drop-sids', class: 'input-pcv', inputmode: 'numeric',
            placeholder: '2034647, 2034648', disabled: pending ? 'disabled' : null }),
          el('button', { class: 'btn btn-r', type: 'button', 'data-suri-drop-add': '1',
            'data-suri-action': '1', disabled: pending ? 'disabled' : null }, _L('차단 추가', 'Add drop SIDs')))
      : null;

    section.appendChild(el('div', { class: 'sg grid-2' },
      HN.card(_L('탐지 정책·룰', 'Detection Policy & Rules'), [policyControls, rulesControls]),
      HN.card(_L('IPS Enforcement', 'IPS Enforcement'), [
        HN.row(_L('엔진', 'Engine'), suricataStatePill(ipsEngine.state || 'unknown', Boolean(ips.degraded))),
        HN.row(_L('실효 모드', 'Effective mode'), ips.degraded ? 'fail-open (degraded)' : String(ips.mode || '-')),
        HN.row(_L('설정', 'Configuration'), (ips.fail_open ? 'fail-open' : 'fail-closed') + ' · queue ' + String(ips.queue_num == null ? '-' : ips.queue_num)),
        el('div', { class: 'mt-10' }, ipsControls)
      ]),
      HN.card(_L('Drop SID List', 'Drop SID List'), [dropAdd, dropBody]),
      HN.card(_L('비동기 작업', 'Async Operations'), [
        HN.row(_L('룰 갱신', 'Rules update'), update.running ? HN.statusPill('warn', 'PENDING') : HN.statusPill(update.last_result === 'fail' ? 'crit' : 'idle', upper(update.last_result || 'IDLE'))),
        HN.row(_L('IPS 변경', 'IPS change'), lastToggle.running ? HN.statusPill('warn', 'PENDING') : HN.statusPill(lastToggle.result === 'fail' ? 'crit' : 'idle', upper(lastToggle.result || 'IDLE'))),
        lastToggle.error ? el('p', { class: 'color-red text-12 mt-8' }, lastToggle.error) : null,
        update.last_error ? el('p', { class: 'color-red text-12 mt-8' }, update.last_error) : null
      ])
    ));
    return section;
  }

  function renderFilters(events) {
    var el = PCV.uxlib.el;
                                                                 
                                                                            
                                                                     
                                                                           
    var counts = { sev: {}, src: {}, st: {} };
    events.forEach(function(ev) {
      var sv = String(ev.severity || 'info');
      counts.sev[sv] = (counts.sev[sv] || 0) + 1;
      var sc = String(ev.source || '');
      if (sc) counts.src[sc] = (counts.src[sc] || 0) + 1;
      var st = String(ev.status || '');
      if (st) counts.st[st] = (counts.st[st] || 0) + 1;
    });
    var SEV = ['crit', 'warn', 'info'];
    var sevExtra = Object.keys(counts.sev).filter(function(v) { return SEV.indexOf(v) === -1; }).sort();
    var fbar = HN.filterBar([
      { key: 'secsev', label: _L('심각도', 'Severity'), options: SEV.concat(sevExtra).map(function(v) {
          return { value: v, label: v.toUpperCase(), count: counts.sev[v] || 0,
                   sw: v === 'crit' ? 'crit' : v === 'warn' ? 'warn' : 'idle' };
        }) },
      { key: 'secsrc', label: _L('소스', 'Source'), options: Object.keys(counts.src).sort().map(function(v) {
          return { value: v, label: v, count: counts.src[v] };
        }) },
      { key: 'secstatus', label: _L('상태', 'Status'), options: Object.keys(counts.st).sort().map(function(v) {
          return { value: v, label: v.toUpperCase(), count: counts.st[v] };
        }) }
    ]);
    return el('div', { class: 'hc mb-12' },
      fbar,
      el('label', { class: 'text-xs', style: 'display:block;margin-top:8px' }, _L('검색', 'Search'),
        el('input', { id: 'sec-filter-q', class: 'input-pcv', type: 'search', placeholder: 'target, summary, event_id', style: 'width:100%' })));
  }

     
                                                        
    
                                                                 
                                                                  
                                                          
                                                 
                                                                     
                                                                    
                                                      
                                                               
                                                       
     
  function renderEventTable(events) {
    var el = PCV.uxlib.el;
    if (!events.length) {
      return el('div', { class: 'empty-state p-20 text-center' },
        el('div', { class: 'empty-title' }, _L('보안 이벤트 없음', 'No security events')),
        el('div', { class: 'empty-desc' }, _L('WARN/CRIT 이벤트는 audit target과 event_id를 공유합니다.', 'WARN/CRIT events share audit target with event_id.')));
    }

    var rows = events.map(function(ev) {
      var text = [
        ev.event_id, ev.source, ev.status, ev.target, ev.summary, ev.recommended_action
      ].join(' ').toLowerCase();
      return el('tr', {
        class: 'sec-event-row',
        'data-sec-event-id': ev.event_id || '',
        'data-sec-severity': ev.severity || '',
        'data-sec-source': ev.source || '',
        'data-sec-status': ev.status || '',
        'data-sec-text': text,
        tabindex: '0',
        style: 'cursor:pointer'
      },
        el('td', { class: 'color-muted nowrap' }, formatTs(ev.timestamp)),
        el('td', null, badgeSeverity(ev.severity || 'info')),
        el('td', null, ev.source || '-'),
        el('td', null, el('code', { class: 'text-xs' }, ev.target || '-')),
        el('td', null, ev.summary || '-'),
        el('td', null, badgeStatus(ev.status || '-')),
        el('td', null, ev.recommended_action || '-'));
    });

    return el('div', { 'data-sec-table-scroll': '1', style: 'overflow:auto;max-height:560px;max-width:100%' },
      el('table', { class: 'data-table text-11' },
        el('thead', null, el('tr', null,
          el('th', null, _L('시간', 'Time')), el('th', null, _L('심각도', 'Severity')),
          el('th', null, _L('소스', 'Source')), el('th', null, _L('대상', 'Target')),
          el('th', null, _L('요약', 'Summary')), el('th', null, _L('상태', 'Status')),
          el('th', null, _L('권고', 'Action')))),
        el('tbody', null,
          rows,
          el('tr', { id: 'sec-filter-empty', style: 'display:none' },
            el('td', { colspan: '7', class: 'color-muted text-center p-12' }, _L('필터 조건과 일치하는 이벤트가 없습니다.', 'No events match the current filters.'))))));
  }

     
                                                 
    
                                                           
                                                                 
                              
    
                 
                                                         
                                                         
                                                        
                                                              
                                                            
                        
                                                                       
     
  function renderPendingActions(actions) {
    var el = PCV.uxlib.el;
    if (!actions.length) {
      return HN.card(_L('Pending Approval Queue', 'Pending Approval Queue'),
        el('div', { class: 'empty-state p-20 text-center' },
          el('div', { class: 'empty-title' }, _L('승인 대기 없음', 'No pending approvals'))),
        'min-w-0');
    }

    var rows = actions.map(function(action) {
      var name = action.action || '';
      var executable = name === 'block_ip' || name === 'revoke_api_key';
        
                                                                             
                                                                                 
         
      var controls = [];
      controls.push(executable
        ? (canRole('admin')
          ? el('button', { class: 'btn btn-r', type: 'button', 'data-sec-approve': action.event_id || '' }, _L('승인', 'Approve'))
          : el('span', { class: 'stat-label' }, _L('admin 승인 필요', 'admin approval required')))
        : el('span', { class: 'stat-label' }, _L('수동 runbook', 'manual runbook')));
      if (canRole('operator')) {
        controls.push(' ', el('button', { class: 'btn', type: 'button', 'data-sec-dismiss': action.event_id || '' }, _L('거부', 'Dismiss')));
      }
      return el('tr', { 'data-sec-action-event-id': action.event_id || '', style: 'cursor:pointer' },
        el('td', null, el('code', { class: 'text-xs' }, action.event_id || '-')),
        el('td', null, HN.statusPill(executable ? 'crit' : 'idle', (name || '-').toUpperCase())),
        el('td', null, el('code', { class: 'text-xs' }, action.target || '-')),
        el('td', null, String(action.ttl_sec || '-') + 's'),
        el('td', { class: 'nowrap' }, controls));
    });

    var body = el('div', { style: 'overflow:auto;max-height:260px;max-width:100%' },
      el('table', { class: 'data-table text-11' },
        el('thead', null, el('tr', null,
          el('th', null, 'event_id'), el('th', null, _L('대응', 'Action')), el('th', null, _L('대상', 'Target')),
          el('th', null, 'TTL'), el('th', null, _L('처리', 'Controls')))),
        el('tbody', null, rows)));
    return HN.card(_L('Pending Approval Queue', 'Pending Approval Queue'), body, 'min-w-0');
  }

  function renderVerificationCard() {
    var el = PCV.uxlib.el;
    return HN.card(_L('Verification Gates', 'Verification Gates'), [
      HN.row('Viewer', HN.statusPill('idle', 'READ')),
      HN.row('Operator', HN.statusPill('warn', 'DISMISS')),
      HN.row('Admin', HN.statusPill('crit', 'APPROVE')),
      HN.row('ADR-0018', HN.statusPill('idle', 'ASYNC')),
      HN.row('Audit', el('code', null, 'target = event_id'))
    ], 'min-w-0');
  }

  function renderShell(cfg, events, actions, suricata) {
                                                                  
                                                             
    var el = PCV.uxlib.el, frag = PCV.uxlib.frag;
    var statusBar = renderStatusBar(cfg || {}, actions.length);
    return frag(
      HN.pagehead({ title: _L('보안 이벤트', 'Security Events') }),
      statusBar,
      renderSuricataRuntime(suricata),
      renderFilters(events),
      el('div', { class: 'sg grid-2 mb-12' },
        HN.card(_L('Event Queue', 'Event Queue'), renderEventTable(events), 'min-w-0'),
        el('div', { id: 'security-detail', class: 'hc min-w-0' },
          el('h4', null, _L('Selected Event', 'Selected Event')),
          el('p', { class: 'color-muted text-12' }, _L('이벤트를 선택하세요.', 'Select an event.')))),
      el('div', { class: 'sg grid-2' }, renderPendingActions(actions), renderVerificationCard())
    );
  }

  function applyFilters(root) {
      
                                                                             
                                                 
       
    root = root || document;
    var fs = PCV.ui && PCV.ui.filterState;
    var cur = fs ? fs.current() : {};
    var sevs = cur.secsev || [];
    var srcs = cur.secsrc || [];
    var sts = cur.secstatus || [];
    var q = ((document.getElementById('sec-filter-q') || {}).value || '').trim().toLowerCase();
    var visible = 0;
    root.querySelectorAll('.sec-event-row').forEach(function(row) {
      var ok = (!sevs.length || sevs.indexOf(row.getAttribute('data-sec-severity') || 'info') >= 0)
        && (!srcs.length || srcs.indexOf(row.getAttribute('data-sec-source')) >= 0)
        && (!sts.length || sts.indexOf(row.getAttribute('data-sec-status')) >= 0)
        && (!q || (row.getAttribute('data-sec-text') || '').indexOf(q) >= 0);
      row.style.display = ok ? '' : 'none';
      if (ok) visible++;
    });
    var empty = document.getElementById('sec-filter-empty');
    if (empty) empty.style.display = visible ? 'none' : '';
  }

  function bindSecurityHandlers(root) {
    root.querySelectorAll('[data-sec-refresh]').forEach(function(btn) {
      btn.addEventListener('click', refresh);
    });
    root.querySelectorAll('[data-sec-event-id]').forEach(function(row) {
      row.addEventListener('click', function() {
        selectEvent(row.getAttribute('data-sec-event-id') || '');
      });
      row.addEventListener('keydown', function(e) {
        if (e.key === 'Enter' || e.key === ' ') {
          e.preventDefault();
          selectEvent(row.getAttribute('data-sec-event-id') || '');
        }
      });
    });
    root.querySelectorAll('[data-sec-action-event-id]').forEach(function(row) {
      row.addEventListener('click', function(e) {
        if (e.target && e.target.closest && e.target.closest('button')) return;
        selectEvent(row.getAttribute('data-sec-action-event-id') || '');
      });
    });
    root.querySelectorAll('[data-sec-approve]').forEach(function(btn) {
      btn.addEventListener('click', function() {
        approveAction(btn.getAttribute('data-sec-approve') || '');
      });
    });
    root.querySelectorAll('[data-sec-dismiss]').forEach(function(btn) {
      btn.addEventListener('click', function() {
        dismissAction(btn.getAttribute('data-sec-dismiss') || '');
      });
    });
    var q = root.querySelector('#sec-filter-q');
    if (q) q.addEventListener('input', function() { applyFilters(root); });
    var enabled = root.querySelector('[data-sec-enabled]');
    if (enabled) {
      enabled.addEventListener('change', function() {
        setGuardEnabled(Boolean(enabled.checked));
      });
    }
    root.querySelectorAll('[data-suri-refresh]').forEach(function(btn) {
      btn.addEventListener('click', refreshSuricata);
    });
    var savePolicy = root.querySelector('[data-suri-policy-save]');
    if (savePolicy) savePolicy.addEventListener('click', function() {
      var mode = ((root.querySelector('#suri-policy-mode') || {}).value || 'dry_run');
      runSuricataMutation('policy', { auto_isolate: mode },
        mode === 'enforce'
          ? _L('자동 격리를 ENFORCE로 바꾸면 탐지 이벤트가 네트워크 격리를 실제 수행합니다. 계속할까요?', 'ENFORCE allows detections to perform real network isolation. Continue?')
          : _L('자동 격리를 DRY RUN으로 바꾸어 실제 격리를 중단할까요?', 'Switch to DRY RUN and stop real automatic isolation?'));
    });
    var rulesUpdate = root.querySelector('[data-suri-rules-update]');
    if (rulesUpdate) rulesUpdate.addEventListener('click', function() {
      var url = ((root.querySelector('#suri-rules-url') || {}).value || '').trim();
      if (!url) { notify(_L('룰 URL을 입력하세요.', 'Enter a rules URL.'), false); return; }
      runSuricataMutation('rules', { url: url },
        _L('룰 파일을 다운로드·검증한 뒤 원자적으로 교체합니다. 갱신을 시작할까요?', 'Download, validate, and atomically replace the rules file?'));
    });
    root.querySelectorAll('[data-suri-ips]').forEach(function(btn) {
      var op = btn.getAttribute('data-suri-ips') || '';
      btn.addEventListener('click', function() {
        runSuricataMutation('ips-' + op, {}, op === 'enable'
          ? _L('IPS를 켜면 forward 트래픽이 NFQUEUE 검사 경로를 통과합니다. 활성화할까요?', 'Enabling IPS sends forwarded traffic through NFQUEUE inspection. Continue?')
          : _L('IPS를 끄면 인라인 차단이 중단됩니다. 비활성화할까요?', 'Disabling IPS stops inline blocking. Continue?'));
      });
    });
    var dropAddBtn = root.querySelector('[data-suri-drop-add]');
    if (dropAddBtn) dropAddBtn.addEventListener('click', function() {
      var sids = parseSuricataSids(((root.querySelector('#suri-drop-sids') || {}).value || ''));
      if (!sids) { notify(_L('양의 정수 SID를 쉼표로 구분해 입력하세요.', 'Enter comma-separated positive integer SIDs.'), false); return; }
      runSuricataMutation('drop-add', { sids: sids },
        _L('선택한 SID의 alert 룰이 drop으로 바뀌어 정상 트래픽도 차단할 수 있습니다. 추가할까요?', 'Selected alert rules will become drop rules and may block legitimate traffic. Continue?'));
    });
    root.querySelectorAll('[data-suri-drop-remove]').forEach(function(btn) {
      btn.addEventListener('click', function() {
        var sid = Number(btn.getAttribute('data-suri-drop-remove'));
        runSuricataMutation('drop-remove', { sids: [sid] },
          _L('SID ', 'SID ') + String(sid) + _L('의 인라인 차단을 해제할까요?', ' will no longer be blocked inline. Continue?'));
      });
    });
  }

                    
                                                                                  
                                                        
                                                                  
                                                      
  var _secUnsub = null;
  function _onSecFilterChange() {
    if (window.currentTab !== 'mon-security') {
      if (_secUnsub) { _secUnsub(); _secUnsub = null; }
      return;
    }
    applyFilters(document);
  }

     
                                                                
    
                                                                        
                                                 
                                                                
                                                             
                                                              
                                                        
     
  async function renderSecurityEvents(b) {
      
                                                                              
                                                                               
       
                                                           
                                                                   
                                         
    var prevQ = ((document.getElementById('sec-filter-q') || {}).value || '');
    showSkeleton(b);
    try {
      var suricataLoad = canRole('admin')
        ? loadSuricataRuntime().catch(function(e) { return { error: e.message || 'failed' }; })
        : Promise.resolve({ restricted: true });
      var loaded = await Promise.all([
        rpc('security.config.get', {}),
        rpc('security.event.list', { limit: 100 }),
        rpc('security.action.pending', {}),
        suricataLoad
      ]);
      var cfg = loaded[0] || {};
      var events = asArray(loaded[1], 'events');
      var actions = asArray(loaded[2], 'actions');
      var suricata = loaded[3];
      lastEvents = events;
      PCV.uxlib.clearEl(b);
      b.appendChild(renderShell(cfg, events, actions, suricata));
      var qEl = document.getElementById('sec-filter-q');
      if (qEl && prevQ) qEl.value = prevQ;
      bindSecurityHandlers(b);
      if (!_secUnsub && PCV.ui && PCV.ui.filterState && PCV.ui.filterState.subscribe) {
        _secUnsub = PCV.ui.filterState.subscribe(_onSecFilterChange);
      }
      applyFilters(b);
      if (currentEventId && events.some(function(ev) { return ev.event_id === currentEventId; })) {
        selectEvent(currentEventId);
      }
    } catch (e) {
                                                                    
                                                             
      var el = PCV.uxlib.el;
      PCV.uxlib.clearEl(b);
      b.appendChild(el('div', { class: 'hc' },
        el('h4', { class: 'color-red' }, _L('보안 이벤트 로드 실패', 'Failed to load security events')),
        el('p', { class: 'color-muted' }, e.message || ''),
        el('button', { class: 'btn', type: 'button', 'data-sec-refresh': '1' }, 'Retry')
      ));
      bindSecurityHandlers(b);
    }
  }

  function renderEventDetail(ev) {
                                                                  
    var el = PCV.uxlib.el;
    var action = ev.recommended_action || '';
    var executable = action === 'block_ip' || action === 'revoke_api_key';
    var controls = [];
    if (executable) {
      controls.push(canRole('admin')
        ? el('button', { class: 'btn btn-r', type: 'button', 'data-sec-approve': ev.event_id || '' }, _L('승인', 'Approve'))
        : el('span', { class: 'stat-label' }, _L('admin 승인 필요', 'admin approval required')));
    } else if (action === 'manual_runbook') {
      controls.push(el('span', { class: 'stat-label' }, _L('수동 runbook 후보입니다. 자동 실행하지 않습니다.', 'Manual runbook candidate. It will not execute automatically.')));
    }
    if (canRole('operator')) {
      controls.push(' ', el('button', { class: 'btn', type: 'button', 'data-sec-dismiss': ev.event_id || '' }, _L('거부', 'Dismiss')));
    }

    return PCV.uxlib.frag(
      el('h4', null, _L('Selected Event', 'Selected Event') + ' ', badgeSeverity(ev.severity || 'info')),
      el('div', { class: 'mb-8' }, el('b', null, ev.summary || ev.event_id || '-')),
      HN.row('event_id', el('code', null, ev.event_id || '')),
      HN.row(_L('소스', 'Source'), ev.source || '-'),
      HN.row(_L('대상 유형', 'Target Kind'), ev.target_kind || '-'),
      HN.row(_L('대상', 'Target'), el('code', { class: 'text-xs' }, ev.target || '-')),
      HN.row(_L('상태', 'Status'), badgeStatus(ev.status || '-')),
      HN.row(_L('신뢰도', 'Confidence'), PCV.uxlib.frag(
        HN.gauge({ value: Number(ev.confidence || 0), warn: 101, crit: 101, inline: true }),
        ' ' + String(ev.confidence || 0) + '%')),                                                  
      HN.row(_L('권고 대응', 'Recommended Response'), HN.statusPill(executable ? 'crit' : 'idle', (action || '-').toUpperCase())),
      HN.row(_L('감사 상관키', 'Audit Correlation'), el('code', null, 'security.event target=' + (ev.event_id || ''))),
      HN.row(_L('발생 횟수', 'Occurrences'), String(ev.occurrence_count || 1)),
      el('div', { class: 'mt-10 mb-8' }, el('b', { class: 'text-12' }, _L('Evidence', 'Evidence'))),
      el('pre', { class: 'stat-label', style: 'white-space:pre-wrap;overflow:auto;max-height:260px;margin:0' }, ev.evidence_json || '{}'),
      el('div', { class: 'flex gap-6 mt-10 flex-wrap' }, controls)
    );
  }

  async function selectEvent(eventId) {
    if (!eventId) return;
    currentEventId = eventId;
    var detail = document.getElementById('security-detail');
    if (!detail) return;
    showSkeleton(detail);
    try {
      var ev = await rpc('security.event.get', { event_id: eventId });
      PCV.uxlib.clearEl(detail);
      detail.appendChild(renderEventDetail(ev || {}));
      bindSecurityHandlers(detail);
      document.querySelectorAll('.sec-event-row').forEach(function(row) {
        row.classList.toggle('selected', row.getAttribute('data-sec-event-id') === eventId);
      });
    } catch (e) {
      PCV.uxlib.clearEl(detail);
      detail.appendChild(PCV.uxlib.el('p', { class: 'color-red' }, e.message || ''));
    }
  }

  function parseSuricataSids(text) {
    var parts = String(text || '').split(/[\s,]+/).filter(Boolean);
    if (!parts.length) return null;
    var seen = {};
    var values = [];
    for (var i = 0; i < parts.length; i++) {
      if (!/^\d+$/.test(parts[i])) return null;
      var value = Number(parts[i]);
      if (!Number.isSafeInteger(value) || value <= 0 || value > 4294967295) return null;
      if (!seen[value]) { seen[value] = true; values.push(value); }
    }
    return values;
  }

  function setSuricataControlsDisabled(disabled) {
    var host = document.getElementById('suricata-runtime');
    var serverPending = host && host.getAttribute('data-suri-pending') === '1';
    document.querySelectorAll('#suricata-runtime [data-suri-action]').forEach(function(control) {
      control.disabled = Boolean(disabled || serverPending);
    });
    if (host) host.setAttribute('aria-busy', disabled ? 'true' : 'false');
  }

  async function refreshSuricata() {
    var host = document.getElementById('suricata-runtime');
    if (!host || !canRole('admin')) return;
    setSuricataControlsDisabled(true);
    try {
      var view = await loadSuricataRuntime();
      var fresh = renderSuricataRuntime(view);
      host.replaceWith(fresh);
      bindSecurityHandlers(fresh);
    } catch (e) {
                                             
                                                 
      notify(e.message || _L('Suricata 상태 조회 실패', 'Failed to refresh Suricata state'), false);
      setSuricataControlsDisabled(false);
    }
  }

  async function runSuricataMutation(kind, payload, confirmMessage) {
    if (!canRole('admin')) {
      notify(_L('ADMIN 권한이 필요합니다.', 'ADMIN role required.'), false);
      return;
    }
    if (suricataBusy) {
      notify(_L('다른 Suricata 변경을 처리 중입니다.', 'Another Suricata change is in progress.'), false);
      return;
    }
    if (!await askConfirm(confirmMessage)) return;

    var request;
    if (kind === 'policy') request = function() { return rest('PUT', EP.SURICATA_POLICY(), payload); };
    else if (kind === 'rules') request = function() { return rest('POST', EP.SURICATA_RULES_UPDATE(), payload); };
    else if (kind === 'ips-enable') request = function() { return rest('POST', EP.SURICATA_IPS_ENABLE(), payload); };
    else if (kind === 'ips-disable') request = function() { return rest('POST', EP.SURICATA_IPS_DISABLE(), payload); };
    else if (kind === 'drop-add') request = function() { return rest('POST', EP.SURICATA_IPS_DROP(), payload); };
    else if (kind === 'drop-remove') request = function() { return rest('DELETE', EP.SURICATA_IPS_DROP(), payload); };
    else return;

    suricataBusy = true;
    setSuricataControlsDisabled(true);
    try {
      var result = await request();
      notify(result && result.status === 'started'
        ? _L('변경 요청이 접수됐습니다. 완료 상태를 재조회합니다.', 'Change accepted; refreshing completion state.')
        : _L('Suricata 변경을 저장했습니다.', 'Suricata change saved.'), true);
      await refreshSuricata();
    } catch (e) {
                                              
                                                
      notify(e.message || _L('Suricata 변경 실패', 'Suricata change failed'), false);
    } finally {
      suricataBusy = false;
      setSuricataControlsDisabled(false);
    }
  }

  function refresh() {
    var b = PCV.ui.renderTarget();                                 
    if (b) renderSecurityEvents(b);
  }

     
                                                                                   
    
                                                                  
                                                         
                                                               
                                               
    
                 
                                                      
                                                       
                                                        
                                                              
     
  async function setGuardEnabled(enabled) {
    if (!canRole('admin')) {
      notify(_L('admin 권한이 필요합니다.', 'admin role required'), false);
      refresh();
      return;
    }
    try {
      var _navGen = PCV.ui.navGen();
      await rpc('security.config.set', { enabled: enabled });
      notify(enabled ? _L('Security Guard 활성화', 'Security Guard enabled') : _L('Security Guard 비활성화', 'Security Guard disabled'), true);
      if (PCV.ui.navGen() === _navGen) refresh();
    } catch (e) {
      notify(e.message || 'failed', false);
      if (PCV.ui.navGen() === _navGen) refresh();
    }
  }

     
                                                                                
    
                                                                       
                                                                  
                                                                 
                                 
                                                               
                                                                  
                              
    
                 
                                                         
                                                           
                                                          
                                                              
     
  async function approveAction(eventId) {
    if (!eventId) return;
    if (!canRole('admin')) {
      notify(_L('admin 권한이 필요합니다.', 'admin role required'), false);
      return;
    }
      
                                                                                
                                                                             
       
    var _navGen = PCV.ui.navGen();
    if (!await askConfirm(_L('이 대응을 승인하시겠습니까?', 'Approve this response?') + '\n' + eventId)) return;
    try {
      await rpc('security.action.approve', { event_id: eventId });
      notify(_L('승인 요청됨', 'Approval accepted'), true);
      if (PCV.ui.navGen() === _navGen) refresh();
    } catch (e) {
      notify(e.message || 'failed', false);
    }
  }

     
                                                                               
    
                                                        
                                                             
    
                 
                                                       
                                                      
                                                                
                                                           
                                                    
     
  async function dismissAction(eventId) {
    if (!eventId) return;
    if (!canRole('operator')) {
      notify(_L('operator 권한이 필요합니다.', 'operator role required'), false);
      return;
    }
    try {
      var _navGen = PCV.ui.navGen();
      await rpc('security.action.dismiss', {
        event_id: eventId,
        reason: 'dismissed from UI'
      });
      notify(_L('거부 완료', 'Dismissed'), true);
      if (PCV.ui.navGen() === _navGen) refresh();
    } catch (e) {
      notify(e.message || 'failed', false);
    }
  }

  PCV.security = {
    render: renderSecurityEvents,
    refresh: refresh,
    selectEvent: selectEvent,
    approveAction: approveAction,
    dismissAction: dismissAction,
    setGuardEnabled: setGuardEnabled,
    _lastEvents: function() { return lastEvents.slice(); }
  };
})(window.PCV);
