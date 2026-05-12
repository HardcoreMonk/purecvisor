#!/usr/bin/env bash
set -euo pipefail

FILE="src/cli/purecvisorctl.c"

if ! grep -q 'if (argc < 4).*pcvctl container exec <name> <cmd>' "$FILE"; then
  echo "container exec still requires the wrong argc threshold"
  exit 1
fi

if ! grep -Eq 'json_object_set_string_member\(p,"cmd",[[:space:]]*argv\[3\]\);' "$FILE"; then
  echo "container exec no longer maps argv[3] to cmd"
  exit 1
fi

if ! grep -q 'json_object_get_object_member(root, "result")' "$FILE"; then
  echo "container env-list is not parsing result as an object map"
  exit 1
fi

if ! grep -q 'json_object_get_members(envs)' "$FILE"; then
  echo "container env-list is not iterating object members"
  exit 1
fi

echo "PASS: container exec/env-list CLI surface aligned"
