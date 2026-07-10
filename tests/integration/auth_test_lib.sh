#!/usr/bin/env bash

# Common auth helper for integration tests.
# Goal: avoid hardcoded admin credentials in local SAFE runs.
# Resolution order is explicit: test env, daemon env, then daemon.conf.
# Public tests must not carry built-in admin password examples; older local
# installs should set PCV_TEST_ADMIN_PASSWORD explicitly.

pcv_trim() {
    local value="$1"
    value="${value#"${value%%[![:space:]]*}"}"
    value="${value%"${value##*[![:space:]]}"}"
    printf '%s' "$value"
}

pcv_cfg_value() {
    local key="$1"
    local cfg="${PCV_TEST_DAEMON_CONF:-/etc/purecvisor/daemon.conf}"
    [ -r "$cfg" ] || return 1
    awk -F= -v wanted="$key" '
        BEGIN { in_daemon=0 }
        /^[[:space:]]*\[/ {
            in_daemon = ($0 ~ /^[[:space:]]*\[daemon\][[:space:]]*$/)
            next
        }
        in_daemon && $1 ~ "^[[:space:]]*" wanted "[[:space:]]*$" {
            sub(/^[[:space:]]+/, "", $2)
            sub(/[[:space:]]+$/, "", $2)
            print $2
            exit
        }
    ' "$cfg"
}

pcv_try_login() {
    local base="$1"
    local user="$2"
    local pass="$3"
    [ -n "$user" ] || return 1
    [ -n "$pass" ] || return 1

    local resp
    resp=$(curl -s --max-time 8 -X POST "${base}/auth/token" \
      -H 'Content-Type: application/json' \
      -d "{\"username\":\"${user}\",\"password\":\"${pass}\"}" 2>/dev/null) || return 1

    local token csrf
    token=$(printf '%s' "$resp" | python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null || true)
    csrf=$(printf '%s' "$resp" | python3 -c "import sys,json;print(json.load(sys.stdin).get('csrf_token',''))" 2>/dev/null || true)
    [ -n "$token" ] || return 1

    PCV_AUTH_RESPONSE="$resp"
    PCV_AUTH_USER="$user"
    PCV_AUTH_PASSWORD="$pass"
    PCV_AUTH_TOKEN="$token"
    PCV_AUTH_CSRF="$csrf"
    return 0
}

pcv_resolve_auth() {
    local base="$1"
    PCV_AUTH_RESPONSE=""
    PCV_AUTH_USER=""
    PCV_AUTH_PASSWORD=""
    PCV_AUTH_TOKEN=""
    PCV_AUTH_CSRF=""

    local cfg_user cfg_pass
    cfg_user=$(pcv_trim "$(pcv_cfg_value admin_user 2>/dev/null || true)")
    cfg_pass=$(pcv_trim "$(pcv_cfg_value admin_password 2>/dev/null || true)")

    local -a candidates=()
    candidates+=("${PCV_TEST_ADMIN_USER:-}:${PCV_TEST_ADMIN_PASSWORD:-}")
    candidates+=("${PURECVISOR_ADMIN_USER:-}:${PURECVISOR_ADMIN_PASSWORD:-}")
    candidates+=("${cfg_user:-admin}:${cfg_pass:-}")

    local pair user pass seen=""
    for pair in "${candidates[@]}"; do
        user="${pair%%:*}"
        pass="${pair#*:}"
        [ -n "$user" ] || continue
        [ -n "$pass" ] || continue
        case " $seen " in
            *" ${user}:${pass} "*) continue ;;
        esac
        seen="${seen} ${user}:${pass}"
        if pcv_try_login "$base" "$user" "$pass"; then
            return 0
        fi
    done
    return 1
}
