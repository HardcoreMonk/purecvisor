#!/usr/bin/env python3
                          
                                                                                          
                                                                                     
                                                                          
                                                                                                         
                                                                                  
                                                              
 
                      
                                                                                  
                          
from __future__ import annotations

import os
import stat
import tempfile
from pathlib import Path


def chain_problem(path: Path) -> str | None:
                                                                                
    me = os.geteuid()
    cur = Path(os.path.realpath(path))
    while True:
        try:
            st = os.lstat(cur)
        except OSError as e:
            return f"{cur}: cannot stat: {e.strerror}"
        if not stat.S_ISDIR(st.st_mode):
            return f"{cur}: not a directory"
        if st.st_uid not in (0, me):
            return f"{cur}: owner uid {st.st_uid}"
        if st.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
            return f"{cur}: group/other writable ({stat.S_IMODE(st.st_mode):04o})"
        if str(cur) == "/":
            return None
        cur = cur.parent


def trusted_base() -> Path:
                                                        
    cands = []
    if os.environ.get("PCV_PLUGIN_TEST_BASE"):
        cands.append(Path(os.environ["PCV_PLUGIN_TEST_BASE"]))
    if os.environ.get("XDG_RUNTIME_DIR"):
        cands.append(Path(os.environ["XDG_RUNTIME_DIR"]))
    cands.append(Path(f"/run/user/{os.geteuid()}"))
    cache = Path.home() / ".cache"
    cands.append(cache)
    seen = []
    for c in cands:
        if c == cache and not c.exists():
            try:
                c.mkdir(mode=0o700, parents=False)
            except OSError:
                pass
        if not c.is_dir():
            seen.append(f"{c}: 없음")
            continue
        why = chain_problem(c)
        if why is None:
            return c
        seen.append(why)
    raise AssertionError("플러그인 신뢰 규칙을 지키는 임시 디렉터리 후보가 없다(PCV_PLUGIN_TEST_BASE 로 지정): " + "; ".join(seen))


def trusted_tempdir(prefix: str) -> tempfile.TemporaryDirectory:
                                                                       
    return tempfile.TemporaryDirectory(prefix=prefix, dir=trusted_base())
