/**
 * @file vm_clone_plan.c
 * @brief VM clone preflight에서 쓰는 libvirt XML disk resolver.
 *
 * 비전공자 기준으로 보면, clone은 "복사할 하드디스크가 정확히 무엇인지"를
 * 먼저 알아야 한다. 이 파일은 VM 정의 XML에서 설치 ISO가 아닌 data disk만
 * 세고, 그 disk가 ZFS zvol인지 qcow2 파일인지 raw 파일인지 분류한다.
 * 이후 storage별로 "새 VM이 사용할 target disk 경로"까지 계산한다.
 */

#include "vm_clone_plan.h"

#include "../../utils/pcv_log.h"
#include "../../utils/pcv_spawn.h"

#include <gio/gio.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <glib/gstdio.h>
#include <string.h>

const gchar *
pcv_vm_clone_disk_kind_to_string(PcvVmCloneDiskKind kind)
{
    switch (kind) {
    case PCV_VM_CLONE_DISK_ZVOL:
        return "zvol";
    case PCV_VM_CLONE_DISK_QCOW2:
        return "qcow2";
    case PCV_VM_CLONE_DISK_RAW:
        return "raw";
    case PCV_VM_CLONE_DISK_UNSUPPORTED:
    default:
        return "unsupported";
    }
}

void
pcv_vm_clone_disk_info_clear(PcvVmCloneDiskInfo *info)
{
    if (!info)
        return;
    g_free(info->source_attr);
    g_free(info->source_path);
    g_free(info->driver_type);
    memset(info, 0, sizeof(*info));
}

void
pcv_vm_clone_disk_plan_clear(PcvVmCloneDiskPlan *plan)
{
    if (!plan)
        return;
    g_free(plan->source_disk_path);
    g_free(plan->target_disk_path);
    g_free(plan->source_dataset);
    g_free(plan->target_dataset);
    g_free(plan->zfs_pool);
    g_free(plan->source_zvol_name);
    memset(plan, 0, sizeof(*plan));
}

static gboolean
xml_node_name_is(xmlNodePtr node, const gchar *name)
{
    return node && node->type == XML_ELEMENT_NODE && node->name &&
           xmlStrcmp(node->name, BAD_CAST name) == 0;
}

static gchar *
xml_node_prop_dup(xmlNodePtr node, const gchar *name)
{
    xmlChar *prop = xmlGetProp(node, BAD_CAST name);
    if (!prop)
        return NULL;
    gchar *dup = g_strdup((const gchar *)prop);
    xmlFree(prop);
    return dup;
}

/* libvirt domain XML 을 깊이우선으로 훑어 data disk 만 센다.
 * device="disk" 만 카운트하므로 설치 ISO/cdrom(device="cdrom")은 제외된다 —
 * clone 은 OS 데이터 디스크만 복제 대상이다. source_path 는 "처음 만난" data
 * disk 것만 채우고(이후 disk 는 개수만 증가), 개수 검증은 호출자
 * pcv_vm_clone_extract_disk_info 가 disk_count!=1 로 처리한다. */
static void
scan_clone_disk_nodes(xmlNodePtr node, PcvVmCloneDiskInfo *info)
{
    for (xmlNodePtr cur = node; cur; cur = cur->next) {
        if (xml_node_name_is(cur, "disk")) {
            gchar *device = xml_node_prop_dup(cur, "device");
            gboolean is_data_disk = (g_strcmp0(device, "disk") == 0);
            g_free(device);

            if (is_data_disk) {
                info->disk_count++;
                if (!info->source_path) {
                    /* dev(블록 장치, zvol) 우선, 없으면 file(qcow2/raw). 하나를
                     * 잡으면 즉시 break — 첫 <source> 가 이 disk 의 백킹이다. */
                    for (xmlNodePtr child = cur->children; child; child = child->next) {
                        if (xml_node_name_is(child, "driver")) {
                            g_free(info->driver_type);
                            info->driver_type = xml_node_prop_dup(child, "type");
                        } else if (xml_node_name_is(child, "source")) {
                            gchar *dev = xml_node_prop_dup(child, "dev");
                            gchar *file = xml_node_prop_dup(child, "file");
                            if (dev) {
                                info->source_attr = g_strdup("dev");
                                info->source_path = dev;
                                g_free(file);
                                break;
                            }
                            if (file) {
                                info->source_attr = g_strdup("file");
                                info->source_path = file;
                                break;
                            }
                        }
                    }
                }
            }
        }

        scan_clone_disk_nodes(cur->children, info);
    }
}

