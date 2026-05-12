





























































#include "uds_server.h"
#include "dispatcher.h"
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <sys/stat.h>
#include <glib.h>
#include <unistd.h>

#include "io/pcv_uring.h"
#if PCV_USE_URING
#include <glib-unix.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif













struct _UdsServer {
    GObject parent_instance;
    GSocketService *service;
    gchar *socket_path;
    PureCVisorDispatcher *dispatcher;
    guint16 connection_count;
#if PCV_USE_URING
    PcvUringCtx *uring;
    int listen_fd;
    gboolean uring_mode;
#endif
};










G_DEFINE_TYPE(UdsServer, uds_server, G_TYPE_OBJECT)
























static void uds_server_finalize(GObject *object) {
    UdsServer *self = PURECVISOR_UDS_SERVER(object);

#if PCV_USE_URING
    if (self->uring) {
        pcv_uring_free(self->uring);
        self->uring = NULL;
    }
    if (self->listen_fd >= 0) {
        close(self->listen_fd);
        self->listen_fd = -1;
    }
#endif






    if (self->service) {
        g_socket_service_stop(self->service);
        g_object_unref(self->service);
    }


    g_free(self->socket_path);


    if (self->dispatcher) g_object_unref(self->dispatcher);


    G_OBJECT_CLASS(uds_server_parent_class)->finalize(object);
}














static void uds_server_class_init(UdsServerClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = uds_server_finalize;
}

















static void uds_server_init(UdsServer *self) {
    self->service = NULL;
    self->socket_path = NULL;
    self->dispatcher = NULL;
    self->connection_count = 0;
#if PCV_USE_URING
    self->uring = NULL;
    self->listen_fd = -1;
    self->uring_mode = FALSE;
#endif
}







#if PCV_USE_URING





typedef struct {
    UdsServer *server;
    int        fd;
    gchar     *buffer;
    gsize      buf_size;
    gchar     *response;
    gsize      resp_len;
} UringConnCtx;








static void _uring_accept_cb(PcvUringCtx *uring, gint result, gpointer data);
static void _uring_read_cb(PcvUringCtx *uring, gint result, gpointer data);
static void _uring_write_cb(PcvUringCtx *uring, gint result, gpointer data);




















static void
_uring_post_accept(UdsServer *self)
{
    pcv_uring_submit_accept(self->uring, self->listen_fd,
                             NULL, NULL,
                             _uring_accept_cb, self);
}




















static void
_uring_accept_cb(PcvUringCtx *uring __attribute__((unused)), gint result, gpointer data)
{
    UdsServer *self = data;

    if (result < 0) {




        if (result != -ECANCELED)
            g_warning("[uring-uds] accept failed: %s", strerror(-result));
        _uring_post_accept(self);
        return;
    }

    int client_fd = result;
    self->connection_count++;


    UringConnCtx *ctx = g_new0(UringConnCtx, 1);
    ctx->server   = self;
    ctx->fd       = client_fd;
    ctx->buf_size = 65536;
    ctx->buffer   = g_malloc(ctx->buf_size);


    pcv_uring_submit_recv(self->uring, client_fd,
                           ctx->buffer, ctx->buf_size - 1,
                           _uring_read_cb, ctx);


    _uring_post_accept(self);
}


























static void
_uring_read_cb(PcvUringCtx *uring __attribute__((unused)), gint result, gpointer data)
{
    UringConnCtx *ctx = data;

    if (result <= 0) {


        close(ctx->fd);
        g_free(ctx->buffer);
        g_free(ctx);
        return;
    }

    ctx->buffer[result] = '\0';

    if (!ctx->server->dispatcher) {
        close(ctx->fd);
        g_free(ctx->buffer);
        g_free(ctx);
        return;
    }













    GError *sock_err = NULL;



    GSocket *gsock = g_socket_new_from_fd(ctx->fd, &sock_err);
    if (!gsock) {


        if (sock_err) {
            g_warning("[uring-uds] GSocket wrap failed: %s", sock_err->message);
            g_error_free(sock_err);
        }
        close(ctx->fd);
        g_free(ctx->buffer);
        g_free(ctx);
        return;
    }





    GSocketConnection *conn = g_socket_connection_factory_create_connection(gsock);
    g_object_unref(gsock);

    if (!conn) {
        close(ctx->fd);
        g_free(ctx->buffer);
        g_free(ctx);
        return;
    }


    purecvisor_dispatcher_dispatch(ctx->server->dispatcher,
                                   ctx->server,
                                   conn,
                                   ctx->buffer);




    g_object_unref(conn);

    g_free(ctx->buffer);
    g_free(ctx);
}





























