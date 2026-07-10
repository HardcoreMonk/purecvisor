/**
 * @file pcv_txn.h
 * @brief 다단계 작업용 트랜잭션/롤백 유틸리티
 *
 * 다단계 오퍼레이션(예: ZFS zvol 생성 → virt-install → etcd 동기화)이
 * 중간에 실패할 경우, 등록된 롤백 함수를 LIFO 순서로 실행하여
 * 부분 완료 상태를 깔끔하게 정리합니다.
 *
 * 사용 패턴:
 *   PcvTxn *txn = pcv_txn_new("vm.create");
 *   // 1단계 수행
 *   pcv_txn_add_rollback(txn, rollback_zvol, ctx1, g_free);
 *   // 2단계 수행 — 실패 시:
 *   if (failed) { pcv_txn_free(txn); return; }  // 자동 롤백
 *   // 모든 단계 성공:
 *   pcv_txn_commit(txn);
 *   pcv_txn_free(txn);
 */
#ifndef PCV_TXN_H
#define PCV_TXN_H

#include <glib.h>

typedef struct PcvTxn PcvTxn;

/** 롤백 함수 시그니처: 실패 시 user_data와 함께 호출됨 */
typedef void (*PcvTxnRollbackFunc)(gpointer user_data);

/** 새 트랜잭션 컨텍스트 생성 */
PcvTxn *pcv_txn_new(const gchar *name);

/**
 * 롤백 단계 등록 (LIFO — 마지막 등록이 먼저 롤백).
 *
 * free_func가 제공되면 commit/rollback 어느 경로든 트랜잭션 해제 시
 * user_data 정리를 담당합니다. rollback 콜백은 같은 user_data를 직접
 * 해제하지 않아야 합니다.
 */
void pcv_txn_add_rollback(PcvTxn *txn, PcvTxnRollbackFunc func,
                           gpointer user_data, GDestroyNotify free_func);

/** 등록된 모든 롤백 단계를 역순으로 실행 */
void pcv_txn_rollback(PcvTxn *txn);

/** 커밋: 롤백 핸들러를 실행하지 않고 폐기 (성공 경로) */
void pcv_txn_commit(PcvTxn *txn);

/** 트랜잭션 컨텍스트 해제 (미커밋 시 롤백 자동 실행) */
void pcv_txn_free(PcvTxn *txn);

#endif /* PCV_TXN_H */
