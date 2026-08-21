                                                                  
                                  
                                             
                                                  
                                                                     
                           
                                                                       
                                                   
  
                       
                                                                   
   
window.PCV = window.PCV || {};
(function (PCV) {

var CAP = 720;
var SPANS = { live: 120e3, '15m': 900e3, '1h': 3600e3 };
var _store = Object.create(null);

function buf(key) {
  if (!_store[key]) _store[key] = [];
  return _store[key];
}

                                                          
                                                      
                                            
function push(key, value) {
  var v = Number(value);
  if (!isFinite(v)) return;
  var b = buf(key);
  b.push({ t: Date.now(), v: v });
  if (b.length > CAP) b.splice(0, b.length - CAP);
}

function windowOf(key, span) {
  var ms = SPANS[span] || SPANS['15m'];
  var cutoff = Date.now() - ms;
  return buf(key).filter(function (s) { return s.t >= cutoff; });
}

function latest(key) {
  var b = buf(key);
  return b.length ? b[b.length - 1].v : null;
}

                                                          
                                             
function peak(key, span) {
  var w = windowOf(key, span);
  if (!w.length) return null;
  return w.reduce(function (m, s) { return s.v > m ? s.v : m; }, -Infinity);
}

                                                  
                                                    
                                                   
function prune(prefix, keepKeys) {
  if (!prefix) return 0;
  var keep = Object.create(null);
  (keepKeys || []).forEach(function (k) { keep[k] = 1; });
  var removed = 0;
  Object.keys(_store).forEach(function (k) {
    if (k.indexOf(prefix) !== 0) return;
    if (keep[k]) return;
    delete _store[k];
    removed++;
  });
  return removed;
}

PCV.metrics = { push: push, window: windowOf, latest: latest, peak: peak, prune: prune,
  SPANS: SPANS, _buf: buf              };
})(window.PCV);
