                                                                                   
                                                                                     
                                                             
                                                                     
                              
                        
  
                                                                       
  
                 
                                        
                                                                   
                           
  
                                                                  
                                                           
  
                                                                 
  
                                     
   

#include <glib.h>
#include <linux/capability.h>
#include <linux/securebits.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../src/utils/pcv_privdrop.h"
#include "../src/utils/pcv_spawn.h"

                                                                         
guint64 pcv_privdrop_daemon_effective_mask(void);
guint64 pcv_privdrop_spawn_ceiling_mask(void);

                                                    

static void test_prctl_constants(void) {
    g_assert_cmpint(PR_SET_NO_NEW_PRIVS, ==, 38);
    g_assert_cmpint(PR_GET_NO_NEW_PRIVS, ==, 39);
}

                                                  

static void test_prctl_get_nnp(void) {
                                           
    int nnp = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
                                           
    g_assert_true(nnp == 0 || nnp == 1);
}

                                                    

static void test_seccomp_mode_readable(void) {
                                                                           
    int mode = prctl(PR_GET_SECCOMP, 0, 0, 0, 0);
    g_assert_true(mode >= 0 && mode <= 2);
}

                                                      
  
                                                  
                                                

                                                       
static void test_apply_all_subprocess(void) {
    if (g_test_subprocess()) {
        pcv_privdrop_apply_all();
        return;
    }
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();
}

static void test_no_new_privs_subprocess(void) {
    if (g_test_subprocess()) {
        gboolean ok = pcv_privdrop_no_new_privs();
                                                                   
        (void)ok;
        return;
    }
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();
}

static void test_capabilities_subprocess(void) {
    if (g_test_subprocess()) {
        gboolean ok = pcv_privdrop_capabilities();
        (void)ok;
        return;
    }
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();
}

static void test_seccomp_subprocess(void) {
    if (g_test_subprocess()) {
        gboolean ok = pcv_privdrop_seccomp();
        (void)ok;
        return;
    }
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();
}

                                                         
                                     
static void test_disable_coredumps_child(void) {
    pcv_privdrop_disable_coredumps();
    struct rlimit rl;
    g_assert_cmpint(getrlimit(RLIMIT_CORE, &rl), ==, 0);
    g_assert_cmpint((int)rl.rlim_cur, ==, 0);
    g_assert_cmpint((int)rl.rlim_max, ==, 0);
    g_assert_cmpint(prctl(PR_GET_DUMPABLE), ==, 0);
}

static void test_disable_coredumps_subprocess(void) {
    if (g_test_subprocess()) {
        test_disable_coredumps_child();
        return;
    }
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();
}

                                                                                
                                                                       
                                                                      
                                                                            

                                                 
static unsigned char enable_coredumps_child_check(void) {
    struct rlimit cur;
    if (getrlimit(RLIMIT_CORE, &cur) != 0) return 2;
                                                                         
                                                                       
                                                                
    rlim_t test_hard = cur.rlim_max;
    if (test_hard == 0) {
        struct rlimit bump = { 0, 8192 };
        if (setrlimit(RLIMIT_CORE, &bump) != 0) return 0;                         
        test_hard = 8192;
    }
    struct rlimit start = { 0, test_hard };                                   
    if (setrlimit(RLIMIT_CORE, &start) != 0) return 3;
    pcv_privdrop_enable_coredumps();
    struct rlimit rl;
    if (getrlimit(RLIMIT_CORE, &rl) != 0) return 4;
    if (rl.rlim_cur != rl.rlim_max) return 5;                           
    if (rl.rlim_cur != test_hard) return 6;                                           
    return 0;
}

static void test_enable_coredumps_subprocess(void) {
                                                                    
                                                             
                                                       
                                                       
                                                               
    int pfd[2];
    g_assert_cmpint(pipe(pfd), ==, 0);
    pid_t pid = fork();
    g_assert_cmpint(pid, >=, 0);
    if (pid == 0) {
        close(pfd[0]);
        unsigned char code = enable_coredumps_child_check();
        (void)!write(pfd[1], &code, 1);
        _exit(code);
    }
    close(pfd[1]);
    unsigned char got = 0xFF;
    ssize_t nread = read(pfd[0], &got, 1);
    close(pfd[0]);
    int status = 0;
    g_assert_cmpint(waitpid(pid, &status, 0), ==, pid);
    g_assert_cmpint((int)nread, ==, 1);                                  
    g_assert_cmpint(got, ==, 0);
}

                                                           
  
                                                                 
                                                              
                                                             

