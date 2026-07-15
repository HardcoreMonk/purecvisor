import subprocess
import sys
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent.parent


def test_consumers_gate_unchanged():
    """리팩터 후 check_rpc_consumers.py 출력이 기준선(리팩터 전 스냅샷)과 동일."""
    r = subprocess.run([sys.executable, str(SCRIPTS / "check_rpc_consumers.py")],
                        capture_output=True, text=True)
    baseline = Path("/tmp/rpc_consumers_before.txt").read_text()
    assert (r.stdout + r.stderr).strip() == baseline.strip(), "게이트 출력이 리팩터로 변함"
    assert r.returncode == 0


if __name__ == "__main__":
    test_consumers_gate_unchanged()
    print("OK")
