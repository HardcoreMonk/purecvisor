#!/usr/bin/env bash
set -euo pipefail

# Build an Ubuntu Server autoinstall ISO for web-service VMs.
# The source ISO is not modified. The generated ISO installs an optimized
# baseline with nginx, qemu-guest-agent, tuned, sysctl, limits, and nginx
# worker settings suitable for HTTP/HTTPS workloads.

SRC_ISO="${1:-/var/lib/libvirt/images/iso-fast/ubuntu-24.04.4-live-server-amd64.iso}"
OUT_ISO="${2:-/var/lib/libvirt/images/iso-fast/ubuntu-24.04.4-live-server-amd64-webperf-autoinstall.iso}"
WORK_DIR="${PCV_WEBPERF_ISO_WORKDIR:-/tmp/pcv-ubuntu-webperf-iso}"
HOSTNAME="${PCV_WEBPERF_HOSTNAME:-ubuntu-webperf}"
USERNAME="${PCV_WEBPERF_USERNAME:-webadmin}"
PASSWORD="${PCV_WEBPERF_PASSWORD:-}"
VOLID="${PCV_WEBPERF_VOLID:-UBUNTU_2404_WEBPERF}"
HOME_SIZE="${PCV_WEBPERF_HOME_SIZE:-}"
STAMP="$(date +%Y%m%d-%H%M%S)"
CRED_FILE="${PCV_WEBPERF_CRED_FILE:-/root/purecvisor-webperf-iso-credentials-${STAMP}.txt}"

need() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "missing dependency: $1" >&2
        exit 2
    fi
}

need xorriso
need openssl
need sed

if [ ! -f "$SRC_ISO" ]; then
    echo "source ISO not found: $SRC_ISO" >&2
    exit 1
fi

if [ -z "$PASSWORD" ]; then
    PASSWORD="$(openssl rand -base64 24 | tr -d '=+/' | cut -c1-22)"
fi
PASSWORD_HASH="$(openssl passwd -6 "$PASSWORD")"

if [ -n "$HOME_SIZE" ]; then
    STORAGE_CONFIG="$(cat <<EOF_STORAGE
  storage:
    config:
      - type: disk
        id: disk0
        match:
          size: largest
        ptable: gpt
        wipe: superblock-recursive
        grub_device: true
      - type: partition
        id: bios-grub
        device: disk0
        size: 1M
        flag: bios_grub
        wipe: superblock
      - type: partition
        id: esp
        device: disk0
        size: 1G
        flag: boot
        wipe: superblock
      - type: partition
        id: boot
        device: disk0
        size: 2G
        wipe: superblock
      - type: partition
        id: pv
        device: disk0
        size: -1
        wipe: superblock
      - type: format
        id: esp-format
        volume: esp
        fstype: fat32
      - type: format
        id: boot-format
        volume: boot
        fstype: ext4
      - type: lvm_volgroup
        id: vg0
        name: ubuntu-vg
        devices:
          - pv
      - type: lvm_partition
        id: lv-home
        name: home
        volgroup: vg0
        size: ${HOME_SIZE}
      - type: lvm_partition
        id: lv-root
        name: root
        volgroup: vg0
        size: -1
      - type: format
        id: root-format
        volume: lv-root
        fstype: ext4
      - type: format
        id: home-format
        volume: lv-home
        fstype: ext4
      - type: mount
        id: root-mount
        device: root-format
        path: /
      - type: mount
        id: boot-mount
        device: boot-format
        path: /boot
      - type: mount
        id: esp-mount
        device: esp-format
        path: /boot/efi
      - type: mount
        id: home-mount
        device: home-format
        path: /home
EOF_STORAGE
)"
else
    STORAGE_CONFIG="$(cat <<'EOF_STORAGE'
  storage:
    layout:
      name: direct
EOF_STORAGE
)"
fi

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/nocloud/files" "$WORK_DIR/boot/grub"

cat > "$WORK_DIR/nocloud/meta-data" <<EOF_META
instance-id: purecvisor-webperf-${STAMP}
local-hostname: ${HOSTNAME}
EOF_META