static gboolean
_uring_listen_start(UdsServer *self, GError **error)
{

    if (g_file_test(self->socket_path, G_FILE_TEST_EXISTS))
        unlink(self->socket_path);





    self->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (self->listen_fd < 0) {
        g_set_error(error, g_quark_from_static_string("uds"), 1,
                    "socket() failed: %s", strerror(errno));
        return FALSE;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, self->socket_path, sizeof(addr.sun_path));


    mode_t old_umask = umask(0111);

    if (bind(self->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        umask(old_umask);
        g_set_error(error, g_quark_from_static_string("uds"), 2,
                    "bind(%s) failed: %s", self->socket_path, strerror(errno));
        close(self->listen_fd);
        return FALSE;
    }

    umask(old_umask);

    if (listen(self->listen_fd, 128) < 0) {
        g_set_error(error, g_quark_from_static_string("uds"), 3,
                    "listen() failed: %s", strerror(errno));
        close(self->listen_fd);
        return FALSE;
    }




    GError *uring_err = NULL;
    self->uring = pcv_uring_new(PCV_URING_DEFAULT_QUEUE_DEPTH, &uring_err);
    if (!self->uring) {
        g_set_error(error, g_quark_from_static_string("uds"), 4,
                    "io_uring init failed: %s",
                    uring_err ? uring_err->message : "unknown");
        if (uring_err) g_error_free(uring_err);
        close(self->listen_fd);
        return FALSE;
    }


    _uring_post_accept(self);

    self->uring_mode = TRUE;
    g_message("UDS Server listening on %s (io_uring mode, queue_depth=%u)",
              self->socket_path, PCV_URING_DEFAULT_QUEUE_DEPTH);
    return TRUE;
}
















static void __attribute__((unused))
_uring_write_cb(PcvUringCtx *uring __attribute__((unused)), gint result __attribute__((unused)),
                gpointer data __attribute__((unused)))
{
}

#endif
















typedef struct {
    UdsServer         *server;
    GSocketConnection *connection;
    gchar             *buffer;
    gsize              buf_size;
} ReadCtx;






















static void on_read_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    ReadCtx *ctx = (ReadCtx *)user_data;
    GError *error = NULL;


    gssize bytes_read = g_input_stream_read_finish(G_INPUT_STREAM(source), res, &error);

    if (bytes_read > 0) {

        ctx->buffer[bytes_read] = '\0';

        if (ctx->server->dispatcher) {








            purecvisor_dispatcher_dispatch(ctx->server->dispatcher,
                                           ctx->server,
                                           ctx->connection,
                                           ctx->buffer);
        } else {
            g_warning("No dispatcher set for UdsServer");
            g_io_stream_close(G_IO_STREAM(ctx->connection), NULL, NULL);
        }
    } else {

        if (bytes_read < 0 && error) {
            g_warning("UDS read error: %s", error->message);
            g_error_free(error);
        }

        g_io_stream_close(G_IO_STREAM(ctx->connection), NULL, NULL);
    }


    g_object_unref(ctx->connection);
    g_free(ctx->buffer);
    g_free(ctx);
}


























static gboolean on_incoming_connection(GSocketService *service,
                                       GSocketConnection *connection,
                                       GObject *source_object,
                                       gpointer user_data) {
    UdsServer *self = PURECVISOR_UDS_SERVER(user_data);

    (void)service;
    (void)source_object;


    ReadCtx *ctx = g_new0(ReadCtx, 1);
    ctx->server     = self;
    ctx->connection = g_object_ref(connection);
    ctx->buf_size   = 65536;
    ctx->buffer     = g_malloc(ctx->buf_size);


    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    g_input_stream_read_async(input,
                               ctx->buffer,
                               ctx->buf_size - 1,
                               G_PRIORITY_DEFAULT,
                               NULL,
                               on_read_done,
                               ctx);

    return TRUE;
}






















