                                                                  
                                                
                                                                   
                                                       
                                   
                                                           
                                                                 
                                                     
                                                                     
                                                                
  
                               
                                                           
                                                                           
                                                           
                                                     
                                              
                                                       
                                                         
                                                   
                                                           
                                                          
                                                                 
                                                           
                                                               
  
                                                                             
                                                                         
                                                    
  
                                            
                                                                             
                                                 
                                                                              
                                                     
                                    
  
                                                     
                                                     
   
window.PCV = window.PCV || {};
(function (PCV) {
  'use strict';

  var MQ = '(max-width: 600px)';
  var SCREEN_ID = 'm-screen';

                                                             
  function _t(ko, en) { return (typeof _L === 'function') ? _L(ko, en) : ko; }

                                                                 
                                                               
                                                               
  var TABS = [
    { id: 'home',    ko: '홈',   en: 'Home',    icon: 'ci-house-01' },
    { id: 'alerts',  ko: '알림', en: 'Alerts',  icon: 'ci-bell', badge: 'alerts' },
    { id: 'power',   ko: '전원', en: 'Power',   icon: 'ci-play' },
    { id: 'healing', ko: '치유', en: 'Healing', icon: 'ci-shield-check', badge: 'healing' }
  ];

  var _state = { activeTab: 'home', badges: { alerts: 0, healing: 0 } };

  var MOBILE_SVG_NS = 'http://www.w3.org/2000/svg';
  function _mobileIcon(symbol) {
    var svg = document.createElementNS(MOBILE_SVG_NS, 'svg');
    svg.setAttribute('class', 'ci-icon mnav-icon');
    svg.setAttribute('aria-hidden', 'true');
    svg.setAttribute('focusable', 'false');
    var use = document.createElementNS(MOBILE_SVG_NS, 'use');
    use.setAttribute('href', 'vendor/coolicons/coolicons.svg#' + symbol);
    svg.appendChild(use);
    return svg;
  }

                                                 
                                                                  
  function _pageHead(title, subtitle) {
    var el = PCV.uxlib.el;
    return el('header', { class: 'm-pagehead' },
      el('h1', { class: 'm-page-title' }, title),
      el('p', { class: 'm-page-subtitle' }, subtitle));
  }

                              
                                                               
                                                       
                                                    
                                                          
  function vmStatus(state) { return state === 'running' ? 'ok' : (state === 'paused' ? 'warn' : 'idle'); }
  function ctrStatus(state) { return state === 'RUNNING' ? 'ok' : 'idle'; }
  function sevStatus(sev) { return (sev === 'crit' || sev === 'critical') ? 'crit' : ((sev === 'warn' || sev === 'warning') ? 'warn' : 'idle'); }
                                                           
                                                 
  function _alertSev(a) { return HN.alertSeverity(a); }
  function _alertLabel(a) {
    var severity = _alertSev(a);
    return severity === 'crit' ? 'CRIT' : (severity === 'warn' ? 'WARN' : 'UNKNOWN');
  }

                                                              
                                                            
  function isActive() { return document.body.classList.contains('mshell'); }

                        
  function _buildNav() {
    var el = PCV.uxlib.el;
    var items = TABS.map(function (tab) {
      var badge = tab.badge
        ? el('span', { class: 'mnav-badge', 'data-badge': tab.badge, hidden: '' }, '0')
        : null;
      return el('button', {
        class: 'mnav-item' + (tab.id === _state.activeTab ? ' on' : ''),
        type: 'button', 'data-tab': tab.id, 'aria-label': _t(tab.ko, tab.en),
        onClick: function () { showTab(tab.id); }
      },
        _mobileIcon(tab.icon),
        el('span', { class: 'mnav-label' }, _t(tab.ko, tab.en)),
        badge);
    });
    return el.apply(null, ['nav', { class: 'mnav', 'aria-label': _t('모바일 탭', 'Mobile tabs') }].concat(items));
  }

  function _syncNavActive() {
    var items = document.querySelectorAll('.mnav .mnav-item');
    Array.prototype.forEach.call(items, function (it) {
      var on = it.getAttribute('data-tab') === _state.activeTab;
      it.classList.toggle('on', on);
      it.setAttribute('aria-current', on ? 'page' : 'false');
    });
  }

                                                
    
                                                 
                                                                    
                                                           
                                                               
                               
    
                                                                  
                                                      
                                                           
                                                
    
                                                     
                                                
  function _tabFromHash() {
    var h = (location.hash || '').replace(/^#\/?/, '');
    if (!h) return null;
    if (/alert/.test(h)) return 'alerts';
    if (/heal/.test(h)) return 'healing';
    return null;                                     
  }
                                                      
                                              
  var _hashConsumed = false;

                                                          
                                                               
  function mount() {
    if (document.getElementById(SCREEN_ID)) { _syncNavActive(); return; }
    document.body.classList.add('mshell');
    var scr = PCV.uxlib.el('div', { id: SCREEN_ID, class: 'mscreen', role: 'main' });
    document.body.appendChild(scr);
    document.body.appendChild(_buildNav());
    if (!_hashConsumed) {
      _hashConsumed = true;
      var deep = _tabFromHash();
      if (deep) _state.activeTab = deep;
    }
    showTab(_state.activeTab);                            
    refreshBadges();
  }

                                                               
                                                               
                                                               
  function unmount() {
    document.body.classList.remove('mshell');
    var scr = document.getElementById(SCREEN_ID);
    if (scr) scr.remove();
    var nav = document.querySelector('.mnav');
    if (nav) nav.remove();
  }

  function sync() {
    var phone = window.matchMedia(MQ).matches;
                                                                      
                                                                      
    var authed = !!window.authToken && !document.body.classList.contains('login-active');
    if (phone && authed && !document.getElementById(SCREEN_ID)) mount();
    else if ((!phone || !authed) && document.getElementById(SCREEN_ID)) unmount();
  }

                   
                                                                
                                                          
                                                            
                                   
  function showTab(id) {
    if (!SCREENS[id]) return;
    _state.activeTab = id;
    _syncNavActive();
    var scr = document.getElementById(SCREEN_ID);
    if (!scr) return;
    PCV.uxlib.clearEl(scr);
    SCREENS[id]();
  }

                                                      
                                                               
  function setBadges(counts) {
    counts = counts || {};
    if (counts.alerts != null) _state.badges.alerts = counts.alerts;
    if (counts.healing != null) _state.badges.healing = counts.healing;
    Object.keys(_state.badges).forEach(function (key) {
      var b = document.querySelector('.mnav .mnav-badge[data-badge="' + key + '"]');
      if (!b) return;
      var n = _state.badges[key] || 0;
      b.textContent = String(n);
      if (n > 0) b.removeAttribute('hidden'); else b.setAttribute('hidden', '');
    });
  }

                                         
  function _paint(node) {
    var scr = document.getElementById(SCREEN_ID);
    if (!scr) return;
    PCV.uxlib.clearEl(scr);
    scr.appendChild(node);
                                                                
                                                                    
                                                   
                                                                    
                                                           
    if (window.currentUser && typeof applyRoleVisibility === 'function') {
      applyRoleVisibility(window.currentUser.role);
    }
  }
                                                        
                                                             
  function _empty(text) {
    return PCV.uxlib.el('div', { class: 'mscreen-body' },
      PCV.uxlib.el('div', { class: 'mscreen-empty' }, text));
  }

                                                                 
  function buildHome(d) {
    var el = PCV.uxlib.el;
    d = d || { vms: [], containers: [], alerts: [], hostCpu: 0, hostMem: 0 };
    var vms = d.vms || [], ctrs = d.containers || [], alerts = d.alerts || [];
    var vmRun = vms.filter(function (v) { return v.state === 'running'; }).length;
    var ctrRun = ctrs.filter(function (c) { return c.state === 'RUNNING'; }).length;
    var critWarn = alerts.filter(function (a) { var s = _alertSev(a); return s === 'crit' || s === 'warn'; }).length;

    var bar = HN.statusBar([
      { count: vmRun, label: _t('VM 실행', 'VMs up'), status: vmRun === vms.length ? 'ok' : 'warn', sub: '/ ' + vms.length },
      { count: ctrRun, label: _t('컨테이너', 'Containers'), status: ctrRun === ctrs.length ? 'ok' : 'warn', sub: '/ ' + ctrs.length },
      { count: critWarn, label: _t('알림', 'Alerts'), status: critWarn > 0 ? 'crit' : 'ok', sub: _t('미해결', 'open') }
    ], { onNavigate: function (s) { if (s.label === _t('알림', 'Alerts')) showTab('alerts'); else showTab('power'); } });

    var cpu = Math.round(d.hostCpu || 0), mem = Math.round(d.hostMem || 0);
    var gauges = el('div', { class: 'sg grid-2' },
      HN.gauge({ value: cpu, warn: 80, crit: 95, unit: '%', label: 'CPU' }),
      HN.gauge({ value: mem, warn: 80, crit: 95, unit: '%', label: _t('메모리', 'Memory') }));

    var quick = el('div', { class: 'm-quick' },
      el('button', { class: 'btn', type: 'button', onClick: function () { showTab('power'); } }, _t('전원', 'Power')),
      el('button', { class: 'btn', type: 'button', onClick: function () { showTab('alerts'); } }, _t('알림', 'Alerts')),
      el('button', { class: 'btn btn-g', type: 'button', 'data-role': 'OPERATOR,ADMIN', onClick: function () { if (typeof window.showCreate === 'function') window.showCreate(); } }, _t('새 VM', 'New VM')));

    var recentItems = alerts.slice(-3).reverse().map(function (a) {
      var record = (a && typeof a === 'object' && !Array.isArray(a)) ? a : {};
      return el('div', { class: 'm-listcard' },
        HN.statusPill(_alertSev(record), _alertLabel(record)),
        el('span', { class: 'm-name' }, record.message || ''));
    });
    var recent = el('div', { class: 'm-recent' }, recentItems.length
      ? el.apply(null, ['div', { class: 'mscreen-body' }].concat(recentItems))
      : el('div', { class: 'm-hint' }, _t('최근 알림 없음', 'No recent alerts')));

    return el('div', { class: 'mscreen-body' },
      _pageHead(_t('운영 개요', 'Operations overview'),
        _t('Single Edge 노드 상태와 빠른 작업', 'Single Edge status and quick actions')),
      bar, gauges,
      el('div', { class: 'mscreen-section-title' }, _t('빠른 작업', 'Quick actions')), quick,
      el('div', { class: 'mscreen-section-title' }, _t('최근 알림', 'Recent alerts')), recent);
  }
                                                           
                                                      
                                                             
                                    
                                                                   
  function buildAlerts(d) {
    var el = PCV.uxlib.el;
    d = d || { alerts: [], suricata: null, ips: null, filter: [] };
    var alerts = d.alerts || [], filter = d.filter || [];

                                                                           
    var counts = { crit: 0, warn: 0 };
    alerts.forEach(function (a) { var s = _alertSev(a); if (s === 'crit') counts.crit++; else if (s === 'warn') counts.warn++; });
    var bar = HN.filterBar([{ key: 'severity', options: [
      { value: 'crit', label: _t('심각', 'Critical'), count: counts.crit, sw: 'crit' },
      { value: 'warn', label: _t('경고', 'Warning'), count: counts.warn, sw: 'warn' }
    ] }]);

                                                     
                                                    
    var pushCard = (PCV.push && PCV.push.toggleNode)
      ? el('div', { class: 'm-push m-listcard' },
          el('span', { class: 'm-name' }, _t('푸시 알림', 'Push alerts')),
          PCV.push.toggleNode({ id: 'm-push-toggle' }))
      : null;

    var shown = filter.length
      ? alerts.filter(function (a) { return filter.indexOf(_alertSev(a)) !== -1; })
      : alerts;
    var rows = shown.slice().reverse().map(function (a) {
      var record = (a && typeof a === 'object' && !Array.isArray(a)) ? a : {};
      var ackCell = record.acknowledged
        ? HN.statusPill('idle', 'ACK')
        : (typeof record.alert_id === 'number'
          ? el('button', {
              class: 'btn btn-sm',
              type: 'button',
              'data-ack-id': record.alert_id,
              'data-role': 'OPERATOR,ADMIN',
              onClick: function () { _ackAlertMobile(record.alert_id); }
            }, _t('확인', 'ACK'))
          : null);
      return el('div', { class: 'm-alert-row m-listcard' },
        HN.statusPill(_alertSev(record), _alertLabel(record)),
        el('div', { class: 'm-name m-copy' },
          el('div', null, record.message || ''),
          el('div', { class: 'm-hint', style: 'padding:0' }, (record.metric || '') + ' · ' + (typeof record.value === 'number' ? record.value.toFixed(1) + '%' : ''))),
        ackCell);
    });
    var list = rows.length
      ? el.apply(null, ['div', { class: 'mscreen-body' }].concat(rows))
      : el('div', { class: 'm-hint' }, _t('알림 없음', 'No alerts'));

    var silenceBtn = el('button', { class: 'btn', type: 'button',
      onClick: function () { if (typeof window.showSilenceCreate === 'function') window.showSilenceCreate(); } },
      '+ ' + _t('새 음소거', 'New Silence'));

                                           
    var sur = d.suricata, ips = d.ips;
    var engineOn = !!(sur && sur.engine && sur.engine.state === 'active');
    var threats = (sur && sur.eve_tail) ? ((sur.eve_tail.alerts_crit || 0) + (sur.eve_tail.alerts_warn || 0)) : 0;
    var ipsOn = !!(ips && ips.enabled);
    var suriBody = el('div', { class: 'mscreen-body' },
      el('div', { class: 'm-listcard' },
        HN.statusDot(engineOn ? 'ok' : 'idle', engineOn ? { glow: true } : null),
        el('span', { class: 'm-name' }, _t('IDS 엔진', 'IDS engine')),
        HN.statusPill(engineOn ? 'ok' : 'crit', engineOn ? 'ACTIVE' : 'OFF')),
      el('div', { class: 'm-listcard' },
        el('span', { class: 'm-name' }, _t('최근 위협', 'Recent threats')),
        HN.statusPill(threats > 0 ? 'warn' : 'ok', String(threats))),
      el('div', { class: 'm-listcard' },
        el('span', { class: 'm-name' }, _t('IPS 인라인 차단', 'IPS inline block')),
        HN.statusPill(ipsOn ? 'ok' : 'crit', ipsOn ? 'ON' : 'OFF')));
    var suriCard = el('div', { class: 'm-suricata' }, HN.card('Suricata IDS/IPS', suriBody));

    return el('div', { class: 'mscreen-body' },
      _pageHead(_t('알림·보안', 'Alerts and security'),
        _t('심각 ' + counts.crit + ' · 경고 ' + counts.warn,
          counts.crit + ' critical · ' + counts.warn + ' warning')),
      pushCard,
      bar,
      el('div', { class: 'mscreen-section-title' }, _t('알림 이력', 'Alert history')), list,
      silenceBtn,
      el('div', { class: 'mscreen-section-title' }, _t('보안', 'Security')), suriCard);
  }
  function buildPower(d) {
    var el = PCV.uxlib.el;
    d = d || { vms: [], containers: [] };
    var vms = d.vms || [], ctrs = d.containers || [];

                                                                          
                                                                 
    function pbtn(label, cls, fn) { return el('button', { class: 'btn ' + cls, type: 'button', 'data-role': 'OPERATOR,ADMIN', onClick: fn }, label); }

    var vmCards = vms.map(function (v) {
      var st = vmStatus(v.state);
      var actions;
      if (v.state === 'running') {
        actions = el('div', { class: 'm-actions' },
          pbtn(_t('정지', 'Stop'), 'btn-r', function () { _vmPowerAction(v.name, 'stop'); }),
          pbtn(_t('일시정지', 'Pause'), '', function () { _vmPowerAction(v.name, 'suspend'); }));
      } else if (v.state === 'paused') {
        actions = el('div', { class: 'm-actions' },
          pbtn(_t('재개', 'Resume'), 'btn-g', function () { _vmPowerAction(v.name, 'resume'); }),
          pbtn(_t('정지', 'Stop'), 'btn-r', function () { _vmPowerAction(v.name, 'stop'); }));
      } else {
        actions = el('div', { class: 'm-actions' },
          pbtn(_t('시작', 'Start'), 'btn-g', function () { _vmPowerAction(v.name, 'start'); }));
      }
      var row = el('div', { class: 'm-vm m-listcard' },
        HN.statusDot(st, st === 'ok' ? { glow: true } : null),
        el('span', { class: 'm-name' }, v.name),
        HN.statusPill(st, v.state || '?'),
        actions);
                                                
      var hint = el('div', { class: 'm-hint' }, v.state === 'running'
        ? _t('콘솔: VNC 사용 가능 (데스크톱)', 'Console: VNC available (desktop)')
        : _t('콘솔: VM 정지됨', 'Console: VM stopped'));
      return el('div', null, row, hint);
    });

    var ctrCards = ctrs.map(function (c) {
      var st = ctrStatus(c.state);
      var actions = c.state === 'RUNNING'
        ? el('div', { class: 'm-actions' }, pbtn(_t('정지', 'Stop'), 'btn-r', function () { _ctrPowerAction(c.name, 'stop'); }))
        : el('div', { class: 'm-actions' }, pbtn(_t('시작', 'Start'), 'btn-g', function () { _ctrPowerAction(c.name, 'start'); }));
      return el('div', { class: 'm-ctr m-listcard' },
        HN.statusDot(st, st === 'ok' ? { glow: true } : null),
        el('span', { class: 'm-name' }, c.name),
        HN.statusPill(st, c.state || '?'),
        actions);
    });

    var vmSection = vmCards.length ? el.apply(null, ['div', { class: 'mscreen-body' }].concat(vmCards)) : el('div', { class: 'm-hint' }, _t('VM 없음', 'No VMs'));
    var ctrSection = ctrCards.length ? el.apply(null, ['div', { class: 'mscreen-body' }].concat(ctrCards)) : el('div', { class: 'm-hint' }, _t('컨테이너 없음', 'No containers'));

    return el('div', { class: 'mscreen-body' },
      _pageHead(_t('전원 관리', 'Power controls'),
        'VM ' + vms.length + ' · ' + _t('컨테이너 ', 'Containers ') + ctrs.length),
      el('div', { class: 'mscreen-section-title' }, 'VM'), vmSection,
      el('div', { class: 'mscreen-section-title' }, _t('컨테이너', 'Containers')), ctrSection);
  }
  function buildHealing(state) {
    var el = PCV.uxlib.el;
    state = state || { mode: null, pending: [], history: [] };
    var pending = state.pending || [], history = state.history || [];
    var active = state.mode === 'active';

                                                       
                                                                   
                                              
                                                   
                                                              
    var modeBadge = HN.statusPill(active ? 'crit' : 'ok', active ? 'ACTIVE' : 'DRY RUN');
    modeBadge.classList.add('m-heal-mode');

    var pendCards = pending.map(function (p) {
      return el('div', { class: 'm-heal-pending m-listcard' },
        el('div', { class: 'm-name m-copy' },
          el('div', null, (p.policy || '') + ' · ' + (p.action || '')),
          el('div', { class: 'm-hint', style: 'padding:0' }, p.reason || '')),
        el('div', { class: 'm-actions' },
          el('button', { class: 'btn btn-g', type: 'button', 'data-role': 'ADMIN', onClick: (function (id) { return function () { _healApprove(id); }; })(p.id) }, _t('승인', 'Approve')),
          el('button', { class: 'btn btn-r', type: 'button', 'data-role': 'ADMIN', onClick: (function (id) { return function () { _healReject(id); }; })(p.id) }, _t('거절', 'Reject'))));
    });
    var pendBody = pendCards.length
      ? el.apply(null, ['div', { class: 'mscreen-body' }].concat(pendCards))
      : el('div', { class: 'm-heal-empty m-hint' }, _t('대기 중인 액션 없음', 'No pending actions'));

    var histCards = history.slice(0, 10).map(function (h) {
      var st = h.result === 'ok' ? 'ok' : (h.result ? 'crit' : 'idle');
      return el('div', { class: 'm-heal-hist m-listcard' },
        HN.statusPill(st, h.result || '-'),
        el('span', { class: 'm-name' }, (h.action || '') + ' → ' + (h.target || '')));
    });
    var histBody = histCards.length
      ? el.apply(null, ['div', { class: 'mscreen-body' }].concat(histCards))
      : el('div', { class: 'm-hint' }, _t('이력 없음', 'No history'));

    return el('div', { class: 'mscreen-body' },
      _pageHead(_t('자가치유 승인', 'Self-healing approvals'),
        _t('승인 대기 ' + pending.length + ' · 처리 이력 ' + history.length,
          pending.length + ' pending · ' + history.length + ' processed')),
      el('div', { class: 'm-listcard' }, el('span', { class: 'm-name' }, _t('엔진 모드', 'Engine mode')), modeBadge),
      el('div', { class: 'mscreen-section-title' }, _t('승인 대기', 'Pending approval') + ' (' + pending.length + ')'), pendBody,
      el('div', { class: 'mscreen-section-title' }, _t('처리 이력', 'History')), histBody);
  }

                                                     
                                           
                                                        
                                                           
                                                            
                                                                
                                                   
                                                    
                                                    
  async function showHome() {
    var scr = document.getElementById(SCREEN_ID); if (!scr) return;
    if (!scr.querySelector('.mscreen-body')) showSkeleton(scr, 4);
    if (!window.authToken) { _paint(_empty(_t('로그인 후 이용 가능', 'Sign in to continue'))); return; }
    var vmsR = await fetchGet(EP.VM_LIST());
    var ctrR = await fetchGet(EP.CTR_LIST());
    var alR = await fetchGet(EP.ALERTS());
    var alerts = unwrapList(alR);
    var d = {
      vms: unwrapList(vmsR), containers: unwrapList(ctrR), alerts: alerts,
      hostCpu: (PCV.metrics && PCV.metrics.latest('host.cpu')) || 0,
      hostMem: (PCV.metrics && PCV.metrics.latest('host.mem')) || 0
    };
    setBadges({ alerts: alerts.filter(function (a) { var s = _alertSev(a); return s === 'crit' || s === 'warn'; }).length });
    if (_state.activeTab === 'home') _paint(buildHome(d));
  }
  async function showAlerts() {
    var scr = document.getElementById(SCREEN_ID); if (!scr) return;
    if (!scr.querySelector('.mscreen-body')) showSkeleton(scr, 4);
    if (!window.authToken) { _paint(_empty(_t('로그인 후 이용 가능', 'Sign in to continue'))); return; }
    var alR = await fetchGet(EP.ALERTS());
    var sur = await _rpc('suricata.status', {}).catch(function () { return null; });
    var ips = await _rpc('suricata.ips.status', {}).catch(function () { return null; });
    _alertsData = { alerts: unwrapList(alR), suricata: sur, ips: ips };
    setBadges({ alerts: _alertsData.alerts.filter(function (a) { var s = _alertSev(a); return s === 'crit' || s === 'warn'; }).length });
    if (_state.activeTab === 'alerts') { _renderAlerts(); _ensureAlertsSub(); }
  }
  async function showPower() {
    var scr = document.getElementById(SCREEN_ID); if (!scr) return;
    if (!scr.querySelector('.mscreen-body')) showSkeleton(scr, 4);
    if (!window.authToken) { _paint(_empty(_t('로그인 후 이용 가능', 'Sign in to continue'))); return; }
    var vmsR = await fetchGet(EP.VM_LIST());
    var ctrR = await fetchGet(EP.CTR_LIST());
    if (_state.activeTab === 'power') _paint(buildPower({ vms: unwrapList(vmsR), containers: unwrapList(ctrR) }));
  }
  async function showHealing() {
    var scr = document.getElementById(SCREEN_ID); if (!scr) return;
    if (!scr.querySelector('.mscreen-body')) showSkeleton(scr, 3);
    if (!window.authToken) { _paint(_empty(_t('로그인 후 이용 가능', 'Sign in to continue'))); return; }
    if (PCV.selfhealing && PCV.selfhealing.refresh) { try { await PCV.selfhealing.refresh(); } catch {                                                                       } }
    var st = (PCV.selfhealing && PCV.selfhealing.state) || { mode: null, pending: [], history: [] };
    setBadges({ healing: (st.pending || []).length });
    if (_state.activeTab === 'healing') _paint(buildHealing(st));
  }

                                                                   
                                                  
                                                                     
                                                               
                                                                 
                                   
  function _rpc(method, params) {
    return fetchPost(EP.RPC(), { jsonrpc: '2.0', method: method, params: params || {}, id: 'm-' + Date.now() })
      .then(function (r) { var d = unwrapData(r); if (d && d.error) throw new Error(d.error.message || 'RPC error'); return d; });
  }

                                               
  var _alertsData = { alerts: [], suricata: null, ips: null };
  var _alertsSub = null;
  function _renderAlerts() {
    var fs = PCV.ui && PCV.ui.filterState;
    var filter = (fs && fs.current().severity) || [];
    _paint(buildAlerts({ alerts: _alertsData.alerts, suricata: _alertsData.suricata, ips: _alertsData.ips, filter: filter }));
  }
  function _ensureAlertsSub() {
    if (_alertsSub) return;
    var fs = PCV.ui && PCV.ui.filterState;
    if (!fs || !fs.subscribe) return;
                                                               
    _alertsSub = fs.subscribe(function () { if (_state.activeTab === 'alerts') _renderAlerts(); });
  }

                                              
     
                                                  
    
                                                                   
                                                     
                                                    
                                     
    
                    
                                                    
                                                     
                                                        
     
  async function _vmPowerAction(name, action) {
                                                                       
    var vms = (PCV.state && PCV.state.vmList) || window.vmList || [];
    var idx = -1;
    for (var i = 0; i < vms.length; i++) { if (vms[i].name === name) { idx = i; break; } }
    if (idx < 0) { toast(_t('VM 목록 동기화 필요', 'VM list out of sync'), false); return; }
    if (action === 'stop') {
      var ok = await customConfirm(_t('VM 정지', 'Stop VM'), name + ' — ' + _t('정지하시겠습니까?', 'Stop this VM?'));
      if (!ok) return;
    }
    window.selectedVmIndex = idx;
    if (typeof window.vmPower === 'function') window.vmPower(action);
  }
                                                        
                                                  
  async function _ctrPowerAction(name, action) {
    if (action === 'stop') {
      var ok = await customConfirm(_t('컨테이너 정지', 'Stop container'), name + ' — ' + _t('정지하시겠습니까?', 'Stop this container?'));
      if (!ok) return;
    }
    if (typeof window.ctrA === 'function') window.ctrA(name, action);
  }

                       
                                                                          
                                                                       
     
                                        
    
                    
                                                         
                                                             
                                           
                                                                    
                   
    
                                                            
                                         
     
  async function _healApprove(actionId) {
    var ok = await customConfirm(_t('자가치유 승인', 'Approve self-healing'),
      'action_id=' + actionId + ' — ' + _t('실제 실행됩니다. 계속?', 'Will execute. Proceed?'));
    if (!ok) return;
    try { await _rpc('ai.healing.approve', { action_id: actionId }); toast(_t('승인됨', 'Approved')); }
    catch (e) { toast(_t('승인 실패', 'Approve failed') + ': ' + (e.message || e), false); }
    showHealing();
  }
  async function _healReject(actionId) {
    var ok = await customConfirm(_t('자가치유 거절', 'Reject self-healing'),
      'action_id=' + actionId + ' — ' + _t('거절하시겠습니까?', 'Reject this action?'));
    if (!ok) return;
    try { await _rpc('ai.healing.reject', { action_id: actionId, reason: 'manual (mobile)' }); toast(_t('거절됨', 'Rejected')); }
    catch (e) { toast(_t('거절 실패', 'Reject failed') + ': ' + (e.message || e), false); }
    showHealing();
  }

                                                                    
  async function _ackAlertMobile(alertId) {
    try {
      var r = await fetchPost(EP.RPC(), {
        jsonrpc: '2.0', method: 'alert.ack',
        params: { alert_id: alertId }, id: 'ack-' + alertId
      });
      if (r && r.error) { toast(r.error.message || _t('확인 실패', 'Acknowledge failed'), false); return; }
      toast(_t('확인됨', 'Acknowledged'));
    } catch (e) { toast(_t('확인 실패', 'Acknowledge failed') + ': ' + (e.message || e), false); return; }
    showAlerts();
  }

  var SCREENS = { home: showHome, alerts: showAlerts, power: showPower, healing: showHealing };

                                                                
  var _mq = window.matchMedia(MQ);
  if (_mq.addEventListener) _mq.addEventListener('change', sync);
  else if (_mq.addListener) _mq.addListener(sync);
  var _rzT = null;
  window.addEventListener('resize', function () { if (_rzT) clearTimeout(_rzT); _rzT = setTimeout(sync, 150); });
                                                                
                                                                
                                                               
  queueMicrotask(sync);

                                                               
                                       
  if (typeof MutationObserver === 'function') {
    new MutationObserver(sync).observe(document.body, { attributes: true, attributeFilter: ['class'] });
  }

                                               
                                                             
                                                          
                                                         
                                                 
  function refreshActive() { if (SCREENS[_state.activeTab]) SCREENS[_state.activeTab](); }
                                                          
                                                                   
  async function refreshBadges() {
    if (!window.authToken) return;
    try {
      var aR = await fetchGet(EP.ALERTS());
      var alerts = unwrapList(aR).filter(function (a) { var s = _alertSev(a); return s === 'crit' || s === 'warn'; }).length;
      var hR = await _rpc('healing.pending', {});
      var healing = Array.isArray(hR) ? hR.length : 0;
      setBadges({ alerts: alerts, healing: healing });
    } catch {                                                                        }
  }
  function _wireLiveRefresh() {
    if (window._mLoadAllWrapped) return;
    if (typeof window.loadAll !== 'function') return;
    window._mLoadAllWrapped = true;
    var orig = window.loadAll;
    window.loadAll = function () {
      var r = orig.apply(this, arguments);
      if (isActive()) { refreshActive(); refreshBadges(); }
      return r;
    };
  }
  window.addEventListener('load', _wireLiveRefresh);

  PCV.mobile = {
    sync: sync, mount: mount, unmount: unmount, isActive: isActive,
    showTab: showTab, setBadges: setBadges,
    buildHome: buildHome, buildAlerts: buildAlerts, buildPower: buildPower, buildHealing: buildHealing,
    vmStatus: vmStatus, ctrStatus: ctrStatus, sevStatus: sevStatus,
    TABS: TABS,
    _wireLiveRefresh: _wireLiveRefresh,
    get activeTab() { return _state.activeTab; }
  };
})(window.PCV);
