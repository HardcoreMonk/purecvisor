#!/usr/bin/env bash



set -euo pipefail
cd "$(dirname "$0")/../.."
exec python3 tests/test_async_shutdown_lifetime.py
