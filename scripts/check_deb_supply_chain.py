#!/usr/bin/env python3
                          
                                                               
                                                                                      
                                                                                                                            
 
                      
                                                                                                                        
                                                                           

    
                                                                    
                                                  
                                       

                                                               
                                                                                        
                                                           
                                                        
                                                                  
                                                    
                                
                                                                    
                                                      

                                                                
                                                               
                                           

              
                                                               
                                                     
                                                  
                                                                  
                                                                       

     
                                                                         
                                                                           
                                  
                                                               
                                                                     
                                                                             
                               
                                                                     
                                                            
                                                  
                                                                   
                                                                  

     
                                                                        
                                                                        
                        
                                                                         

       
                                                                   
                                                        
                      

       
                  
          
                        
   
from __future__ import annotations

import argparse
import re
import sys
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
REQUIRED_UI_ASSETS = ("index.html", "app.bundle.js", "sw.js")


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


def check(raw: str) -> tuple[list[str], dict]:
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

    return bad, info


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
              "docs/DEVELOPMENT_VERIFICATION_POLICY.md", file=sys.stderr)
        return 1

    print("\033[32m[PASS]\033[0m deb 공급망: 의존 버전 하한 + control 배선 + md5sums 전수 + "
          "--root-owner-group + vendor 핀 전이 + 필수 자산 검증")
    return 0


if __name__ == "__main__":
    sys.exit(main())