cat > "$WORK_DIR/nocloud/files/pcv-webperf-install.sh" <<'EOF_INSTALL'
#!/usr/bin/env bash
set -euo pipefail

cat > /etc/sysctl.d/99-webservice-performance.conf <<'EOF_SYSCTL'
# PureCVisor web-service performance baseline.
fs.file-max = 2097152
net.core.somaxconn = 65535
net.core.netdev_max_backlog = 250000
net.core.rmem_max = 134217728
net.core.wmem_max = 134217728
net.ipv4.ip_local_port_range = 1024 65535
net.ipv4.tcp_congestion_control = bbr
net.ipv4.tcp_fastopen = 3
net.ipv4.tcp_fin_timeout = 15
net.ipv4.tcp_keepalive_time = 600
net.ipv4.tcp_max_syn_backlog = 65535
net.ipv4.tcp_mtu_probing = 1
net.ipv4.tcp_tw_reuse = 1
net.core.default_qdisc = fq
vm.swappiness = 10
vm.dirty_background_ratio = 5
vm.dirty_ratio = 10
EOF_SYSCTL

cat > /etc/modules-load.d/99-webservice-performance.conf <<'EOF_MODULES'
tcp_bbr
EOF_MODULES

cat > /etc/security/limits.d/99-webservice-performance.conf <<'EOF_LIMITS'
* soft nofile 1048576
* hard nofile 1048576
www-data soft nofile 1048576
www-data hard nofile 1048576
root soft nofile 1048576
root hard nofile 1048576
EOF_LIMITS

mkdir -p /etc/systemd/system/nginx.service.d
cat > /etc/systemd/system/nginx.service.d/10-webservice-performance.conf <<'EOF_NGINX_LIMIT'
[Service]
LimitNOFILE=1048576
EOF_NGINX_LIMIT

if [ -d /etc/nginx ]; then
    cp -a /etc/nginx/nginx.conf /etc/nginx/nginx.conf.pcv-default 2>/dev/null || true
    cat > /etc/nginx/nginx.conf <<'EOF_NGINX'
