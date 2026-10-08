#!/usr/bin/env python3












from __future__ import annotations

import argparse
import json
import re
from datetime import datetime, timezone
from pathlib import Path


POOL = "hugepages-2048kB"
COUNTERS = ("nr_hugepages", "free_hugepages", "surplus_hugepages")
MAX_COUNTER = (1 << 63) - 1
MAX_MEMORY_MB = (1 << 31) - 1
MAX_PLAN_BYTES = 65536


class CapacityError(ValueError):
    pass



def integer(value, label, minimum=0, maximum=MAX_MEMORY_MB):

    if type(value) is not int or not minimum <= value <= maximum:
        raise CapacityError(f"{label}: integer in [{minimum}, {maximum}] required")
    return value


def exact_object(value, keys, label):

    if not isinstance(value, dict) or set(value) != set(keys):
        raise CapacityError(f"{label}: expected fields {', '.join(sorted(keys))}")


def unique_object(pairs):

    result = {}
    for key, value in pairs:
        if key in result:
            raise CapacityError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_plan(path):

    with path.open("rb") as stream:
        raw = stream.read(MAX_PLAN_BYTES + 1)
    if len(raw) > MAX_PLAN_BYTES:
        raise CapacityError("plan exceeds 64 KiB")
    plan = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object)
    exact_object(plan, {"schema_version", "page_size_kib", "ovs", "guests"}, "plan")
    integer(plan["schema_version"], "schema_version", 1, 1)
    integer(plan["page_size_kib"], "page_size_kib", 2048, 2048)
    ovs = plan["ovs"]
    exact_object(ovs, {"allocation", "socket_mem_mb", "headroom_mb"}, "ovs")
    if ovs["allocation"] not in ("pending", "accounted"):
        raise CapacityError("ovs.allocation: pending or accounted required")
    socket_mem = ovs["socket_mem_mb"]
    if not isinstance(socket_mem, str) or not re.fullmatch(r"[0-9]+(?:,[0-9]+)*", socket_mem):
        raise CapacityError("ovs.socket_mem_mb: explicit comma-separated nonnegative MB required")
    values = socket_mem.split(",")
    if len(values) > 4096 or any(len(value) > 10 for value in values):
        raise CapacityError("ovs.socket_mem_mb: value or node count exceeds supported range")
    ovs["socket_mem_values"] = [integer(int(v), "ovs.socket_mem_mb") for v in values]
    headroom = ovs["headroom_mb"]
    if not isinstance(headroom, dict):
        raise CapacityError("ovs.headroom_mb: node-to-MB object required")
    for node, memory in headroom.items():
        if not re.fullmatch(r"0|[1-9][0-9]{0,3}", node):
            raise CapacityError("ovs.headroom_mb: canonical host node ID required")
        integer(memory, f"headroom_mb[{node}]")
    guests = plan["guests"]
    if not isinstance(guests, list):
        raise CapacityError("guests: array required")
    names = set()
    for guest in guests:
        exact_object(guest, {"name", "host_node", "memory_mb"}, "guest")
        name = guest["name"]
        if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}", name):
            raise CapacityError("guest.name: 1..64 ASCII identifier required")
        if name in names:
            raise CapacityError(f"duplicate guest: {name}")
        names.add(name)
        integer(guest["host_node"], "guest.host_node", 0, 4095)
        integer(guest["memory_mb"], "guest.memory_mb", 1)
    return plan


def read_pool(path, global_pool=False):

    names = COUNTERS + (("resv_hugepages",) if global_pool else ())
    result = {}
    for name in names:
        raw = (path / name).read_text(encoding="ascii").strip()
        if not re.fullmatch(r"[0-9]{1,19}", raw):
            raise CapacityError(f"invalid counter: {path / name}")
        result[name] = integer(int(raw), str(path / name), maximum=MAX_COUNTER)
    if result["free_hugepages"] > result["nr_hugepages"] + result["surplus_hugepages"]:
        raise CapacityError(f"free exceeds total plus surplus: {path}")
    return result


