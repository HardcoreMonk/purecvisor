#!/usr/bin/env python3







                          
                                                               
                                                                                      
                                                                                                                            
 
                      
                                                                                                                        
                                                                           

    
                                                                    
                                                  
                                       

                                                               
                                                                                        
                                                           
                                                        
                                                                  
                                                    
                                
                                                                    
                                                      

                                                                
                                                               
                                           

              
                                                               
                                                     
                                                  
                                                                  
                                                                       

     
                                                                         
                                                                           
                                  
                                                               
                                                                     
                                                                             
                               
                                                                     
                                                            
                                                  
                                                                   
                                                                  





     
                                                                        
                                                                        
                        
                                                                         

       
                                                                   
                                                        
                      

       
                  
          
                        
   

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_deb_apparmor import strip_comments              

ROOT = Path(__file__).resolve().parent.parent
BUILD_REL = "packaging/deb/build-deb.sh"

                                                      
                                                      
REQUIRED_FLOOR_COMPONENTS = {
    "openssl": ("libssl3", "libssl3t64"),
    "libsoup": ("libsoup-3.0-0", "libsoup-3.0-0t64"),
    "sqlite": ("libsqlite3-0", "libsqlite3-0t64"),
    "glib": ("libglib2.0-0", "libglib2.0-0t64"),
}
REQUIRED_UI_ASSETS = (
    "index.html",
    "app.bundle.js",
    "sw.js",
    "offline.html",
    "maintenance.html",
    "maintenance-status.json",
)
REQUIRED_RUNTIME_RECOMMENDS = {"openvswitch-switch"}
LXC_UNIT_NAME = "purecvisor-lxc@.service"
LXC_UNIT_REL = "packaging/systemd/" + LXC_UNIT_NAME
LXC_UNIT_ETC = "/etc/systemd/system/" + LXC_UNIT_NAME
DEPLOY_REL = "scripts/deploy.sh"
LXC_UNIT_HEADER_REL = "src/modules/lxc/lxc_unit.h"

LXC_UNIT_REQUIRED = {
    ("Unit", "StartLimitIntervalSec"): "0",
    ("Service", "Type"): "forking",
    ("Service", "EnvironmentFile"): "/run/purecvisor/lxc-unit.env",
    ("Service", "RuntimeDirectory"): "purecvisor-lxc",
    ("Service", "RuntimeDirectoryPreserve"): "yes",
    ("Service", "PIDFile"): "/run/purecvisor-lxc/%i.pid",
    ("Service", "ExecStart"): "/usr/bin/lxc-start -d -P ${PCV_LXC_PATH} -n %i -f /run/purecvisor/lxc-runtime/%i/config "
                              "-p /run/purecvisor-lxc/%i.pid -o ${PCV_LXC_PATH}/%i/%i.log -l INFO",
    ("Service", "ExecStop"): "-/usr/bin/lxc-stop -P ${PCV_LXC_PATH} -n %i",
    ("Service", "KillMode"): "mixed",
    ("Service", "Delegate"): "yes",
    ("Service", "Restart"): "no",
    ("Service", "TimeoutStartSec"): "25",
    ("Service", "TimeoutStopSec"): "90",
    ("Service", "LimitNOFILE"): "1048576",
    ("Service", "LimitCORE"): "0",
    ("Service", "UMask"): "0077",
    ("Service", "CapabilityBoundingSet"): "~CAP_SYS_MODULE CAP_SYS_RAWIO CAP_SYS_TIME CAP_MAC_OVERRIDE",
}

LXC_UNIT_FORBIDDEN_KEYS = (
    "NoNewPrivileges", "SystemCallFilter", "SystemCallArchitectures", "LockPersonality",
    "RestrictRealtime", "Nice", "IOSchedulingClass", "IOSchedulingPriority",
)
NOTICES_REL = "THIRD_PARTY_NOTICES.md"
LOGROTATE_REL = "systemd/purecvisor.logrotate"

