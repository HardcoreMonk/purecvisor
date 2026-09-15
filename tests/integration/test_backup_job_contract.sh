#!/usr/bin/env bash




set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source_c="$root_dir/src/modules/dispatcher/handler_backup.c"

for method in restore incremental replicate; do
    rg -q "pcv_job_create\(\"backup\.${method}\"" "$source_c"
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

rg -q 'pcv_job_update_status\(d->job_id, PCV_JOB_RUNNING' "$source_c"
rg -q 'pcv_job_set_result\(d->job_id, PCV_JOB_COMPLETED' "$source_c"
rg -q 'pcv_job_set_result\(d->job_id, PCV_JOB_FAILED' "$source_c"

echo "backup canonical job contract: PASS"
