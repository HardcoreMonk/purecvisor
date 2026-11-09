#!/usr/bin/env bash




set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source_c="$root_dir/src/modules/dispatcher/handler_backup.c"

for method in restore incremental replicate; do
    rg -q "pcv_job_admit\(\"backup\.${method}\"" "$source_c"
done

job_id_responses="$(rg -c 'json_object_set_string_member\(accepted, "job_id", job_id\)' "$source_c")"
[[ "$job_id_responses" -eq 3 ]] || {
    echo "all three accepted backup responses must contain canonical job_id" >&2
    exit 1
}

if rg -q 'g_strdup_printf\("backup\.(restore|incremental|replicate):' "$source_c"; then
    echo "synthetic backup job ID remains" >&2
    exit 1
fi

rg -q 'pcv_job_progress\(d->job_ref'  "$source_c"
rg -q 'pcv_ws_finish_job\(d->job_ref, PCV_JOB_COMPLETED' "$source_c"
rg -q 'pcv_ws_finish_job\(d->job_ref, PCV_JOB_FAILED' "$source_c"

                                                                           
python3 - "$source_c" <<'PY'
import re, sys
text = open(sys.argv[1], encoding="utf-8").read()
start = text.index("void handle_backup_replicate(")
body = text[start:text.index("\n}\n", start)]
req = body.find("pcv_repl_hostkey_require(target_node")
assert req > 0, "handle_backup_replicate: pcv_repl_hostkey_require 없음"
assert req > body.find("pcv_validate_ssh_user(ssh_user)"), "등록 확인이 인자 검증보다 앞에 있다"
assert req < body.find('pcv_job_admit("backup.replicate"'), "등록 확인이 Job 접수 뒤에 있다"
assert "PURE_RPC_ERR_ZFS_OPERATION, 0" not in text[text.index("static void _replicate_worker("):start], "replicate 감사 코드가 −32000 고정"
assert re.search(r'json_object_set_int_member\(failed, "error_code"', text), "Job 실패 결과에 error_code 없음"
PY

echo "backup canonical job contract: PASS"