/* 백킹 종류 판정: source 속성 + 경로/드라이버 타입으로 zvol/qcow2/raw 를 가른다.
 *   - dev + /dev/zvol/ 접두 → ZVOL(ZFS 블록 복제 경로).
 *   - file + (driver type qcow2 | .qcow2 접미) → QCOW2.
 *   - file + (driver type raw | .raw/.img 접미) → RAW.
 * 어디에도 안 맞으면 UNSUPPORTED — beta guard 가 여기서 clone 을 거부한다.
 * driver type 을 확장자보다 우선 신뢰하되, type 미기재 XML 을 위해 확장자로 폴백. */
static PcvVmCloneDiskKind
classify_clone_disk(const PcvVmCloneDiskInfo *info)
{
    if (!info || !info->source_attr || !info->source_path)
        return PCV_VM_CLONE_DISK_UNSUPPORTED;

    if (g_strcmp0(info->source_attr, "dev") == 0 &&
        g_str_has_prefix(info->source_path, "/dev/zvol/"))
        return PCV_VM_CLONE_DISK_ZVOL;

    if (g_strcmp0(info->source_attr, "file") == 0) {
        if (g_strcmp0(info->driver_type, "qcow2") == 0 ||
            g_str_has_suffix(info->source_path, ".qcow2"))
            return PCV_VM_CLONE_DISK_QCOW2;
        if (g_strcmp0(info->driver_type, "raw") == 0 ||
            g_str_has_suffix(info->source_path, ".raw") ||
            g_str_has_suffix(info->source_path, ".img"))
            return PCV_VM_CLONE_DISK_RAW;
    }

    return PCV_VM_CLONE_DISK_UNSUPPORTED;
}

/* target_name 은 zvol dataset leaf / 파일명으로 그대로 합쳐진다. '/'·'\\' 를 막아
 * 경로 이탈(../, 절대경로 주입)과 다른 dataset/디렉터리로의 write 를 차단한다 —
 * 이 이름이 실제 디스크 생성 경로를 결정하므로 주입되면 임의 위치를 덮어쓸 수 있다. */
static gboolean
clone_target_name_is_safe(const gchar *target_name)
{
    return target_name && *target_name &&
           !strchr(target_name, '/') &&
           !strchr(target_name, '\\');
}

gboolean
pcv_vm_clone_extract_disk_info(const gchar *xml,
                               PcvVmCloneDiskInfo *info,
                               gchar **error_msg)
{
    if (!xml || !*xml || !info) {
        if (error_msg)
            *error_msg = g_strdup("Missing source VM XML");
        return FALSE;
    }

    memset(info, 0, sizeof(*info));
    xmlDocPtr doc = xmlReadMemory(xml, (int)strlen(xml), "pcv-vm-clone.xml",
                                  NULL, XML_PARSE_NONET | XML_PARSE_NOERROR |
                                                XML_PARSE_NOWARNING);
    if (!doc) {
        if (error_msg)
            *error_msg = g_strdup("Cannot parse source VM XML");
        return FALSE;
    }

    scan_clone_disk_nodes(xmlDocGetRootElement(doc), info);
    xmlFreeDoc(doc);

    /* beta guard: 정확히 1개의 data disk 만 지원한다. 다중 디스크 VM 은 어느
     * 디스크를 복제/재초기화할지 모호하고 부분 복제로 부팅 불능 VM 을 만들 수
     * 있어 지금은 명시적으로 거부한다(0개=복제 대상 없음, 2개+=모호). 실패 시
     * info 는 호출자가 pcv_vm_clone_disk_info_clear 로 해제한다. */
    info->kind = classify_clone_disk(info);
    if (info->disk_count != 1) {
        if (error_msg) {
            *error_msg = g_strdup_printf(
                "vm.clone beta guard: exactly one data disk is supported, found %u",
                info->disk_count);
        }
        return FALSE;
    }
    if (!info->source_path || info->kind == PCV_VM_CLONE_DISK_UNSUPPORTED) {
        if (error_msg)
            *error_msg = g_strdup("vm.clone beta guard: unsupported disk source");
        return FALSE;
    }
    return TRUE;
}

static gboolean
zvol_dataset_split(const gchar *dataset, gchar **pool_out, gchar **leaf_out)
{
    if (!dataset || !*dataset || !pool_out || !leaf_out)
        return FALSE;

    const gchar *slash = strrchr(dataset, '/');
    if (!slash || slash == dataset || !slash[1])
        return FALSE;

    *pool_out = g_strndup(dataset, (gsize)(slash - dataset));
    *leaf_out = g_strdup(slash + 1);
    return TRUE;
}

