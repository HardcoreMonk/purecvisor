














#include "hot_reload.h"
#include "drain.h"
#include "purecvisor/version.h"
#include "utils/pcv_log.h"
#include <glib-unix.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#define HR_LOG_DOM "hot_reload"
#define PCV_VERSION PCV_PRODUCT_VERSION









static struct {
    gchar          *binary_path;
    int             uds_fd;
    PcvUpgradeState state;
    GMutex          mu;
} G = {0};




















static gboolean
_do_upgrade(gpointer user_data __attribute__((unused)))
{
    g_mutex_lock(&G.mu);
    G.state = PCV_UPGRADE_EXECUTING;
    g_mutex_unlock(&G.mu);

    PCV_LOG_INFO(HR_LOG_DOM, "Executing binary upgrade: %s", G.binary_path);


    if (G.uds_fd >= 0) {

        int flags = fcntl(G.uds_fd, F_GETFD);
        if (flags >= 0)
            fcntl(G.uds_fd, F_SETFD, flags & ~FD_CLOEXEC);

        gchar fd_str[16];
        g_snprintf(fd_str, sizeof(fd_str), "%d", G.uds_fd);
        g_setenv("PCV_UPGRADE_FD", fd_str, TRUE);
        g_setenv("LISTEN_FDS", "1", TRUE);
        gchar pid_str[16];
        g_snprintf(pid_str, sizeof(pid_str), "%d", getpid());
        g_setenv("LISTEN_PID", pid_str, TRUE);
    }


    gchar *argv[] = {G.binary_path, NULL};
    execv(G.binary_path, argv);


    PCV_LOG_WARN(HR_LOG_DOM, "execve failed: %s — resuming service", strerror(errno));
    g_mutex_lock(&G.mu);
    G.state = PCV_UPGRADE_IDLE;
    g_mutex_unlock(&G.mu);


    pcv_drain_cancel();

    return G_SOURCE_REMOVE;
}
















static gboolean
_drain_check_timer(gpointer user_data __attribute__((unused)))
{
    if (!(pcv_drain_get_inflight() == 0)) {

        return G_SOURCE_CONTINUE;
    }

    g_mutex_lock(&G.mu);
    G.state = PCV_UPGRADE_READY;
    g_mutex_unlock(&G.mu);

    PCV_LOG_INFO(HR_LOG_DOM, "Drain complete — proceeding with upgrade");
    g_idle_add(_do_upgrade, NULL);
    return G_SOURCE_REMOVE;
}

















static gboolean
_on_sigusr2(gpointer user_data __attribute__((unused)))
{
    PCV_LOG_INFO(HR_LOG_DOM, "SIGUSR2 received — initiating hot reload");

    g_mutex_lock(&G.mu);
    if (G.state != PCV_UPGRADE_IDLE) {
        PCV_LOG_WARN(HR_LOG_DOM, "Upgrade already in progress (state=%d)", G.state);
        g_mutex_unlock(&G.mu);
        return G_SOURCE_CONTINUE;
    }
    G.state = PCV_UPGRADE_DRAINING;
    g_mutex_unlock(&G.mu);


    pcv_drain_begin(NULL, 30);


    g_timeout_add(100, _drain_check_timer, NULL);

    return G_SOURCE_CONTINUE;
}













void
pcv_hot_reload_init(const gchar *binary_path, int uds_listen_fd)
{
    g_mutex_init(&G.mu);
    G.binary_path = g_strdup(binary_path);
    G.uds_fd = uds_listen_fd;
    G.state = PCV_UPGRADE_IDLE;

    g_unix_signal_add(SIGUSR2, _on_sigusr2, NULL);

    PCV_LOG_INFO(HR_LOG_DOM, "Hot reload ready (binary=%s, uds_fd=%d)",
                 binary_path, uds_listen_fd);
}












PcvUpgradeState
pcv_hot_reload_get_state(void)
{
    g_mutex_lock(&G.mu);
    PcvUpgradeState s = G.state;
    g_mutex_unlock(&G.mu);
    return s;
}












gboolean
pcv_hot_reload_prepare(GError **error)
{
    g_mutex_lock(&G.mu);
    if (G.state != PCV_UPGRADE_IDLE) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, g_quark_from_static_string("hot_reload"), 1,
                    "Upgrade already in progress");
        return FALSE;
    }
    g_mutex_unlock(&G.mu);


    _on_sigusr2(NULL);
    return TRUE;
}








const gchar *
pcv_hot_reload_get_version(void)
{
    return PCV_VERSION;
}
