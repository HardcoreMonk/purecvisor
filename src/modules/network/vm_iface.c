/**
 * @file vm_iface.c
 * @brief VM 인터페이스 열거 — `virsh domiflist` 출력을 vnet/tap 이름 목록으로.
 *
 * 이 파일은 libvirt 도메인의 host-side 네트워크 인터페이스(호스트에 생기는
 * vnet0/tapN 같은 tap 장치) 이름만 뽑아내는 얇은 어댑터다. VM 삭제/네트워크
 * 정리 경로에서 "이 VM이 호스트에 남긴 tap 장치가 무엇인가"를 알아내는 데 쓴다.
 *
 * Operator note:
 *   목록이 비면(파싱 실패·virsh 오류) 호출자는 정리할 인터페이스가 없다고 보고
 *   넘어간다. 그래서 실패해도 예외 대신 항상 non-NULL 빈 배열을 돌려주며, 고아
 *   tap 장치가 의심되면 `virsh domiflist <vm>` 을 직접 확인한다.
 */
#include "vm_iface.h"
#include <string.h>
#include "../../utils/pcv_spawn.h"

/* [R5] virsh domiflist 정상 sub-second — hung libvirtd bound (spawn-hardening) */
#define VM_IFACE_SPAWN_TIMEOUT_SEC 30

/* virsh domiflist 데이터 행: " vnet0  bridge  pcvbr0  virtio  52:54:..." */
GPtrArray *
pcv_vm_iface_parse_domiflist(const gchar *out)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
    if (!out) return arr;
    gchar **lines = g_strsplit(out, "\n", -1);
    for (gchar **l = lines; *l; l++) {
        gchar *trimmed = g_strstrip(*l);
        if (g_str_has_prefix(trimmed, "vnet") || g_str_has_prefix(trimmed, "tap")) {
            gchar *space = strchr(trimmed, ' ');
            g_ptr_array_add(arr, space ? g_strndup(trimmed, (gsize)(space - trimmed))
                                       : g_strdup(trimmed));
        }
    }
    g_strfreev(lines);
    return arr;
}

/*
 * pcv_vm_iface_list: <vm_name> 의 host-side 인터페이스 이름을 조회한다.
 * 계약: 성공/실패 어느 쪽이든 free-func 등록된 non-NULL GPtrArray 를 돌려주며,
 * 호출자가 소유권을 넘겨받아 g_ptr_array_unref 해야 한다. spawn 타임아웃은
 * hung libvirtd 로 삭제 워커가 무한 대기하는 것을 막는 상한이다(R5).
 */
GPtrArray *
pcv_vm_iface_list(const gchar *vm_name)
{
    const gchar *argv[] = {"virsh", "domiflist", vm_name, NULL};
    gchar *out = NULL;
    if (!pcv_spawn_sync_timeout(argv, &out, NULL, VM_IFACE_SPAWN_TIMEOUT_SEC, NULL)) {
        g_free(out);
        return g_ptr_array_new_with_free_func(g_free);  /* 계약: 항상 non-NULL */
    }
    GPtrArray *arr = pcv_vm_iface_parse_domiflist(out);
    g_free(out);
    return arr;
}
