                              
                                                       
                                                 
                                                               
  
                           
                                                      
  
                                                                  
                                            
  
         
                                                         
                                                     
                                                       
                                                
                                                             
                                  
                                                             
                                                                
                          
                                                         
                                                 
  
                                                              
                                                     
   
(function (PCV) {
  var _state = {};
  var _subs = [];

  function serialize(state) {
    return Object.keys(state)
      .filter(function (k) { return state[k] && state[k].length; })
      .map(function (k) {
        return encodeURIComponent(k) + '=' + state[k].map(encodeURIComponent).join(',');
      })
      .join('&');
  }
  function parse(search) {
    var out = {};
    var qs = (search || '').replace(/^\?/, '');
    if (!qs) return out;
    qs.split('&').forEach(function (pair) {
      var eq = pair.indexOf('=');
      if (eq < 0) return;
                                                                      
                                                                
                                                                    
      try {
        var key = decodeURIComponent(pair.slice(0, eq));
        var raw = pair.slice(eq + 1);
                                                               
                                                                    
                                      
        if (raw !== '') out[key] = raw.split(',').map(decodeURIComponent);
      } catch {                                                                 }
    });
    return out;
  }
  function _copy(s) { return JSON.parse(JSON.stringify(s)); }
                                                                   
                                                                                   
                                                                 
  function _notify() { _subs.slice().forEach(function (fn) { fn(_copy(_state)); }); }
  function _syncUrl() {
    var qs = serialize(_state);
    var url = location.pathname + (qs ? '?' + qs : '') + location.hash;
    history.replaceState(null, '', url);
  }
  function apply(patch) {
    Object.keys(patch).forEach(function (k) {
      _state[k] = Array.isArray(patch[k]) ? patch[k].slice() : patch[k];
    });
    _syncUrl();
    _notify();
  }
  function readFromUrl() { _state = parse(location.search); _notify(); }
  function subscribe(fn) {
    _subs.push(fn);
    return function () { var i = _subs.indexOf(fn); if (i >= 0) _subs.splice(i, 1); };
  }

  PCV.ui = PCV.ui || {};
  PCV.ui.filterState = {
    serialize: serialize, parse: parse,
    current: function () { return _copy(_state); },
    apply: apply, readFromUrl: readFromUrl, subscribe: subscribe,
  };

                                                               
                                                                 
                                       
  window.addEventListener('popstate', function () { readFromUrl(); });
})(window.PCV);
