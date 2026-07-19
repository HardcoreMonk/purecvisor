
"""check_safety_controls.py — 안전통제 효과 테스트 레지스트리 게이트.
설계: docs/superpowers/specs/2026-07-11-safety-control-effect-test-gate-design.md
"""
import json, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REGISTRY = ROOT / "contracts" / "safety_controls.json"
BASELINE = ROOT / "contracts" / "safety_controls_baseline.txt"
MARKER_RE = re.compile(r'PCV_SAFETY_CONTROL:\s*([a-z0-9][a-z0-9-]*)')

def collect_markers(src_texts) -> set:
    ids = set()
    for t in src_texts:
        ids.update(MARKER_RE.findall(t))
    return ids

def effect_test_exists(ref) -> bool:
    if not ref:
        return False
    if "::" in ref:
        path, fn = ref.split("::", 1)
        p = ROOT / path
        return p.exists() and re.search(r'\b' + re.escape(fn) + r'\b',
                                        p.read_text(errors="replace")) is not None
    return (ROOT / ref).exists()

def _load_baseline() -> set:
    if not BASELINE.exists():
        return set()
    return {ln.strip() for ln in BASELINE.read_text().splitlines()
            if ln.strip() and not ln.lstrip().startswith("#")}

def _load_registry():
    """레지스트리 로드 + 중복 키 감지.

    표준 json.loads는 중복 키를 조용히 last-wins 처리한다 — 마커 id가 중복 등록되면
    status가 사람 눈에 안 띄게 뒤집힐 수 있는 사각지대. object_pairs_hook으로 각 JSON
    객체의 (key, value) 쌍을 직접 훑어 중복을 잡는다(값은 last-wins로 그대로 둠 —
    나머지 로직은 이전과 동일하게 동작).
    """
    dups = []

    def _hook(pairs):
        seen = set()
        d = {}
        for k, v in pairs:
            if k in seen:
                dups.append(k)
            seen.add(k)
            d[k] = v
        return d

    reg = json.loads(REGISTRY.read_text(), object_pairs_hook=_hook)
    return reg, dups

def main() -> int:
    reg, dup_keys = _load_registry()
    baseline = _load_baseline()
    src = [p.read_text(errors="replace") for p in (ROOT / "src").rglob("*.c")]
    markers = collect_markers(src)
    fails, untested = [], set()

    for k in sorted(set(dup_keys)):
        fails.append(f"레지스트리 중복 키 '{k}' — 마지막 값만 남아 status 조용히 뒤집힘")

    for m in sorted(markers - set(reg)):
        fails.append(f"미등록 마커 '{m}' — contracts/safety_controls.json 등록 필요")

    for cid, spec in reg.items():
        st = spec.get("status")
        if st == "tested":

            if not effect_test_exists(spec.get("effect_test")):
                fails.append(f"'{cid}' status=tested인데 effect_test 부재: {spec.get('effect_test')}")

            if cid in baseline:
                fails.append(f"'{cid}' status=tested인데 baseline에 잔존 — 승격 시 baseline에서 제거 필요(tested→untested 조용한 강등 마스킹 방지)")
        elif st == "untested-baseline":
            untested.add(cid)

            if cid not in baseline:
                fails.append(f"신규 미검증 통제 '{cid}' — 효과 테스트 동반 필요(baseline 밖)")
        else:
            fails.append(f"'{cid}' 알 수 없는 status: {st}")

    stale = sorted(baseline - untested)
    print(f"[check-safety-controls] 마커 {len(markers)} / 레지스트리 {len(reg)} / "
          f"untested {len(untested)} / baseline {len(baseline)} / FAIL {len(fails)}")
    if untested:
        print(f"[INFO] 효과 미검증 통제(gap): {', '.join(sorted(untested))}")
    if stale:
        print(f"[INFO] baseline에서 제거 가능(tested 승격): {', '.join(stale)}")
    if fails:
        print(f"[FAIL] 안전통제 계약 위반 {len(fails)}건:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("[PASS] 마커 전부 등록 + tested 실재 + 신규 untested 없음")
    return 0

if __name__ == "__main__":
    sys.exit(main())
