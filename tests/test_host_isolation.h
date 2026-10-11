


























#ifndef PCV_TEST_HOST_ISOLATION_H
#define PCV_TEST_HOST_ISOLATION_H

#include <glib.h>





const char *pcv_test_host_root_dir(void);
const char *pcv_test_audit_log_dir(void);
const char *pcv_test_audit_log_path(void);
const char *pcv_test_sg_db_path(void);
const char *pcv_test_sg_nft_script_dir(void);
const char *pcv_test_sg_nft_script_path(void);

#ifdef PCV_TEST_HOST_PATH_SEAM_MAP





#define PCV_TEST_OVERRIDE_AUDIT_LOG_DIR       pcv_test_audit_log_dir()
#define PCV_TEST_OVERRIDE_AUDIT_LOG_PATH      pcv_test_audit_log_path()
#define PCV_TEST_OVERRIDE_SG_DB_PATH          pcv_test_sg_db_path()
#define PCV_TEST_OVERRIDE_SG_NFT_SCRIPT_DIR   pcv_test_sg_nft_script_dir()
#define PCV_TEST_OVERRIDE_SG_NFT_SCRIPT_PATH  pcv_test_sg_nft_script_path()
#endif






static inline gboolean
pcv_test_live_opt_in(const char *env_name)
{
    return g_strcmp0(g_getenv(env_name), "1") == 0;
}

#endif
