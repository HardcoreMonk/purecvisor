
#include "pcv_txn.h"
#include "utils/pcv_log.h"

#define TXN_LOG_DOM "txn"

typedef struct {
    PcvTxnRollbackFunc func;
    gpointer           user_data;
    GDestroyNotify     free_func;
} TxnEntry;

struct PcvTxn {
    gchar     *name;
    GPtrArray *entries;
    gboolean   committed;
    gboolean   rolled_back;
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

    for (gint i = (gint)txn->entries->len - 1; i >= 0; i--) {
        TxnEntry *e = g_ptr_array_index(txn->entries, i);
        e->func(e->user_data);

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

    if (!txn->committed && !txn->rolled_back)
        pcv_txn_rollback(txn);

    g_ptr_array_free(txn->entries, TRUE);
    g_free(txn->name);
    g_free(txn);
}
