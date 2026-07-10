/**
 * @file pcv_txn.c
 * @brief 다단계 작업용 트랜잭션/롤백 유틸리티 구현
 *
 * GPtrArray에 롤백 엔트리를 저장하고, 실패 시 역순으로 실행합니다.
 * 커밋되면 롤백 함수 없이 리소스만 해제합니다.
 */
#include "pcv_txn.h"
#include "utils/pcv_log.h"

#define TXN_LOG_DOM "txn"

/** 개별 롤백 단계 */
typedef struct {
    PcvTxnRollbackFunc func;       /* 롤백 함수 */
    gpointer           user_data;  /* 롤백 함수에 전달할 데이터 */
    GDestroyNotify     free_func;  /* user_data 해제 함수 (NULL 가능) */
} TxnEntry;

/** 트랜잭션 컨텍스트 */
struct PcvTxn {
    gchar     *name;        /* 트랜잭션 이름 (로깅용) */
    GPtrArray *entries;     /* TxnEntry* 배열 */
    gboolean   committed;   /* 커밋 여부 */
    gboolean   rolled_back; /* 롤백 완료 여부 */
};

static void
_entry_free(gpointer p)
{
    TxnEntry *e = p;
    if (e->free_func && e->user_data)
        e->free_func(e->user_data);
    g_free(e);
}

PcvTxn *
pcv_txn_new(const gchar *name)
{
    PcvTxn *txn = g_new0(PcvTxn, 1);
    txn->name = g_strdup(name ? name : "unnamed");
    txn->entries = g_ptr_array_new_with_free_func(_entry_free);
    txn->committed = FALSE;
    txn->rolled_back = FALSE;
    return txn;
}

void
pcv_txn_add_rollback(PcvTxn *txn, PcvTxnRollbackFunc func,
                      gpointer user_data, GDestroyNotify free_func)
{
    g_return_if_fail(txn != NULL && func != NULL);

    TxnEntry *e = g_new0(TxnEntry, 1);
    e->func = func;
    e->user_data = user_data;
    e->free_func = free_func;
    g_ptr_array_add(txn->entries, e);
}

void
pcv_txn_rollback(PcvTxn *txn)
{
    g_return_if_fail(txn != NULL);
    if (txn->rolled_back || txn->committed) return;

    PCV_LOG_WARN(TXN_LOG_DOM, "txn '%s': rolling back %u steps",
                 txn->name, txn->entries->len);

    /* LIFO 순서: 마지막 등록된 것부터 역순 실행 */
    for (gint i = (gint)txn->entries->len - 1; i >= 0; i--) {
        TxnEntry *e = g_ptr_array_index(txn->entries, i);
        e->func(e->user_data);
        /* user_data 소유권 정리는 등록된 free_func가 담당한다. */
    }
    txn->rolled_back = TRUE;
}

void
pcv_txn_commit(PcvTxn *txn)
{
    g_return_if_fail(txn != NULL);
    if (txn->committed || txn->rolled_back) return;

    txn->committed = TRUE;
    PCV_LOG_INFO(TXN_LOG_DOM, "txn '%s': committed (%u steps discarded)",
                 txn->name, txn->entries->len);
}

void
pcv_txn_free(PcvTxn *txn)
{
    if (!txn) return;

    /* 미커밋 + 미롤백이면 자동 롤백 */
    if (!txn->committed && !txn->rolled_back)
        pcv_txn_rollback(txn);

    g_ptr_array_free(txn->entries, TRUE);
    g_free(txn->name);
    g_free(txn);
}
