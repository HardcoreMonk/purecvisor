#!/usr/bin/env bash
                                                                                 
                                                                                          
                                                           
                                                            
                              

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

shell_tests=(
  tests/integration/test_agent_ai_safe.sh
  tests/integration/test_backend_phase2.sh
  tests/integration/test_core_enhancement.sh
  tests/integration/test_frontend_api.sh
  tests/integration/test_security_groups_safe.sh
  tests/integration/test_security_scan.sh
  tests/integration/test_rbac_template_backup.sh
  tests/integration/run_integration_tests.sh
)

for file in "${shell_tests[@]}"; do
  bash -n "$file"
  grep -Fq 'trap cleanup EXIT' "$file"
  grep -Fq 'CLEANUP-FAIL' "$file"
done

                                                                   
for file in "${shell_tests[@]}"; do
  set +e
  invalid_output=$(PCV_TEST_RUN_ID='../unsafe' bash "$file" 2>&1)
  invalid_rc=$?
  set -e
  [[ "$invalid_rc" -eq 2 ]] || {
    printf 'invalid run-id gate returned %s: %s\n' "$invalid_rc" "$file" >&2
    exit 1
  }
  printf '%s' "$invalid_output" | grep -Fq 'Invalid PCV_TEST_RUN_ID'
done

                                                            
set +e
disposable_output=$(env -u PCV_DISPOSABLE_HOST PCV_TEST_RUN_ID=cleanup-contract \
  bash tests/integration/run_integration_tests.sh 2>&1)
