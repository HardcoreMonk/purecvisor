#!/usr/bin/env python3




import argparse
import base64
import hashlib
import hmac
import json
import os
from pathlib import Path
import sqlite3
import struct
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

OLD = 'FixtureOldPassword!42'
NEW = 'FixtureNewPassword!73'
CONFIG = Path('/etc/purecvisor/daemon.conf')
DB = Path('/var/lib/purecvisor/rbac.db')
AUDIT = Path('/var/lib/purecvisor/pcv_audit.db')
BASE = 'http://127.0.0.1:28080/api/v1/'


def check(value, label):

    if not value:
        raise AssertionError(label)
    print('PASS', label, flush=True)


def request(path, data=None, token=None):
    headers = {'Content-Type': 'application/json'}
    if token:
        headers['Authorization'] = 'Bearer ' + token
    req = urllib.request.Request(BASE + path, headers=headers,
                                 data=None if data is None else json.dumps(data).encode())
    try:
        with urllib.request.urlopen(req, timeout=10) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        return error.code, json.load(error)


def login(password):
    return request('auth/token', {'username': 'admin', 'password': password})


def sql(statement):

    with sqlite3.connect(DB) as conn:
        return conn.execute(statement).fetchall()


def snapshot():
    return sql('SELECT password_hash,salt FROM users WHERE username="admin"'), sql(
        'SELECT id,revoked FROM sessions ORDER BY id')


def audit_rows():
    with sqlite3.connect(AUDIT) as conn:
        return conn.execute('SELECT username,result,src_ip,error_code FROM audit_log '
                            'WHERE method="auth.password.change" ORDER BY id').fetchall()


def write_config(password=OLD, roles=''):
    CONFIG.write_text('[daemon]\nadmin_user = admin\nadmin_password = ' + password + '\n'
                      'rest_port = 28080\nsocket_path = /var/lib/purecvisor/daemon.sock\n'
                      'jwt_secret = fixture-secret-admin-rotation-0000000000000001\n'
                      'libvirt_uri = test:///default\ndrain_timeout = 1\n'
                      '[auth]\nrequire_totp_roles = ' + roles + '\n')
    CONFIG.chmod(0o600)


