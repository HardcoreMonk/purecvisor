/**
 * @file aws_client.c
 * @brief AWS CLI 래핑 — pcv_spawn_sync argv 패턴으로 command injection 방지
 *
 * [설계 원칙]
 *   - 모든 AWS 호출은 pcv_spawn_sync(argv[]) 경유 (system/popen 금지)
 *   - JSON 출력 파싱: json-glib (기존 의존성)
 *   - STS AssumeRole 지원 (선택적)
 *   - 에러 시 GError 반환, stdout/stderr 분리
 */

#include "cloud_migration.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_config.h"

#include <json-glib/json-glib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/*
 * ============================================================================
 *  [주니어 개발자 필독] AWS CLI 래퍼 핵심 개념
 * ============================================================================
 *
 *  이 모듈은 AWS CLI를 pcv_spawn_sync(argv[]) 배열 방식으로 호출합니다.
 *  system()이나 popen()을 사용하면 셸 인젝션 공격에 취약하지만,
 *  argv[] 배열은 셸 해석 없이 직접 exec하므로 안전합니다.
 *
 *  AWS CLI가 --output json 옵션으로 JSON을 반환하면 json-glib로 파싱합니다.
 *
 *  모든 AWS 호출은 동기(blocking)이며, GTask 워커 스레드에서 실행됩니다.
 *  메인 이벤트 루프는 블로킹되지 않습니다.
 * ============================================================================
 */

#define AWS_LOG "aws_client"

/* B2-WARN6: AWS CLI 호출 무제한 대기 방지.
 * Why: pcv_spawn_sync는 timeout이 없어 aws CLI가 멈추면 워커 스레드가 영구 블록.
 * How: coreutils `timeout` 유틸로 래핑한다. 빠른 메타 호출은 60s, 폴링/조회는 120s.
 *      장기 작업(export-image/import-image 자체)은 폴링 루프에서 처리하므로 여기는 호출 1회의 시간만 보장. */
#define AWS_DEFAULT_TIMEOUT_SEC "120"

static gchar **
_wrap_with_timeout(const gchar * const *argv, const gchar *timeout_sec)
{
    gsize n = 0;
    while (argv[n]) n++;
    gchar **wrapped = g_new0(gchar *, n + 4);
    wrapped[0] = g_strdup("timeout");
    wrapped[1] = g_strdup("--kill-after=10");
    wrapped[2] = g_strdup(timeout_sec ? timeout_sec : AWS_DEFAULT_TIMEOUT_SEC);
    for (gsize i = 0; i < n; i++) wrapped[3 + i] = g_strdup(argv[i]);
    wrapped[3 + n] = NULL;
    return wrapped;
}

static gboolean
_aws_spawn(const gchar * const *argv, gchar **out, gchar **err_out, GError **error)
{
    gchar **wrapped = _wrap_with_timeout(argv, NULL);
    gboolean ok = pcv_spawn_sync((const gchar * const *)wrapped, out, err_out, error);
    g_strfreev(wrapped);
    return ok;
}

/* ══════════════════════════════════════════════════════════════
 * 내부 유틸: AWS CLI 실행 + JSON 파싱
 * ══════════════════════════════════════════════════════════════ */
static JsonObject *
_aws_run_json(const gchar * const *argv, GError **error)
{
    gchar *out = NULL, *err_out = NULL;
    if (!_aws_spawn(argv, &out, &err_out, error)) {
        PCV_LOG_WARN(AWS_LOG, "AWS CLI failed: %s",
                     err_out ? err_out : (error && *error ? (*error)->message : "unknown"));
        g_free(out); g_free(err_out);
        return NULL;
    }
    g_free(err_out);

    if (!out || !*out) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "AWS CLI returned empty output");
        g_free(out);
        return NULL;
    }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, out, -1, error)) {
        PCV_LOG_WARN(AWS_LOG, "JSON parse failed: %.100s", out);
        g_object_unref(parser);
        g_free(out);
        return NULL;
    }

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = NULL;
    if (root && JSON_NODE_HOLDS_OBJECT(root)) {
        obj = json_object_ref(json_node_get_object(root));
    }
    g_object_unref(parser);
    g_free(out);

    if (!obj) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "AWS CLI response is not JSON object");
    }
    return obj;
}

/* ══════════════════════════════════════════════════════════════
 * AWS 인증 확인
 * ══════════════════════════════════════════════════════════════ */