#define PRIVDROP1_CAP_BIT(cap) (G_GUINT64_CONSTANT(1) << (cap))

                                                               
static gboolean
privdrop1_read_cap_mask(const char *field, guint64 *out)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return FALSE;
    char line[512];
    gboolean found = FALSE;
    size_t flen = strlen(field);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, field, flen) == 0 && line[flen] == ':') {
            *out = (guint64)g_ascii_strtoull(line + flen + 1, NULL, 16);
            found = TRUE;
            break;
        }
    }
    fclose(f);
    return found;
}

                                                
static gboolean
privdrop1_parse_cap_mask(const gchar *status, const gchar *field, guint64 *out)
{
    g_return_val_if_fail(status && field && out, FALSE);
    gsize field_len = strlen(field);
    const gchar *line = status;
    while (line && *line) {
        if ((line == status || line[-1] == '\n') &&
            strncmp(line, field, field_len) == 0 && line[field_len] == ':') {
            *out = (guint64)g_ascii_strtoull(line + field_len + 1, NULL, 16);
            return TRUE;
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    return FALSE;
}

static void
privdrop1_assert_status_masks(const gchar *status, guint64 expected)
{
    static const gchar *fields[] = { "CapInh", "CapPrm", "CapEff", "CapBnd", "CapAmb" };
    for (guint i = 0; i < G_N_ELEMENTS(fields); i++) {
        guint64 actual = 0;
        g_assert_true(privdrop1_parse_cap_mask(status, fields[i], &actual));
        g_assert_cmpuint(actual, ==, expected);
    }
}

typedef struct {
    guint64 inheritable;
    guint64 permitted;
    guint64 effective;
    guint64 bounding;
    guint64 ambient;
    gint securebits;
    guint8 ok;
} Privdrop1ChildState;

                                                                                
static Privdrop1ChildState
privdrop1_capture_child_setup(PcvChildCapabilityProfile profile)
{
    int pfd[2];
    g_assert_cmpint(pipe(pfd), ==, 0);
    pid_t pid = fork();
    g_assert_cmpint(pid, >=, 0);
    if (pid == 0) {
        close(pfd[0]);
        Privdrop1ChildState state = {0};
        pcv_privdrop_child_setup(GINT_TO_POINTER((gint)profile));
        state.ok =
            privdrop1_read_cap_mask("CapInh", &state.inheritable) &&
            privdrop1_read_cap_mask("CapPrm", &state.permitted) &&
            privdrop1_read_cap_mask("CapEff", &state.effective) &&
            privdrop1_read_cap_mask("CapBnd", &state.bounding) &&
            privdrop1_read_cap_mask("CapAmb", &state.ambient);
        state.securebits = prctl(PR_GET_SECUREBITS, 0, 0, 0, 0);
        (void)!write(pfd[1], &state, sizeof(state));
        _exit(state.ok && state.securebits >= 0 ? 0 : 1);
    }

    close(pfd[1]);
    Privdrop1ChildState state = {0};
    ssize_t got = read(pfd[0], &state, sizeof(state));
    close(pfd[0]);
    int wait_status = 0;
    g_assert_cmpint(waitpid(pid, &wait_status, 0), ==, pid);
    g_assert_cmpint((gint)got, ==, (gint)sizeof(state));
    g_assert_true(WIFEXITED(wait_status));
    g_assert_cmpint(WEXITSTATUS(wait_status), ==, 0);
    return state;
}

static gchar *
privdrop1_spawn_status(PcvChildCapabilityProfile profile)
{
    const gchar *argv[] = { "/bin/cat", "/proc/self/status", NULL };
    gchar *out = NULL;
    gchar *stderr_buf = NULL;
    GError *error = NULL;
    gboolean ok = pcv_spawn_sync_profile(argv, profile, &out, &stderr_buf, &error);
    g_assert_no_error(error);
    g_assert_true(ok);
    g_assert_nonnull(out);
    g_assert_cmpstr(stderr_buf, ==, "");
    g_free(stderr_buf);
    return out;
}

typedef struct {
    PcvChildCapabilityProfile profile;
    gint failed;
} Privdrop1SpawnThread;

static gpointer
privdrop1_spawn_thread(gpointer user_data)
{
    Privdrop1SpawnThread *ctx = user_data;
    guint64 expected = pcv_privdrop_child_profile_mask(ctx->profile);
    for (guint i = 0; i < 8; i++) {
        gchar *status = privdrop1_spawn_status(ctx->profile);
        static const gchar *fields[] = { "CapInh", "CapPrm", "CapEff", "CapBnd", "CapAmb" };
        for (guint f = 0; f < G_N_ELEMENTS(fields); f++) {
            guint64 actual = 0;
            if (!privdrop1_parse_cap_mask(status, fields[f], &actual) || actual != expected)
                g_atomic_int_set(&ctx->failed, 1);
        }
        g_free(status);
    }
    return NULL;
}

static gboolean
privdrop1_host_can_apply_ceiling(void)
{
    guint64 eff = 0, prm = 0, bnd = 0;
    guint64 ceiling = pcv_privdrop_spawn_ceiling_mask();
    return privdrop1_read_cap_mask("CapEff", &eff) &&
           privdrop1_read_cap_mask("CapPrm", &prm) &&
           privdrop1_read_cap_mask("CapBnd", &bnd) &&
           (eff & PRIVDROP1_CAP_BIT(CAP_SETPCAP)) != 0 &&
           (prm & ceiling) == ceiling && (bnd & ceiling) == ceiling;
}

static void
privdrop1_parent_and_children_body(void)
{
    g_assert_true(pcv_privdrop_capabilities());
    g_assert_true(pcv_privdrop_child_profiles_enabled());
    pcv_spawn_launcher_init();

    guint64 keep = pcv_privdrop_daemon_effective_mask();
    guint64 ceiling = pcv_privdrop_spawn_ceiling_mask();
    guint64 eff = 0, prm = 0, inh = 0, bnd = 0;
    g_assert_true(privdrop1_read_cap_mask("CapEff", &eff));
    g_assert_true(privdrop1_read_cap_mask("CapPrm", &prm));
    g_assert_true(privdrop1_read_cap_mask("CapInh", &inh));
    g_assert_true(privdrop1_read_cap_mask("CapBnd", &bnd));
    g_assert_cmpuint(eff, ==, keep);
    g_assert_cmpuint(prm, ==, ceiling);
    g_assert_cmpuint(inh, ==, keep);
    g_assert_cmpuint(bnd, ==, ceiling);

    const gint permanently_dropped[] = {
        CAP_SYS_MODULE, CAP_SYS_RAWIO, CAP_SYS_TIME, CAP_MAC_OVERRIDE, CAP_MAC_ADMIN
    };
    for (guint i = 0; i < G_N_ELEMENTS(permanently_dropped); i++)
        g_assert_cmpuint(ceiling & PRIVDROP1_CAP_BIT(permanently_dropped[i]), ==, 0);

    for (gint p = PCV_CHILD_CAP_BASE; p < PCV_CHILD_CAP_N_PROFILES; p++) {
        PcvChildCapabilityProfile profile = (PcvChildCapabilityProfile)p;
        guint64 expected = pcv_privdrop_child_profile_mask(profile);
        Privdrop1ChildState state = privdrop1_capture_child_setup(profile);
        g_assert_true(state.ok);
        g_assert_cmpuint(state.inheritable, ==, expected);
        g_assert_cmpuint(state.permitted, ==, expected);
        g_assert_cmpuint(state.effective, ==, expected);
        g_assert_cmpuint(state.bounding, ==, expected);
        g_assert_cmpuint(state.ambient, ==, expected);
        if (profile == PCV_CHILD_CAP_RUNTIME) {
            g_assert_cmpint(state.securebits & (SECBIT_NOROOT | SECBIT_NOROOT_LOCKED), ==, 0);
        } else {
            g_assert_cmpint(state.securebits & (SECBIT_NOROOT | SECBIT_NOROOT_LOCKED), ==,
                            SECBIT_NOROOT | SECBIT_NOROOT_LOCKED);
        }

        gchar *status = privdrop1_spawn_status(profile);
        privdrop1_assert_status_masks(status, expected);
        g_free(status);
    }

                                                                     
                                                                    
    pcv_spawn_launcher_shutdown();
    gchar *fallback_status = privdrop1_spawn_status(PCV_CHILD_CAP_BASE);
    privdrop1_assert_status_masks(
        fallback_status, pcv_privdrop_child_profile_mask(PCV_CHILD_CAP_BASE));
    g_free(fallback_status);
    pcv_spawn_launcher_init();

                                                                    
                                                                         
    const gchar *private_argv[] = { "/bin/cp", "/proc/self/status", "/dev/stdout", NULL };
    const gchar *private_env[] = { "PCV_PRIVDROP_PROBE=1", NULL };
    gchar *private_status = NULL;
    gchar *private_stderr = NULL;
    GError *private_error = NULL;
    g_assert_true(pcv_spawn_sync_env(private_argv, private_env,
                                     &private_status, &private_stderr, &private_error));
    g_assert_no_error(private_error);
    g_assert_cmpstr(private_stderr, ==, "");
    privdrop1_assert_status_masks(
        private_status, pcv_privdrop_child_profile_mask(PCV_CHILD_CAP_STORAGE));
    g_free(private_status);
    g_free(private_stderr);

                                                                          
                                                                          
    const gchar *cp_argv[] = { "/bin/cp", "/proc/self/status", "/dev/stdout", NULL };
    gchar *cp_status = NULL;
    gchar *cp_stderr = NULL;
    GError *cp_error = NULL;
    g_assert_true(pcv_spawn_sync(cp_argv, &cp_status, &cp_stderr, &cp_error));
    g_assert_no_error(cp_error);
    g_assert_cmpstr(cp_stderr, ==, "");
    privdrop1_assert_status_masks(
        cp_status, pcv_privdrop_child_profile_mask(PCV_CHILD_CAP_STORAGE));
    g_free(cp_status);
    g_free(cp_stderr);

                                                                           
    Privdrop1SpawnThread base = { PCV_CHILD_CAP_BASE, 0 };
    Privdrop1SpawnThread runtime = { PCV_CHILD_CAP_RUNTIME, 0 };
    GThread *t1 = g_thread_new("privdrop-base", privdrop1_spawn_thread, &base);
    GThread *t2 = g_thread_new("privdrop-runtime", privdrop1_spawn_thread, &runtime);
    g_thread_join(t1);
    g_thread_join(t2);
    g_assert_cmpint(g_atomic_int_get(&base.failed), ==, 0);
    g_assert_cmpint(g_atomic_int_get(&runtime.failed), ==, 0);

    pcv_spawn_launcher_shutdown();
}

static void
test_privdrop1_parent_and_child_profiles(void)
{
    if (!privdrop1_host_can_apply_ceiling()) {
        g_test_skip("CAP_SETPCAP과 full spawn ceiling을 가진 root 격리 환경이 필요함");
        return;
    }
    if (g_test_subprocess()) {
        privdrop1_parent_and_children_body();
        return;
    }
    g_test_trap_subprocess(NULL, 30 * G_USEC_PER_SEC, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();
}

                                                        

void test_privdrop_register(void) {
    g_test_add_func("/privdrop/prctl_constants",     test_prctl_constants);
    g_test_add_func("/privdrop/prctl_get_nnp",       test_prctl_get_nnp);
    g_test_add_func("/privdrop/seccomp_mode_readable", test_seccomp_mode_readable);
    g_test_add_func("/privdrop/apply_all_subprocess",   test_apply_all_subprocess);
    g_test_add_func("/privdrop/no_new_privs_subprocess", test_no_new_privs_subprocess);
    g_test_add_func("/privdrop/capabilities_subprocess", test_capabilities_subprocess);
    g_test_add_func("/privdrop/seccomp_subprocess",      test_seccomp_subprocess);
    g_test_add_func("/privdrop/disable_coredumps_subprocess", test_disable_coredumps_subprocess);
    g_test_add_func("/privdrop/enable_coredumps_subprocess", test_enable_coredumps_subprocess);
    g_test_add_func("/privdrop/privdrop1_parent_and_child_profiles",
                    test_privdrop1_parent_and_child_profiles);
}
