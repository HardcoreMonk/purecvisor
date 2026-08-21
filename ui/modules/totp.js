                                                                  
                                        
                                                  
                                          
                                                                     
  
                           
                                                          
  
                                                                       
                                                                  
                                                        
                                                              
                                                                         
                                              
                                                                    
  
                                                                           
                                                                     
  
                                                      
                                                                   
                                                  
  
                       
                                                    
                                                   
   
window.PCV = window.PCV || {};
(function (PCV) {

                                       
  function _t(key, fallback) {
    if (typeof t === 'function') {
      var v = t(key);
      if (v != null && v !== '' && v !== key) return v;
    }
    return fallback != null ? fallback : key;
  }
                                   
  function _bi(ko, en) {
    return (typeof _L === 'function') ? _L(ko, en) : ko;
  }

                                                                        
  function _readJson(r) {
    return r.text().then(function (txt) {
      var body = {};
      if (txt) { try { body = JSON.parse(txt); } catch (e) { body = {}; } }
      return { ok: r.ok, status: r.status, body: body };
    });
  }

                                                      
  function _errText(d) {
    if (d && d.error && d.error.message) return d.error.message;
    return _t('login.totp.invalid', 'Invalid code');
  }

                                                                      
  function _renderQrCanvas(canvas, uri) {
    var qr = qrcode(0, 'M');
    qr.addData(uri);
    qr.make();
    var count = qr.getModuleCount();
    var quiet = 4;                           
    var cell = 5;                
    var size = (count + quiet * 2) * cell;
    canvas.width = size;
    canvas.height = size;
    var ctx = canvas.getContext('2d');
    ctx.fillStyle = '#ffffff';
    ctx.fillRect(0, 0, size, size);
    ctx.fillStyle = '#000000';
    for (var row = 0; row < count; row++) {
      for (var col = 0; col < count; col++) {
        if (qr.isDark(row, col)) {
          ctx.fillRect((col + quiet) * cell, (row + quiet) * cell, cell, cell);
        }
      }
    }
  }

                                            
                             
                                                          
                                                                  
                                                                 
                                                             
                                                                           
                                                                   
                                                                
                                                       
    
                         
                                                        
                                                         
                         
     
  function _enrollFetch(ctx) {
    if (ctx.mode === 'login') {
      return PCV.api._fetchWithTimeout(EP.AUTH_TOTP_ENROLL(), {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ pending_token: ctx.pendingToken })
      }).then(_readJson);
    }
                                                                          
    return fetchPost(EP.AUTH_TOTP_ENROLL(), {}).then(function (d) {
      return { ok: !(d && d.error), status: 200, body: d || {} };
    });
  }

  function _verifyFetch(ctx, code) {
    if (ctx.mode === 'login') {
      return PCV.api._fetchWithTimeout(EP.AUTH_TOTP_VERIFY(), {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ pending_token: ctx.pendingToken, code: code })
      }).then(_readJson);
    }
    return fetchPost(EP.AUTH_TOTP_VERIFY(), { code: code }).then(function (d) {
      return { ok: !(d && d.error), status: 200, body: d || {} };
    });
  }

  function beginForcedEnroll(user, pendingToken, finishLogin) {
    _openEnrollWizard({ mode: 'login', user: user, pendingToken: pendingToken, finishLogin: finishLogin });
  }

  function _openEnrollWizard(ctx) {
    var el = PCV.uxlib.el;
    showModal([el('div', { id: 'totp-wiz', style: 'min-width:260px' },
      PCV.uxlib.msg('loading', { tag: 'div', style: 'text-align:center;padding:20px' }, _t('loading', 'Loading...')))]);
    _enrollFetch(ctx).then(function (res) {
      var host = document.getElementById('totp-wiz');
      if (!host) return;                  
      if (!res.ok || !res.body || !res.body.secret) {
        _renderWizError(host, _errText(res.body));
        return;
      }
      _renderEnrollStep(ctx, host, res.body.secret, res.body.otpauth_uri);
    }).catch(function (e) {
      var host = document.getElementById('totp-wiz');
      if (host) _renderWizError(host, (e && e.message) ? e.message : String(e));
    });
  }

  function _renderWizError(host, text) {
    var el = PCV.uxlib.el;
    PCV.uxlib.clearEl(host);
    host.appendChild(PCV.uxlib.frag(
      el('h2', null, _t('totp.enroll.title', 'Enroll TOTP')),
      el('p', { class: 'color-red text-12', role: 'alert' }, text),
      el('div', { class: 'text-right mt-12' },
        el('button', { class: 'btn', onClick: function () { closeModal(); } }, _t('btn.close', 'Close')))));
  }

  function _renderEnrollStep(ctx, host, secret, uri) {
    var el = PCV.uxlib.el;
    var canvas = el('canvas', { 'aria-label': _t('totp.enroll.scan', 'Scan QR'),
      style: 'background:#fff;border-radius:8px;padding:6px;image-rendering:pixelated;max-width:100%' });
                                                              
    try { if (typeof qrcode !== 'undefined' && uri) _renderQrCanvas(canvas, uri); } catch (e) {                    }
    var codeInput = el('input', { id: 'totp-enroll-code', class: 'login-input', inputmode: 'numeric',
      maxlength: '6', autocomplete: 'one-time-code', placeholder: '000000',
      style: 'text-align:center;letter-spacing:4px' });
    var errEl = el('div', { class: 'color-red text-12', style: 'min-height:16px', role: 'alert' });
    function submit() {
      var code = codeInput.value.trim();
      if (!code) { errEl.textContent = _t('login.required', 'Required'); return; }
      errEl.textContent = _t('msg.processing', '처리 중...');
      _verifyFetch(ctx, code).then(function (res) {
        if (res.ok && res.body && (res.body.access_token || res.body.confirmed || res.body.verified)) {
          _onEnrollConfirmed(ctx, res.body);
        } else {
          errEl.textContent = _errText(res.body);
        }
      }).catch(function (e) { errEl.textContent = (e && e.message) ? e.message : String(e); });
    }
    codeInput.addEventListener('keydown', function (ev) { if (ev.key === 'Enter') { ev.preventDefault(); submit(); } });
    PCV.uxlib.clearEl(host);
    host.appendChild(PCV.uxlib.frag(
      el('h2', null, _t('totp.enroll.title', 'Enroll TOTP')),
      el('p', { class: 'color-muted text-12' }, _t('totp.enroll.scan', 'Scan or enter key manually')),
      el('div', { style: 'text-align:center;margin:10px 0' }, canvas),
      el('div', { class: 'fr' },
        el('label', null, _bi('키', 'Key')),
        el('code', { class: 'break-all', style: 'user-select:all' }, secret)),
      el('div', { class: 'fr' },
        el('label', { for: 'totp-enroll-code' }, _t('totp.enroll.confirm', 'Confirm with app code')),
        codeInput),
      errEl,
      el('div', { class: 'text-right mt-12' },
        el('button', { class: 'btn btn-g', onClick: submit }, _t('totp.enroll.confirm', 'Confirm')),
        ' ',
        el('button', { class: 'btn btn-r', onClick: function () { closeModal(); } }, _t('btn.cancel', 'Cancel')))));
    try { codeInput.focus(); } catch (e) {                }
  }

  function _onEnrollConfirmed(ctx, body) {
    closeModal();                                         
    var codes = body && body.recovery_codes;
    if (codes && codes.length) showRecoveryCodes(codes);
    if (ctx.mode === 'login') {
      if (typeof ctx.finishLogin === 'function') ctx.finishLogin(ctx.user, body);
    } else if (typeof ctx.onDone === 'function') {
      ctx.onDone();
    }
  }

                          
                             
                                                               
                                                          
                                                          
                                                      
                                                         
    
                         
                                                        
                                                          
     
  function showRecoveryCodes(codes) {
    var el = PCV.uxlib.el;
    var list = el('div', { style: 'display:flex;flex-wrap:wrap;gap:6px;margin:10px 0' },
      (codes || []).map(function (c) {
        return el('code', { style: 'display:inline-block;padding:4px 8px;background:var(--bg3);border:1px solid var(--border);border-radius:4px;font-size:13px;user-select:all' }, c);
      }));
    var copyBtn = el('button', { class: 'btn' }, _bi('복사', 'Copy'));
    copyBtn.addEventListener('click', function () {
      var text = (codes || []).join('\n');
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).then(function () {
          if (typeof toast === 'function') toast(_bi('복사됨', 'Copied'));
        }, function () {});
      }
    });
    var saveBtn = el('button', { class: 'btn btn-g' }, _t('totp.recovery.saved', 'I saved them'));
                                                           
                                                            
                                                                
                                                          
    var dlg = showModal(HN.card(_t('totp.recovery.title', 'Recovery codes'), [
      el('p', { class: 'color-yellow text-12' }, _t('totp.recovery.notice', 'Shown only once — store safely')),
      list,
      el('div', { class: 'flex gap-8 mt-12 justify-end' }, copyBtn, saveBtn)
    ]), { noDismiss: true });
    saveBtn.addEventListener('click', function () { PCV.modalCore.closeDialog(dlg); });
  }

                                
  function renderSettingsCard(container) {
    if (!container) return;
    PCV.uxlib.setMsg(container, 'loading', { tag: 'div', style: 'padding:14px' }, _bi('불러오는 중...', 'Loading...'));
    fetchGet(EP.AUTH_TOTP_STATUS()).then(function (r) {
      _paintSettingsCard(container, unwrapData(r) || {});
    }).catch(function () {
      _paintSettingsCard(container, null);
    });
  }

  function _paintSettingsCard(container, st) {
    if (!container) return;
    var el = PCV.uxlib.el, clearEl = PCV.uxlib.clearEl, frag = PCV.uxlib.frag;
    var title = _t('totp.settings.title', 'Two-factor (TOTP)');
    if (st === null) {
      clearEl(container);
      container.appendChild(HN.card(title, el('p', { class: 'color-red text-12' }, _bi('상태 조회 실패', 'Failed to load status'))));
      return;
    }
    var confirmed = !!st.confirmed;
    var badge = HN.statusPill(confirmed ? 'ok' : 'idle',
      confirmed ? _t('totp.settings.enabled', 'Enabled') : _t('totp.settings.disabled', 'Disabled'));
    var kids = [];
    if (confirmed) {
      kids.push(HN.row(_t('totp.settings.remaining', 'Recovery codes left'),
        String(st.recovery_remaining != null ? st.recovery_remaining : 0)));
      kids.push(el('div', { class: 'flex gap-8 mt-8 flex-wrap' },
        el('button', { class: 'btn btn-r', onClick: function () {
          _promptCode(_t('totp.settings.disable', 'Disable'), function (code, errEl) { _doDisable(container, code, errEl); });
        } }, _t('totp.settings.disable', 'Disable')),
        el('button', { class: 'btn', onClick: function () {
          _promptCode(_t('totp.settings.regen', 'Regenerate codes'), function (code, errEl) { _doRegen(container, code, errEl); });
        } }, _t('totp.settings.regen', 'Regenerate codes'))));
    } else {
      kids.push(el('p', { class: 'color-muted text-12 mt-4' }, _t('totp.enroll.scan', 'Scan or enter key manually')));
      kids.push(el('button', { class: 'btn btn-g mt-8', onClick: function () { _beginSessionEnroll(container); } },
        _t('totp.settings.enable', 'Enable')));
    }
    var titleNode = el('span', null, title, ' ', badge);
    clearEl(container);
    container.appendChild(HN.card(titleNode, frag(kids)));
  }

  function _beginSessionEnroll(container) {
    _openEnrollWizard({ mode: 'session', onDone: function () { renderSettingsCard(container); } });
  }

                                             
  function _promptCode(title, onCode) {
    var el = PCV.uxlib.el;
    var input = el('input', { id: 'totp-code-input', class: 'login-input', inputmode: 'numeric',
      maxlength: '11', autocomplete: 'one-time-code', placeholder: '000000',
      style: 'text-align:center;letter-spacing:3px' });
    var errEl = el('div', { class: 'color-red text-12', style: 'min-height:16px', role: 'alert' });
    function go() { onCode(input.value.trim(), errEl); }
    input.addEventListener('keydown', function (ev) { if (ev.key === 'Enter') { ev.preventDefault(); go(); } });
    showModal([
      el('h2', null, title),
      el('p', { class: 'color-muted text-12' }, _t('login.totp.prompt', 'Enter the 6-digit code')),
      el('div', { class: 'fr' }, input),
      errEl,
      el('div', { class: 'text-right mt-12' },
        el('button', { class: 'btn btn-g', onClick: go }, _t('btn.confirm', 'Confirm')),
        ' ',
        el('button', { class: 'btn btn-r', onClick: function () { closeModal(); } }, _t('btn.cancel', 'Cancel')))
    ]);
    try { input.focus(); } catch (e) {                }
  }

                             
                                                                        
                                                                          
                                                               
                                                                
                                                                     
                                                             
                                                     
    
                         
                                                       
                                                        
                                                    
     
  function _doDisable(container, code, errEl) {
    if (!code) { errEl.textContent = _t('login.required', 'Required'); return; }
    errEl.textContent = _t('msg.processing', '처리 중...');
    fetchPost(EP.AUTH_TOTP_DISABLE(), { code: code }).then(function (d) {
      if (d && d.error) { errEl.textContent = _errText(d); return; }
      closeModal();
      if (typeof toast === 'function') toast(_t('totp.settings.disabled', 'Disabled'));
      renderSettingsCard(container);
    }).catch(function (e) { errEl.textContent = (e && e.message) ? e.message : String(e); });
  }

  function _doRegen(container, code, errEl) {
    if (!code) { errEl.textContent = _t('login.required', 'Required'); return; }
    errEl.textContent = _t('msg.processing', '처리 중...');
    fetchPost(EP.AUTH_TOTP_RECOVERY(), { code: code }).then(function (d) {
      if (d && d.error) { errEl.textContent = _errText(d); return; }
      closeModal();
      var data = unwrapData(d) || {};
      var codes = data.recovery_codes || d.recovery_codes;
      if (codes && codes.length) showRecoveryCodes(codes);
      renderSettingsCard(container);
    }).catch(function (e) { errEl.textContent = (e && e.message) ? e.message : String(e); });
  }

                                       
                             
                                                                          
                                                                        
                                                                      
                                                           
                                                          
                                                  
    
                         
                                                         
                                                            
                                              
     
  function adminResetButton(username, onDone) {
    var btn = PCV.uxlib.el('button', { class: 'btn btn-r btn-xxs',
      'aria-label': _t('totp.settings.reset', 'Reset TOTP') + ' ' + username }, _t('totp.settings.reset', 'Reset TOTP'));
    btn.addEventListener('click', function () { _adminReset(username, onDone); });
    return btn;
  }

  function _adminReset(username, onDone) {
    Promise.resolve(customConfirm(_t('totp.settings.reset', 'Reset TOTP'), username + '?')).then(function (ok) {
      if (!ok) return;
      fetchPost(EP.AUTH_TOTP_RESET(), { username: username }).then(function (d) {
        if (d && d.error) { if (typeof toast === 'function') toast(_errText(d), false); return; }
        if (typeof toast === 'function') toast(_t('totp.settings.reset', 'Reset TOTP') + ': ' + username);
        if (typeof addEvt === 'function') addEvt('IAM TOTP reset — ' + username);
        if (typeof onDone === 'function') onDone();
      }).catch(function (e) { if (typeof toast === 'function') toast((e && e.message) ? e.message : String(e), false); });
    });
  }

                                                        
  PCV.totp = {
    beginForcedEnroll: beginForcedEnroll,
    showRecoveryCodes: showRecoveryCodes,
    renderSettingsCard: renderSettingsCard,
    adminResetButton: adminResetButton
  };

})(window.PCV);