REQUIRED_NOTICE_PATHS = (
    "ui/vendor/chart.umd.min.js", "ui/vendor/novnc/", "ui/vendor/coolicons/",
    "ui/vendor/pretendard/", "ui/vendor/qrcode/",
)


def func_body(code: str, name: str) -> str:
                                                      
    m = re.search(r"^\s*" + re.escape(name) + r"\s*\(\)\s*\{", code, re.M)
    if not m:
        return ""
    depth, i = 0, m.end() - 1
    while i < len(code):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return code[m.end():i]
        i += 1
    return ""


def control_body(raw: str) -> str:
                                                                           
    m = re.search(
        r'^cat\s*>\s*"\$STAGE/DEBIAN/control"\s*<<\s*([A-Za-z0-9_]+)\s*$'
        r"(?P<body>.*?)"
        r"^\1\s*$",
        raw, re.M | re.S)
    return m.group("body") if m else ""


def check(raw: str, root: Path = ROOT, deploy_raw: str | None = None) -> tuple[list[str], dict]:
    code = strip_comments(raw)
    bad: list[str] = []
    info: dict = {}

                                                         
    floor_body = func_body(code, "apply_security_floors")
    if not floor_body:
        bad.append("[불변식1] apply_security_floors() 정의가 없다 — "
                   "의존성 버전 하한 로직이 통째로 제거됐다")
    m_libs = re.search(r'^\s*SECURITY_FLOOR_LIBS\s*=\s*"([^"]*)"', code, re.M)
    if not m_libs:
        bad.append("[불변식1] SECURITY_FLOOR_LIBS 정의가 없다 — 하한 대상 목록 소실")
        floor_libs: set[str] = set()
    else:
        floor_libs = set(m_libs.group(1).split())
    info["floor_libs"] = sorted(floor_libs)

                                                     
    for component, spellings in sorted(REQUIRED_FLOOR_COMPONENTS.items()):
        if not floor_libs & set(spellings):
            bad.append(f"[불변식2] SECURITY_FLOOR_LIBS 가 {component} 를 커버하지 않는다 "
                       f"(허용 철자 중 하나 필요: {', '.join(spellings)}) — "
                       "취약 구버전이 버전 제약 없이 의존성으로 허용된다")

                                                         
                                                                
                                                             
    if floor_body and "(>=" not in floor_body:
        bad.append("[불변식3] apply_security_floors 본문이 '(>= …)' 버전 제약을 만들지 "
                   "않는다 — 함수는 남았으나 하한이 무력화됐다")
    if floor_body and "dpkg-query" not in floor_body:
        bad.append("[불변식3] apply_security_floors 가 dpkg-query 로 설치 버전을 읽지 않는다 "
                   "— 하한 값의 근거가 사라졌다")

                                                           
    m_apply = re.search(r'^\s*LIBDEPS\s*=\s*"\$\(\s*apply_security_floors\s+"?\$LIBDEPS"?\s*\)"',
                        code, re.M)
    if not m_apply:
        bad.append("[불변식4] LIBDEPS 가 apply_security_floors 를 통과하지 않는다 — "
                   'LIBDEPS="$(apply_security_floors "$LIBDEPS")" 배선이 끊겼다')
    m_all = re.search(r'^\s*ALLDEPS\s*=\s*"(?P<rhs>[^"]*)"', code, re.M)
    if not m_all:
        bad.append("[불변식4] ALLDEPS 대입을 찾지 못했다")
    else:
        if "LIBDEPS" not in m_all.group("rhs"):
            bad.append("[불변식4] ALLDEPS 가 LIBDEPS 를 담지 않는다 — "
                       "floor 가 적용된 라이브러리 의존이 control 에 도달하지 않는다")
        if m_apply and m_apply.start() > m_all.start():
            bad.append("[불변식4] apply_security_floors 적용이 ALLDEPS 조립보다 뒤에 있다 — "
                       "control 에는 floor 없는 값이 들어간다")

    ctrl = control_body(raw)
    if not ctrl:
        bad.append("[구조] DEBIAN/control heredoc 을 찾지 못했다 — 빌드 스크립트 구조 변경?")
    else:
        m_dep = re.search(r"^Depends:\s*(?P<val>.*)$", ctrl, re.M)
        if not m_dep:
            bad.append("[불변식4] DEBIAN/control 에 Depends 행이 없다")
        elif "$ALLDEPS" not in m_dep.group("val"):
            bad.append(f"[불변식4] control 의 Depends 가 $ALLDEPS 를 쓰지 않는다 "
                       f"(현재: '{m_dep.group('val').strip()[:60]}') — "
                       "floor 적용 결과가 패키지 메타에 반영되지 않는다")

                                                             
    m_md5 = re.search(r"^[^\n]*DEBIAN/md5sums[^\n]*$", code, re.M)
    if not m_md5:
        bad.append("[불변식5] DEBIAN/md5sums 생성이 없다 — "
                   "설치본 변조를 dpkg -V 로 탐지할 수 없다")
    else:
        line = m_md5.group(0)
        if "md5sum" not in line:
            bad.append("[불변식5] md5sums 를 md5sum 으로 만들지 않는다")
        if not re.search(r"\bfind\b[^\n]*\busr\b", line) or " etc" not in line:
            bad.append("[불변식5] md5sums 대상이 usr·etc 전수가 아니다 — "
                       "일부 설치 파일이 무결성 목록에서 빠진다")

                                                       
    m_build = re.search(r"^[^\n]*dpkg-deb\s+--build[^\n]*$", code, re.M)
    if not m_build:
        bad.append("[구조] dpkg-deb --build 호출을 찾지 못했다")
    elif "--root-owner-group" not in m_build.group(0):
        bad.append("[불변식6] dpkg-deb --build 에 --root-owner-group 이 없다 — "
                   "빌드 호스트의 uid/gid 가 아카이브에 박혀 재현성이 깨진다")

                                                       
    if not re.search(r"\bcp\s+-a\s+ui/vendor\b", code):
        bad.append("[불변식7] ui/vendor 를 패키지로 복사하지 않는다 — "
                   "check-vendor-integrity 의 SHA-256 핀이 산출물로 전이되지 않는다")

                                                     
    m_assets = re.search(r"^\s*for\s+f\s+in\s+(?P<list>[^\n;]*index\.html[^\n;]*);", code, re.M)
    if not m_assets:
        bad.append("[불변식8] 스테이징 UI 필수 자산 검증 루프가 없다 — "
                   "cp 실패를 삼키는 경로(2>/dev/null || true)가 무방비가 된다")
    else:
        listed = set(m_assets.group("list").split())
        missing = [a for a in REQUIRED_UI_ASSETS if a not in listed]
        if missing:
            bad.append(f"[불변식8] 필수 자산 검증 목록에서 빠짐: {', '.join(missing)}")
        info["verified_assets"] = sorted(listed)






    if ctrl:
        m_recommends = re.search(r"^Recommends:\s*(?P<val>.*)$", ctrl, re.M)
        if not m_recommends:
            bad.append("[불변식9] DEBIAN/control 에 Recommends 행이 없다 — "
                       "선택형 OVS overlay 런타임이 설치되지 않는다")
        else:
            recommended = {
                item.strip().split()[0]
                for item in m_recommends.group("val").split(",")
                if item.strip()
            }
            missing_runtime = sorted(REQUIRED_RUNTIME_RECOMMENDS - recommended)
            if missing_runtime:
                bad.append("[불변식9] Recommends 에 선택형 overlay 런타임이 빠졌다: "
                           + ", ".join(missing_runtime))
            info["runtime_recommends"] = sorted(recommended)


    if not re.search(r'install\s+-m\s*0?644\s+LICENSE\s+"\$STAGE/usr/share/doc/\$PKG_NAME/copyright"',
                     code):
        bad.append("[불변식10] LICENSE 를 /usr/share/doc/$PKG_NAME/copyright 로 싣지 않는다 — "
                   "LICENSE §2 재배포 조건 위반")
    if not re.search(r'install\s+-m\s*0?644\s+THIRD_PARTY_NOTICES\.md\s+'
                     r'"\$STAGE/usr/share/doc/\$PKG_NAME/THIRD_PARTY_NOTICES\.md"', code):
        bad.append("[불변식10] THIRD_PARTY_NOTICES.md 를 /usr/share/doc/$PKG_NAME/ 로 싣지 않는다 — "
                   "ui/vendor 제3자 라이선스 고지가 산출물에서 사라진다")
    notices = ROOT / NOTICES_REL
    if notices.is_file():
        ntext = notices.read_text(encoding="utf-8", errors="replace")
        for needle in REQUIRED_NOTICE_PATHS:
            if needle not in ntext:
                bad.append(f"[불변식10] {NOTICES_REL} 에 {needle} 항목이 없다")
    else:
        bad.append(f"[불변식10] {NOTICES_REL} 가 저장소에 없다")


    m_conf = re.search(r"<<'CF'\n(?P<body>.*?)\nCF\n", raw, re.S)
    conffiles = set(m_conf.group("body").split()) if m_conf else set()
    for target, install_re in (
        ("/etc/logrotate.d/purecvisor",
         r'install\s+-m\s*0?644\s+systemd/purecvisor\.logrotate\s+"\$STAGE/etc/logrotate\.d/purecvisor"'),
        ("/etc/systemd/system/purecvisorsd.service.d/95-coredump-hardening.conf",
         r'install\s+-m\s*0?644\s+packaging/systemd/95-coredump-hardening\.conf\s+'
         r'"\$STAGE/etc/systemd/system/purecvisorsd\.service\.d/95-coredump-hardening\.conf"'),
        ("/etc/systemd/system/purecvisorsd.service.d/97-sandbox.conf",
         r'install\s+-m\s*0?644\s+packaging/systemd/97-sandbox\.conf\s+'
         r'"\$STAGE/etc/systemd/system/purecvisorsd\.service\.d/97-sandbox\.conf"'),
    ):
        if not re.search(install_re, code):
            bad.append(f"[불변식11] {target} 를 deb 에 싣지 않는다")
        if target not in conffiles:
            bad.append(f"[불변식11] {target} 가 DEBIAN/conffiles 에 없다 — 운영자 편집이 업그레이드에 덮인다")
    logrotate = ROOT / LOGROTATE_REL
    if not logrotate.is_file():
        bad.append(f"[불변식11] {LOGROTATE_REL} 정본이 없다")
    else:
        ltext = logrotate.read_text(encoding="utf-8", errors="replace")
        if re.search(r"^[^#\n]*purecvisormd", ltext, re.M):
            bad.append("[불변식11] logrotate 가 존재하지 않는 유닛 purecvisormd 를 참조한다")
        if not re.search(r"^[^#\n]*reload\s+purecvisorsd\b", ltext, re.M):
            bad.append("[불변식11] logrotate 가 purecvisorsd 를 reload 하지 않는다")


    sandbox_path = root / "packaging/systemd/97-sandbox.conf"
    if not sandbox_path.exists():
        bad.append("[불변식12] 97-sandbox.conf 없음")
    else:
        sandbox = sandbox_path.read_text(encoding="utf-8")
        mount_ns = re.compile(r"^(ProtectSystem|ProtectHome|PrivateTmp|PrivateDevices|ProtectKernelTunables|"
                              r"ProtectControlGroups|ProtectProc|ProcSubset|PrivateMounts|ProtectKernelModules|"
                              r"ProtectKernelLogs|ReadOnlyPaths|ReadWritePaths|InaccessiblePaths|TemporaryFileSystem)=",
                              re.M)
        if mount_ns.search(sandbox):
            bad.append("[불변식12] 97-sandbox.conf에 mount namespace 옵션 — 하드 요구사항 1 위반")
        analyze = shutil.which("systemd-analyze")
        if analyze:
            with tempfile.TemporaryDirectory(prefix="pcv-sandbox-verify.") as d:
                unit = Path(d) / "pcv-sandbox-verify.service"
                unit.write_text("[Unit]\nDescription=verify\n[Service]\nType=notify\nExecStart=/bin/true\n",
                                encoding="utf-8")
                (Path(d) / "pcv-sandbox-verify.service.d").mkdir()
                shutil.copy(sandbox_path, Path(d) / "pcv-sandbox-verify.service.d/97-sandbox.conf")
                r = subprocess.run([analyze, "verify", str(unit)], capture_output=True, text=True)
                out = (r.stdout + r.stderr).strip()

                own = [ln for ln in out.splitlines() if "97-sandbox.conf" in ln]
                if r.returncode != 0 or own:
                    bad.append(f"[불변식12] 97-sandbox.conf systemd-analyze verify 실패: "
                               f"{(chr(10).join(own) if own else out)[:300]}")
        else:
            print("[NOTE] systemd-analyze 없음 — 97-sandbox 병합 검증 생략")

    _check_lxc_unit(code, root, bad, deploy_raw)

    return bad, info

