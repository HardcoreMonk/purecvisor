                           
                                                                      
  
                           
                                                                                          
  
                                                                     
                                                       
                                         
  
                                                                            
                                                                    
                                                            
   
window.PCV = window.PCV || {};
(function (PCV) {
  var openDialogs = [];                                       
  var _titleSeq = 0;

                                                                        
  function _fillBody(bodyEl, body) {
    if (typeof body === 'string') { bodyEl.innerHTML = body; return; }                  
    PCV.uxlib.clearEl(bodyEl);
    bodyEl.appendChild(PCV.uxlib.frag(body));
  }

                                                          
  function _syncMcId() {
    for (var i = 0; i < openDialogs.length; i++) {
      var b = openDialogs[i].querySelector('.modal-body');
      if (!b) continue;
      if (i === openDialogs.length - 1) b.id = 'mc';
      else if (b.id === 'mc') b.removeAttribute('id');
    }
  }

                                                                      
                                                                     
                                                           
                                  
  function _closeTop() {
    var dlg = openDialogs.pop();
    if (!dlg) return;
    var cb = dlg._pcvOnClose;
    try { if (dlg.open) dlg.close(); } catch {}
    if (dlg.parentNode) dlg.parentNode.removeChild(dlg);
    _syncMcId();
    if (cb) { try { cb(); } catch {} }
  }

                                                                                 
                                                        
                              
    
                                                                     
                                                      
                                                   
                                                           
                                                                    
  function _wireDialog(dlg, noDismiss) {
                                                                     
                                               
    if (!noDismiss) {
      dlg.addEventListener('click', function (e) {
        if (e.target !== dlg) return;
        var r = dlg.getBoundingClientRect();
        var inside = e.clientX >= r.left && e.clientX <= r.right &&
                     e.clientY >= r.top && e.clientY <= r.bottom;
        if (!inside) modalCore.close(false);
      });
    }
                                                                    
                                                                          
    dlg.addEventListener('cancel', function (e) {
      e.preventDefault();
      if (!noDismiss) modalCore.close(false);
    });
                                                                                  
                                                              
                                                                              
    dlg.addEventListener('close', function () {
      var idx = openDialogs.indexOf(dlg);
      if (idx === -1) return;
      openDialogs.splice(idx, 1);
      if (dlg.parentNode) dlg.parentNode.removeChild(dlg);
      _syncMcId();
      if (dlg._pcvOnClose) { try { dlg._pcvOnClose(); } catch {} }
    });
  }

  var modalCore = {
       
                                                     
      
                                                               
                                                          
                                                                   
                                     
                                                             
                                                  
       
    open: function (body, opts) {
      opts = opts || {};
      if (opts.replace) modalCore.close(false);                         

      var dlg = PCV.uxlib.el('dialog', { class: 'modal' + (opts.wide ? ' modal-wide' : '') });
      var bodyEl = PCV.uxlib.el('div', { class: 'modal-body' });
      _fillBody(bodyEl, body);
      dlg.appendChild(bodyEl);
                                                                          
                                                                    
                               
      var _hd = bodyEl.querySelector('h1, h2, h3, h4');
      if (opts.ariaLabel) {
        dlg.setAttribute('aria-label', opts.ariaLabel);
      } else if (_hd) {
        if (!_hd.id) _hd.id = 'pcv-dlg-title-' + (++_titleSeq);
        dlg.setAttribute('aria-labelledby', _hd.id);
      } else {
        dlg.setAttribute('aria-label', 'Dialog');
      }
      dlg._pcvOnClose = (typeof opts.onClose === 'function') ? opts.onClose : null;
                                                                 
                                               
                                          
                                                        
      dlg._pcvNoDismiss = opts.noDismiss === true;
                                                              
                                                    
      if (opts.width) {
        var w = (typeof opts.width === 'number' ? opts.width + 'px' : opts.width);
        dlg.style.width = 'min(' + w + ', 94vw)';
      }

      _wireDialog(dlg, opts.noDismiss);

      document.body.appendChild(dlg);
      openDialogs.push(dlg);
      _syncMcId();
      dlg.showModal();                                                           
      return dlg;
    },

                                                            
                                                                            
                                                                            
    openBare: function (contentEl, opts) {
      opts = opts || {};
      var dlg = PCV.uxlib.el('dialog', {
        class: 'pcv-dialog' + (opts.dialogClass ? ' ' + opts.dialogClass : '')
      });
      dlg.appendChild(PCV.uxlib.frag(contentEl));
                                                                 
                                                                      
                                                
      var _hd = dlg.querySelector('h1, h2, h3, h4');
      if (opts.ariaLabel) {
        dlg.setAttribute('aria-label', opts.ariaLabel);
      } else if (_hd) {
        if (!_hd.id) _hd.id = 'pcv-dlg-title-' + (++_titleSeq);
        dlg.setAttribute('aria-labelledby', _hd.id);
      } else {
        dlg.setAttribute('aria-label', 'Dialog');
      }
      dlg._pcvOnClose = (typeof opts.onClose === 'function') ? opts.onClose : null;
      dlg._pcvNoDismiss = opts.noDismiss === true;
      _wireDialog(dlg, opts.noDismiss);
      document.body.appendChild(dlg);
      openDialogs.push(dlg);
      _syncMcId();                                              
      dlg.showModal();
      return dlg;
    },

                                                                            
                              
                                                            
                                                                  
                                           
    close: function (force) {
      if (force) { while (openDialogs.length) _closeTop(); return; }
      var top = openDialogs.length ? openDialogs.at(-1) : null;
      if (top && top._pcvNoDismiss) return;
      if (openDialogs.length) _closeTop();
    },

                                                                                 
                                                          
                                                                           
                                                                         
                                                                   
                                                                           
    closeDialog: function (dlg) {
      if (!dlg) return;
      var idx = openDialogs.indexOf(dlg);
      var cb = dlg._pcvOnClose;
      if (idx !== -1) openDialogs.splice(idx, 1);
      try { if (dlg.open) dlg.close(); } catch {}
      if (dlg.parentNode) dlg.parentNode.removeChild(dlg);
      _syncMcId();
      if (idx !== -1 && cb) { try { cb(); } catch {} }
    },

    currentDialog: function () {
      return openDialogs.length ? openDialogs.at(-1) : null;
    },

    currentBody: function () {
      var d = modalCore.currentDialog();
      return d ? d.querySelector('.modal-body') : null;
    }
  };

  PCV.modalCore = modalCore;
})(window.PCV);
