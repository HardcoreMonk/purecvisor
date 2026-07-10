#include "bootstrap/pcv_single_edge_runtime.h"

/*
 * Single Edge용 etcd inflight lock stub.
 *
 * [비전공자 설명]
 * etcd lock은 여러 서버가 같은 ZFS pool을 동시에 건드리지 못하게 하는
 * 분산 자물쇠입니다. Single Edge는 서버가 한 대이므로 이 분산 자물쇠를
 * 실제로 잡을 상대가 없습니다. 그래서 acquire/release는 성공으로 처리해
 * 공통 코드 흐름만 유지합니다.
 *
 * [주니어 참고]
 * 이 파일의 TRUE 반환을 "스토리지 동시성 검사가 전부 끝났다"로 해석하면
 * 안 됩니다. 단일 노드 내부의 실제 ZFS 작업 직렬화는 pcv_zfs_lock 계층과
 * 각 작업 핸들러의 operation lock이 담당합니다.
 */
gboolean
pcv_etcd_acquire_inflight_lock(PcvEtcdClient *c,
                               const gchar *pool,
                               const gchar *node_name,
                               const gchar *op,
                               gint ttl_sec,
                               GError **error)
{
    (void)c;
    (void)pool;
    (void)node_name;
    (void)op;
    (void)ttl_sec;
    (void)error;
    return TRUE;
}

gboolean
pcv_etcd_release_inflight_lock(PcvEtcdClient *c, const gchar *pool, GError **error)
{
    (void)c;
    (void)pool;
    (void)error;
    return TRUE;
}

gint
pcv_etcd_compute_inflight_ttl(const gchar *op, gint size_gb)
{
    (void)op;
    (void)size_gb;
    return 60;
}
