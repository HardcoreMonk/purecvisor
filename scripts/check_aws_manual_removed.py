#!/usr/bin/env python3





from pathlib import Path
import argparse
import re
import subprocess

ROOT = Path(__file__).resolve().parent.parent
RETIRED = (
    'vm.import.ec2', 'vm.export.ec2', 'vm.import.status', 'vm.export.status',
    'cloud.import', 'cloud.export', 'cloud.jobs.list', 'cloud.job.cancel',
    'jobs.persist.list', 'pcv_cloud_', 'pcv_aws_', 'pcv_disk_convert_raw_to_qcow2',
    'backup.export_s3', 'pcv_backup_export_s3', 'S3ExportCtx', '_s3_export_worker',
    '_s3_upload_', '_s3_build_env', 's3_access_key', 's3_secret_key',
)


def check(root: Path, cli: str | None = None) -> list[str]:
    errors = []
    for name in ('cloud_migration.c', 'cloud_migration.h', 'aws_client.c', 'disk_converter.c'):
        if (root / 'src/modules/cloud' / name).exists():
            errors.append('전용 소스 잔류: ' + name)
    for file in (root / 'src').rglob('*'):
        if file.suffix not in ('.c', '.h'):
            continue
        text = file.read_text()
        for token in RETIRED:
            if token in text:
                errors.append(f'{file.relative_to(root)}: 폐기 계약 {token}')
    for name in ('Makefile', 'ui/modules/endpoints.js', 'ui/modules/shell.js',
                 'ui/modules/nav.js', 'ui/modules/help.js', 'ui/modules/advanced.js', 'ui/app.js', 'ui/i18n.js',
                 'scripts/gen_openapi.py', 'ui/manifest.json', 'ui/guide-content.md', 'docs/GUIDE.md'):
        text = (root / name).read_text()
        for token in (*RETIRED, 'modules/cloud.js', 'modules/cloud/', 'CLOUD_IMPORT',
                      'CLOUD_EXPORT', 'CLOUD_CANCEL', 'CLOUD_JOBS', 'cloud-migration',
                      'cloud_migration', 'cloud migration', 'renderCloudMigration', 'cmDoImport', 'cmDoExport',
                      'JOBS_PERSIST', 'renderPersistentJobs', '/import-ec2', '/export-ec2', 'tests/cloud_legacy/',
                      '/import-status', '/export-status', '/cloud/jobs', '/cloud/cancel', '/jobs/persistent',
                      'export-s3'):
            if token in text:
                errors.append(f'{name}: 폐기 표면 {token}')
    if (root / 'ui/modules/cloud.js').exists():
        errors.append('Cloud renderer 잔류')
    dispatcher = (root / 'src/api/dispatcher.c').read_text()
    for method in ('vm.import.ova', 'vm.export.ova', 'backup.restore',
                   'backup.incremental', 'backup.verify', 'backup.policy.set', 'backup.policy.list',
                   'jobs.list', 'jobs.get', 'jobs.cancel', 'vm.list'):
        if not re.search(r'g_hash_table_insert\(g_rpc_routes,\s*"' + re.escape(method) + '"', dispatcher):
            errors.append('보존 RPC 누락: ' + method)
    backup = (root / 'src/modules/backup/backup_scheduler.c').read_text()
    for symbol in ('pcv_backup_restore', 'pcv_backup_incremental', 'pcv_backup_verify', '_prune_snapshots_by_prefix'):
        if symbol not in backup:
            errors.append('로컬 백업/리텐션 구현 누락: ' + symbol)
    if '"aws"' in backup:
        errors.append('백업 엔진의 AWS CLI 실행 잔류')
    profile = (root / 'packaging/apparmor/usr.local.bin.purecvisorsd').read_text()
    if re.search(r'^\s*[^#\n]*bin/aws\s', profile, re.MULTILINE):
        errors.append('AppArmor의 AWS CLI 실행 허용 잔류')
    rest = (root / 'src/api/rest_server.c').read_text()
    if any(token in rest for token in ('import-ec2', 'export-ec2', 'export-s3')):
        errors.append('폐기 AWS REST 경로 잔류')
    if cli:
        for action in ('import', 'export', 'status', 'jobs', 'cancel', 'finalize'):
            result = subprocess.run([cli, '--no-color', 'cloud', action], text=True,
                                    capture_output=True, timeout=10)
            if result.returncode == 0 or 'LINK_SEVERED' in result.stdout + result.stderr:
                errors.append('CLI가 폐기 명령을 로컬에서 거절하지 않음: ' + action)

        result = subprocess.run([cli, '--no-color', 'backup', 'export-s3'], text=True,
                                capture_output=True, timeout=10)
        if result.returncode == 0 or 'LINK_SEVERED' in result.stdout + result.stderr:
            errors.append('CLI가 폐기 S3 명령을 로컬에서 거절하지 않음')
        result = subprocess.run([cli, '--no-color', 'help', 'all'], text=True,
                                capture_output=True, timeout=10)
        if result.returncode or re.search(r'\bcloud\s+(import|export|jobs|status|cancel|finalize)\b', result.stdout):
            errors.append('CLI 전체 도움말 계약 실패')
        if 'export-s3' in result.stdout:
            errors.append('CLI 전체 도움말에 폐기 S3 명령 잔류')
        for action in ('import-ova', 'export-ova', 'backup restore', 'backup incremental', 'backup verify'):
            if action not in result.stdout:
                errors.append('CLI 보존 도움말 누락: ' + action)
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--cli')
    args = parser.parse_args()
    errors = check(args.root, args.cli)
    for error in errors:
        print('FAIL:', error)
    if not errors:
        print('PASS: AWS 수동 이관·S3 제거 및 VM/OVA/로컬 백업/일반 Job 보존 계약')
    return int(bool(errors))


if __name__ == '__main__':
    raise SystemExit(main())
