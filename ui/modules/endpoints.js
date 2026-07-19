/**
 * @module endpoints
 * @description REST API 엔드포인트 중앙 레지스트리
 *
 * [목적]
 *   80+ 엔드포인트 경로를 한 곳에서 관리하여:
 *   1. 백엔드 경로 변경 시 수정 지점 단일화
 *   2. 프론트엔드↔백엔드 정합성 자동 검증 가능
 *   3. 오타/불일치 방지
 *
 * [사용법]
 *   var url = EP.VM_STOP('web-prod');  // → '/api/v1/vms/web-prod/stop'
 */

window.PCV = window.PCV || {};
(function(PCV) {

var EP = (function() {
  var B = function() { return window.API_BASE || '/api/v1'; };
  var enc = encodeURIComponent;

  var COMMON_ENDPOINTS = {
    /* ═══ VM ═══ */
    VM_LIST:              function()     { return B() + '/vms'; },
    UPDATE_CHECK:         function()     { return B() + '/update-check'; },
    VM_CREATE:            function()     { return B() + '/vms'; },
    VM_DETAIL:            function(n)    { return B() + '/vms/' + enc(n); },
    VM_STOP:              function(n)    { return B() + '/vms/' + enc(n) + '/stop'; },
    VM_ACTION:            function(n,a)  { return B() + '/vms/' + enc(n) + '/' + enc(a); },
    VM_SNAPSHOT_LIST:     function(n)    { return B() + '/vms/' + enc(n) + '/snapshot'; },
    VM_SNAPSHOT_CREATE:   function(n)    { return B() + '/vms/' + enc(n) + '/snapshot/create'; },
    VM_SNAPSHOT_ROLLBACK: function(n)    { return B() + '/vms/' + enc(n) + '/snapshot/rollback'; },
    VM_SNAPSHOT_DELETE:   function(n,s)  { return B() + '/vms/' + enc(n) + '/snapshot/' + enc(s); },
    VM_SNAPSHOT_DELETE_ALL:function(n)   { return B() + '/vms/' + enc(n) + '/snapshot/delete_all'; },
    VM_NICS:              function(n)    { return B() + '/vms/' + enc(n) + '/nics'; },
    VM_NIC_DETACH:        function(n,m)  { return B() + '/vms/' + enc(n) + '/nics/' + enc(m); },
    VM_VCPU:              function(n)    { return B() + '/vms/' + enc(n) + '/vcpu'; },
    VM_MEMORY:            function(n)    { return B() + '/vms/' + enc(n) + '/memory_mb'; },
    VM_ISO:               function(n)    { return B() + '/vms/' + enc(n) + '/iso'; },
    VM_CLONE:             function(n)    { return B() + '/vms/' + enc(n) + '/clone'; },
    VM_DISK:              function(n)    { return B() + '/vms/' + enc(n) + '/disk'; },
    VM_DELETE_STATUS:     function(n)    { return B() + '/vms/' + enc(n) + '/delete-status'; },
    VM_RENAME:            function(n)    { return B() + '/vms/' + enc(n) + '/rename'; },
    VM_EXPORT:            function(n)    { return B() + '/vms/' + enc(n) + '/export'; },
    VM_CPU_PIN:           function(n)    { return B() + '/vms/' + enc(n) + '/cpu-pin'; },
    VM_BANDWIDTH:         function(n)    { return B() + '/vms/' + enc(n) + '/bandwidth'; },
    VM_RPC:               function(n)    { return B() + '/vms/' + enc(n) + '/rpc'; },
    VM_DISK_RESIZE:       function(n)    { return B() + '/vms/' + enc(n) + '/disk-resize'; },
    VM_GUEST_PING:        function(n)    { return B() + '/vms/' + enc(n) + '/guest-ping'; },
    VM_GUEST_AGENT:       function(n)    { return B() + '/vms/' + enc(n) + '/guest-agent'; },
    VM_GUEST_AGENT_CHANNEL:function(n)   { return B() + '/vms/' + enc(n) + '/guest-agent-channel'; },
    VM_GUEST_SHUTDOWN:    function(n)    { return B() + '/vms/' + enc(n) + '/guest-shutdown'; },
    VM_GUEST_EXEC:        function(n)    { return B() + '/vms/' + enc(n) + '/guest-exec'; },
    VM_DISK_USAGE:        function(n)    { return B() + '/vms/' + enc(n) + '/disk-usage'; },
    VNC:                  function(n)    { return B() + '/vnc/' + enc(n); },

    /* ═══ CONTAINER ═══ */
    CTR_LIST:             function()     { return B() + '/containers'; },
    CTR_CREATE:           function()     { return B() + '/containers'; },
    CTR_DETAIL:           function(n)    { return B() + '/containers/' + enc(n); },
    CTR_METRICS:          function(n)    { return B() + '/containers/' + enc(n) + '/metrics'; },
    CTR_EXEC:             function(n)    { return B() + '/containers/' + enc(n) + '/exec'; },
    CTR_NICS:             function(n)    { return B() + '/containers/' + enc(n) + '/nics'; },
    CTR_SNAPSHOTS:        function(n)    { return B() + '/containers/' + enc(n) + '/snapshots'; },
    CTR_SNAP_DELETE:      function(n,s)  { return B() + '/containers/' + enc(n) + '/snapshots/' + enc(s); },
    CTR_SNAP_ROLLBACK:    function(n)    { return B() + '/containers/' + enc(n) + '/snapshots/rollback'; },
    CTR_STOP:             function(n)    { return B() + '/containers/' + enc(n) + '/stop'; },
    CTR_START:            function(n)    { return B() + '/containers/' + enc(n) + '/start'; },
    CTR_LIMITS:           function(n)    { return B() + '/containers/' + enc(n) + '/limits'; },
    CTR_BANDWIDTH:        function(n)    { return B() + '/containers/' + enc(n) + '/bandwidth'; },

    /* ═══ STORAGE ═══ */
    STORAGE_POOLS:        function()     { return B() + '/storage/pools'; },
    STORAGE_SCRUB:        function()     { return B() + '/storage/pools/scrub'; },
    STORAGE_ZVOLS:        function()     { return B() + '/storage/zvols'; },
    ISCSI_TARGETS:        function()     { return B() + '/iscsi/targets'; },

    /* ═══ NETWORK ═══ */
    NET_LIST:             function()     { return B() + '/networks'; },
    NET_DETAIL:           function(n)    { return B() + '/networks/' + enc(n); },
    NET_MODE:             function(n)    { return B() + '/networks/' + enc(n) + '/mode'; },
    OVN_STATUS:           function()     { return B() + '/ovn/status'; },
    OVN_SWITCHES:         function()     { return B() + '/ovn/switches'; },
    OVN_ROUTERS:          function()     { return B() + '/ovn/routers'; },
    OVN_ACL:              function()     { return B() + '/ovn/acl'; },
    DEMO_OVN_HEALTH:      function()     { return B() + '/demo/ovn-ovs/health'; },
    OVERLAY_LIST:         function()     { return B() + '/overlay'; },

    /* ═══ CLOUD ═══ */
    CLOUD_JOBS:           function()     { return B() + '/cloud/jobs'; },
    CLOUD_CANCEL:         function()     { return B() + '/cloud/cancel'; },
    CLOUD_IMPORT:         function(n)    { return B() + '/vms/' + enc(n) + '/import-ec2'; },
    CLOUD_EXPORT:         function(n)    { return B() + '/vms/' + enc(n) + '/export-ec2'; },

    /* ═══ AUTH ═══ */
    AUTH_TOKEN:           function()     { return B() + '/auth/token'; },
    AUTH_REGISTER:        function()     { return B() + '/auth/register'; },
    AUTH_PASSWORD:        function()     { return B() + '/auth/password'; },
    AUTH_REFRESH:         function()     { return B() + '/auth/refresh'; },
    AUTH_USERS:           function()     { return B() + '/auth/users'; },
    AUTH_USER:            function(u)    { return B() + '/auth/users/' + enc(u); },
    AUTH_ROLE:            function()     { return B() + '/auth/role'; },

    /* ═══ MONITOR ═══ */
    HEALTH:               function()     { return B() + '/health'; },
    ALERTS:               function()     { return B() + '/alerts'; },
    ALERTS_CONFIG:        function()     { return B() + '/alerts/config'; },
    METRICS:              function()     { return B() + '/metrics'; },
    MONITOR_FLEET:        function()     { return B() + '/monitor/fleet'; },
    DPDK_STATUS:          function()     { return B() + '/dpdk/status'; },
    DPDK_LIST:            function()     { return B() + '/dpdk/list'; },
    DPDK_HUGEPAGE:        function()     { return B() + '/dpdk/hugepage'; },
    DPDK_BIND:            function()     { return B() + '/dpdk/bind'; },
    DPDK_UNBIND:          function()     { return B() + '/dpdk/unbind'; },
    SRIOV_STATUS:         function()     { return B() + '/sriov/status'; },
    SRIOV_LIST:           function()     { return B() + '/sriov/list'; },
    SRIOV_ENABLE:         function()     { return B() + '/sriov/enable'; },
    SRIOV_DISABLE:        function()     { return B() + '/sriov/disable'; },
    SRIOV_ATTACH:         function()     { return B() + '/sriov/attach'; },
    SRIOV_DETACH:         function()     { return B() + '/sriov/detach'; },

    /* ═══ ADVANCED ═══ */
    TEMPLATES:            function()     { return B() + '/templates'; },
    TEMPLATE:             function(n)    { return B() + '/templates/' + enc(n); },
    TEMPLATE_HISTORY:     function()     { return B() + '/templates/history'; },
    BACKUP_POLICIES:      function()     { return B() + '/backup/policies'; },
    BACKUP_RESTORE:       function()     { return B() + '/backup/restore'; },
    CONFIG_BACKUP:        function()     { return B() + '/config/backup'; },
    CONFIG_HISTORY:       function()     { return B() + '/config/history'; },
    CONFIG_DAEMON:        function()     { return B() + '/config/daemon'; },
    AGENT_CONFIG:         function()     { return B() + '/agent/config'; },
    AGENT_HISTORY:        function()     { return B() + '/agent/history'; },

    /* ═══ ISO ═══ */
    ISO_LIST:             function()     { return B() + '/iso'; },

    /* ═══ [백엔드 4차] 신규 엔드포인트 ═══ */

    /* 보안 — API Key + 세션 */
    AUTH_APIKEY_CREATE:   function()     { return B() + '/auth/apikeys'; },
    AUTH_APIKEY_LIST:     function()     { return B() + '/auth/apikeys'; },
    AUTH_APIKEY_REVOKE:   function(n)    { return B() + '/auth/apikeys/' + enc(n) + '/revoke'; },
    AUTH_SESSION_REVOKE:  function()     { return B() + '/auth/sessions/revoke'; },

    /* VM 배치 + 필터 */
    VM_BATCH:             function()     { return B() + '/vms/batch'; },
    VM_LIST_FILTERED:     function()     { return B() + '/vms/filtered'; },

    /* 운영 */
    CONFIG_RELOAD:        function()     { return B() + '/config/reload'; },
    HEALTH_DEEP:          function()     { return B() + '/health/deep'; },
    BACKUP_VERIFY:        function()     { return B() + '/backup/verify'; },
    JOBS_PERSIST:         function()     { return B() + '/jobs/persistent'; },
    POOL_CONNINFO:        function()     { return B() + '/pool/conninfo'; },
    DB_MIGRATION:         function()     { return B() + '/db/migration'; },

    /* 알림 음소거/라우팅 */
    ALERT_SILENCE:        function()     { return B() + '/alerts/silence'; },
    ALERT_SILENCE_LIST:   function()     { return B() + '/alerts/silences'; },
    ALERT_ROUTING:        function()     { return B() + '/alerts/routing'; },

    /* 컨테이너 확장 */
    CTR_CLONE:            function(n)    { return B() + '/containers/' + enc(n) + '/clone'; },
    CTR_MEMORY_STATS:     function(n)    { return B() + '/containers/' + enc(n) + '/memory-stats'; },
    CTR_HEALTH:           function(n)    { return B() + '/containers/' + enc(n) + '/health'; },

    /* ═══ OVA ═══ */
    OVA_IMPORT:           function()     { return B() + '/vms/import/import'; },

    /* ═══ GENERIC RPC ═══ */
    RPC:                  function()     { return B() + '/rpc'; }
  };

  function buildEndpointSurface(edition) {
    (void edition);
    return Object.assign({}, COMMON_ENDPOINTS);
  }

  function applyEditionEndpointSurface(edition) {
    (void edition);
    var surface = buildEndpointSurface('single');
    window.PCV_UI_EDITION = 'single';
    PCV.endpoints = surface;
    window.EP = surface;
    return surface;
  }

  PCV.buildEndpointSurface = buildEndpointSurface;
  PCV.applyEditionEndpointSurface = applyEditionEndpointSurface;
  PCV.getOptionalEndpoint = function(name) {
    var registry = window.EP || {};
    var fn = registry[name];
    if (typeof fn !== 'function') return null;
    return fn.apply(null, Array.prototype.slice.call(arguments, 1));
  };

  return applyEditionEndpointSurface('single');
})();

/* ── PCV.endpoints namespace export ─────────────── */
PCV.endpoints = EP;

/* ── Backward-compat global shim ────────────────── */
window.EP = EP;
})(window.PCV);