def parse_unit(text: str) -> tuple[dict[tuple[str, str], list[str]], list[str]]:

    values: dict[tuple[str, str], list[str]] = {}
    sections: list[str] = []
    section = ""
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith(("#", ";")):
            continue
        m = re.fullmatch(r"\[([^\]]+)\]", line)
        if m:
            section = m.group(1)
            sections.append(section)
            continue
        if "=" in line:
            key, _, val = line.partition("=")
            values.setdefault((section, key.strip()), []).append(val.strip())
    return values, sections

def c_define(text: str, name: str) -> str | None:

    m = re.search(r'^\s*#\s*define\s+' + re.escape(name) + r'\s+(?:"((?:[^"\\\n]|\\.)*)"|(\d+)\b)', text, re.M)
    if not m:
        return None
    return m.group(1) if m.group(1) is not None else m.group(2)

def _check_lxc_unit_constants(root: Path, values: dict, bad: list[str]) -> None:

    hp = root / LXC_UNIT_HEADER_REL
    if not hp.exists():
        bad.append(f"[불변식13] {LXC_UNIT_HEADER_REL} 없음 — 데몬 쪽 유닛 상수를 대조할 수 없다")
        return
    h = hp.read_text(encoding="utf-8")
    env = c_define(h, "PCV_LXC_UNIT_ENV_PATH")
    unit_env = (values.get(("Service", "EnvironmentFile")) or [None])[0]
    if env is None or env != unit_env:
        bad.append(f"[불변식13] PCV_LXC_UNIT_ENV_PATH={env!r} 가 유닛 EnvironmentFile={unit_env!r} 와 다르다 "
                   "— 데몬이 쓴 경로를 유닛이 읽지 못한다")

    rt_dir = c_define(h, "PCV_LXC_RUNTIME_CONFIG_DIR")
    exec_start = (values.get(("Service", "ExecStart")) or [""])[0]
    m_f = re.search(r"(?:^|\s)-f\s+(\S+)/%i/config(?:\s|$)", exec_start)
    unit_rt = m_f.group(1) if m_f else None
    if rt_dir is None or rt_dir != unit_rt:
        bad.append(f"[불변식13] PCV_LXC_RUNTIME_CONFIG_DIR={rt_dir!r} 가 유닛 ExecStart -f 디렉터리={unit_rt!r} 와 다르다 "
                   "— 데몬이 쓴 런타임 config로 유닛이 시작하지 않는다")
    tmpl = c_define(h, "PCV_LXC_UNIT_TEMPLATE")
    prefix = LXC_UNIT_NAME[:-len(".service")]
    if tmpl != prefix:
        bad.append(f"[불변식13] PCV_LXC_UNIT_TEMPLATE={tmpl!r} 가 유닛 파일 이름 접두 {prefix!r} 와 다르다")
    nums = {k: c_define(h, k) for k in ("PCV_LXC_UNIT_START_TIMEOUT_SEC", "PCV_LXC_UNIT_START_BUDGET_SEC",
                                        "PCV_LXC_UNIT_PREP_BUDGET_SEC")}
    tss = (values.get(("Service", "TimeoutStartSec")) or [""])[0]
    if not tss.isdigit() or any(v is None for v in nums.values()):
        bad.append(f"[불변식13] 시작 상한을 읽지 못함 — 유닛 TimeoutStartSec={tss!r}, lxc_unit.h {nums}")
        return
    start = int(nums["PCV_LXC_UNIT_START_TIMEOUT_SEC"])
    budget = int(nums["PCV_LXC_UNIT_START_BUDGET_SEC"])
    prep = int(nums["PCV_LXC_UNIT_PREP_BUDGET_SEC"])
    if not int(tss) < start <= budget - prep:
        bad.append(f"[불변식13] 시작 상한 순서 위반: 유닛 TimeoutStartSec {tss} < PCV_LXC_UNIT_START_TIMEOUT_SEC {start} "
                   f"≤ BUDGET {budget} − PREP {prep} 이어야 한다 — PID 1 이 먼저 포기하고 데몬 전체 상한을 넘지 않게")