UdsServer *uds_server_new(const gchar *socket_path) {
    UdsServer *self = g_object_new(PURECVISOR_TYPE_UDS_SERVER, NULL);
    self->socket_path = g_strdup(socket_path);
    return self;
}




















void uds_server_set_dispatcher(UdsServer *self, PureCVisorDispatcher *dispatcher) {

    if (self->dispatcher) g_object_unref(self->dispatcher);

    self->dispatcher = g_object_ref(dispatcher);
}








































static int _sd_listen_fds(void) {
    const gchar *pid_str = g_getenv("LISTEN_PID");
    const gchar *fds_str = g_getenv("LISTEN_FDS");


    if (!pid_str || !fds_str) return 0;


    pid_t expected = (pid_t)g_ascii_strtoll(pid_str, NULL, 10);
    if (expected != getpid()) return 0;


    int n = (int)g_ascii_strtoll(fds_str, NULL, 10);
    return n > 0 ? n : 0;
}



























gboolean uds_server_start(UdsServer *self, GError **error) {
    GError *err = NULL;

#if PCV_USE_URING




    {
        GError *uring_err = NULL;
        if (_uring_listen_start(self, &uring_err)) {
            return TRUE;
        }

        g_message("io_uring UDS init failed (%s) — falling back to GSocketService",
                  uring_err ? uring_err->message : "unknown");
        if (uring_err) g_error_free(uring_err);
        self->uring_mode = FALSE;
    }
#endif


    self->service = g_socket_service_new();

    int sd_fds = _sd_listen_fds();
    if (sd_fds > 0) {





        GSocket *sock = g_socket_new_from_fd(3, &err);
        if (!sock) {
            g_propagate_error(error, err);
            return FALSE;
        }
        if (!g_socket_listener_add_socket(G_SOCKET_LISTENER(self->service),
                                           sock, NULL, &err)) {
            g_propagate_error(error, err);
            g_object_unref(sock);
            return FALSE;
        }
        g_object_unref(sock);
        g_message("UDS Server using systemd socket activation (fd=3)");

    } else {





        if (g_file_test(self->socket_path, G_FILE_TEST_EXISTS)) {
            unlink(self->socket_path);
        }


        GSocketAddress *address = g_unix_socket_address_new(self->socket_path);


        mode_t old_umask = umask(0111);

        if (!g_socket_listener_add_address(G_SOCKET_LISTENER(self->service),
                                           address,
                                           G_SOCKET_TYPE_STREAM,
                                           G_SOCKET_PROTOCOL_DEFAULT,
                                           NULL, NULL, &err)) {
            umask(old_umask);
            g_propagate_error(error, err);
            g_object_unref(address);
            return FALSE;
        }

        umask(old_umask);
        g_object_unref(address);

        g_message("UDS Server listening on %s", self->socket_path);
    }








    g_signal_connect(self->service, "incoming", G_CALLBACK(on_incoming_connection), self);


    g_socket_service_start(self->service);

    return TRUE;
}





















void uds_server_stop(UdsServer *self) {
#if PCV_USE_URING
    if (self->uring_mode && self->uring) {
        pcv_uring_free(self->uring);
        self->uring = NULL;
        if (self->listen_fd >= 0) {
            close(self->listen_fd);
            self->listen_fd = -1;
        }
        return;
    }
#endif
    if (self->service)
        g_socket_service_stop(self->service);
}


































void pure_uds_server_send_response(UdsServer *self, GSocketConnection *connection, const gchar *response) {
    (void)self;


    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    GError *error = NULL;




    if (!g_output_stream_write_all(output, response, strlen(response), NULL, NULL, &error)) {
        g_warning("Failed to send response: %s", error->message);
        g_error_free(error);
    }








    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
}