/* zvol clone 계획: source `/dev/zvol/<pool>/<name>` 에서 pool 을 뽑아
 * target dataset `<pool>/<target_name>` 와 그 예상 device 노드 경로를 계산한다.
 * "unchecked" = target_name 안전성은 상위 pcv_vm_clone_build_disk_plan 이 이미 검증.
 *
 * Developer note:
 *   여기서 만드는 target_disk_path 는 아직 존재하지 않는 "예상" 경로 문자열일 뿐이다.
 *   실제 zvol 생성과 /dev/zvol 노드 등장은 이 모듈이 아니라 실행 워커의 책임이며,
 *   udev 가 노드를 비동기로 만들기 때문에 zvol 생성 직후 이 경로가 곧바로 존재한다고
 *   가정하면 안 된다(open 전 노드 settle 대기는 실행 경로에서 처리). */
static gboolean
build_zvol_plan_unchecked(const gchar *target_name,
                          const PcvVmCloneDiskInfo *disk,
                          PcvVmCloneDiskPlan *plan,
                          gchar **error_msg)
{
    const gchar *dataset = disk->source_path + strlen("/dev/zvol/");
    gchar *pool = NULL;
    gchar *leaf = NULL;
    if (!zvol_dataset_split(dataset, &pool, &leaf)) {
        if (error_msg)
            *error_msg = g_strdup("vm.clone beta guard: invalid zvol dataset path");
        return FALSE;
    }

    plan->source_disk_path = g_strdup(disk->source_path);
    plan->source_dataset = g_strdup(dataset);
    plan->zfs_pool = pool;
    plan->source_zvol_name = leaf;
    plan->target_dataset = g_strdup_printf("%s/%s", plan->zfs_pool, target_name);
    plan->target_disk_path = g_strdup_printf("/dev/zvol/%s", plan->target_dataset);
    return TRUE;
}

static const gchar *
file_disk_target_extension(const PcvVmCloneDiskInfo *disk)
{
    if (!disk)
        return ".img";

    if (disk->kind == PCV_VM_CLONE_DISK_QCOW2)
        return ".qcow2";

    if (disk->source_path) {
        gchar *lower = g_ascii_strdown(disk->source_path, -1);
        gboolean has_raw = g_str_has_suffix(lower, ".raw");
        gboolean has_img = g_str_has_suffix(lower, ".img");
        g_free(lower);
        if (has_raw)
            return ".raw";
        if (has_img)
            return ".img";
    }
    return ".img";
}

static gboolean
build_file_plan_unchecked(const gchar *target_name,
                          const PcvVmCloneDiskInfo *disk,
                          PcvVmCloneDiskPlan *plan,
                          gchar **error_msg)
{
    /* file clone 계획: target 을 source 와 같은 디렉터리에 <target_name><ext> 로
     * 놓는다. 절대경로만 허용해(상대경로면 데몬 cwd 기준으로 엉뚱한 곳에 쓰일 수
     * 있다) 예측 가능한 위치에만 생성하고, target==source 는 거부해 원본 덮어쓰기를
     * 막는다. 확장자는 file_disk_target_extension 이 kind/원본 확장자에서 결정. */
    if (!g_path_is_absolute(disk->source_path)) {
        if (error_msg)
            *error_msg = g_strdup("vm.clone file disk plan: source path must be absolute");
        return FALSE;
    }

    gchar *dir = g_path_get_dirname(disk->source_path);
    if (!dir || g_strcmp0(dir, ".") == 0 || !g_path_is_absolute(dir)) {
        if (error_msg)
            *error_msg = g_strdup("vm.clone file disk plan: invalid source directory");
        g_free(dir);
        return FALSE;
    }

    const gchar *ext = file_disk_target_extension(disk);
    gchar *target_base = g_strdup_printf("%s%s", target_name, ext);
    gchar *target_path = g_build_filename(dir, target_base, NULL);
    g_free(target_base);
    g_free(dir);

    if (g_strcmp0(target_path, disk->source_path) == 0) {
        if (error_msg)
            *error_msg = g_strdup("vm.clone file disk plan: target must differ from source");
        g_free(target_path);
        return FALSE;
    }

    plan->source_disk_path = g_strdup(disk->source_path);
    plan->target_disk_path = target_path;
    return TRUE;
}

gboolean
pcv_vm_clone_build_disk_plan(const gchar *target_name,
                             const PcvVmCloneDiskInfo *disk,
                             PcvVmCloneDiskPlan *plan,
                             gchar **error_msg)
{
    if (!clone_target_name_is_safe(target_name) || !disk || !disk->source_path || !plan) {
        if (error_msg)
            *error_msg = g_strdup("Missing or invalid clone disk plan");
        return FALSE;
    }

    /* 재사용 안전: 넘겨받은 plan 을 먼저 clear 해 이전 계획의 문자열이 새 값에
     * 덮여 누수되지 않게 한 뒤, kind 별 빌더로 분기한다. */
    pcv_vm_clone_disk_plan_clear(plan);
    plan->kind = disk->kind;

    switch (disk->kind) {
    case PCV_VM_CLONE_DISK_ZVOL:
        return build_zvol_plan_unchecked(target_name, disk, plan, error_msg);
    case PCV_VM_CLONE_DISK_QCOW2:
    case PCV_VM_CLONE_DISK_RAW:
        return build_file_plan_unchecked(target_name, disk, plan, error_msg);
    case PCV_VM_CLONE_DISK_UNSUPPORTED:
    default:
        if (error_msg)
            *error_msg = g_strdup("vm.clone disk plan: unsupported disk source");
        return FALSE;
    }
}

