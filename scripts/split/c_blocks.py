#!/usr/bin/env python3
                          
                                                        
                                                 
                                                                  
                                                            
 
                      
                                                     
                                        
                                  
from __future__ import annotations

import re
from dataclasses import dataclass

IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
PP_COND_RE = re.compile(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b", re.M)
FUNC_NAME_DEPENDENT_RE = re.compile(
    r"\b(__func__|__FUNCTION__|G_STRFUNC|g_return_if_fail|g_return_val_if_fail)\b")


def pp_balanced(src: str) -> bool:
                                                                                 
    depth = 0
    for mt in PP_COND_RE.finditer(src):
        kw = mt.group(1)
        if kw in ("if", "ifdef", "ifndef"):
            depth += 1
        elif depth == 0:
            return False
        elif kw == "endif":
            depth -= 1
    return depth == 0


def classify(src: str) -> str:
                                                             
    kinds = bytearray(b"c" * len(src))
    i, n = 0, len(src)
    while i < n:
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            kinds[i:j] = b"m" * (j - i)
            i = j
        elif src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            kinds[i:j] = b"m" * (j - i)
            i = j
        elif src[i] in "\"'":
            quote, j = src[i], i + 1
            while j < n and src[j] != quote and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            j = min(j + 1, n)
            kinds[i:j] = b"s" * (j - i)
            i = j
        else:
            i += 1
    return kinds.decode("ascii")


def masked(src: str, kinds: str | None = None) -> str:
                                        
    kinds = kinds if kinds is not None else classify(src)
    return "".join(ch if k == "c" or ch == "\n" else " " for ch, k in zip(src, kinds))


def identifiers(code: str) -> set[str]:
                            
    return set(IDENT_RE.findall(code))


@dataclass(frozen=True)
class FnBlock:
                                           
    name: str
    region_start: int                                              
    doc_start: int                                         
    decl_start: int                            
    paren_open: int                 
    body_open: int             
    body_close: int           
    end: int                               
    is_static: bool


@dataclass(frozen=True)
class TopDecl:
                                                    
    start: int
    end: int
    text: str


def _line_start(s: str, i: int) -> int:
    return s.rfind("\n", 0, i) + 1


def _next_line(s: str, i: int) -> int:
    j = s.find("\n", i)
    return len(s) if j < 0 else j + 1


def _match_brace(m: str, i: int) -> int:
    depth = 0
    for j in range(i, len(m)):
        if m[j] == "{":
            depth += 1
        elif m[j] == "}":
            depth -= 1
            if depth == 0:
                return j
    raise ValueError(f"짝 없는 '{{' (offset {i})")


def _match_paren_back(m: str, i: int) -> int:
    depth = 0
    for j in range(i, -1, -1):
        if m[j] == ")":
            depth += 1
        elif m[j] == "(":
            depth -= 1
            if depth == 0:
                return j
    raise ValueError(f"짝 없는 ')' (offset {i})")


def _doc_start(src: str, kinds: str, decl_start: int) -> int:
    start = _line_start(src, decl_start)
    while start > 0:
        prev = _line_start(src, start - 1)
        line, k = src[prev:start - 1], kinds[prev:start - 1]
        if line.strip() == "" or any(kk == "c" and not ch.isspace() for ch, kk in zip(line, k)):
            break
        start = prev
    return start


def _decl_start(m: str, po: int, top_end: int) -> int:
                                                         
                                                                               
    start = _line_start(m, po)
    while start > top_end:
        prev = _line_start(m, start - 1)
        line = m[prev:start - 1].strip()
        if not line or line[-1] in ");}," or line.startswith("#"):
            break
        start = prev
    start = max(start, top_end)
    seg = m[start:po]
    return start + (len(seg) - len(seg.lstrip()))


def scan(src: str) -> tuple[list[FnBlock], list[TopDecl]]:
                                              
    kinds = classify(src)
    m = masked(src, kinds)
    fns: list[FnBlock] = []
    decls: list[TopDecl] = []
    n, i, top_end = len(m), 0, 0
    prev_end: int | None = None
    while i < n:
        ch = m[i]
        if ch == "#" and m[_line_start(m, i):i].strip() == "":
            j = i
            while True:
                e = m.find("\n", j)
                if e < 0:
                    e = n
                    break
                if m[e - 1] == "\\":
                    j = e + 1
                    continue
                break
            decls.append(TopDecl(i, min(e + 1, n), src[i:e]))
            i = top_end = min(e + 1, n)
            continue
        if ch == ";":
            stmt_start = top_end + (len(m[top_end:i]) - len(m[top_end:i].lstrip()))
            decls.append(TopDecl(stmt_start, i + 1, src[stmt_start:i + 1]))
            top_end = i + 1
        elif ch == "{":
            close = _match_brace(m, i)
            k = i - 1
            while k >= 0 and m[k].isspace():
                k -= 1
            is_fn = False
            if k >= 0 and m[k] == ")":
                po = _match_paren_back(m, k)
                head = m[top_end:po]
                name_m = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*$", head)
                if name_m and "=" not in head:
                    is_fn = True
                    decl_start = _decl_start(m, po, top_end)
                    doc = _doc_start(src, kinds, decl_start)
                    fns.append(FnBlock(
                        name=name_m.group(1),
                        region_start=prev_end if prev_end is not None else doc,
                        doc_start=doc, decl_start=decl_start, paren_open=po,
                        body_open=i, body_close=close, end=_next_line(m, close),
                        is_static=re.search(r"\bstatic\b", m[decl_start:po]) is not None))
                    prev_end = _next_line(m, close)
            i = close + 1
            if is_fn:
                top_end = i
            continue
        i += 1
    return fns, decls


def find_functions(src: str) -> list[FnBlock]:
    return scan(src)[0]


def apply_renames(src: str, renames: dict[str, str]) -> str:
                                                
    if not renames:
        return src
    kinds = classify(src)
    pat = re.compile(r"(?<![A-Za-z0-9_])("
                     + "|".join(sorted(map(re.escape, renames), key=len, reverse=True))
                     + r")(?![A-Za-z0-9_])")
    out, last = [], 0
    for mt in pat.finditer(src):
        if kinds[mt.start()] == "s":
            continue
        out.append(src[last:mt.start()])
        out.append(renames[mt.group(1)])
        last = mt.end()
    out.append(src[last:])
    return "".join(out)


def drop_static(src: str, names: set[str]) -> str:
                                                        
    for fb in sorted(find_functions(src), key=lambda f: f.decl_start, reverse=True):
        if fb.name in names and fb.is_static:
            head = src[fb.decl_start:fb.paren_open]
            src = src[:fb.decl_start] + re.sub(r"\bstatic\s+", "", head, count=1) + src[fb.paren_open:]
    return src


def prototype(src: str, name: str) -> str:
                                                         
    fb = next(f for f in find_functions(src) if f.name == name)
    head = re.sub(r"\bstatic\s+", "", src[fb.decl_start:fb.body_open], count=1).rstrip()
    return head + ";"


def normalized_function_text(src: str, fb: FnBlock, renames: dict[str, str]) -> str:
                                                      
    head = re.sub(r"\bstatic\s+", "", src[fb.decl_start:fb.body_open], count=1)
    return apply_renames(src[fb.doc_start:fb.decl_start] + head + src[fb.body_open:fb.end], renames)
