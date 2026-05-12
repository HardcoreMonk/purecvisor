#!/usr/bin/env bash
set -euo pipefail

FILE="src/modules/lxc/lxc_driver.c"

if ! grep -q 'config is already gone; continuing with ZFS cleanup' "$FILE"; then
  echo "destroy fallback for config-gone partial cleanup is missing"
  exit 1
fi

if ! grep -q 'g_file_test(config_path, G_FILE_TEST_EXISTS)' "$FILE"; then
  echo "destroy path is not checking whether config already disappeared"
  exit 1
fi

if ! grep -q 'nftw(container_dir' "$FILE"; then
  echo "destroy path is not attempting recursive final container directory cleanup"
  exit 1
fi

echo "PASS: container destroy cleanup fallback layout present"
