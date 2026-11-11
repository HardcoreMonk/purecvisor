#include <glib.h>

#define PCV_T4_STORE   "/usr/lib/purecvisor/bpf"
#define PCV_T4_PINDIR  "/sys/fs/bpf/purecvisor/pcv_lsm"

static void test_bpf_load_and_pin_root(void) {
    if (geteuid() != 0) {
        g_test_skip("root 필요 — BPF 로드/핀은 CAP_BPF/CAP_SYS_ADMIN 요구");
        return;
    }
    if (!pcv_bpf_caps_btf() || !pcv_bpf_caps_lsm()) {
        g_test_skip("BTF 또는 lsm=bpf 부재 — LSM BPF 로드 불가(커널 lsm=...,bpf 재부팅 선행)");
        return;
    }

                                                          
    gchar *manifest = g_build_filename(PCV_T4_STORE, "manifest.json", NULL);
    gboolean have_store = g_file_test(manifest, G_FILE_TEST_EXISTS);
    g_free(manifest);
    if (!have_store) {
        g_test_skip("BPF 스토어 미설치 — make bpf-install 선행 필요");
        return;
    }

    GError *err = NULL;
    g_assert_true(pcv_bpf_init(PCV_T4_STORE, &err));
    g_assert_no_error(err);
    if (pcv_bpf_health() != PCV_BPF_OK) {
                                                         
        g_test_skip("eBPF 로드 능력 부재(libbpf 미컴파일 또는 degraded)");
        pcv_bpf_shutdown();
        return;
    }

                                             
                                                                            
    g_assert_true(pcv_bpf_rehydrate(PCV_T4_STORE, &err));
    g_assert_no_error(err);

                                  
    g_assert_true(g_file_test(PCV_T4_PINDIR, G_FILE_TEST_IS_DIR));

    pcv_bpf_shutdown();
}

void test_bpf_integration_pre_fix_register(void) {
    g_test_add_func("/bpf/load_and_pin", test_bpf_load_and_pin_root);
}
