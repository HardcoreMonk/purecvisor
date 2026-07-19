
"""check_rng_safe.py — 보안 RNG/PBKDF2 하드닝 정적 게이트 (Wave B 6 / A02·V11).

[목적]
  ADR-0025 반사실 게이트. 보안 바이트(salt·JWT secret·refresh token·API key·jti)
  생성 경로에 비암호 PRNG(g_random*)가 잔존하면 FAIL, PBKDF2 반복수가 600,000
  미만이면 FAIL 한다. 누군가 실수로 fail-closed 를 g_random 폴백으로 되돌리거나
  반복수를 낮추면(둘 다 인증 우회/약한 해시로 회귀) 조용한 회귀를 막는다.

[검사 대상]
  src/modules/auth/pcv_rbac.c   (salt / refresh token / API key / PBKDF2)
  src/utils/pcv_jwt.c           (JWT secret / jti)

[검사 항목]
  ① 대상 파일에 g_random* (비암호 PRNG) 사용이 없어야 한다.
  ② 대상 파일이 OpenSSL RAND_bytes (CSPRNG 폴백)를 사용해야 한다.
  ③ PBKDF2 목표 반복수(PBKDF2_ITER_TARGET) >= 600000.

[종료 코드]
  0: RNG/PBKDF2 하드닝 계약 통과
  1: 계약 위반 (g_random 잔존 / RAND_bytes 부재 / 반복수 < 600000)
  2: 대상 파일 파싱 실패
"""
from __future__ import annotations
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RNG_FILES = [
    ROOT / "src" / "modules" / "auth" / "pcv_rbac.c",
    ROOT / "src" / "utils" / "pcv_jwt.c",
]

PBKDF2_MIN_ITERATIONS = 600000

GRANDOM_RE = re.compile(r'\bg_random[a-z_]*\s*\(')
RANDBYTES_RE = re.compile(r'\bRAND_bytes\s*\(')

PBKDF2_DEFINE_RE = re.compile(
    r'#\s*define\s+PBKDF2_ITER_TARGET\s+(\d+)')

PBKDF2_LITERAL_RE = re.compile(
    r'pcv_config_get_int\s*\(\s*"auth"\s*,\s*"pbkdf2_iterations"\s*,\s*(\d+)\s*\)')

def scan_rng_text(text: str) -> dict:
    """단일 파일 텍스트에서 RNG 안전 신호를 추출한다."""
    return {
        "uses_grandom": bool(GRANDOM_RE.search(text)),
        "uses_randbytes": bool(RANDBYTES_RE.search(text)),
    }

def extract_pbkdf2_target(text: str) -> int | None:
    """PBKDF2 목표 반복수를 추출한다 (매크로 우선, 없으면 config 기본값 리터럴)."""
    m = PBKDF2_DEFINE_RE.search(text)
    if m:
        return int(m.group(1))
    m = PBKDF2_LITERAL_RE.search(text)
    if m:
        return int(m.group(1))
    return None

def main(argv: list[str]) -> int:
    files = [Path(a) for a in argv[1:]] if len(argv) > 1 else RNG_FILES

    fails: list[str] = []
    pbkdf2_target: int | None = None

    for f in files:
        if not f.exists():
            print(f"ERROR: {f} 미존재", file=sys.stderr)
            return 2
        text = f.read_text(errors="replace")
        sig = scan_rng_text(text)
        if sig["uses_grandom"]:
            fails.append(f"{f.name}: 비암호 PRNG g_random* 사용 잔존 — "
                         f"보안 바이트는 /dev/urandom→RAND_bytes fail-closed 만 허용")
        if not sig["uses_randbytes"]:
            fails.append(f"{f.name}: OpenSSL RAND_bytes (CSPRNG 폴백) 미사용 — "
                         f"/dev/urandom 실패 시 fail-closed 경로 부재")
        t = extract_pbkdf2_target(text)
        if t is not None:
            pbkdf2_target = t if pbkdf2_target is None else min(pbkdf2_target, t)

    if pbkdf2_target is None:
        fails.append("PBKDF2 목표 반복수(PBKDF2_ITER_TARGET)를 찾지 못함 — "
                     "pcv_rbac.c PBKDF2 하드닝 제거?")
    elif pbkdf2_target < PBKDF2_MIN_ITERATIONS:
        fails.append(f"PBKDF2 반복수 {pbkdf2_target} < {PBKDF2_MIN_ITERATIONS} — "
                     f"downgrade (약한 해시)")

    print(f"[check-rng-safe] 대상 {len(files)}파일, PBKDF2 target={pbkdf2_target}")

    if fails:
        print("\033[31m[FAIL]\033[0m RNG/PBKDF2 하드닝 계약 위반:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        print("       근거: docs/operations 2026-07-16 보안 시정 로드맵 Item 6 (A02/V11)",
              file=sys.stderr)
        return 1

    print(f"\033[32m[PASS]\033[0m 보안 RNG fail-closed (g_random 무·RAND_bytes 유) + "
          f"PBKDF2 {pbkdf2_target} 회 (>= {PBKDF2_MIN_ITERATIONS})")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