def read_snapshot(sysfs_root):

    global_path = sysfs_root / "kernel/mm/hugepages" / POOL
    node_root = sysfs_root / "devices/system/node"

    def pools():
        return {int(p.name[4:]): p / "hugepages" / POOL
                for p in node_root.glob("node[0-9]*")
                if re.fullmatch(r"node(?:0|[1-9][0-9]*)", p.name)
                and (p / "hugepages" / POOL).is_dir()}

    before = read_pool(global_path, True)
    paths = pools()
    if not paths:
        raise CapacityError("no NUMA 2MiB hugepage pools observed")
    nodes = {node: read_pool(path) for node, path in sorted(paths.items())}
    after_nodes = {node: read_pool(path) for node, path in sorted(paths.items())}
    after = read_pool(global_path, True)
    if before != after or nodes != after_nodes or paths != pools():
        raise CapacityError("hugepage counters or node topology changed during observation")
    for key in COUNTERS:
        if before[key] != sum(values[key] for values in nodes.values()):
            raise CapacityError(f"global/NUMA counter mismatch: {key}")
    return before, nodes


def pages(memory_mb):

    return (memory_mb + 1) // 2


def evaluate(plan, global_pool, node_pools):

    demands = {node: {"ovs_pages": 0, "guest_pages": 0, "headroom_pages": 0}
               for node in node_pools}
    ovs = plan["ovs"]
    for node, memory_mb in enumerate(ovs["socket_mem_values"]):
        if memory_mb:
            if node not in demands:
                raise CapacityError(f"OVS requested unobserved host node {node}")
            if ovs["allocation"] == "pending":
                demands[node]["ovs_pages"] = pages(memory_mb)
    for node_text, memory_mb in ovs["headroom_mb"].items():
        node = int(node_text)
        if node not in demands:
            raise CapacityError(f"headroom requested unobserved host node {node}")
        demands[node]["headroom_pages"] = pages(memory_mb)
    for guest in plan["guests"]:
        node = guest["host_node"]
        if node not in demands:
            raise CapacityError(f"guest requested unobserved host node {node}")
        demands[node]["guest_pages"] += pages(guest["memory_mb"])
    if not any(sum(value.values()) for value in demands.values()):
        raise CapacityError("no additional hugepage demand specified")

    reserved = global_pool["resv_hugepages"]
    reports = []
    for node, counts in sorted(node_pools.items()):
        demand = demands[node]
        available = max(0, counts["free_hugepages"] - reserved)
        required = sum(demand.values())
        reports.append({"host_node": node, **counts, **demand,
                        "reserved_upper_bound_pages": reserved,
                        "available_pages": available, "required_pages": required,
                        "shortage_pages": max(0, required - available)})
    sufficient = all(node["shortage_pages"] == 0 for node in reports)
    return {
        "status": "CAPACITY-SUFFICIENT" if sufficient else "CAPACITY-INSUFFICIENT",
        "advisory_only": True,
        "observed_at": datetime.now(timezone.utc).isoformat(),
        "page_size_kib": 2048,
        "ovs_allocation": ovs["allocation"],
        "ovs_allocation_source": "plan",
        "reservation_policy": "global-reserved-upper-bound-per-node",
        "global": global_pool,
        "nodes": reports,
        "limitations": ["plan OVS allocation and host NUMA placement are not independently verified",
                        "snapshot does not reserve memory or guarantee VM/OVS admission",
                        "OVS dynamic growth beyond explicit headroom is not covered"],
    }, 0 if sufficient else 1


def main():

    parser = argparse.ArgumentParser(description="Read-only 2MiB DPDK/VM NUMA capacity snapshot")
    parser.add_argument("--plan", type=Path, required=True, help="schema_version=1 JSON plan")
    parser.add_argument("--sysfs-root", type=Path, default=Path("/sys"), help="sysfs root (default: /sys)")
    args = parser.parse_args()
    try:
        plan = load_plan(args.plan)
        global_pool, node_pools = read_snapshot(args.sysfs_root)
        report, code = evaluate(plan, global_pool, node_pools)
    except (OSError, ValueError, RecursionError) as error:
        report, code = {"status": "INDETERMINATE", "advisory_only": True, "reason": str(error)}, 2
    print(json.dumps(report, ensure_ascii=False, sort_keys=True, indent=2))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