gboolean
pcv_aws_check_credentials(const gchar *region, GError **error)
{
    const gchar *r = region ? region :
        pcv_config_get_string("aws", "default_region", "ap-northeast-2");

    const gchar *argv[] = {
        "aws", "sts", "get-caller-identity",
        "--region", r,
        "--output", "json",
        NULL
    };

    JsonObject *obj = _aws_run_json(argv, error);
    if (!obj) return FALSE;

    const gchar *account = json_object_has_member(obj, "Account")
        ? json_object_get_string_member(obj, "Account") : NULL;
    PCV_LOG_INFO(AWS_LOG, "AWS credentials valid — Account: %s", account ?: "unknown");
    json_object_unref(obj);
    return TRUE;
}

/* ══════════════════════════════════════════════════════════════
 * EC2 이미지 내보내기 (AMI → S3 RAW)
 * ══════════════════════════════════════════════════════════════ */
gchar *
pcv_aws_export_image(const gchar *ami_id, const gchar *s3_bucket,
                       const gchar *region, GError **error)
{
    gchar *s3_loc = g_strdup_printf("S3Bucket=%s,S3Prefix=pcv-export/", s3_bucket);

    const gchar *argv[] = {
        "aws", "ec2", "export-image",
        "--image-id", ami_id,
        "--disk-image-format", "RAW",
        "--s3-export-location", s3_loc,
        "--region", region,
        "--output", "json",
        NULL
    };

    JsonObject *obj = _aws_run_json(argv, error);
    g_free(s3_loc);
    if (!obj) return NULL;

    gchar *task_id = NULL;
    if (json_object_has_member(obj, "ExportImageTaskId")) {
        task_id = g_strdup(json_object_get_string_member(obj, "ExportImageTaskId"));
        PCV_LOG_INFO(AWS_LOG, "Export started: %s → %s (task: %s)",
                     ami_id, s3_bucket, task_id);
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "No ExportImageTaskId in response");
    }
    json_object_unref(obj);
    return task_id;
}

/* ══════════════════════════════════════════════════════════════
 * Export 진행 상태 폴링
 * 반환: 0~100 진행률, status 문자열
 * ══════════════════════════════════════════════════════════════ */
gint
pcv_aws_check_export_progress(const gchar *task_id, const gchar *region,
                                gchar **out_status, gchar **out_s3_key,
                                GError **error)
{
    const gchar *argv[] = {
        "aws", "ec2", "describe-export-image-tasks",
        "--export-image-task-ids", task_id,
        "--region", region,
        "--output", "json",
        NULL
    };

    JsonObject *obj = _aws_run_json(argv, error);
    if (!obj) return -1;

    gint progress = 0;
    if (json_object_has_member(obj, "ExportImageTasks")) {
        JsonArray *tasks = json_object_get_array_member(obj, "ExportImageTasks");
        if (json_array_get_length(tasks) > 0) {
            JsonObject *task = json_array_get_object_element(tasks, 0);
            const gchar *status = json_object_has_member(task, "Status")
                ? json_object_get_string_member(task, "Status") : "unknown";
            if (out_status) *out_status = g_strdup(status);

            if (json_object_has_member(task, "Progress"))
                progress = (gint)json_object_get_int_member(task, "Progress");

            /* S3 키 추출 */
            if (out_s3_key && json_object_has_member(task, "S3ExportLocation")) {
                JsonObject *s3 = json_object_get_object_member(task, "S3ExportLocation");
                if (json_object_has_member(s3, "S3Key"))
                    *out_s3_key = g_strdup(json_object_get_string_member(s3, "S3Key"));
            }

            if (g_strcmp0(status, "completed") == 0) progress = 100;
        }
    }
    json_object_unref(obj);
    return progress;
}

/* ══════════════════════════════════════════════════════════════
 * S3 다운로드
 * ══════════════════════════════════════════════════════════════ */