user www-data;
worker_processes auto;
worker_rlimit_nofile 1048576;
pid /run/nginx.pid;
include /etc/nginx/modules-enabled/*.conf;

events {
    worker_connections 65535;
    multi_accept on;
    use epoll;
}

http {
    sendfile on;
    tcp_nopush on;
    tcp_nodelay on;
    keepalive_timeout 30;
    keepalive_requests 10000;
    types_hash_max_size 4096;
    server_tokens off;

    client_body_timeout 12;
    client_header_timeout 12;
    send_timeout 10;

    gzip on;
    gzip_comp_level 5;
    gzip_min_length 1024;
    gzip_proxied any;
    gzip_vary on;
    gzip_types
        text/plain
        text/css
        text/xml
        application/json
        application/javascript
        application/xml
        application/rss+xml
        image/svg+xml;

    access_log /var/log/nginx/access.log;
    error_log /var/log/nginx/error.log warn;

    include /etc/nginx/mime.types;
    default_type application/octet-stream;
    include /etc/nginx/conf.d/*.conf;
    include /etc/nginx/sites-enabled/*;
}
EOF_NGINX
fi

cat > /usr/local/sbin/pcv-webperf-apply <<'EOF_APPLY'
#!/usr/bin/env bash
set -euo pipefail
modprobe tcp_bbr 2>/dev/null || true
sysctl --system >/dev/null || true
systemctl daemon-reload
systemctl restart nginx 2>/dev/null || true
tuned-adm profile throughput-performance 2>/dev/null || true
EOF_APPLY
chmod 755 /usr/local/sbin/pcv-webperf-apply

systemctl enable qemu-guest-agent 2>/dev/null || true
systemctl enable nginx 2>/dev/null || true
systemctl enable tuned 2>/dev/null || true
systemctl enable sysstat 2>/dev/null || true
systemctl enable fail2ban 2>/dev/null || true
tuned-adm profile throughput-performance 2>/dev/null || true
chage -d 0 __PCV_USERNAME__ 2>/dev/null || true
/usr/local/sbin/pcv-webperf-apply || true
EOF_INSTALL
sed -i "s|__PCV_USERNAME__|${USERNAME}|g" "$WORK_DIR/nocloud/files/pcv-webperf-install.sh"
chmod 755 "$WORK_DIR/nocloud/files/pcv-webperf-install.sh"

cat > "$WORK_DIR/nocloud/user-data" <<EOF_USERDATA
#cloud-config
autoinstall:
  version: 1
  locale: en_US.UTF-8
  keyboard:
    layout: us
  timezone: Asia/Seoul
  updates: security
  ssh:
    install-server: true
    allow-pw: true
  identity:
    hostname: ${HOSTNAME}
    username: ${USERNAME}
    password: "${PASSWORD_HASH}"
${STORAGE_CONFIG}
  packages:
    - nginx
    - qemu-guest-agent
    - tuned
    - sysstat
    - chrony
    - fail2ban
    - curl
    - htop
    - vim
    - unzip
  late-commands:
    - cp /cdrom/nocloud/files/pcv-webperf-install.sh /target/usr/local/sbin/pcv-webperf-install.sh
    - curtin in-target --target=/target -- chmod 755 /usr/local/sbin/pcv-webperf-install.sh
    - curtin in-target --target=/target -- /usr/local/sbin/pcv-webperf-install.sh
  user-data:
    disable_root: true
    final_message: "PureCVisor Ubuntu web-performance image installed."
EOF_USERDATA

for grub_path in /boot/grub/grub.cfg /boot/grub/loopback.cfg; do
    target="$WORK_DIR${grub_path}"
    mkdir -p "$(dirname "$target")"
    if xorriso -osirrox on -indev "$SRC_ISO" -extract "$grub_path" "$target" >/dev/null 2>&1; then
        sed -i \
            -e 's|quiet splash|quiet splash autoinstall ds=nocloud\\;s=/cdrom/nocloud/|g' \
            -e 's| ---| autoinstall ds=nocloud\\;s=/cdrom/nocloud/ ---|g' \
            -e 's/^set timeout=.*/set timeout=5/' \
            "$target"
        # Avoid duplicate kernel arguments if both patterns matched.
        sed -i 's|autoinstall ds=nocloud\\;s=/cdrom/nocloud/ autoinstall ds=nocloud\\;s=/cdrom/nocloud/|autoinstall ds=nocloud\\;s=/cdrom/nocloud/|g' "$target"
    else
        rm -f "$target"
    fi
done

rm -f "$OUT_ISO"
xorriso \
    -indev "$SRC_ISO" \
    -outdev "$OUT_ISO" \
    -boot_image any replay \
    -volid "$VOLID" \
    -map "$WORK_DIR/nocloud" /nocloud \
    $(test -f "$WORK_DIR/boot/grub/grub.cfg" && printf '%s\n' -map "$WORK_DIR/boot/grub/grub.cfg" /boot/grub/grub.cfg) \
    $(test -f "$WORK_DIR/boot/grub/loopback.cfg" && printf '%s\n' -map "$WORK_DIR/boot/grub/loopback.cfg" /boot/grub/loopback.cfg) \
    >/tmp/pcv-webperf-xorriso-${STAMP}.log 2>&1

chmod 0644 "$OUT_ISO"
sha256sum "$OUT_ISO" > "${OUT_ISO}.sha256"

mkdir -p "$(dirname "$CRED_FILE")"
cat > "$CRED_FILE" <<EOF_CRED
PureCVisor Ubuntu web-performance autoinstall ISO
created_at=${STAMP}
iso=${OUT_ISO}
username=${USERNAME}
initial_password=${PASSWORD}
password_policy=expires_on_first_login
home_size=${HOME_SIZE:-default-direct-layout}
warning=Autoinstall wipes the target VM disk.
EOF_CRED
chmod 0600 "$CRED_FILE"

cat <<EOF_DONE
OK
source=${SRC_ISO}
output=${OUT_ISO}
sha256=$(cut -d' ' -f1 "${OUT_ISO}.sha256")
credentials=${CRED_FILE}
warning=autoinstall_wipes_target_vm_disk
EOF_DONE
