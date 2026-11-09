#!/usr/bin/env bash

set -uo pipefail

COVER_PATHS="${PCV_ROOT_NS_COVER_PATHS-/var/lib/purecvisor /var/log/purecvisor /etc/purecvisor /run/purecvisor /var/log/suricata /run/libvirt /run/openvswitch /var/lib/lxc /var/lib/libvirt/images}"

if [[ "$#" -eq 0 ]]; then
    echo "usage: run_root_in_private_ns.sh <command> [args...]" >&2
    exit 2
fi

exec sudo env PCV_NS_COVER="$COVER_PATHS" \
    unshare --mount --propagation private -- bash -c '
    set -u
    for p in $PCV_NS_COVER; do
        [ -d "$p" ] || continue
        mount -t tmpfs tmpfs "$p" || { echo "[root-ns] cover failed: $p" >&2; exit 97; }
        if [ "$p" = /run/libvirt ]; then mkdir -p /run/libvirt/qemu || exit 97; fi
    done
    if [ -d /sys/fs/bpf ]; then
        mount -t bpf bpf /sys/fs/bpf || { echo "[root-ns] bpffs mount failed" >&2; exit 97; }
    fi
    exec "$@"
' pcv-ns "$@"
