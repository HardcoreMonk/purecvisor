#!/usr/bin/env python3
                          
                                                                                      
                                                                         
                                               
                                                               
               
from __future__ import annotations

import os
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(os.environ.get("PCV_SPLIT_ROOT") or Path(__file__).resolve().parents[2])


@dataclass(frozen=True)
class SplitTarget:
    name: str
    source: str
    manifest: str
    rename_map: str
    family_globs: tuple[str, ...]
    makefile_anchor: str
    internal_header: str | None
    header_includes: tuple[str, ...]
    c_prologue: str
    h_prologue: str | None                                                       
    group_comment: str
    split_files: tuple[str, ...]
    identity_routes: bool
    identity_async: bool
    identity_policy: str                                  
                                                                                     
    blocks: str | None = None                                
    block_function: str | None = None                                             
    chain_files: tuple[str, ...] = ()                    
    context_type: str | None = None                                        
    context_var: str | None = None                           
    extract_c_prologue: str | None = None                         
    extra: dict = field(default_factory=dict)


def load(name: str = "dispatcher") -> SplitTarget:
    path = ROOT / "scripts/split/targets" / f"{name}.toml"
    if not path.exists():
        raise FileNotFoundError(f"분할 대상 설정이 없다: {path}")
    d = tomllib.loads(path.read_text())
    ident = d.get("identity", {})
    anchor = d["makefile_anchor"]
    return SplitTarget(
        name=d["name"], source=d["source"], manifest=d["manifest"], rename_map=d["rename_map"],
        family_globs=tuple(d["family_globs"]),
        makefile_anchor=anchor if anchor.endswith("\n") else anchor + "\n",
        internal_header=d.get("internal_header"), header_includes=tuple(d.get("header_includes", [])),
        c_prologue=d.get("c_prologue", ""), h_prologue=d.get("h_prologue"),
        group_comment=d.get("group_comment", ""),
        split_files=tuple(d.get("split_files", [])),
        identity_routes=bool(ident.get("routes", False)), identity_async=bool(ident.get("async", False)),
        identity_policy=ident.get("policy", "none"),
        blocks=d.get("blocks"), block_function=d.get("block_function"),
        chain_files=tuple(d.get("chain_files", [])), context_type=d.get("context_type"),
        context_var=d.get("context_var"), extract_c_prologue=d.get("extract_c_prologue"),
        extra=d.get("extra", {}))