static const gchar *
file_disk_qemu_img_format(PcvVmCloneDiskKind kind)
{
    switch (kind) {
    case PCV_VM_CLONE_DISK_QCOW2:
        return "qcow2";
    case PCV_VM_CLONE_DISK_RAW:
        return "raw";
    case PCV_VM_CLONE_DISK_ZVOL:
    case PCV_VM_CLONE_DISK_UNSUPPORTED:
    default:
        return NULL;
    }
}

static const gchar *
disk_guestfs_format(PcvVmCloneDiskKind kind)
{
    switch (kind) {
    case PCV_VM_CLONE_DISK_QCOW2:
        return "qcow2";
    case PCV_VM_CLONE_DISK_ZVOL:
    case PCV_VM_CLONE_DISK_RAW:
        return "raw";
    case PCV_VM_CLONE_DISK_UNSUPPORTED:
    default:
        return NULL;
    }
}

static gboolean
program_available(const gchar *name)
{
    gchar *path = g_find_program_in_path(name);
    if (!path)
        return FALSE;
    g_free(path);
    return TRUE;
}

/**
 * pcv_vm_clone_copy_file_disk:
 * @plan: qcow2/raw clone disk plan
 * @error: 실패 원인을 담는 GError
 *
 * qemu-img convert로 파일 기반 VM 디스크를 새 target file로 full copy한다.
 * 원본 파일을 공유하지 않는 것이 핵심 계약이다. 같은 경로, 상대 경로, 이미
 * 존재하는 target은 모두 거부한다.
 *
 * 파일 기반 clone 실행 경로에서 worker가 호출한다. guest identity reset은
 * 이 함수의 책임이 아니라, 복제된 target disk에 대해 별도 단계로 수행한다.
 */
gboolean
pcv_vm_clone_copy_file_disk(const PcvVmCloneDiskPlan *plan,
                            GError **error)
{
    if (!plan || !plan->source_disk_path || !plan->target_disk_path) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "vm.clone file copy: missing disk plan");
        return FALSE;
    }

    const gchar *format = file_disk_qemu_img_format(plan->kind);
    if (!format) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "vm.clone file copy: only qcow2/raw file disks are supported");
        return FALSE;
    }

    if (!g_path_is_absolute(plan->source_disk_path) ||
        !g_path_is_absolute(plan->target_disk_path)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "vm.clone file copy: source and target paths must be absolute");
        return FALSE;
    }

    if (g_strcmp0(plan->source_disk_path, plan->target_disk_path) == 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "vm.clone file copy: target must differ from source");
        return FALSE;
    }

    if (!g_file_test(plan->source_disk_path, G_FILE_TEST_EXISTS)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "vm.clone file copy: source disk does not exist: %s",
                    plan->source_disk_path);
        return FALSE;
    }

    if (g_file_test(plan->target_disk_path, G_FILE_TEST_EXISTS)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "vm.clone file copy: target disk already exists: %s",
                    plan->target_disk_path);
        return FALSE;
    }

    const gchar *argv[] = {
        "qemu-img", "convert",
        "-f", format,
        "-O", format,
        plan->source_disk_path,
        plan->target_disk_path,
        NULL
    };

    /* 외부 명령은 argv 배열 경로(pcv_spawn_sync)로만 실행한다(shell 미경유).
     * 실패 시 부분 기록된 target 을 지워, 재시도 때 "이미 존재" 오류나 손상된
     * 반쪽 디스크가 남지 않게 한다(source 는 절대 건드리지 않는다). */
    gboolean ok = pcv_spawn_sync(argv, NULL, NULL, error);
    if (!ok)
        g_remove(plan->target_disk_path);
    return ok;
}

gboolean
pcv_vm_clone_file_copy_available(void)
{
    return program_available("qemu-img");
}

gboolean
pcv_vm_clone_guest_reset_available(void)
{
    /* 일반 VM clone은 네 도구가 모두 있어야 identity, FS UUID, fstab,
     * boot artifact를 한 worker 흐름에서 끝낼 수 있다. */
    return program_available("virt-sysprep") &&
           program_available("virt-customize") &&
           program_available("virt-filesystems") &&
           program_available("guestfish");
}

typedef struct {
    gchar *device;
    gchar *uuid;
} PcvVmCloneFsUuid;

