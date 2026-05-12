#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import re
import shlex
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / "ui"
BUNDLER = ROOT / "scripts" / "bundle-ui.sh"
BUNDLE = UI / "app.bundle.js"
SW = UI / "sw.js"


def fail(message: str) -> int:
    print(f"FAIL: {message}", file=sys.stderr)
    print("hint: run PCV_NO_DEPLOY=1 scripts/bundle-ui.sh", file=sys.stderr)
    return 1


def load_bundle_order() -> list[str]:
    text = BUNDLER.read_text(encoding="utf-8")
    match = re.search(r"^ORDER=\(\s*(?P<body>.*?)\s*\)", text, re.M | re.S)
    if not match:
        raise RuntimeError("scripts/bundle-ui.sh ORDER array not found")
    body = "\n".join(
        line.split("#", 1)[0]
        for line in match.group("body").splitlines()
    )
    return shlex.split(body)


def expected_bundle_bytes(order: list[str]) -> bytes:
    chunks: list[bytes] = []
    for module in order:
        path = UI / "modules" / f"{module}.js"
        if not path.exists():
            raise FileNotFoundError(path)
        chunks.append(b"\n")
        chunks.append(path.read_bytes())
    return b"".join(chunks)


def main() -> int:
    try:
        order = load_bundle_order()
        expected = expected_bundle_bytes(order)
    except Exception as exc:
        return fail(str(exc))

    if not BUNDLE.exists():
        return fail("ui/app.bundle.js is missing")

    actual = BUNDLE.read_bytes()
    if actual != expected:
        expected_hash = hashlib.sha1(expected).hexdigest()[:8]
        actual_hash = hashlib.sha1(actual).hexdigest()[:8]
        return fail(
            "ui/app.bundle.js is stale "
            f"(expected pcv-ui-v{expected_hash}, found pcv-ui-v{actual_hash})"
        )

    expected_cache = hashlib.sha1(expected).hexdigest()[:8]
    if not SW.exists():
        return fail("ui/sw.js is missing")
    sw_text = SW.read_text(encoding="utf-8")
    cache_match = re.search(r"const CACHE_NAME = 'pcv-ui-v([^']+)';", sw_text)
    if not cache_match:
        return fail("ui/sw.js CACHE_NAME not found")
    if cache_match.group(1) != expected_cache:
        return fail(
            "ui/sw.js CACHE_NAME is stale "
            f"(expected pcv-ui-v{expected_cache}, found pcv-ui-v{cache_match.group(1)})"
        )

    print(f"OK: UI bundle is fresh (pcv-ui-v{expected_cache})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
