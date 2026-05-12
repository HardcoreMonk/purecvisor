

















#ifndef PCV_TXN_H
#define PCV_TXN_H

#include <glib.h>

typedef struct PcvTxn PcvTxn;


typedef void (*PcvTxnRollbackFunc)(gpointer user_data);


PcvTxn *pcv_txn_new(const gchar *name);








void pcv_txn_add_rollback(PcvTxn *txn, PcvTxnRollbackFunc func,
                           gpointer user_data, GDestroyNotify free_func);


void pcv_txn_rollback(PcvTxn *txn);


void pcv_txn_commit(PcvTxn *txn);


void pcv_txn_free(PcvTxn *txn);

#endif
