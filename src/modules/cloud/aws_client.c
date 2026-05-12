










#include "cloud_migration.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_config.h"

#include <json-glib/json-glib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

















#define AWS_LOG "aws_client"





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




gboolean
pcv_aws_s3_download(const gchar *s3_bucket, const gchar *s3_key,
                      const gchar *local_dir, const gchar *region,
                      GError **error)
{
    gchar *s3_uri = g_strdup_printf("s3://%s/%s", s3_bucket, s3_key);
    gchar *filename = g_path_get_basename(s3_key);
    gchar *local_path = g_strdup_printf("%s/%s", local_dir, filename);


    g_mkdir_with_parents(local_dir, 0755);

    const gchar *argv[] = {
        "aws", "s3", "cp", s3_uri, local_path,
        "--region", region,
        "--no-progress",
        NULL
    };

    PCV_LOG_INFO(AWS_LOG, "S3 download: %s → %s", s3_uri, local_path);

    gchar *out = NULL, *err_out = NULL;

    gchar **wrapped = _wrap_with_timeout(argv, "3600");
    gboolean ok = pcv_spawn_sync((const gchar * const *)wrapped, &out, &err_out, error);
    g_strfreev(wrapped);
    if (!ok) {
        PCV_LOG_WARN(AWS_LOG, "S3 download failed: %s",
                     err_out ? err_out : "unknown");


        if (access(local_path, F_OK) == 0) {
            unlink(local_path);
        }
    }
    g_free(out); g_free(err_out);
    g_free(s3_uri); g_free(filename); g_free(local_path);
    return ok;
}




gboolean
pcv_aws_s3_upload(const gchar *local_path, const gchar *s3_bucket,
                    const gchar *s3_prefix, const gchar *region,
                    GError **error)
{
    gchar *filename = g_path_get_basename(local_path);
    gchar *s3_uri = g_strdup_printf("s3://%s/%s%s", s3_bucket,
                                      s3_prefix ? s3_prefix : "", filename);


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




gboolean
pcv_aws_wait_snapshot(const gchar *snapshot_id, const gchar *region, GError **error)
{
    const gchar *argv[] = {
        "aws", "ec2", "wait", "snapshot-completed",
        "--snapshot-ids", snapshot_id,
        "--region", region,
        NULL
    };

    gchar **wrapped = _wrap_with_timeout(argv, "1800");
    gboolean ok = pcv_spawn_sync((const gchar * const *)wrapped, NULL, NULL, error);
    g_strfreev(wrapped);
    return ok;
}
