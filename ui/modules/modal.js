/* ═══════════════════════════════════════════════════════════════
   PureCVisor — modules/modal.js (F-2)
   Unified Modal component. Replaces ad-hoc inline modals.

   Usage:
     Modal.show({
       title: '제목',
       body: '<p>HTML 또는 DOM 노드</p>',
       width: 360,
       actions: [
         { label: '취소', secondary: true },
         { label: '저장', primary: true, onClick: function(){...} }
       ],
       onClose: function(){...}
     });
     Modal.close();

   Built-in helpers:
     Modal.showRegister()
     Modal.showChangePassword()

   Features:
     - Single overlay element reused (no DOM bloat)
     - ESC key + backdrop click to close
     - Focus trap (Tab cycles within modal)
     - i18n hooks via t()
     - Theme-aware (uses CSS variables)
   ═══════════════════════════════════════════════════════════════ */

window.PCV = window.PCV || {};
(function(PCV) {
  var overlay = null;
  var prevFocus = null;
  var currentOpts = null;

  function _t(key, fallback) {
    if (typeof t === 'function') {
      var val = t(key);
      if (val != null && val !== '') return val;
    }
    return fallback || key;
  }

  function _ensureOverlay() {
    if (overlay) return overlay;
    overlay = document.createElement('div');
    overlay.id = 'pcv-modal-overlay';
    overlay.setAttribute('role', 'dialog');
    overlay.setAttribute('aria-modal', 'true');
    overlay.style.cssText =
      'display:none;position:fixed;inset:0;background:rgba(0,0,0,0.7);' +
      'z-index:10000;align-items:center;justify-content:center;backdrop-filter:blur(4px)';
    overlay.addEventListener('click', function(e) {
      if (e.target === overlay) Modal.close();
    });
    document.body.appendChild(overlay);

    document.addEventListener('keydown', function(e) {
      if (overlay.style.display !== 'flex') return;
      if (e.key === 'Escape') { Modal.close(); }
      if (e.key === 'Tab') { _trapFocus(e); }
    });
    return overlay;
  }

  function _trapFocus(e) {
    var focusables = overlay.querySelectorAll(
      'input,button,select,textarea,a[href],[tabindex]:not([tabindex="-1"])'
    );
    if (focusables.length === 0) return;
    var first = focusables[0], last = focusables[focusables.length - 1];
    if (e.shiftKey && document.activeElement === first) {
      e.preventDefault(); last.focus();
    } else if (!e.shiftKey && document.activeElement === last) {
      e.preventDefault(); first.focus();
    }
  }


  var Modal = {
    show: function(opts) {
      currentOpts = opts || {};
      _ensureOverlay();
      prevFocus = document.activeElement;

      var width = (opts.width || 360) + 'px';
      var mkEl = PCV.uxlib.el;
      var actionsEl = null;
      if (opts.actions && opts.actions.length) {
        actionsEl = mkEl('div', { style: 'display:flex;gap:8px;justify-content:flex-end;margin-top:16px' },
          opts.actions.map(function(a, i) {
            var bg = a.primary ? '' :
                     a.danger  ? 'background:var(--red);color:#fff' :
                                'background:var(--bg3)';
            var cls = a.primary ? 'btn login-btn' : a.danger ? 'btn btn-r' : 'btn';
            return mkEl('button', { class: cls, 'data-pcv-action': String(i), style: 'margin:0;' + bg }, a.label);
          }));
      }

      /* body: Node(권장) | HTML 문자열(레거시 — innerHTML 파싱 경로) */
      var bodyEl = mkEl('div', { id: 'pcv-modal-body' });
      if (typeof opts.body === 'string') { bodyEl.innerHTML = opts.body; }
      else if (opts.body && opts.body.nodeType) { bodyEl.appendChild(opts.body); }

      PCV.uxlib.clearEl(overlay);
      overlay.appendChild(mkEl('div', { role: 'document', style: 'background:var(--bg2);border:1px solid var(--accent);border-radius:8px;padding:24px;min-width:' + width + ';max-width:90vw;box-shadow:0 0 24px rgba(0,240,255,0.2);color:var(--fg)' },
        mkEl('div', { style: 'font-size:18px;font-weight:bold;color:var(--accent);margin-bottom:16px;display:flex;justify-content:space-between;align-items:center' },
          mkEl('span', null, opts.title || ''),
          mkEl('button', { id: 'pcv-modal-close-x', 'aria-label': 'Close', style: 'background:none;border:none;color:var(--fg2);font-size:20px;cursor:pointer;padding:0 4px' }, '×')),
        bodyEl,
        actionsEl));

      /* Wire close button */
      overlay.querySelector('#pcv-modal-close-x').addEventListener('click', Modal.close);

      /* Wire action buttons */
      if (opts.actions) {
        var btns = overlay.querySelectorAll('[data-pcv-action]');
        btns.forEach(function(btn) {
          btn.addEventListener('click', function() {
            var idx = parseInt(btn.getAttribute('data-pcv-action'), 10);
            var a = opts.actions[idx];
            if (a && typeof a.onClick === 'function') {
              var keep = a.onClick();
              if (keep !== false) Modal.close();
            } else {
              Modal.close();
            }
          });
        });
      }

      overlay.style.display = 'flex';
      /* Focus first focusable */
      setTimeout(function() {
        var first = overlay.querySelector('input,select,textarea,button[data-pcv-action]');
        if (first) first.focus();
      }, 50);
    },

    close: function() {
      if (!overlay) return;
      overlay.style.display = 'none';
      PCV.uxlib.clearEl(overlay);
      if (currentOpts && typeof currentOpts.onClose === 'function') {
        try { currentOpts.onClose(); } catch (e) {}
      }
      currentOpts = null;
      if (prevFocus && prevFocus.focus) { try { prevFocus.focus(); } catch (e) {} }
    },

    setMessage: function(text, type) {
      var el = overlay && overlay.querySelector('#pcv-modal-msg');
      if (!el) return;
      el.textContent = text || '';
      el.className = 'text-sm';
      if (type === 'error') el.style.color = 'var(--red)';
      else if (type === 'success') el.style.color = 'var(--accent)';
      else el.style.color = '';
    },

    /* ── Built-in: 회원가입 ─────────────────────────── */
    showRegister: function() {
      var body =
        '<div style="margin-bottom:12px"><input aria-label="' + _t('register.user_ph', 'Username (3-32, a-z 0-9 _)') + '" id="pcv-reg-user" class="login-input" ' +
        'placeholder="' + _t('register.user_ph', 'Username (3-32, a-z 0-9 _)') + '" ' +
        'autocomplete="username" style="width:100%"></div>' +
        '<div style="margin-bottom:12px"><input aria-label="' + _t('register.pass_ph', 'Password (8자 이상)') + '" id="pcv-reg-pass" type="password" class="login-input" ' +
        'placeholder="' + _t('register.pass_ph', 'Password (8자 이상)') + '" ' +
        'autocomplete="new-password" style="width:100%"></div>' +
        '<div style="margin-bottom:16px"><input aria-label="' + _t('register.pass2_ph', 'Password 확인') + '" id="pcv-reg-pass2" type="password" class="login-input" ' +
        'placeholder="' + _t('register.pass2_ph', 'Password 확인') + '" ' +
        'autocomplete="new-password" style="width:100%"></div>' +
        '<div id="pcv-modal-msg" class="text-sm" style="min-height:18px" role="alert" aria-live="assertive"></div>' +
        '<div class="text-xs color-muted mt-12" style="text-align:center">' +
        _t('register.note', '가입 후 기본 권한: VIEWER (조회 전용). 관리자가 권한 승격 가능.') + '</div>';

      Modal.show({
        title: _t('register.title', '회원가입'),
        body: body,
        width: 360,
        actions: [
          { label: _t('btn.cancel', '취소') },
          { label: _t('btn.register', '가입'), primary: true, onClick: function() {
            var u = document.getElementById('pcv-reg-user').value.trim();
            var p = document.getElementById('pcv-reg-pass').value;
            var p2 = document.getElementById('pcv-reg-pass2').value;
            if (!/^[a-z0-9_]{3,32}$/.test(u)) { Modal.setMessage(_t('register.err_user', '사용자명은 3-32자 (a-z, 0-9, _)'), 'error'); return false; }
            if (p.length < 8) { Modal.setMessage(_t('register.err_pass', '비밀번호는 8자 이상'), 'error'); return false; }
            if (p !== p2) { Modal.setMessage(_t('register.err_match', '비밀번호 확인이 일치하지 않습니다'), 'error'); return false; }
            if (u === p) { Modal.setMessage(_t('register.err_same', '비밀번호는 사용자명과 달라야 합니다'), 'error'); return false; }
            Modal.setMessage(_t('msg.processing', '처리 중...'));
            fetch(EP.AUTH_REGISTER(), {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify({ username: u, password: p })
            }).then(function(r) { return r.json().then(function(d) { return { status: r.status, body: d }; }); })
              .then(function(res) {
                if (res.status === 201) {
                  Modal.setMessage(_t('register.ok', '가입 성공! 로그인 화면으로 돌아갑니다.'), 'success');
                  setTimeout(function() {
                    Modal.close();
                    var lu = document.getElementById('login-user');
                    var lp = document.getElementById('login-pass');
                    if (lu) lu.value = u;
                    if (lp) lp.focus();
                  }, 1200);
                } else {
                  var em = (res.body && res.body.error && res.body.error.message) || ('HTTP ' + res.status);
                  if (res.status === 403) em = _t('register.err_disabled', '회원가입이 비활성화되어 있습니다 (관리자에게 문의)');
                  Modal.setMessage(em, 'error');
                }
              })
              .catch(function(e) { Modal.setMessage((_t('msg.network_err', '네트워크 오류') + ': ' + e.message), 'error'); });
            return false; /* keep modal open */
          }}
        ]
      });
    },

    /* ── Built-in: 비밀번호 변경 ─────────────────────── */
    showChangePassword: function() {
      var body =
        '<div style="margin-bottom:12px"><input aria-label="' + _t('changepw.old_ph', '현재 비밀번호') + '" id="pcv-cp-old" type="password" class="login-input" ' +
        'placeholder="' + _t('changepw.old_ph', '현재 비밀번호') + '" ' +
        'autocomplete="current-password" style="width:100%"></div>' +
        '<div style="margin-bottom:12px"><input aria-label="' + _t('changepw.new_ph', '새 비밀번호 (8자 이상)') + '" id="pcv-cp-new" type="password" class="login-input" ' +
        'placeholder="' + _t('changepw.new_ph', '새 비밀번호 (8자 이상)') + '" ' +
        'autocomplete="new-password" style="width:100%"></div>' +
        '<div style="margin-bottom:16px"><input aria-label="' + _t('changepw.new2_ph', '새 비밀번호 확인') + '" id="pcv-cp-new2" type="password" class="login-input" ' +
        'placeholder="' + _t('changepw.new2_ph', '새 비밀번호 확인') + '" ' +
        'autocomplete="new-password" style="width:100%"></div>' +
        '<div id="pcv-modal-msg" class="text-sm" style="min-height:18px" role="alert" aria-live="assertive"></div>' +
        '<div class="text-xs color-muted mt-12" style="text-align:center">' +
        _t('changepw.note', '변경 후 다른 디바이스의 모든 세션이 자동 로그아웃됩니다.') + '</div>';

      Modal.show({
        title: '\uD83D\uDD12 ' + _t('changepw.title', '비밀번호 변경'),
        body: body,
        width: 360,
        actions: [
          { label: _t('btn.cancel', '취소') },
          { label: _t('btn.change', '변경'), primary: true, onClick: function() {
            var op = document.getElementById('pcv-cp-old').value;
            var np = document.getElementById('pcv-cp-new').value;
            var np2 = document.getElementById('pcv-cp-new2').value;
            if (!op) { Modal.setMessage(_t('changepw.err_old', '현재 비밀번호를 입력하세요'), 'error'); return false; }
            if (np.length < 8) { Modal.setMessage(_t('changepw.err_short', '새 비밀번호는 8자 이상'), 'error'); return false; }
            if (np !== np2) { Modal.setMessage(_t('changepw.err_match', '새 비밀번호 확인이 일치하지 않습니다'), 'error'); return false; }
            if (op === np) { Modal.setMessage(_t('changepw.err_same', '새 비밀번호는 현재와 달라야 합니다'), 'error'); return false; }
            Modal.setMessage(_t('msg.processing', '처리 중...'));
            var token = sessionStorage.getItem('pcv_token') || sessionStorage.getItem('access_token') || '';
            fetch(EP.AUTH_PASSWORD(), {
              method: 'POST',
              headers: { 'Content-Type': 'application/json', 'Authorization': 'Bearer ' + token },
              body: JSON.stringify({ old_password: op, new_password: np })
            }).then(function(r) { return r.json().then(function(d) { return { status: r.status, body: d }; }); })
              .then(function(res) {
                if (res.status === 200) {
                  Modal.setMessage(_t('changepw.ok', '변경 완료. 3초 후 다시 로그인합니다.'), 'success');
                  setTimeout(function() { sessionStorage.clear(); location.reload(); }, 3000);
                } else {
                  var em = (res.body && res.body.error && res.body.error.message) || ('HTTP ' + res.status);
                  Modal.setMessage(em, 'error');
                }
              })
              .catch(function(e) { Modal.setMessage(_t('msg.network_err', '네트워크 오류') + ': ' + e.message, 'error'); });
            return false;
          }}
        ]
      });
    }
  };

  /* ── PCV.modal namespace export ─────────────────── */
  PCV.modal = Modal;

  /* ── Backward-compat global shims ─────────────── */
  window.Modal = Modal;
  window.showRegisterModal = function() { Modal.showRegister(); };
  window.showChangePwModal = function() { Modal.showChangePassword(); };
})(window.PCV);