def run_inside(daemon, host_net):
    check(os.readlink('/proc/self/ns/net') != host_net, 'private network namespace')
    proc = None
    log = open('/tmp/daemon.log', 'w+')

    def stop():
        nonlocal proc
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        proc = None

    def boot():
        nonlocal proc
        proc = subprocess.Popen([daemon], stdout=log, stderr=log,
                                env={'PATH': '/usr/sbin:/usr/bin:/sbin:/bin', 'LANG': 'C.UTF-8'})
        for _ in range(100):
            if proc.poll() is not None:
                raise RuntimeError('isolated daemon exited: ' + str(proc.returncode))
            try:
                code, _ = request('health')
                if code in (200, 503) and DB.exists() and sql(
                        'SELECT count(*) FROM users WHERE username="admin"') == [(1,)]:
                    time.sleep(0.3)
                    return
            except (OSError, ValueError, sqlite3.Error):
                pass
            time.sleep(0.2)
        raise RuntimeError('isolated REST did not become ready')

    try:
        write_config()
        boot()
        status, pair = login(OLD)
        check(status == 200 and pair.get('access_token') and pair.get('refresh_token'), 'seed admin login')
        access, refresh = pair['access_token'], pair['refresh_token']
        check(request('auth/password', {'old_password': OLD, 'new_password': NEW})[0] == 401,
              'password change requires bearer')
        for old, new, expected, label in [
            ('wrong-old-password', NEW, 401, 'wrong old password'),
            (OLD, 'short', 400, 'short new password'), (OLD, OLD, 400, 'same new password')]:
            before = snapshot()
            code, _ = request('auth/password', {'old_password': old, 'new_password': new}, access)
            check(code == expected and snapshot() == before, label + ' rejected without mutation')


        code, body = request('auth/password', {'old_password': OLD, 'new_password': NEW}, access)
        check(code == 200, 'bootstrap admin normal password API returns 200 (actual ' + str(code) + ')')
        check('Refresh sessions revoked' in body.get('message', ''), 'API describes refresh scope')
        check(sql('SELECT count(*) FROM sessions WHERE revoked=0') == [(0,)], 'all existing refresh sessions revoked')
        check(request('auth/refresh', {'refresh_token': refresh})[0] == 401, 'old refresh rejected')
        check(login(OLD)[0] == 401, 'old config password rejected')
        status, pair = login(NEW)
        check(status == 200 and pair.get('access_token'), 'new DB password accepted')
        access = pair['access_token']


        for table, column in [('users', 'password_hash'), ('sessions', 'revoked')]:
            before = snapshot()
            sql('CREATE TRIGGER fixture_failure BEFORE UPDATE OF ' + column + ' ON ' + table +
                " BEGIN SELECT RAISE(ABORT,'fixture write failure'); END")
            code, _ = request('auth/password', {'old_password': NEW, 'new_password': OLD}, access)
            check(code == 500 and snapshot() == before, table + ' failure returns 500 and rolls back')
            sql('DROP TRIGGER fixture_failure')

        for _ in range(30):
            rows = audit_rows()
            if len(rows) >= 6:
                break
            time.sleep(0.2)
        check(len(rows) == 6 and [r[1] for r in rows] == ['fail', 'fail', 'fail', 'ok', 'fail', 'fail']
              and all(r[0] == 'admin' and r[2] == '127.0.0.1' for r in rows),
              'one persistent password audit per authenticated result with actor and IP')
        stop()
        boot()
        check(login(OLD)[0] == 401 and login(NEW)[0] == 200, 'restart preserves DB password over stale config')
        stop()
        write_config('')
        boot()
        check(login(NEW)[0] == 200, 'empty bootstrap config preserves existing user')
        stop()
        write_config(NEW)
        boot()
        status, pair = login(NEW)
        access = pair['access_token']
        status, enrolled = request('auth/totp/enroll', {}, access)
        check(status == 200 and enrolled.get('secret'), 'bootstrap admin TOTP enrollment')
        secret = enrolled['secret']
        digest = hmac.new(base64.b32decode(secret + '=' * ((-len(secret)) % 8)),
                          struct.pack('>Q', int(time.time()) // 30), hashlib.sha1).digest()
        offset = digest[-1] & 15
        otp = str((struct.unpack('>I', digest[offset:offset + 4])[0] & 0x7fffffff) % 1000000).zfill(6)
        check(request('auth/totp/verify', {'code': otp}, access)[0] == 200, 'bootstrap admin TOTP confirmed')
        status, pending = login(NEW)
        check(status == 200 and pending.get('totp_required') and not pending.get('access_token'),
              'matching config credentials still require TOTP')
        check(request('auth/password', {'old_password': NEW, 'new_password': OLD},
                      pending['pending_token'])[0] == 401, 'TOTP pending token cannot change password')

        stop()
        sql('DELETE FROM user_totp')
        write_config(NEW, 'admin')
        boot()
        status, pending = login(NEW)
        check(status == 200 and pending.get('totp_enroll_required') and not pending.get('access_token'),
              'matching config credentials respect mandatory TOTP enrollment')

        sql('DELETE FROM users WHERE username="admin"')
        status, pair = login(NEW)
        check(status == 200 and pair.get('access_token'), 'SEC-2 absent-user bootstrap recovery preserved')
        sql('ALTER TABLE users RENAME TO fixture_users_unavailable')
        check(login(NEW)[0] == 401, 'SEC-2 unknown DB state denies bootstrap fallback')
    except Exception:
        log.flush()
        log.seek(0)
        lines = log.read().splitlines()
        print('\n'.join(lines[:22] + lines[-18:]))
        raise
    finally:
        stop()
        log.close()


def prepare_namespace(root, daemon, host_net):



    check(os.readlink('/proc/self/ns/net') != host_net, 'namespace established before mounts')
    subprocess.run(['mount', '-t', 'tmpfs', 'tmpfs', str(root)], check=True)
    for relative in ['usr', 'etc', 'proc', 'dev', 'sys', 'run', 'tmp', 'var/lib/purecvisor',
                     'var/log', 'work', 'home', 'root']:
        (root / relative).mkdir(parents=True, exist_ok=True)

    def bind_readonly(source, relative):
        target = root / relative
        subprocess.run(['mount', '--bind', str(source), str(target)], check=True)
        subprocess.run(['mount', '-o', 'remount,bind,ro', str(target)], check=True)

    bind_readonly('/usr', 'usr')
    bind_readonly('/etc', 'etc')
    bind_readonly(Path(__file__).resolve().parents[2], 'work')
    (root / 'fixture-daemon').touch()
    bind_readonly(daemon, 'fixture-daemon')
    subprocess.run(['mount', '-t', 'tmpfs', 'tmpfs', str(root / 'etc/purecvisor')], check=True)
    for relative in ['bin', 'sbin', 'lib', 'lib64']:
        (root / relative).symlink_to('usr/' + relative)
    import stat
    for name, minor in [('null', 3), ('zero', 5), ('random', 8), ('urandom', 9)]:
        os.mknod(root / 'dev' / name, stat.S_IFCHR | 0o666, os.makedev(1, minor))
    (root / 'dev/fd').symlink_to('/proc/self/fd')
    subprocess.run(['mount', '-t', 'proc', 'proc', str(root / 'proc')], check=True)
    subprocess.run(['ip', 'link', 'set', 'lo', 'up'], check=True)
    os.chroot(root)
    os.chdir('/')
    os.execv('/usr/bin/python3', ['python3', '/work/tests/integration/test_admin_password_rotation.py',
                                '--inside', '--daemon', '/fixture-daemon', '--host-net', host_net])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--daemon', type=Path, default=Path(__file__).resolve().parents[2] / 'bin/purecvisorsd')
    parser.add_argument('--inside', action='store_true')
    parser.add_argument('--prepare', type=Path)
    parser.add_argument('--host-net')
    args = parser.parse_args()
    if args.inside:
        run_inside(str(args.daemon), args.host_net)
        return 0
    if os.geteuid() != 0:
        parser.error('root is required only to establish isolated namespaces; run with sudo')
    if args.prepare:
        prepare_namespace(args.prepare, args.daemon, args.host_net)
        return 0
    with tempfile.TemporaryDirectory(prefix='pcv-password-http-') as temporary:
        command = ['unshare', '--mount', '--net', '--pid', '--fork', '--ipc', '--uts',
                   '--propagation', 'private', '/usr/bin/python3', str(Path(__file__).resolve()),
                   '--prepare', temporary, '--daemon', str(args.daemon.resolve()),
                   '--host-net', os.readlink('/proc/self/ns/net')]
        return subprocess.run(command, check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
