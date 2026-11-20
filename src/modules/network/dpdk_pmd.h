#ifndef PURECVISOR_DPDK_PMD_H
#define PURECVISOR_DPDK_PMD_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef struct {
    gchar   *port;
    guint    qid;
    gboolean enabled;
} PcvDpdkPmdRxq;

typedef struct {
    guint      core;
    gint       numa;
    gboolean   isolated;
    GPtrArray *rxqs;
} PcvDpdkPmd;

typedef enum {
    PCV_PMD_MODE_AUTO,
    PCV_PMD_MODE_OFF,
    PCV_PMD_MODE_INVALID
} PcvPmdMode;

typedef enum {
    PCV_PMD_CACHE_UNKNOWN,
    PCV_PMD_CACHE_EMPTY,
    PCV_PMD_CACHE_VALID
} PcvPmdCacheState;

typedef enum {
    PCV_PMD_KEEP,
    PCV_PMD_APPLY,
    PCV_PMD_CLEAR_OWNED,
    PCV_PMD_SKIP,
    PCV_PMD_OPERATOR,
    PCV_PMD_NOT_NEEDED
} PcvPmdAction;

typedef struct {
    PcvPmdMode mode;
    gint       vm_numa;
    guint      nq;
    GPtrArray *pmds;
    GArray    *host_nodes;
    gboolean   isolate;
    gboolean   query_ok;
} PcvDpdkPmdInput;

typedef struct {
    gint         action;
    guint        core;
    guint        nq;
    gchar       *affinity;
    const gchar *reason;
    gboolean     numa_remote;
    gboolean     clear_marker;
    gboolean     changed;
    const gchar *extra[13];
    gchar       *extra_buf[3];
} PcvDpdkPmdPlan;

void pcv_dpdk_pmd_free(gpointer pmd);

gboolean pcv_dpdk_pmd_parse_rxq_show(const gchar *text, GPtrArray **pmds_out, GError **error);

gboolean pcv_dpdk_pmd_parse_mask(const gchar *mask, GArray **cores_out);

PcvPmdMode pcv_dpdk_pmd_mode_parse(const gchar *value);

PcvPmdMode pcv_dpdk_pmd_mode_from_config(void);

gboolean pcv_dpdk_pmd_affinity_owned(const gchar *cur_aff, const gchar *marker);

void pcv_dpdk_pmd_choose(const PcvDpdkPmdInput *in, const gchar *self_port,
                         const gchar *cur_aff, const gchar *prev_marker,
                         GHashTable *port_markers, PcvDpdkPmdPlan *out);

void pcv_dpdk_pmd_input_clear(PcvDpdkPmdInput *in);

void pcv_dpdk_pmd_plan_clear(PcvDpdkPmdPlan *plan);

void pcv_dpdk_pmd_input_prepare(PcvDpdkPmdInput *in, const gchar *domain_xml);

void pcv_dpdk_pmd_plan_locked(const gchar *port, gboolean port_exists,
                              const PcvDpdkPmdInput *in, PcvDpdkPmdPlan *plan);

gboolean pcv_dpdk_pmd_verify_locked(const gchar *port, PcvDpdkPmdPlan *plan, GError **error);

void pcv_dpdk_pmd_record_start(const gchar *vm_name, const PcvDpdkPmdPlan *plan);

gchar *pcv_dpdk_pmd_audit_detail(const PcvDpdkPmdPlan *plan);

gboolean pcv_dpdk_pmd_query(GPtrArray **pmds_out, gboolean *isolate_out, GError **error);

void pcv_dpdk_pmd_cache_update(const GPtrArray *pmds, const GArray *mask_cores,
                               const gchar *source);

GArray *pcv_dpdk_pmd_cache_snapshot(PcvPmdCacheState *state_out);

void pcv_dpdk_pmd_cache_init_from_mask(void);

void pcv_dpdk_pmd_cache_refresh(void);

JsonObject *pcv_dpdk_status(void);

JsonObject *pcv_dpdk_pmd_status_object(gboolean admin_view);

typedef gboolean (*PcvDpdkPmdTestRunner)(const gchar * const *argv, gchar **stdout_out,
                                         GError **error, gpointer user_data);

G_END_DECLS

#endif