gboolean
pcv_aws_s3_download(const gchar *s3_bucket, const gchar *s3_key,
                      const gchar *local_dir, const gchar *region,
                      GError **error)
{
    gchar *s3_uri = g_strdup_printf("s3://%s/%s", s3_bucket, s3_key);
    gchar *filename = g_path_get_basename(s3_key);
    gchar *local_path = g_strdup_printf("%s/%s", local_dir, filename);

    /* 디렉터리 생성 */
    g_mkdir_with_parents(local_dir, 0755);

    const gchar *argv[] = {
        "aws", "s3", "cp", s3_uri, local_path,
        "--region", region,
        "--no-progress",
        NULL
    };

    PCV_LOG_INFO(AWS_LOG, "S3 download: %s → %s", s3_uri, local_path);

    gchar *out = NULL, *err_out = NULL;
    /* S3 다운로드는 큰 파일이 가능하므로 1시간 허용 */
    gchar **wrapped = _wrap_with_timeout(argv, "3600");
    gboolean ok = pcv_spawn_sync((const gchar * const *)wrapped, &out, &err_out, error);
    g_strfreev(wrapped);
    if (!ok) {
        PCV_LOG_WARN(AWS_LOG, "S3 download failed: %s",
                     err_out ? err_out : "unknown");
        /* B2-WARN2: aws s3 cp는 실패해도 부분 다운로드 파일을 남기는 경우가 있다.
         * 재시도 시 stale 데이터를 사용하지 않도록 정리한다. */
        if (access(local_path, F_OK) == 0) {
            unlink(local_path);
        }
    }
    g_free(out); g_free(err_out);
    g_free(s3_uri); g_free(filename); g_free(local_path);
    return ok;
}

/* ══════════════════════════════════════════════════════════════
 * S3 업로드
 * ══════════════════════════════════════════════════════════════ */
gboolean
pcv_aws_s3_upload(const gchar *local_path, const gchar *s3_bucket,
                    const gchar *s3_prefix, const gchar *region,
                    GError **error)
{
    gchar *filename = g_path_get_basename(local_path);
    gchar *s3_uri = g_strdup_printf("s3://%s/%s%s", s3_bucket,
                                      s3_prefix ? s3_prefix : "", filename);

    /* 파일 크기 확인 → expected-size */
    struct stat st;
    gchar *size_str = NULL;
    if (stat(local_path, &st) == 0) {
        size_str = g_strdup_printf("%" G_GINT64_FORMAT, (gint64)st.st_size);
    }

    const gchar *argv_no_size[] = {
        "aws", "s3", "cp", local_path, s3_uri,
        "--region", region,
        "--no-progress",
        NULL
    };
    const gchar *argv_with_size[] = {
        "aws", "s3", "cp", local_path, s3_uri,
        "--region", region,
        "--expected-size", size_str ? size_str : "0",
        "--no-progress",
        NULL
    };

    PCV_LOG_INFO(AWS_LOG, "S3 upload: %s → %s", local_path, s3_uri);

    gchar *out = NULL, *err_out = NULL;
    gchar **wrapped = _wrap_with_timeout(
        size_str ? argv_with_size : argv_no_size, "3600");
    gboolean ok = pcv_spawn_sync((const gchar * const *)wrapped,
                                   &out, &err_out, error);
    g_strfreev(wrapped);
    if (!ok) {
        PCV_LOG_WARN(AWS_LOG, "S3 upload failed: %s",
                     err_out ? err_out : "unknown");
    }
    g_free(out); g_free(err_out);
    g_free(filename); g_free(s3_uri); g_free(size_str);
    return ok;
}

/* ══════════════════════════════════════════════════════════════
 * EC2 이미지 가져오기 (S3 RAW → AMI)
 * ══════════════════════════════════════════════════════════════ */
gchar *
pcv_aws_import_image(const gchar *s3_bucket, const gchar *s3_key,
                       const gchar *region, const gchar *description,
                       GError **error)
{
    gchar *s3_url = g_strdup_printf("s3://%s/%s", s3_bucket, s3_key);
    gchar *disk_container = g_strdup_printf(
        "Format=RAW,UserBucket={S3Bucket=%s,S3Key=%s}", s3_bucket, s3_key);

    const gchar *argv[] = {
        "aws", "ec2", "import-image",
        "--disk-containers", disk_container,
        "--description", description ? description : "PureCVisor export",
        "--region", region,
        "--output", "json",
        NULL
    };

    JsonObject *obj = _aws_run_json(argv, error);
    g_free(s3_url); g_free(disk_container);
    if (!obj) return NULL;

    gchar *task_id = NULL;
    if (json_object_has_member(obj, "ImportTaskId")) {
        task_id = g_strdup(json_object_get_string_member(obj, "ImportTaskId"));
        PCV_LOG_INFO(AWS_LOG, "Import-image started: %s (task: %s)",
                     s3_key, task_id);
    }
    json_object_unref(obj);
    return task_id;
}

/* ══════════════════════════════════════════════════════════════
 * Import-image 진행 상태 폴링
 * ══════════════════════════════════════════════════════════════ */