static void
fs_uuid_free(gpointer data)
{
    PcvVmCloneFsUuid *entry = data;
    if (!entry)
        return;
    g_free(entry->device);
    g_free(entry->uuid);
    g_free(entry);
}

static GPtrArray *
split_ascii_fields(const gchar *line)
{
    GPtrArray *fields = g_ptr_array_new_with_free_func(g_free);
    const gchar *p = line;
    while (p && *p) {
        while (*p && g_ascii_isspace(*p))
            p++;
        if (!*p)
            break;

        const gchar *start = p;
        while (*p && !g_ascii_isspace(*p))
            p++;
        g_ptr_array_add(fields, g_strndup(start, (gsize)(p - start)));
    }
    return fields;
}

static gboolean
fs_uuid_vfs_supported(const gchar *vfs)
{
    return g_strcmp0(vfs, "ext2") == 0 ||
           g_strcmp0(vfs, "ext3") == 0 ||
           g_strcmp0(vfs, "ext4") == 0;
}

static PcvVmCloneFsUuid *
fs_uuid_find_by_device(GPtrArray *entries, const gchar *device)
{
    if (!entries || !device)
        return NULL;

    for (guint i = 0; i < entries->len; i++) {
        PcvVmCloneFsUuid *entry = g_ptr_array_index(entries, i);
        if (entry && g_strcmp0(entry->device, device) == 0)
            return entry;
    }
    return NULL;
}

static GPtrArray *
collect_ext_filesystem_uuids(const PcvVmCloneDiskPlan *plan,
                             gchar **error_msg)
{
    /* virt-sysprep fs-uuids가 ext UUID를 실제로 바꾸지 못하는 환경이 있어,
     * guestfish 보정 전후 비교용으로 ext filesystem과 UUID를 직접 수집한다. */
    const gchar *format = disk_guestfs_format(plan->kind);
    gchar *format_arg = g_strdup_printf("--format=%s", format);
    const gchar *argv[] = {
        "virt-filesystems",
        format_arg,
        "-a", plan->target_disk_path,
        "--filesystems",
        "--long",
        "--uuid",
        "--no-title",
        NULL
    };

    gchar *stdout_out = NULL;
    gchar *stderr_out = NULL;
    GError *error = NULL;
    gboolean ok = pcv_spawn_sync(argv, &stdout_out, &stderr_out, &error);
    g_free(format_arg);
    if (!ok) {
        if (error_msg) {
            *error_msg = g_strdup_printf(
                "vm.clone guest reset: virt-filesystems failed: %s",
                error ? error->message :
                ((stderr_out && *stderr_out) ? stderr_out : "unknown"));
        }
        g_clear_error(&error);
        g_free(stdout_out);
        g_free(stderr_out);
        return NULL;
    }

    GPtrArray *entries = g_ptr_array_new_with_free_func(fs_uuid_free);
    gchar **lines = g_strsplit(stdout_out ? stdout_out : "", "\n", -1);
    for (gsize i = 0; lines && lines[i]; i++) {
        gchar *trimmed = g_strstrip(lines[i]);
        if (!*trimmed)
            continue;

        GPtrArray *fields = split_ascii_fields(trimmed);
        if (fields->len >= 7) {
            const gchar *device = g_ptr_array_index(fields, 0);
            const gchar *type = g_ptr_array_index(fields, 1);
            const gchar *vfs = g_ptr_array_index(fields, 2);
            const gchar *uuid = g_ptr_array_index(fields, fields->len - 1);

            if (g_strcmp0(type, "filesystem") == 0 &&
                fs_uuid_vfs_supported(vfs) &&
                g_str_has_prefix(device, "/dev/") &&
                uuid && *uuid && g_strcmp0(uuid, "-") != 0) {
                PcvVmCloneFsUuid *entry = g_new0(PcvVmCloneFsUuid, 1);
                entry->device = g_strdup(device);
                entry->uuid = g_strdup(uuid);
                g_ptr_array_add(entries, entry);
            }
        }
        g_ptr_array_unref(fields);
    }

    g_strfreev(lines);
    g_free(stdout_out);
    g_free(stderr_out);
    return entries;
}