def _check_lxc_unit(code: str, root: Path, bad: list[str], deploy_raw: str | None) -> None:

    if not re.search(r'install\s+-m\s*0?644\s+packaging/systemd/purecvisor-lxc@\.service\s+'
                     r'"\$STAGE/etc/systemd/system/purecvisor-lxc@\.service"', code):
        bad.append(f"[불변식13] {LXC_UNIT_ETC} 를 deb 에 싣지 않는다")
    m_conf = re.search(r"<<'CF'\n(?P<body>.*?)\nCF\n", code + "\n", re.S)
    if not m_conf or LXC_UNIT_ETC not in m_conf.group("body").split():
        bad.append(f"[불변식13] {LXC_UNIT_ETC} 가 DEBIAN/conffiles 에 없다 — 운영자 편집이 업그레이드에 덮인다")

    unit_path = root / LXC_UNIT_REL
    if not unit_path.exists():
        bad.append(f"[불변식13] {LXC_UNIT_NAME} 없음")
    else:
        text = unit_path.read_text(encoding="utf-8")
        values, sections = parse_unit(text)
        _check_lxc_unit_constants(root, values, bad)
        if "Install" in sections:
            bad.append("[불변식13] 유닛에 [Install] 이 있다 — 데몬의 start 로만 떠야 한다(부팅 자동 시작 금지)")
        for (section, key), want in LXC_UNIT_REQUIRED.items():
            got = values.get((section, key))
            if got != [want]:
                bad.append(f"[불변식13] 유닛 [{section}] {key}={got if got is None else got[0] if len(got) == 1 else got}"
                           f" — 기대 '{want}'")
        for (section, key) in values:
            if key in LXC_UNIT_FORBIDDEN_KEYS:
                bad.append(f"[불변식13] 유닛에 금지 키 {key} — 데몬 샌드박스 계열은 컨테이너 init 에 걸면 안 된다")
        if re.search(r"^(ProtectSystem|ProtectHome|PrivateTmp|PrivateDevices|ProtectKernelTunables|"
                     r"ProtectControlGroups|ProtectProc|ProcSubset|PrivateMounts|ProtectKernelModules|"
                     r"ProtectKernelLogs|ReadOnlyPaths|ReadWritePaths|InaccessiblePaths|TemporaryFileSystem)=",
                     text, re.M):
            bad.append("[불변식13] 유닛에 mount namespace 옵션 — 컨테이너 마운트가 깨진다")
        if "%I" in re.sub(r"(?m)^\s*#.*$", "", text):
            bad.append("[불변식13] 유닛에 %I(이스케이프 해제 인스턴스) — %i 만 쓴다")
        stop = values.get(("Service", "TimeoutStopSec"), [""])[0]
        if not (stop.isdigit() and int(stop) > 30):
            bad.append(f"[불변식13] TimeoutStopSec={stop!r} 는 데몬 정상 정지 30초보다 길어야 한다")
        start = values.get(("Service", "ExecStart"), [""])[0]
        for token in ("-d", "-P ${PCV_LXC_PATH}", "-n %i", "-f /run/purecvisor/lxc-runtime/%i/config",
                      "-p /run/purecvisor-lxc/%i.pid", "-o ${PCV_LXC_PATH}/%i/%i.log", "-l INFO"):
            if token not in start:
                bad.append(f"[불변식13] ExecStart 에 '{token}' 토큰이 없다")
        analyze = shutil.which("systemd-analyze")
        if analyze:
            with tempfile.TemporaryDirectory(prefix="pcv-lxc-unit-verify.") as d:
                shutil.copy(unit_path, Path(d) / LXC_UNIT_NAME)
                r = subprocess.run([analyze, "verify", str(Path(d) / "purecvisor-lxc@probe.service")],
                                   capture_output=True, text=True)
                out = (r.stdout + r.stderr).strip()

                own = [ln for ln in out.splitlines()
                       if "purecvisor-lxc@" in ln and "not executable" not in ln]
                if r.returncode != 0 or own:
                    bad.append(f"[불변식13] {LXC_UNIT_NAME} systemd-analyze verify 실패: "
                               f"{(chr(10).join(own) if own else out)[:300]}")
        else:
            print("[NOTE] systemd-analyze 없음 — 컨테이너 유닛 검증 생략")

    if deploy_raw is None:
        dp = root / DEPLOY_REL
        deploy_raw = dp.read_text(encoding="utf-8", errors="replace") if dp.exists() else ""
    dcode = re.sub(r"(?m)^\s*#.*$", "", deploy_raw)
    dcode = re.sub(r"\\\n\s*", " ", dcode)
    installs = list(re.finditer(r"^[^\n]*\binstall\s+-m\s*0?644\b[^\n]*"
                                r"/etc/systemd/system/purecvisor-lxc@\.service[^\n]*$", dcode, re.M))
    if len(installs) < 2:
        bad.append(f"[불변식13] scripts/deploy.sh 가 {LXC_UNIT_ETC} 를 원격·로컬 두 경로 모두 설치하지 않는다 "
                   f"(설치 줄 {len(installs)}개)")
    for m in installs:
        if "||" in m.group(0):
            bad.append("[불변식13] deploy.sh 의 컨테이너 유닛 설치가 비차단(`||`)이다 — 실패하면 배포가 실패해야 한다")
        if not re.match(r"\s*sudo\s+systemctl\s+daemon-reload\b", dcode[m.end():]):
            bad.append("[불변식13] deploy.sh 의 컨테이너 유닛 설치 바로 뒤에 `systemctl daemon-reload` 가 없다")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=".deb 산출물 공급망 계약 게이트 (A03)")
    ap.add_argument("--build-script", default=str(ROOT / BUILD_REL))
    args = ap.parse_args(argv)

    path = Path(args.build_script)
    if not path.is_file():
        print(f"\033[31m[FAIL]\033[0m 빌드 스크립트가 없다: {path}", file=sys.stderr)
        return 2
    raw = path.read_text(encoding="utf-8", errors="replace")

    bad, info = check(raw)

    print(f"[check-deb-supply-chain] {path.name} {len(raw.splitlines())}줄 검사 — "
          f"floor 대상 {len(info.get('floor_libs', []))}개 / "
          f"필수 자산 {len(info.get('verified_assets', []))}개")

    if bad:
        print("\033[31m[FAIL]\033[0m deb 공급망 계약 위반:", file=sys.stderr)
        for b in bad:
            print(f"  - {b}", file=sys.stderr)
        print("       근거: OWASP Top 10:2025 A03 — "
              "docs/PUBLIC_SOURCE_POLICY.md", file=sys.stderr)
        return 1

    print("\033[32m[PASS]\033[0m deb 공급망: 의존 버전 하한 + control 배선 + md5sums 전수 + "
          "--root-owner-group + vendor 핀 전이 + 필수 자산 검증 + OVS runtime 권장 + "
          "재배포 고지 + logrotate/coredump 자산 + 97-sandbox 드롭인 + 컨테이너 전용 유닛")
    return 0


if __name__ == "__main__":
    sys.exit(main())
