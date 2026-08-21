#!/usr/bin/env python3
                          
                                                       
                                                                                      
                                                                                                                            
 
                      
                                                                                                                
                                                                            

                                                            
                                                 
                                                        

                                                   
                                                                    
                                                            
                                                           
                                                     
               
                                                             
import re
import sys
import pathlib

                                                         
                                  
SECRET_RE = re.compile(
    r"(privkey|raw_priv|\bpriv\b|secret\w*|passwo\w*|plaintext\w*|apikey|api_key|\w+_key)\b",
    re.I,
)
                                                          
                                                   
                                                                       
                                                         
                                      
                                                                    
ALLOW = re.compile(
    r"^(pubkey|raw_pub|key_path|key_hash|keyfile|keyid|pkey_path|"
    r"cert\w*|ca_path|\w*pub\w*|hash_key|route_key|cache_key|map_key|"
    r"rate_key|del_key|alloc_key|opt_key|parp_key|"
    r"s3_key|s3_data_key|s3_meta_key)$",
    re.I,
)
                                                                
                                                 
GFREE_RE = re.compile(r"\bg_free\s*\(\s*&?\s*([A-Za-z_][\w.>-]*)\s*\)")
                                                                 
                                                                
                                           
GSTRFREE_RE = re.compile(r"\bg_string_free\s*\(\s*([A-Za-z_][\w.>-]*)\s*,\s*(?:TRUE|true|1)\b")
SAFE_RE = re.compile(r"pcv_secure_(free_str|wipe)")


def _is_secret_member(var):
    member = re.split(r"->|\.", var)[-1]                         
    return (not ALLOW.match(member)) and bool(SECRET_RE.search(member)), member


def scan_text(text, fname="<mem>"):
    out = []
    lines = text.splitlines()
    for i, line in enumerate(lines, 1):
        if SAFE_RE.search(line):                                
            continue
        m = GFREE_RE.search(line)
        if m:
            secret, _ = _is_secret_member(m.group(1))
            if secret:
                out.append((fname, i, m.group(1), line.strip()))
            continue
        gm = GSTRFREE_RE.search(line)                           
        if gm:
            secret, member = _is_secret_member(gm.group(1))
            if secret:
                                                                       
                window = lines[max(0, i - 4):i - 1]
                if not any("pcv_secure_wipe" in w and member in w for w in window):
                    out.append((fname, i, gm.group(1), line.strip()))
    return out


def selftest():
    bad = "void f(void){ gchar *plaintext_key=x(); use(plaintext_key); g_free(plaintext_key); }"
    bad_member = "void f(void){ g_free(g_cfg.admin_password); }"                  
    bad_secret_dup = "void f(void){ gchar *secret_dup=x(); g_free(secret_dup); }"
    ok_wipe = "void f(void){ gchar *priv=x(); pcv_secure_free_str(&priv); }"
    ok_allow = "void f(void){ gchar *key_hash=x(); g_free(key_hash); }"
    ok_pub = "void f(void){ gchar *pubkey=x(); g_free(pubkey); }"
    ok_member = "void f(void){ g_free(t->members); }"              
    bad_gstr = "gchar *x;\ng_string_free(plaintext_key, TRUE);"                         
    ok_gstr = ("pcv_secure_wipe(plaintext_key->str, plaintext_key->len);\n"
               "g_string_free(plaintext_key, TRUE);")                            
    ok_gstr_false = "gchar *o = g_string_free(plaintext_key, FALSE);"                 
    fails = []
    if not scan_text(bad):
        fails.append("양성 fixture(plaintext_key g_free)를 잡지 못함")
    if not scan_text(bad_member):
        fails.append("양성 fixture(구조체멤버 g_cfg.admin_password)를 잡지 못함")
    if not scan_text(bad_secret_dup):
        fails.append("양성 fixture(secret_dup g_free)를 잡지 못함")
    if not scan_text(bad_gstr):
        fails.append("양성 fixture(g_string_free 평문 미-wipe)를 잡지 못함")
    if scan_text(ok_wipe):
        fails.append("음성 fixture(pcv_secure_free_str)를 오탐")
    if scan_text(ok_allow):
        fails.append("음성 fixture(key_hash allowlist)를 오탐")
    if scan_text(ok_pub):
        fails.append("음성 fixture(pubkey allowlist)를 오탐")
    if scan_text(ok_member):
        fails.append("음성 fixture(t->members)를 오탐")
    if scan_text(ok_gstr):
        fails.append("음성 fixture(g_string_free wipe-then-free)를 오탐")
    if scan_text(ok_gstr_false):
        fails.append("음성 fixture(g_string_free FALSE 소유이전)를 오탐")
    if fails:
        for f in fails:
            print(f"SELFTEST FAIL: {f}", file=sys.stderr)
        return 2
    print("check_secret_wipe self-test: OK (양성 4 / 음성 6)")
    return 0


def main():
    if "--selftest" in sys.argv:
        return selftest()
    root = pathlib.Path(__file__).resolve().parent.parent / "src"
    violations = []
    for path in sorted(root.rglob("*.c")):
        violations += scan_text(path.read_text(errors="replace"),
                                str(path.relative_to(root.parent)))
    if violations:
        print("secret free-without-wipe 위반(파일:라인:변수):", file=sys.stderr)
        for fn, ln, var, src in violations:
            print(f"  {fn}:{ln}: {var}  |  {src}", file=sys.stderr)
        print(f"총 {len(violations)}건 — pcv_secure_free_str/wipe 로 해제하거나 "
              f"비밀 아니면 allowlist 에 추가.", file=sys.stderr)
        return 1
    print("check_secret_wipe: 위반 없음 ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())