static gboolean
update_guest_fstab_uuid_refs(const PcvVmCloneDiskPlan *plan,
                             GPtrArray *before,
                             GPtrArray *after,
                             gchar **error_msg)
{
    /* UUID를 강제로 바꾼 뒤 guest의 /etc/fstab이 옛 UUID를 가리키면
     * boot가 깨질 수 있으므로, 바뀐 ext UUID만 치환한다. */
    const gchar *format = disk_guestfs_format(plan->kind);
    GString *cmd = g_string_new("if [ -f /etc/fstab ]; then sed -i");
    guint replacements = 0;

    for (guint i = 0; before && i < before->len; i++) {
        PcvVmCloneFsUuid *old_entry = g_ptr_array_index(before, i);
        PcvVmCloneFsUuid *new_entry = fs_uuid_find_by_device(after,
                                                             old_entry->device);
        if (!new_entry) {
            if (error_msg) {
                *error_msg = g_strdup_printf(
                    "vm.clone guest reset: filesystem disappeared after UUID reset: %s",
                    old_entry->device);
            }
            g_string_free(cmd, TRUE);
            return FALSE;
        }
        if (g_strcmp0(old_entry->uuid, new_entry->uuid) == 0) {
            if (error_msg) {
                *error_msg = g_strdup_printf(
                    "vm.clone guest reset: filesystem UUID did not change: %s",
                    old_entry->device);
            }
            g_string_free(cmd, TRUE);
            return FALSE;
        }

        g_string_append_printf(cmd, " -e 's|%s|%s|g'",
                               old_entry->uuid,
                               new_entry->uuid);
        replacements++;
    }

    g_string_append(cmd, " /etc/fstab; fi");
    if (replacements == 0) {
        g_string_free(cmd, TRUE);
        return TRUE;
    }

    const gchar *argv[] = {
        "virt-customize",
        "--format", format,
        "-a", plan->target_disk_path,
        "--no-network",
        "--run-command", cmd->str,
        NULL
    };
    gchar *stderr_out = NULL;
    GError *error = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, &stderr_out, &error);
    if (!ok) {
        if (error_msg) {
            *error_msg = g_strdup_printf(
                "vm.clone guest reset: fstab UUID refresh failed: %s",
                error ? error->message :
                ((stderr_out && *stderr_out) ? stderr_out : "unknown"));
        }
        if (stderr_out && *stderr_out)
            PCV_LOG_WARN("vm_clone_plan", "fstab UUID refresh failed: %s",
                         stderr_out);
    }

    g_clear_error(&error);
    g_free(stderr_out);
    g_string_free(cmd, TRUE);
    return ok;
}

static gboolean
randomize_ext_filesystem_uuids(const PcvVmCloneDiskPlan *plan,
                               gchar **error_msg)
{
    /* guestfish는 여러 ext filesystem을 한 libguestfs 세션에서 처리한다.
     * e2fsck-f 후 set-uuid-random을 실행해야 ext4 UUID 변경이 안정적이다. */
    const gchar *format = disk_guestfs_format(plan->kind);
    GPtrArray *before = collect_ext_filesystem_uuids(plan, error_msg);
    if (!before)
        return FALSE;
    if (before->len == 0) {
        g_ptr_array_unref(before);
        return TRUE;
    }

    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup("guestfish"));
    g_ptr_array_add(argv, g_strdup("--rw"));
    g_ptr_array_add(argv, g_strdup_printf("--format=%s", format));
    g_ptr_array_add(argv, g_strdup("-a"));
    g_ptr_array_add(argv, g_strdup(plan->target_disk_path));
    g_ptr_array_add(argv, g_strdup("run"));

    for (guint i = 0; i < before->len; i++) {
        PcvVmCloneFsUuid *entry = g_ptr_array_index(before, i);
        g_ptr_array_add(argv, g_strdup(":"));
        g_ptr_array_add(argv, g_strdup("e2fsck-f"));
        g_ptr_array_add(argv, g_strdup(entry->device));
        g_ptr_array_add(argv, g_strdup(":"));
        g_ptr_array_add(argv, g_strdup("set-uuid-random"));
        g_ptr_array_add(argv, g_strdup(entry->device));
    }
    g_ptr_array_add(argv, NULL);

    gchar *stderr_out = NULL;
    GError *error = NULL;
    gboolean ok = pcv_spawn_sync((const gchar * const *)argv->pdata,
                                 NULL,
                                 &stderr_out,
                                 &error);
    if (!ok) {
        if (error_msg) {
            *error_msg = g_strdup_printf(
                "vm.clone guest reset: guestfish filesystem UUID reset failed: %s",
                error ? error->message :
                ((stderr_out && *stderr_out) ? stderr_out : "unknown"));
        }
        if (stderr_out && *stderr_out)
            PCV_LOG_WARN("vm_clone_plan", "guestfish fs UUID reset failed: %s",
                         stderr_out);
        g_clear_error(&error);
        g_free(stderr_out);
        g_ptr_array_unref(argv);
        g_ptr_array_unref(before);
        return FALSE;
    }
    g_clear_error(&error);
    g_free(stderr_out);
    g_ptr_array_unref(argv);

    GPtrArray *after = collect_ext_filesystem_uuids(plan, error_msg);
    if (!after) {
        g_ptr_array_unref(before);
        return FALSE;
    }

    ok = update_guest_fstab_uuid_refs(plan, before, after, error_msg);
    g_ptr_array_unref(after);
    g_ptr_array_unref(before);
    return ok;
}

