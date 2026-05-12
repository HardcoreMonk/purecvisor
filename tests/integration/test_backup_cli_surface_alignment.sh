#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo_root"

cli=src/cli/purecvisorctl.c

set_block="$(sed -n '2812,2845p' "$cli")"
history_block="$(sed -n '2870,2923p' "$cli")"
restore_block="$(sed -n '6043,6052p' "$cli")"

printf 'checking backup set param names...\n'
grep -q 'json_object_set_int_member(params, "interval_hours"' <<<"$set_block"
grep -q 'json_object_set_int_member(params, "retention_count"' <<<"$set_block"

printf 'checking backup history response shape...\n'
grep -q 'json_array_get_string_element' <<<"$history_block"
! grep -q 'json_array_get_object_element' <<<"$history_block"

printf 'checking backup restore param names...\n'
grep -q 'json_object_set_string_member(p, "snapshot_name"' <<<"$restore_block"

printf 'backup CLI surface alignment: OK\n'
