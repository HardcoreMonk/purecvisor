#!/usr/bin/env python3
                          
                                                                               
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                                             
                                                                    

from __future__ import annotations

import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / "scripts/check_network_mode_contract.py"


def run(root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["python3", str(GATE), "--root", str(root)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def main() -> int:
                                                    
    production = run(ROOT)
    assert production.returncode == 0, production.stdout

    with tempfile.TemporaryDirectory(prefix="pcv-network-mode-contract-") as temp:
        fixture = Path(temp)
        ui_target = fixture / "ui/modules/network.js"
        be_target = fixture / "src/modules/network/network_manager.c"
        bootstrap_target = fixture / "src/bootstrap/pcv_bootstrap_single.c"
        ui_target.parent.mkdir(parents=True)
        be_target.parent.mkdir(parents=True)
        bootstrap_target.parent.mkdir(parents=True)
        shutil.copy2(ROOT / "ui/modules/network.js", ui_target)
        shutil.copy2(ROOT / "src/modules/network/network_manager.c", be_target)
        shutil.copy2(ROOT / "src/bootstrap/pcv_bootstrap_single.c", bootstrap_target)

        original = ui_target.read_text(encoding="utf-8")
        mutated = original.replace(
            "Object.freeze(['nat', 'isolated', 'routed'])",
            "Object.freeze(['nat', 'isolated', 'routed', 'bridge'])",
            1,
        )
        assert mutated != original, "frontend mutation anchor missing"
        ui_target.write_text(mutated, encoding="utf-8")
        rejected = run(fixture)
        assert rejected.returncode == 1, rejected.stdout
        assert "frontend-only=['bridge']" in rejected.stdout, rejected.stdout

        ui_target.write_text(original, encoding="utf-8")
        backend = be_target.read_text(encoding="utf-8")
        mutated_backend = backend.replace(
            "network_dhcp_start(br, cidr, &dhcp_err)",
            "legacy_dhcp_start(br, cidr, &dhcp_err)",
            1,
        )
        assert mutated_backend != backend, "DHCP convergence mutation anchor missing"
        be_target.write_text(mutated_backend, encoding="utf-8")
        rejected = run(fixture)
        assert rejected.returncode == 1, rejected.stdout
        assert "DHCP 재수렴 계약 누락" in rejected.stdout, rejected.stdout

        be_target.write_text(backend, encoding="utf-8")
        bootstrap = bootstrap_target.read_text(encoding="utf-8")
        mutated_bootstrap = bootstrap.replace(
            "if (network_dhcp_start_ex(def_br, def_cidr, TRUE, NULL, &net_err)) {",
            "if (!dhcp_up && network_dhcp_start_ex(def_br, def_cidr, TRUE, NULL, &net_err)) {",
            1,
        )
        assert mutated_bootstrap != bootstrap, "bootstrap DHCP gate mutation anchor missing"
        bootstrap_target.write_text(mutated_bootstrap, encoding="utf-8")
        rejected = run(fixture)
        assert rejected.returncode == 1, rejected.stdout
        assert "TOCTOU 게이트 잔존" in rejected.stdout, rejected.stdout

    print("PASS: network mode contract production scan + FE enum/DHCP/bootstrap mutations RED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