/* virt-sysprep argv 조립: 복제된 디스크에서 원본 VM 의 "정체성"을 지운다.
 * operations 목록(fs-uuids·lvm-uuids·net-hostname·net-hwaddr 등)은 두 VM 이 같은
 * UUID/MAC/hostname 으로 부팅해 네트워크·스토리지 충돌을 일으키는 것을 막는다.
 * --no-network 로 게스트 안에서 외부 통신을 막고, cloud-init seed 를 지워 복제본이
 * 원본의 cloud-init 인스턴스 신원을 재사용하지 않게 한다. argv 는 호출자 소유(GStrv). */
GStrv
pcv_vm_clone_build_guest_reset_argv(const PcvVmCloneDiskPlan *plan,
                                    const gchar *hostname,
                                    gchar **error_msg)
{
    if (!plan || !plan->target_disk_path || !hostname || !*hostname) {
        if (error_msg)
            *error_msg = g_strdup("vm.clone guest reset: missing disk plan or hostname");
        return NULL;
    }

    const gchar *format = disk_guestfs_format(plan->kind);
    if (!format) {
        if (error_msg)
            *error_msg = g_strdup("vm.clone guest reset: unsupported disk kind");
        return NULL;
    }

    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup("virt-sysprep"));
    g_ptr_array_add(argv, g_strdup("--format"));
    g_ptr_array_add(argv, g_strdup(format));
    g_ptr_array_add(argv, g_strdup("-a"));
    g_ptr_array_add(argv, g_strdup(plan->target_disk_path));
    g_ptr_array_add(argv, g_strdup("--no-network"));
    g_ptr_array_add(argv, g_strdup("--operations"));
    g_ptr_array_add(argv, g_strdup(
        "defaults,fs-uuids,lvm-uuids,lvm-system-devices,"
        "net-hostname,net-hwaddr,customize"));
    g_ptr_array_add(argv, g_strdup("--hostname"));
    g_ptr_array_add(argv, g_strdup(hostname));

    g_ptr_array_add(argv, g_strdup("--run-command"));
    g_ptr_array_add(argv, g_strdup(
        "cloud-init clean --logs --seed 2>/dev/null || true; "
        "rm -rf /var/lib/cloud/instances/* /var/lib/cloud/instance "
        "2>/dev/null || true"));

    g_ptr_array_add(argv, NULL);
    return (GStrv)g_ptr_array_free(argv, FALSE);
}

/* virt-customize argv 조립: UUID/hostname 을 바꾼 뒤 게스트가 정상 부팅하도록
 * boot artifact 를 재생성한다. initramfs(update-initramfs/dracut)는 바뀐 FS UUID 를
 * 다시 임베드하고, grub 설정을 재작성하며, SELinux 시스템이면 /.autorelabel 로
 * 다음 부팅 relabel 을 예약한다. 배포판마다 도구가 달라 존재 검사 후 조건 실행한다. */
GStrv
pcv_vm_clone_build_guest_boot_rebuild_argv(const PcvVmCloneDiskPlan *plan,
                                           gchar **error_msg)
{
    if (!plan || !plan->target_disk_path) {
        if (error_msg)
            *error_msg = g_strdup("vm.clone guest boot rebuild: missing disk plan");
        return NULL;
    }

    const gchar *format = disk_guestfs_format(plan->kind);
    if (!format) {
        if (error_msg)
            *error_msg = g_strdup("vm.clone guest boot rebuild: unsupported disk kind");
        return NULL;
    }

    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup("virt-customize"));
    g_ptr_array_add(argv, g_strdup("--format"));
    g_ptr_array_add(argv, g_strdup(format));
    g_ptr_array_add(argv, g_strdup("-a"));
    g_ptr_array_add(argv, g_strdup(plan->target_disk_path));
    g_ptr_array_add(argv, g_strdup("--no-network"));

    g_ptr_array_add(argv, g_strdup("--run-command"));
    g_ptr_array_add(argv, g_strdup(
        "if command -v update-initramfs >/dev/null 2>&1; then "
        "update-initramfs -u -k all; fi; "
        "if command -v dracut >/dev/null 2>&1; then "
        "dracut -f --regenerate-all || dracut -f; fi"));

    g_ptr_array_add(argv, g_strdup("--run-command"));
    g_ptr_array_add(argv, g_strdup(
        "if command -v update-grub >/dev/null 2>&1; then "
        "update-grub; "
        "elif command -v grub2-mkconfig >/dev/null 2>&1; then "
        "if [ -d /boot/grub2 ]; then grub2-mkconfig -o /boot/grub2/grub.cfg; "
        "elif [ -d /boot/grub ]; then grub2-mkconfig -o /boot/grub/grub.cfg; "
        "else grub2-mkconfig -o /etc/grub2.cfg; fi; fi"));

    g_ptr_array_add(argv, g_strdup("--run-command"));
    g_ptr_array_add(argv, g_strdup(
        "if [ -e /etc/selinux/config ]; then touch /.autorelabel; fi"));

    g_ptr_array_add(argv, NULL);
    return (GStrv)g_ptr_array_free(argv, FALSE);
}

