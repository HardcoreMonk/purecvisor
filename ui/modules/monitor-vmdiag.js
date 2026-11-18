                           
                                                                                                                  
                                                                                              
                                                                                  
                                                                                                              
                                                                                                                                
                                                                                     
                                                                                  
                                                                                            
                                                                                                  
                                                                                                 
  
                                                                          
                                                     
   
(function (PCV) {
  var DASH = '—';
  var LAST = null;                                               
  var LAST_STATE = null;                                          

  function _el() { return PCV.uxlib.el.apply(null, arguments); }
  function _fmtBytes(n) { return typeof window.fmtBytes === 'function' ? window.fmtBytes(n) : n + ' B'; }
  function _num(v) { return typeof v === 'number' && isFinite(v); }
                                                                                           
  function _pill(level, text) { var p = window.HN.statusPill(level, text); p.classList.add('vmdiag-pill'); return p; }

                                                                     
  function fmtRateBps(n) { return _num(n) && n >= 0 ? (n === 0 ? '0 B/s' : _fmtBytes(Math.round(n)) + '/s') : DASH; }
  function fmtMiB(n) { if (!_num(n) || n < 0) return DASH; return n >= 1024 ? (n / 1024).toFixed(1) + ' GiB' : Math.round(n) + ' MiB'; }
  function fmtPct(n, d) { return _num(n) && n >= 0 ? n.toFixed(d === undefined ? 1 : d) + '%' : DASH; }
  function fmtFix(n, d, unit) { return _num(n) && n >= 0 ? n.toFixed(d) + (unit || '') : DASH; }

                                                                     
  var MISSING_TEXT = {
    cpu: 'CPU 시간을 받지 못했습니다',
    vcpu_delay: '이 호스트에서 vCPU 대기 값을 제공하지 않습니다',
    guest_memory: '게스트 메모리 통계 없음(balloon 통계를 내지 않는 게스트 — Windows 등에서는 정상)',
    rss: '호스트 RSS 값을 받지 못했습니다',
    disk: '디스크 통계를 받지 못했습니다',
    net: '네트워크 통계를 받지 못했습니다',
    cgroup: 'CPU 상한·압력 값을 읽지 못했습니다'
  };
  var MISSING_SHORT = { cpu: 'CPU 없음', vcpu_delay: 'vCPU 대기 없음', guest_memory: '게스트 메모리 없음', rss: 'RSS 없음',
                        disk: '디스크 없음', net: '네트워크 없음', cgroup: 'cgroup 없음' };

  function _nowWallMs(resp) {
    var s = resp && resp.sampler;
    return s && _num(s.observed_at_ms) && _num(s.age_ms) ? s.observed_at_ms + s.age_ms : Date.now();
  }
  function _lastOkAgo(rec, resp) {
    if (!_num(rec.last_ok_at_ms) || rec.last_ok_at_ms <= 0) return null;
    return Math.max(0, Math.round((_nowWallMs(resp) - rec.last_ok_at_ms) / 1000));
  }
  function stateText(rec) {
    if (rec.state === 'running' || rec.state === 'blocked') return '실행';
    if (rec.state === 'paused' || rec.state === 'pmsuspended') return '일시정지';
    return '정지';
  }
  function _isStopped(rec) { return rec.quality === 'stopped'; }

                                                                                       
  function reasonText(rec, bundle, resp) {
    if (!rec) return '';
    if (_isStopped(rec)) return (stateText(rec) === '일시정지' ? '일시정지 VM' : '정지 VM') + ' — 설정값만 있습니다';
    if (rec.quality === 'warming_up')
      return rec.reason === 'reset' ? '재시작·장치 변경으로 기준을 다시 잡는 중입니다' : '첫 표본 — 다음 표본(약 5초)부터 표시됩니다';
    var miss = rec.missing || [];
    var b = bundle === 'disk_lat' ? 'disk' : bundle;
    if (b && miss.indexOf(b) >= 0) return MISSING_TEXT[b] || '값을 받지 못했습니다';
    if (rec.quality === 'busy') {
      var ago = _lastOkAgo(rec, resp);
      return 'VM 작업 중이라 이번 표본을 읽지 못했습니다' + (ago === null ? '' : ' · 마지막 정상 ' + ago + '초 전');
    }
    if (bundle === 'disk_lat') return '이 간격에 요청 없음';
    return '값 없음';
  }

                                                                     
  var HINT = {
    cpu_wait: ['CPU 대기', 'vCPU가 실행할 준비가 됐지만 호스트 CPU를 기다린 시간이 {v}%입니다(잠정 기준 {t}% 이상). 호스트 CPU 과할당이나 CPU 핀 겹침을 살펴볼 후보입니다. CPU 상한(vm.limit)에 걸려 멈춘 시간은 빼고 셉니다.'],
    cpu_throttled: ['CPU 상한', 'CPU 상한(vm.limit)에 걸려 멈춘 시간이 {v}%입니다(잠정 기준 {t}% 이상). 상한 설정을 살펴볼 후보입니다.'],
    mem_swap: ['게스트 스왑', '게스트가 초당 {v} KiB를 스왑하고 있습니다. 게스트 메모리 부족 후보입니다.'],
    mem_pressure: ['메모리 압박', '이 VM의 메모리 압력(10초 평균)이 {v}%입니다(잠정 기준 {t}% 이상). 호스트 쪽 메모리 압박 신호입니다.'],
    io_pressure: ['I/O 대기', '이 VM의 I/O 압력(10초 평균)이 {v}%입니다(잠정 기준 {t}% 이상). I/O 대기 신호입니다.'],
    disk_latency: ['디스크 지연', '이 간격의 평균 디스크 응답 시간이 {v} ms입니다(잠정 기준 {t} ms 이상, 요청이 초당 5건 이상일 때만 판단).'],
    guest_stats_stale: ['게스트 값 오래됨', '게스트 메모리 값이 {v}초 전 것입니다. 게스트 메모리 칸은 그 시점 값입니다.']
  };
  var HINT_FOOT = '신호는 잠정 기준에 따른 참고 표시이며 알림을 보내지 않습니다. VMware의 CPU Ready와 같은 지표가 아닙니다.';
  function _levelWord(h) { return h.level === 'warn' ? '주의' : '참고'; }
  function hintSentence(h) {
    var t = HINT[h.code];
    if (!t) return '';
    var v = _num(h.value) ? (Math.abs(h.value) >= 100 ? h.value.toFixed(0) : h.value.toFixed(1)) : DASH;
    var th = _num(h.threshold) ? (Math.abs(h.threshold) >= 100 ? h.threshold.toFixed(0) : String(+h.threshold.toFixed(1))) : DASH;
    return t[1].replace('{v}', v).replace('{t}', th);
  }
  function hintBadge(h) {
    var t = HINT[h.code] || [h.code];
    var sentence = hintSentence(h);
    return _el('span', { class: 'pill pill-' + (h.level === 'warn' ? 'warn' : 'idle') + ' vmdiag-pill', title: sentence },
      t[0] + ' · ' + _levelWord(h), _el('span', { class: 'sr-only' }, ' — ' + sentence));
  }
  function hintBadges(rec, max) {
    var hs = rec.hints || [];
    var shown = hs.slice(0, max), rest = hs.slice(max);
    var out = shown.map(hintBadge);
    if (rest.length) {
      var list = rest.map(function (h) { return (HINT[h.code] || [h.code])[0] + ' · ' + _levelWord(h) + ': ' + hintSentence(h); }).join(' / ');
      out.push(_el('span', { class: 'pill pill-idle vmdiag-pill', title: list }, '+' + rest.length,
        _el('span', { class: 'sr-only' }, ' — ' + list)));
    }
    return out;
  }

                                                                    
  function qualityCell(rec, resp) {
    if (_isStopped(rec)) return DASH;
    if (rec.quality === 'ok') return _el('span', { class: 'color-muted' }, '정상');
    if (rec.quality === 'warming_up') return [_pill('idle', '준비 중'), ' ', _el('span', { class: 'stat-label' }, reasonText(rec, null, resp))];
    if (rec.quality === 'busy') {
      var ago = _lastOkAgo(rec, resp);
      return [_pill('warn', '읽기 보류'), ' ', _el('span', { class: 'stat-label' }, ago === null ? '마지막 정상 기록 없음' : '마지막 정상 ' + ago + '초 전')];
    }
    var miss = (rec.missing || []).map(function (m) { return MISSING_SHORT[m] || m; }).join(', ');
    return [_pill('idle', '일부'), ' ', _el('span', { class: 'stat-label' }, miss)];
  }
                                                                  
  function valueCell(text, rec, bundle, resp) {
    if (text !== DASH) return text;
    var why = reasonText(rec, bundle, resp);
    return _el('span', { title: why }, DASH, _el('span', { class: 'sr-only' }, ' ' + why));
  }

                                                                   
  var ERR_TEXT = {
    connect_failed: 'libvirt 연결에 실패했습니다',
    running_stats_failed: '실행 VM 통계를 읽지 못했습니다',
    defined_stats_failed: '등록 VM 설정값을 읽지 못했습니다'
  };
  function _stateKey(diag) {
    if (!diag) return null;
    if (!diag.ok) return 'rpc:' + diag.err;
    return (diag.data.sampler && diag.data.sampler.state) || 'unknown';
  }
  function _stateMessage(diag) {
    if (!diag.ok) return diag.err === 'unsupported' ? '이 데몬은 VM 진단을 지원하지 않습니다' : 'VM 진단을 불러오지 못했습니다';
    var s = diag.data.sampler || {};
    if (s.state === 'stale') return 'VM 진단 표본이 오래됐습니다';
    if (s.state === 'error') return 'VM 진단 표본 실패: ' + (ERR_TEXT[s.error] || s.error || '');
    if (s.state === 'starting') return 'VM 진단 표본 준비 중';
    return 'VM 진단 표본 정상';
  }
  function _announce(diag) {
    var key = _stateKey(diag);
    if (key === LAST_STATE) return;
    var first = LAST_STATE === null;
    LAST_STATE = key;
    if (first) return;                                  
    var live = document.getElementById('vmdiag-live');
    if (!live) {
      live = _el('div', { id: 'vmdiag-live', role: 'status', class: 'sr-only' });
      document.body.appendChild(live);
    }
    live.textContent = _stateMessage(diag);
  }

  function statusLine(diag, opts) {
    opts = opts || {};
    if (!diag || !diag.ok) {
      if (diag && diag.err === 'unsupported')
        return PCV.uxlib.msg('warn', { tag: 'div', cls: 'mb-8 vmdiag-status' }, '이 데몬은 VM 진단을 지원하지 않습니다 — 데몬·UI 버전을 확인하세요');
      return _el('div', { class: 'mb-8 vmdiag-status' },
        PCV.uxlib.msg('err', null, 'VM 진단을 불러오지 못했습니다'), ' ',
        _el('button', { class: 'btn', type: 'button', onClick: function () { if (opts.onRetry) opts.onRetry(); } }, '다시 시도'));
    }
    var s = diag.data.sampler || {};
    var age = _num(s.age_ms) ? Math.round(s.age_ms / 1000) : null;
    var parts = [];
    if (s.state === 'starting') parts.push(_pill('idle', '표본 준비 중'), ' ');
    if (s.state === 'stale') parts.push(_pill('warn', '표본 오래됨'), ' ',
      _el('span', null, 'VM 진단 표본이 ' + (age === null ? '' : age + '초') + ' 동안 갱신되지 않았습니다. 값은 마지막 표본 기준입니다.'), ' ');
    if (s.state === 'error') parts.push(_pill('crit', '표본 실패'), ' ',
      _el('span', null, (ERR_TEXT[s.error] || '표본을 만들지 못했습니다') + ' — 값은 마지막 표본 기준입니다.'), ' ');
    parts.push(_el('span', { class: 'stat-label' }, 'VM 표본 ' + (age === null ? DASH : age + '초 전') + ' · ' +
      (_num(s.interval_ms) ? Math.round(s.interval_ms / 1000) + '초 간격' : DASH) + ' · ' + (_num(s.tracked) ? s.tracked : 0) + '대'));
    var line = _el('div', { class: 'mb-8 vmdiag-status' }, parts);
    if (_num(s.truncated) && s.truncated > 0)
      return _el('div', null, line, _el('div', { class: 'mb-8' }, _pill('warn', '상한 밖'), ' ',
        'VM ' + s.truncated + '대는 상한(256대) 밖이라 진단하지 않았습니다'));
    return line;
  }
  function _dim(diag) { var s = diag && diag.ok && diag.data.sampler; return s && (s.state === 'stale' || s.state === 'error'); }

                                                                      
  function fetchDiag(vm) {
    var EP = PCV.endpoints || window.EP;
    var url = EP && EP.MONITOR_VM_DIAG ? EP.MONITOR_VM_DIAG(vm) : '/api/v1/monitor/vm-diag' + (vm ? '?vm=' + encodeURIComponent(vm) : '');
    return Promise.resolve().then(function () { return window.fetchGet(url); }).then(function (r) {
      if (r && r.error) {
                                                                                    
                                                                                                             
        var c = r.error.code;
        return { ok: false, data: null, err: (c === -32601 || c === 404 || c === 'NOT_FOUND') ? 'unsupported' : 'http' };
      }
                                                                                                     
      var d = r && r.data !== undefined ? r.data : r;
      if (!d || d.schema !== 1 || !Array.isArray(d.vms)) return { ok: false, data: null, err: 'http' };
      return { ok: true, data: d, err: null };
    }, function () { return { ok: false, data: null, err: 'http' }; });
  }
                                                                           
  function refresh(tab) {
    return fetchDiag().then(function (d) { LAST = d; _announce(d); return d; });
  }
  function last() { return LAST; }

                                                                
  var THR = null, THR_AT = 0;
  function throttledFetch(minGapMs) {
    var now = Date.now();
    if (minGapMs && now - THR_AT < minGapMs) return Promise.resolve(null);
    THR_AT = now;
    return fetchDiag().then(function (d) { THR = d; return d; });
  }
  function throttledLast() { return THR; }

  function dashCells(v) { return { cpu: opsCpu(v, THR), mem: opsMem(v, THR) }; }

  function legacyVms(diag) {
    if (!diag || !diag.ok) return [];
    return diag.data.vms.map(function (r) {
      return { name: r.name, node: '', running: stateText(r) === '실행' ? 1 : 0, state: stateText(r),
               vcpu: _num(r.vcpus) ? r.vcpus : 0, memory_max_mb: _num(r.mem_max_mb) ? r.mem_max_mb : 0,
               memory_rss_mb: _num(r.mem_rss_mb) ? r.mem_rss_mb : null, diag: r };                                     
    });
  }

                                                                   
                                                                          
  function _diagOk(diag) { return !!diag && !!diag.ok; }
  function countText(diag, n) { return _diagOk(diag) ? String(n) : DASH; }
  function countOrNull(diag, n) { return _diagOk(diag) ? n : null; }                                                   
  function vmRunSegment(diag, running, total) {
    if (!_diagOk(diag)) return { count: DASH, label: 'VM 실행', status: 'idle', sub: '진단 없음', key: 'vms' };
    return { count: running, label: 'VM 실행', status: total === 0 ? 'idle' : (running === total ? 'ok' : 'warn'), sub: '/ ' + total, key: 'vms' };
  }

                                                                   
                                                                                       
                                                                                       
                                                                    
                                                                                
                                                                                                
                                                                             
  function _opsRec(v, diag) {
    if (!diag || !diag.ok) {
      var unsup = !!diag && diag.err === 'unsupported';
      return { cell: { pct: null, note: unsup ? '진단 미지원' : '진단 실패',
                       title: unsup ? '이 데몬은 VM 진단을 지원하지 않습니다' : 'VM 진단을 불러오지 못했습니다' } };
    }
    var name = v.name || v.vm;
    var rec = diag.data.vms.filter(function (r) { return r.name === name; })[0];
    if (!rec) return { cell: { pct: null, note: null, title: '이 VM 의 진단 기록이 없습니다' } };
    if (_isStopped(rec)) return { cell: { pct: null, note: null, title: reasonText(rec, null, diag.data) } };
    return { rec: rec };
  }
  function opsMem(v, diag) {
    var o = _opsRec(v, diag);
    if (o.cell) return o.cell;
    var rec = o.rec;
    var used = rec.mem_guest_used_mb, alloc = rec.mem_alloc_mb, rss = rec.mem_rss_mb;
    var stale = _staleAge(rec);
    if (stale === null && _num(used) && used >= 0 && _num(alloc) && alloc > 0)
      return { pct: used / alloc * 100, note: null, title: '게스트 사용 추정 ' + fmtMiB(used) + ' / 할당 ' + fmtMiB(alloc) };
    var haveRss = _num(rss) && rss >= 0;
    return { pct: null, note: haveRss ? 'RSS ' + fmtMiB(rss) : null,
             title: (stale !== null ? '게스트 값 오래됨(' + stale + '초 전)' : reasonText(rec, 'guest_memory', diag.data)) +
                    (haveRss ? ' — 호스트 RSS 를 대신 표시' : '') };
  }
                                                                                     
  function _staleAge(rec) {
    var h = (rec.hints || []).filter(function (x) { return x.code === 'guest_stats_stale'; })[0];
    if (!h) return null;
    var age = _num(rec.guest_stats_age_s) ? rec.guest_stats_age_s : h.value;
    return _num(age) ? Math.round(age) : null;
  }
                                                                 
  function opsCpu(v, diag) {
    var o = _opsRec(v, diag);
    if (o.cell) return o.cell;
    var rec = o.rec;
    if (_num(rec.cpu_pct) && rec.cpu_pct >= 0)
      return { pct: rec.cpu_pct, note: null,
               title: 'vCPU 대비 ' + fmtPct(rec.cpu_pct) + (_num(rec.cpu_host_pct) && rec.cpu_host_pct >= 0 ? ' · 호스트 대비 ' + fmtPct(rec.cpu_host_pct) : '') };
    return { pct: null, note: null, title: reasonText(rec, 'cpu', diag.data) };
  }

  function opsRows(vms, diag) {
    return (vms || []).map(function (v) { return Object.assign({}, v, { _mem: opsMem(v, diag), _cpu: opsCpu(v, diag) }); });
  }
                                                                                                        
  function opsMemCell(m, barFn) {
    if (m.pct !== null) { var bar = barFn(m.pct); bar.title = m.title; return bar; }
    return _el('span', { class: 'color-muted text-12', title: m.title }, DASH, m.note ? ' ' + m.note : '',
      _el('span', { class: 'sr-only' }, ' ' + m.title));
  }

                                                                        
  function _sum2(a, b) { return _num(a) && _num(b) ? a + b : null; }
  function _top5(title, header, recs, keyFn, valFn, subFn, titleFn) {
    var running = recs.filter(function (r) { return !_isStopped(r); });
    var withVal = running.filter(function (r) { return keyFn(r) !== null; });
    var excluded = running.length - withVal.length;
    var top = withVal.slice().sort(function (a, b) { return keyFn(b) - keyFn(a) || (a.name < b.name ? -1 : 1); }).slice(0, 5);
    var body;
    if (running.length === 0) body = _el('tr', null, _el('td', { colspan: '3', class: 'color-muted' }, '실행 중인 VM 없음'));
    else if (withVal.length === 0) body = _el('tr', null, _el('td', { colspan: '3', class: 'color-muted' }, '첫 표본 대기 중 — 약 5초 뒤 표시'));
    else body = top.map(function (r, i) {
      return _el('tr', null,
        _el('td', { class: 'w-16 color-muted' }, String(i + 1)),
        _el('td', { class: 'vmdiag-name', title: r.name }, _el('b', null, r.name)),                                         
        _el('td', { class: 'text-right mono vmdiag-val', title: titleFn ? titleFn(r) : null }, valFn(r),
          subFn ? _el('div', { class: 'stat-label' }, subFn(r)) : null));
    });
    return window.HN.card(title, [
      _el('table', { class: 'text-11 vmdiag-top5' },
        _el('caption', { class: 'sr-only' }, title),
        _el('thead', null, _el('tr', null, _el('th', { scope: 'col' }, '순위'), _el('th', { scope: 'col' }, 'VM'),
          _el('th', { scope: 'col', class: 'text-right' }, header))),
        _el('tbody', null, body)),
      excluded > 0 && withVal.length > 0 ? _el('div', { class: 'stat-label mt-4' }, '값 없는 VM ' + excluded + '대 제외(준비 중·읽기 보류 등)') : null
    ]);
  }
  function top5Tables(diag) {
    var recs = diag.data.vms;
    var cpu = _top5('Top 5 CPU', '호스트 %', recs, function (r) { return _num(r.cpu_host_pct) ? r.cpu_host_pct : null; },
      function (r) { return fmtPct(r.cpu_host_pct); },
      function (r) { return 'vCPU ' + (_num(r.vcpus) ? r.vcpus : DASH) + ' · ' + fmtPct(r.cpu_pct) + '(vCPU 대비)'; });
    var disk = _top5('Top 5 디스크 I/O', '읽기+쓰기 B/s', recs, function (r) { return _sum2(r.disk && r.disk.rd_bps, r.disk && r.disk.wr_bps); },
      function (r) { return fmtRateBps(r.disk.rd_bps + r.disk.wr_bps); }, null,
      function (r) { return '읽기 ' + fmtRateBps(r.disk.rd_bps) + ' · 쓰기 ' + fmtRateBps(r.disk.wr_bps); });
    var net = _top5('Top 5 네트워크', '수신+송신 B/s', recs, function (r) { return _sum2(r.net && r.net.rx_bps, r.net && r.net.tx_bps); },
      function (r) { return fmtRateBps(r.net.rx_bps + r.net.tx_bps); }, null,
      function (r) { return '수신 ' + fmtRateBps(r.net.rx_bps) + ' · 송신 ' + fmtRateBps(r.net.tx_bps); });
    var vcpu = _top5('Top 5 vCPU 할당', 'vCPU', recs, function (r) { return _num(r.vcpus) ? r.vcpus : null; },
      function (r) { return String(r.vcpus); });
    var mem = _top5('Top 5 메모리(호스트 RSS)', 'RSS', recs, function (r) { return _num(r.mem_rss_mb) ? r.mem_rss_mb : null; },
      function (r) { return fmtMiB(r.mem_rss_mb); });
    return { cpu: cpu, disk: disk, net: net, vcpu: vcpu, mem: mem };
  }

                                                                     
  function _gib(mb) { return (mb / 1024).toFixed(1); }
  function _ratioCell(ratio, num, den, sub) {
    var over = _num(ratio) && ratio > 1;
    return _el('td', { class: 'text-right' },
      _el('div', { class: 'stat-lg mono' }, _num(ratio) ? ratio.toFixed(2) : DASH,
        over ? [' ', _el('span', { class: 'pill pill-warn vmdiag-pill', title: '설정 합이 물리 용량보다 큽니다. 동시에 모두 쓰면 경합할 수 있습니다.' }, '과할당')] : null),
      _el('div', { class: 'stat-label' }, num + ' / ' + den),
      sub ? _el('div', { class: 'stat-label' }, sub) : null);
  }
  function allocationCard(diag) {
    if (!diag || !diag.ok) return window.HN.card('할당률(물리 용량 대비 설정 합)', statusLine(diag));
    var h = diag.data.host || {}, a = h.allocation || {};
    var lc = _num(h.logical_cpus) && h.logical_cpus > 0 ? h.logical_cpus : null;
    var mt = _num(h.mem_total_mb) && h.mem_total_mb > 0 ? h.mem_total_mb : null;
    var pc = _num(h.physical_cores) && h.physical_cores > 0 ? h.physical_cores : null;
    function phys(v) { return pc ? '물리 코어 ' + pc + '개 기준 ' + (v / pc).toFixed(2) : null; }
    var rows = [
      ['실행 중', a.vcpu_running, a.vcpu_running_per_logical, a.mem_running_max_mb, a.mem_running_max_ratio],
      ['등록 전체', a.vcpu_defined, a.vcpu_defined_per_logical, a.mem_defined_max_mb, a.mem_defined_max_ratio]
    ].map(function (r) {
      return _el('tr', null, _el('th', { scope: 'row' }, r[0]),
        _ratioCell(r[2], r[1], lc === null ? DASH : lc, phys(r[1] || 0)),
        _ratioCell(r[4], _gib(r[3] || 0), mt === null ? DASH : _gib(mt) + ' GiB', null));
    });
    var table = _el('table', { class: 'text-11 vmdiag-alloc' },
      _el('caption', { class: 'sr-only' }, '할당률(물리 용량 대비 설정 합)'),
      _el('thead', null, _el('tr', null, _el('td', null, ''), _el('th', { scope: 'col', class: 'text-right' }, 'vCPU'),
        _el('th', { scope: 'col', class: 'text-right' }, '최대 메모리'))),
      _el('tbody', null, rows));
    var note = 'vCPU는 논리 CPU ' + (lc === null ? DASH : lc + '개') + ' 대비, 메모리는 ' + (mt === null ? DASH : _gib(mt) + ' GiB') +
      ' 대비입니다. 격리 코어 예약과 운영 여유는 넣지 않았습니다.';
    return window.HN.card('할당률(물리 용량 대비 설정 합)', [table, _el('div', { class: 'stat-label mt-4 vmdiag-alloc-note' }, note)]);
  }
                                    
  function storageMemCards(diag) {
    var HN = window.HN;
    if (!diag || !diag.ok) return [HN.card('등록 VM 최대 메모리 합', statusLine(diag), 'text-center')];
    var h = diag.data.host || {}, a = h.allocation || {};
    var mt = _num(h.mem_total_mb) && h.mem_total_mb > 0 ? h.mem_total_mb : null;
    var ratio = a.mem_defined_max_ratio;
    return [
      HN.card('등록 VM 최대 메모리 합', _el('div', { class: 'stat-xl color-yellow' }, _gib(a.mem_defined_max_mb || 0) + ' GiB'), 'text-center'),
      HN.card('등록 VM 최대 메모리 / 메모리 총량', [
        _el('div', { class: 'stat-xl mono' }, _num(ratio) ? ratio.toFixed(2) : DASH,
          _num(ratio) && ratio > 1 ? [' ', _el('span', { class: 'pill pill-warn vmdiag-pill' }, '과할당')] : null),
        _el('div', { class: 'stat-label' }, '메모리는 ' + (mt === null ? DASH : _gib(mt) + ' GiB') + ' 대비입니다.')], 'text-center')
    ];
  }

                                                                     
  function overviewSection(diag, opts) {
    var HN = window.HN;
    var head = HN.section('VM 자원');
    if (!diag || !diag.ok) return _el('div', { class: 'mb-12 vmdiag-overview' }, head, statusLine(diag, opts));
    var t = top5Tables(diag);
    return _el('div', { class: 'mb-12 vmdiag-overview' + (_dim(diag) ? ' color-muted' : '') }, head, statusLine(diag, opts),
      _el('h4', { class: 'stat-label mb-4' }, '사용(속도)'),
      HN.grid(3, t.cpu, t.disk, t.net),
      _el('h4', { class: 'stat-label mb-4 mt-8' }, '배치'),
      HN.grid(3, allocationCard(diag), t.vcpu, t.mem));
  }
                                              
  function rollupSegment(diag) {
    if (!diag || !diag.ok) return { count: DASH, label: 'VM 신호', status: 'idle', sub: '진단 없음', key: 'vmsignal' };
    var run = diag.data.vms.filter(function (r) { return !_isStopped(r); });
    var withH = run.filter(function (r) { return (r.hints || []).length > 0; });
    var warn = withH.some(function (r) { return r.hints.some(function (h) { return h.level === 'warn'; }); });
    return { count: withH.length, label: 'VM 신호', status: warn ? 'warn' : (withH.length ? 'idle' : 'ok'), sub: '실행 VM', key: 'vmsignal' };
  }

                                                                     
  var COLS = ['VM', '상태', '데이터', 'CPU %(vCPU 대비)', '호스트 %', '게스트 메모리(사용/할당)', 'RSS',
              '디스크(읽기/쓰기 B/s)', '디스크 지연 ms', '네트워크(수신/송신 B/s)', '신호'];
  function _td(i, content, cls) { return _el('td', { 'data-label': COLS[i], class: cls || null }, content); }
  function _two(a, b) { return [_el('div', null, a), _el('div', null, b)]; }
  function vmRow(r, resp) {
    var st = _isStopped(r);
    function v(text, bundle) { return st ? valueCell(DASH, r, bundle, resp) : valueCell(text, r, bundle, resp); }
    var d = r.disk || {}, n = r.net || {};
    var lat = !st && (_num(d.rd_lat_ms) || _num(d.wr_lat_ms))
      ? fmtFix(d.rd_lat_ms, 1) + ' / ' + fmtFix(d.wr_lat_ms, 1) : DASH;
    var guest = _num(r.mem_guest_used_mb) ? fmtMiB(r.mem_guest_used_mb) + ' / ' + fmtMiB(r.mem_alloc_mb) : DASH;
    return _el('tr', null,
      _td(0, _el('b', null, r.name)),
      _td(1, stateText(r)),
      _td(2, qualityCell(r, resp)),
      _td(3, v(fmtPct(r.cpu_pct), 'cpu'), 'text-right mono'),
      _td(4, v(fmtPct(r.cpu_host_pct), 'cpu'), 'text-right mono'),
      _td(5, v(guest, 'guest_memory'), 'text-right mono'),
      _td(6, v(fmtMiB(r.mem_rss_mb), 'rss'), 'text-right mono'),
      _td(7, st || fmtRateBps(d.rd_bps) === DASH ? v(DASH, 'disk') : _two(fmtRateBps(d.rd_bps), fmtRateBps(d.wr_bps)), 'text-right mono'),
      _td(8, v(lat, 'disk_lat'), 'text-right mono'),
      _td(9, st || fmtRateBps(n.rx_bps) === DASH ? v(DASH, 'net') : _two(fmtRateBps(n.rx_bps), fmtRateBps(n.tx_bps)), 'text-right mono'),
      _td(10, st ? DASH : hintBadges(r, 2)));
  }
  function vmTable(diag, keepFn, opts) {
    var HN = window.HN;
    if (!diag || !diag.ok) return HN.card('VM 진단', statusLine(diag, opts));
    var recs = diag.data.vms.filter(function (r) { return keepFn ? keepFn(r) : true; });
    var body = recs.length ? recs.map(function (r) { return vmRow(r, diag.data); })
      : _el('tr', null, _el('td', { colspan: String(COLS.length), class: 'color-muted' }, diag.data.scope === 'own' ? '볼 수 있는 VM 지표가 없습니다' : 'VM 없음'));
    var table = _el('table', { class: 'text-11 card-mobile vmdiag-table' },
      _el('caption', { class: 'sr-only' }, 'VM 진단'),
      _el('thead', null, _el('tr', null, COLS.map(function (c) { return _el('th', { scope: 'col' }, c); }))),
      _el('tbody', null, body));
    var details = _el('details', { class: 'mt-8 vmdiag-details' },
      _el('summary', null, '지표·신호 설명'),
      _el('p', null, '메모리: 최대(설정 상한) · 할당(지금 게스트에 준 양) · 게스트 사용(게스트 balloon 통계 — 없으면 —) · RSS(호스트가 이 VM 에 실제로 쓴 메모리).'),
      _el('p', null, 'CPU %(vCPU 대비)는 VM 자기 vCPU 수 대비이고 100 을 넘을 수 있습니다(에뮬레이터·I/O 스레드 포함). 호스트 %는 논리 CPU 전체 대비입니다.'),
      _el('table', { class: 'text-11' },
        _el('caption', null, '신호 7종(잠정 기준)'),
        _el('thead', null, _el('tr', null, _el('th', { scope: 'col' }, '신호'), _el('th', { scope: 'col' }, '뜻'))),
        _el('tbody', null, Object.keys(HINT).map(function (k) {
          return _el('tr', null, _el('td', null, HINT[k][0]), _el('td', null, HINT[k][1].replace(/\{v\}/g, 'n').replace(/\{t\}/g, '기준값')));
        }))),
      _el('p', null, HINT_FOOT));
    return _el('div', { class: 'hc vmdiag-tablecard' + (_dim(diag) ? ' color-muted' : '') },
      _el('h4', { role: 'heading', 'aria-level': '3' }, 'VM 진단'), statusLine(diag, opts),
      _el('div', { class: 'table-wrap' }, table),
      _el('div', { class: 'stat-label mt-4' }, HINT_FOOT), details);
  }

                                                                   
  function _wide(title, body) {
    return _el('div', { class: 'hc', style: 'grid-column:1 / -1' }, _el('h4', { role: 'heading', 'aria-level': '3' }, title), body);
  }
  function _psiText(p) {
    if (!p) return DASH;
    return ['cpu', 'memory', 'io'].map(function (k) {
      var x = p[k] || {};
      return k + ' ' + fmtFix(x.some_avg10, 1) + ' / ' + fmtFix(x.full_avg10, 1);
    }).join(' · ') + ' %(some / full)';
  }
                                                                                                                
  function perfView(diag, name, opts) {
    var HN = window.HN;
    opts = opts || {};
    if (!diag || !diag.ok) return { rec: null, lead: [statusLine(diag, opts)], cards: [], cpuPct: null, guestPct: null };
    var resp = diag.data;
    var rec = resp.vms.filter(function (r) { return r.name === name; })[0] || null;
    if (!rec) return { rec: null, cards: [], cpuPct: null, guestPct: null,
      lead: [statusLine(diag, opts), PCV.uxlib.msg('warn', { tag: 'div', cls: 'mb-8' }, '진단 표본에 이 VM 이 아직 없습니다 — 다음 표본(약 5초)을 기다립니다')] };
    var st = _isStopped(rec);
    function v(text, bundle) { return st ? valueCell(DASH, rec, bundle, resp) : valueCell(text, rec, bundle, resp); }
    var d = rec.disk || {}, n = rec.net || {}, c = rec.contention || {};
    var chartH = opts.chartH || '80px';
    function chart(id) { return _el('div', { style: 'position:relative;height:' + chartH + ';width:100%;margin-top:6px' }, _el('canvas', { id: id })); }
    function note() { return _el('div', { class: 'stat-label mt-4' }, '최근 표본 60개 · 10초 간격'); }
    var cpuPct = !st && _num(rec.cpu_pct) ? rec.cpu_pct : null;
    var guestPct = !st && _num(rec.mem_guest_used_mb) && _num(rec.mem_alloc_mb) && rec.mem_alloc_mb > 0
      ? rec.mem_guest_used_mb / rec.mem_alloc_mb * 100 : null;
    var lat = !st && (_num(d.rd_lat_ms) || _num(d.wr_lat_ms) || _num(d.fl_lat_ms))
      ? fmtFix(d.rd_lat_ms, 1) + ' / ' + fmtFix(d.wr_lat_ms, 1) + ' / ' + fmtFix(d.fl_lat_ms, 1) + ' ms' : DASH;
    var cards = [
      HN.card('CPU', [
        HN.row('vCPU', _num(rec.vcpus) ? String(rec.vcpus) : DASH),
        HN.row('사용률(vCPU 대비)', v(fmtPct(rec.cpu_pct), 'cpu')),
        HN.row('호스트 대비', v(fmtPct(rec.cpu_host_pct), 'cpu')),
        cpuPct !== null ? HN.gauge({ value: Math.min(100, +cpuPct.toFixed(1)), warn: 80, crit: 95, inline: true }) : null,
        chart('cg'), note()]),
      HN.card('메모리', [
        HN.row('최대', fmtMiB(rec.mem_max_mb)),
        HN.row('할당', v(fmtMiB(rec.mem_alloc_mb), null)),
        HN.row('게스트 사용', v(fmtMiB(rec.mem_guest_used_mb), 'guest_memory')),
        HN.row('호스트 RSS', v(fmtMiB(rec.mem_rss_mb), 'rss')),
        guestPct !== null ? HN.gauge({ value: +guestPct.toFixed(1), warn: 80, crit: 95, inline: true })
          : _el('div', { class: 'stat-label' }, DASH + ' ' + (st ? reasonText(rec, null, resp) : '게스트 메모리 통계 없음')),
        chart('mg'), note()]),
      HN.card('디스크', [
        HN.row('읽기', v(fmtRateBps(d.rd_bps), 'disk')), HN.row('쓰기', v(fmtRateBps(d.wr_bps), 'disk')),
        HN.row('읽기 IOPS', v(fmtFix(d.rd_iops, 1, ' IOPS'), 'disk')), HN.row('쓰기 IOPS', v(fmtFix(d.wr_iops, 1, ' IOPS'), 'disk')),
        HN.row('평균 지연(읽기 / 쓰기 / flush)', v(lat, 'disk_lat'))]),
      HN.card('네트워크', [
        HN.row('수신', v(fmtRateBps(n.rx_bps), 'net')), HN.row('송신', v(fmtRateBps(n.tx_bps), 'net')),
        HN.row('수신 pps', v(fmtFix(n.rx_pps, 1, ' pps'), 'net')), HN.row('송신 pps', v(fmtFix(n.tx_pps, 1, ' pps'), 'net')),
        HN.row('드롭/s', v(fmtFix(n.drop_ps, 1, '/s'), 'net')), HN.row('오류/s', v(fmtFix(n.err_ps, 1, '/s'), 'net'))]),
      _wide('경합', [
        HN.row('vCPU 대기(평균 / 최대)', v(_num(c.vcpu_delay_pct) ? fmtPct(c.vcpu_delay_pct) + ' / ' + fmtPct(c.vcpu_delay_max_pct) : DASH, 'vcpu_delay')),
        HN.row('CPU 상한(시간 / 걸린 주기)', v(_num(c.throttled_pct) ? fmtPct(c.throttled_pct) + ' / ' + fmtPct(_num(c.throttled_periods_ratio) ? c.throttled_periods_ratio * 100 : null) : DASH, 'cgroup')),
        HN.row('압력 PSI(10초 평균)', v(c.psi ? _psiText(c.psi) : DASH, 'cgroup')),
        HN.row('게스트 스왑 in / out', v(_num(c.swap_in_kbps) ? fmtFix(c.swap_in_kbps, 1) + ' / ' + fmtFix(c.swap_out_kbps, 1) + ' KiB/s' : DASH, 'guest_memory')),
        HN.row('주 페이지 폴트/s', v(fmtFix(c.major_faults_ps, 1, '/s'), 'guest_memory')),
        _el('h4', { class: 'stat-label mt-8' }, '신호'),
        (rec.hints || []).length
          ? _el('ul', { class: 'vmdiag-hint-list' }, rec.hints.map(function (h) {
              return _el('li', null, hintBadge(h), ' ', _el('span', null, hintSentence(h)));
            }))
          : _el('div', { class: 'stat-label' }, '신호 없음'),
        _el('div', { class: 'stat-label mt-4' }, HINT_FOOT)])
    ];
    var lead = [statusLine(diag, opts)];
    if (st) lead.push(_el('div', { class: 'mb-8 color-muted' }, reasonText(rec, null, resp)));
    return { rec: rec, lead: lead, cards: cards, cpuPct: cpuPct, guestPct: guestPct };
  }

                                                            
  function rateAt(arr, ts, i) {
    if (!arr || !ts || i < 1 || i >= arr.length || !_num(ts[i])) return null;
    var j = i - 1;
    while (j >= 0 && ts[i] - ts[j] < 2000) j--;
    if (j < 0 || !_num(arr[i]) || !_num(arr[j])) return null;
    var d = arr[i] - arr[j];
    if (d < 0) return null;                
    return d / ((ts[i] - ts[j]) / 1000);
  }
  function rateText(arr, ts, i) {
    var r = rateAt(arr, ts, i);
    return r === null ? DASH : fmtRateBps(r);
  }
  function rateSeries(arr, ts) {
    var out = [];
    for (var i = 0; i < (arr ? arr.length : 0); i++) { var r = rateAt(arr, ts, i); out.push(r === null ? null : r / 1e6); }
    return out;
  }

  PCV.vmdiag = {
    fetch: fetchDiag, refresh: refresh, last: last, throttledFetch: throttledFetch, throttledLast: throttledLast, dashCells: dashCells, legacyVms: legacyVms, opsMem: opsMem, opsCpu: opsCpu, opsRows: opsRows, opsMemCell: opsMemCell,
    countText: countText, countOrNull: countOrNull, vmRunSegment: vmRunSegment,
    overviewSection: overviewSection, allocationCard: allocationCard, vmTable: vmTable, rollupSegment: rollupSegment,
    statusLine: statusLine, storageMemCards: storageMemCards, reasonText: reasonText, hintSentence: hintSentence,
    hintBadges: hintBadges, qualityCell: qualityCell, valueCell: valueCell, stateText: stateText,
    rateAt: rateAt, rateText: rateText, rateSeries: rateSeries, perfView: perfView,
    fmtRateBps: fmtRateBps, fmtMiB: fmtMiB, fmtPct: fmtPct, fmtFix: fmtFix, HINT_FOOT: HINT_FOOT, DASH: DASH
  };
})(window.PCV);