gint
pcv_aws_check_import_progress(const gchar *task_id, const gchar *region,
                                gchar **out_status, gchar **out_ami_id,
                                GError **error)
{
    const gchar *argv[] = {
        "aws", "ec2", "describe-import-image-tasks",
        "--import-task-ids", task_id,
        "--region", region,
        "--output", "json",
        NULL
    };

    JsonObject *obj = _aws_run_json(argv, error);
    if (!obj) return -1;

    gint progress = 0;
    if (json_object_has_member(obj, "ImportImageTasks")) {
        JsonArray *tasks = json_object_get_array_member(obj, "ImportImageTasks");
        if (json_array_get_length(tasks) > 0) {
            JsonObject *task = json_array_get_object_element(tasks, 0);
            const gchar *status = json_object_has_member(task, "Status")
                ? json_object_get_string_member(task, "Status") : "unknown";
            if (out_status) *out_status = g_strdup(status);

            if (json_object_has_member(task, "Progress")) {
                const gchar *prog_str = json_object_get_string_member(task, "Progress");
                if (prog_str) progress = atoi(prog_str);
            }

            if (out_ami_id && json_object_has_member(task, "ImageId"))
                *out_ami_id = g_strdup(json_object_get_string_member(task, "ImageId"));

            if (g_strcmp0(status, "completed") == 0) progress = 100;
        }
    }
    json_object_unref(obj);
    return progress;
}

/* ══════════════════════════════════════════════════════════════
 * Near-Live Import: EC2 인스턴스 중지
 * ══════════════════════════════════════════════════════════════ */
gboolean
pcv_aws_stop_instance(const gchar *instance_id, const gchar *region, GError **error)
{
    const gchar *argv[] = {
        "aws", "ec2", "stop-instances",
        "--instance-ids", instance_id,
        "--region", region,
        "--output", "json",
        NULL
    };
    gchar *out = NULL;
    if (!_aws_spawn(argv, &out, NULL, error)) {
        g_free(out);
        return FALSE;
    }
    g_free(out);
    /* Wait for stopped state — aws wait의 기본 타임아웃은 600s, 안전 한도 900s */
    const gchar *wait_argv[] = {
        "aws", "ec2", "wait", "instance-stopped",
        "--instance-ids", instance_id,
        "--region", region,
        NULL
    };
    gchar **wrapped = _wrap_with_timeout(wait_argv, "900");
    gboolean ok = pcv_spawn_sync((const gchar * const *)wrapped, NULL, NULL, error);
    g_strfreev(wrapped);
    return ok;
}

/* ══════════════════════════════════════════════════════════════
 * Near-Live Import: EBS 스냅샷 생성
 * ══════════════════════════════════════════════════════════════ */
gchar *
pcv_aws_create_snapshot(const gchar *volume_id, const gchar *description,
                          const gchar *region, GError **error)
{
    const gchar *argv[] = {
        "aws", "ec2", "create-snapshot",
        "--volume-id", volume_id,
        "--description", description ? description : "PureCVisor near-live delta",
        "--region", region,
        "--output", "json",
        NULL
    };
    gchar *out = NULL;
    if (!_aws_spawn(argv, &out, NULL, error)) {
        g_free(out);
        return NULL;
    }
    /* Parse SnapshotId from JSON response */
    gchar *snap_id = NULL;
    JsonParser *p = json_parser_new();
    if (json_parser_load_from_data(p, out, -1, NULL)) {
        JsonObject *obj = json_node_get_object(json_parser_get_root(p));
        if (json_object_has_member(obj, "SnapshotId"))
            snap_id = g_strdup(json_object_get_string_member(obj, "SnapshotId"));
    }
    g_object_unref(p);
    g_free(out);
    if (!snap_id) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to parse SnapshotId");
    }
    return snap_id;
}

/* ══════════════════════════════════════════════════════════════
 * Near-Live Import: 스냅샷 완료 대기
 * ══════════════════════════════════════════════════════════════ */
gboolean
pcv_aws_wait_snapshot(const gchar *snapshot_id, const gchar *region, GError **error)
{
    const gchar *argv[] = {
        "aws", "ec2", "wait", "snapshot-completed",
        "--snapshot-ids", snapshot_id,
        "--region", region,
        NULL
    };
    /* aws wait 기본 600s, 안전 한도 1800s */
    gchar **wrapped = _wrap_with_timeout(argv, "1800");
    gboolean ok = pcv_spawn_sync((const gchar * const *)wrapped, NULL, NULL, error);
    g_strfreev(wrapped);
    return ok;
}