/**
 * pcv_vm_clone_reset_guest_identity:
 * 복제된 target 디스크의 게스트 신원을 3단계로 재설정한다(순서 고정):
 *   1) virt-sysprep     — 정체성 리셋(hostname/hwaddr/cloud-init 등).
 *   2) fs UUID 무작위화  — ext UUID 를 실제로 바꾸고 /etc/fstab 참조를 함께 갱신.
 *   3) boot artifact 재생성 — 바뀐 UUID 로 initramfs/grub 재작성.
 *
 * 각 단계 실패는 즉시 FALSE 로 중단한다(다음 단계로 진행하지 않는다) — 예컨대 UUID 만
 * 바꾸고 fstab/부트를 갱신하지 못한 채 넘어가면 복제본이 부팅 불능이 된다. 실행 워커가
 * 원본이 아닌 복제 target 에 대해서만 호출해야 한다(원본 게스트를 건드리면 안 됨).
 */
gboolean
pcv_vm_clone_reset_guest_identity(const PcvVmCloneDiskPlan *plan,
                                  const gchar *hostname,
                                  GError **error)
{
    if (!pcv_vm_clone_guest_reset_available()) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "vm.clone guest reset: virt-sysprep, virt-customize, virt-filesystems, and guestfish are required; install libguestfs-tools");
        return FALSE;
    }

    gchar *build_error = NULL;
    GStrv argv = pcv_vm_clone_build_guest_reset_argv(plan, hostname, &build_error);
    if (!argv) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            build_error ? build_error : "vm.clone guest reset: invalid plan");
        g_free(build_error);
        return FALSE;
    }

    gchar *stderr_out = NULL;
    gboolean ok = pcv_spawn_sync((const gchar * const *)argv,
                                 NULL,
                                 &stderr_out,
                                 error);
    if (!ok && stderr_out && *stderr_out) {
        PCV_LOG_WARN("vm_clone_plan", "virt-sysprep failed: %s", stderr_out);
    }

    g_free(stderr_out);
    g_strfreev(argv);
    if (!ok)
        return FALSE;

    build_error = NULL;
    if (!randomize_ext_filesystem_uuids(plan, &build_error)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            build_error ? build_error :
                                          "vm.clone guest reset: filesystem UUID reset failed");
        g_free(build_error);
        return FALSE;
    }

    build_error = NULL;
    argv = pcv_vm_clone_build_guest_boot_rebuild_argv(plan, &build_error);
    if (!argv) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            build_error ? build_error :
                                          "vm.clone guest boot rebuild: invalid plan");
        g_free(build_error);
        return FALSE;
    }

    stderr_out = NULL;
    ok = pcv_spawn_sync((const gchar * const *)argv,
                        NULL,
                        &stderr_out,
                        error);
    if (!ok && stderr_out && *stderr_out) {
        PCV_LOG_WARN("vm_clone_plan", "virt-customize failed: %s", stderr_out);
    }

    g_free(stderr_out);
    g_strfreev(argv);
    return ok;
}

gboolean
pcv_vm_clone_disk_plan_beta_allowed(const PcvVmCloneDiskPlan *plan,
                                    gchar **error_msg)
{
    if (!plan) {
        if (error_msg)
            *error_msg = g_strdup("Missing clone disk plan");
        return FALSE;
    }

    if (plan->kind == PCV_VM_CLONE_DISK_ZVOL ||
        plan->kind == PCV_VM_CLONE_DISK_QCOW2 ||
        plan->kind == PCV_VM_CLONE_DISK_RAW)
        return TRUE;

    if (error_msg)
        *error_msg = g_strdup("vm.clone beta guard: unsupported disk source");
    return FALSE;
}

gboolean
pcv_vm_clone_build_zvol_plan(const gchar *target_name,
                             const PcvVmCloneDiskInfo *disk,
                             PcvVmCloneDiskPlan *plan,
                             gchar **error_msg)
{
    if (!pcv_vm_clone_build_disk_plan(target_name, disk, plan, error_msg))
        return FALSE;

    if (plan->kind != PCV_VM_CLONE_DISK_ZVOL) {
        if (error_msg)
            *error_msg = g_strdup("vm.clone zvol plan: only ZFS zvol disks are supported");
        pcv_vm_clone_disk_plan_clear(plan);
        return FALSE;
    }
    return TRUE;
}