disposable_rc=$?
set -e
[[ "$disposable_rc" -eq 2 ]]
printf '%s' "$disposable_output" | grep -Fq 'REFUSED: set PCV_DISPOSABLE_HOST=1'

                                                                                 
grep -Fq 'BASE_CONFIG_RESULT=' tests/integration/test_agent_ai_safe.sh
grep -Fq 'LOCKOUT_USER="pcv-lock-${RUN_ID}"' tests/integration/test_backend_phase2.sh
grep -Fq 'ALERT_BASE_CAPTURED=1' tests/integration/test_core_enhancement.sh
grep -Fq 'VIEW_USER="pcv-view-${RUN_ID}"' tests/integration/test_frontend_api.sh
grep -Fq 'XSS_SG="pcv-xss-${RUN_ID}"' tests/integration/test_security_groups_safe.sh
grep -Fq 'SCAN_USER="pcv-scan-${RUN_ID}"' tests/integration/test_security_scan.sh
grep -Fq 'OP_USER="pcv-op-${RUN_ID}"' tests/integration/test_rbac_template_backup.sh
grep -Fq 'capture_fixture_baseline' tests/integration/run_integration_tests.sh
grep -Fq 'PCV_DISPOSABLE_HOST' tests/integration/run_integration_tests.sh
grep -Fq 'SERVICE_RESTORE_ARMED=1' tests/integration/run_integration_tests.sh
grep -Fq 'SOCKET_CLEANUP_ARMED=1' tests/integration/run_integration_tests.sh
grep -Fq 'if [ "$SOCKET_CLEANUP_ARMED" -eq 1 ] && [ -e "$SOCKET_PATH" ]; then' tests/integration/run_integration_tests.sh
python3 -c '
from pathlib import Path
s = Path("tests/integration/run_integration_tests.sh").read_text()
guard = "if [ \"$SOCKET_CLEANUP_ARMED\" -eq 1 ] && [ -e \"$SOCKET_PATH\" ]; then\n        rm -f -- \"$SOCKET_PATH\""
assert guard in s
assert s.count("rm -f -- \"$SOCKET_PATH\"") == 1
assert "capture_fixture_baseline || exit 1\ntest_daemon_starts" in s
assert s.index("SERVICE_RESTORE_ARMED=1") < s.index("systemctl stop \"$DAEMON_SERVICE\"")
assert s.index("SOCKET_CLEANUP_ARMED=1") < s.index("\"$DAEMON_BIN\" > \"$LOG_FILE\"")
'

                                            
! grep -Fq '"e2e-viewer"' tests/integration/test_frontend_api.sh
! grep -Fq '"safe-vm"' tests/integration/test_security_scan.sh
! grep -Fq '"test-op"' tests/integration/test_rbac_template_backup.sh
! grep -Fq 'TEST_VM="pcv-test-gio-vm"' tests/integration/run_integration_tests.sh
! grep -Fq 'pkill -f "$DAEMON_SERVICE"' tests/integration/run_integration_tests.sh
! grep -Fq '"interface":"lo"' tests/integration/test_core_enhancement.sh
! grep -Fq 'nonexistent-healing-id-9999' tests/integration/test_agent_ai_safe.sh
! grep -Fq '"name":"test-vm"' tests/integration/test_backend_phase2.sh
! grep -Fq '192.0.2.10' tests/integration/test_backend_phase2.sh
! grep -Fq '"method":"alert.config.set","params":{"expected_revision":$BASE_REV' tests/integration/test_core_enhancement.sh
! grep -Fq 'alert set --' tests/integration/test_core_enhancement.sh
! grep -Fq '"method":"network.qos.remove","params":{"interface"' tests/integration/test_core_enhancement.sh
! grep -Fq 'rm -rf /' tests/integration/test_agent_ai_safe.sh
! grep -Fq 'rm -rf /' tests/integration/test_core_enhancement.sh
! grep -Fq '/import-ec2' tests/integration/test_frontend_api.sh

                                          
grep -Fq 'HEALING_FIXTURE="pcv-healing-${RUN_ID}"' tests/integration/test_agent_ai_safe.sh
grep -Fq 'assert_error_or_unregistered "config.push: missing params rejected"' tests/integration/test_backend_phase2.sh
! grep -Fq 'g_hash_table_insert(g_rpc_routes, "config.push"' src/api/dispatcher.c
! grep -Fq 'g_hash_table_insert(g_rpc_routes, "cluster.node.evacuate"' src/api/dispatcher.c
grep -Fq 'ALERT_BASE_RESULT=' tests/integration/test_core_enhancement.sh
grep -Fq 'network.qos.remove","params":{}' tests/integration/test_core_enhancement.sh
grep -Fq 'BASE_URL="${PCV_TEST_BASE_URL:-http://$HOST}"' tests/integration/test_frontend_api.sh
grep -Fq 'pcv_try_login "$BASE" "$PCV_TEST_ADMIN_USER" "$PCV_TEST_ADMIN_PASSWORD"' tests/integration/test_frontend_api.sh
! grep -Fq 'pcv_resolve_auth "$BASE"' tests/integration/test_frontend_api.sh
grep -Fq 'wait_vm_delete_terminal' tests/integration/test_security_scan.sh
grep -Fq '"data"[[:space:]]*:[[:space:]]*"done"' tests/integration/test_security_scan.sh
grep -Fq 'REFUSED: set dedicated PCV_TEST_ADMIN_USER and PCV_TEST_ADMIN_PASSWORD' tests/integration/test_security_scan.sh
! grep -Fq 'PURECVISOR_ADMIN_USER' tests/integration/test_security_scan.sh

                                                    
                                                        
grep -Fq 'owner_remove_tree(fx->root);' tests/test_container_owner_scope.c
grep -Fq 'lio_remove_tree_and_free' tests/test_lio.c
grep -Fq 'iscsi_remove_tree(f->root);' tests/test_iscsi.c
grep -Fq 'old_config_path = g_strdup(g_getenv("PCV_CONFIG_PATH"))' tests/test_iscsi.c
grep -Fq 'g_setenv("PCV_CONFIG_PATH", "/nonexistent/pcv-test-isolated.conf", TRUE);' tests/test_container_owner_scope.c
for file in tests/test_lio.c tests/test_iscsi.c tests/test_container_owner_scope.c; do
  grep -Fq 'g_assert_cmpint(errno, ==, ENOENT);' "$file"
done
python3 -c '
from pathlib import Path
lio = Path("tests/test_lio.c").read_text()
iscsi = Path("tests/test_iscsi.c").read_text()
owner = Path("tests/test_container_owner_scope.c").read_text()
assert lio.count("g_dir_make_tmp(\"pcvlio") == lio.count("lio_remove_tree_and_free(") - 1
assert iscsi.count("g_dir_make_tmp(") == iscsi.count("iscsi_remove_tree(") - 2
assert owner.count("g_dir_make_tmp(") == owner.count("owner_remove_tree(") - 2
'

echo "PASS: live integration cleanup contracts are present (no live resources touched)"
