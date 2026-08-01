#include "mux-ipc.h"
#include "mux-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct _MuxIpc {
    int fd;
    guint source;
    GByteArray *input;
    gchar *id;
    MuxIpcCommandFunc command_func;
    gpointer user_data;
};

static void ipc_disconnect(MuxIpc *ipc)
{
    if (ipc->fd >= 0)
        close(ipc->fd);
    ipc->fd = -1;
}

static void handle_server_line(MuxIpc *ipc, const gchar *line)
{
    gchar **fields = g_strsplit(line, "\t", 3);
    if (g_strcmp0(fields[0], "DO") == 0 && fields[1] && ipc->command_func) {
        gchar *argument = mux_decode(fields[2] ? fields[2] : "");
        ipc->command_func(ipc, fields[1], argument, ipc->user_data);
        g_free(argument);
    }
    g_strfreev(fields);
}

static void parse_input(MuxIpc *ipc)
{
    while (TRUE) {
        guint newline = 0;
        gboolean found = FALSE;
        for (; newline < ipc->input->len; newline++) {
            if (ipc->input->data[newline] == '\n') {
                found = TRUE;
                break;
            }
        }
        if (!found)
            return;

        gchar *line = g_strndup((const gchar *)ipc->input->data, newline);
        g_strchomp(line);
        handle_server_line(ipc, line);
        g_free(line);
        g_byte_array_remove_range(ipc->input, 0, newline + 1);
    }
}

static gboolean ipc_ready(gint fd, GIOCondition condition, gpointer user_data)
{
    MuxIpc *ipc = user_data;
    if (condition & (G_IO_HUP | G_IO_ERR)) {
        ipc->source = 0;
        ipc_disconnect(ipc);
        return G_SOURCE_REMOVE;
    }

    guint8 buffer[4096];
    while (TRUE) {
        ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count > 0) {
            g_byte_array_append(ipc->input, buffer, (guint)count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        ipc->source = 0;
        ipc_disconnect(ipc);
        return G_SOURCE_REMOVE;
    }

    parse_input(ipc);
    return G_SOURCE_CONTINUE;
}

MuxIpc *mux_ipc_connect(
    const gchar *layer,
    const gchar *initial_uri,
    MuxIpcCommandFunc command_func,
    gpointer user_data)
{
    int fd = mux_connect_socket();
    if (fd < 0)
        return NULL;

    MuxIpc *ipc = g_new0(MuxIpc, 1);
    ipc->fd = fd;
    ipc->input = g_byte_array_new();
    ipc->id = g_uuid_string_random();
    ipc->command_func = command_func;
    ipc->user_data = user_data;

    const gchar *kitty_window = g_getenv("KITTY_WINDOW_ID");
    const gchar *kitty_socket = g_getenv("KITTY_LISTEN_ON");
    gchar *id = mux_encode(ipc->id);
    gchar *kitty = mux_encode(kitty_window ? kitty_window : "");
    gchar *socket = mux_encode(kitty_socket ? kitty_socket : "");
    gchar *encoded_layer = mux_encode(layer ? layer : "main");
    gchar *uri = mux_encode(initial_uri ? initial_uri : "");
    gboolean sent = mux_send_line(
        fd,
        "VIEW\t%s\t%ld\t%s\t%s\t%s\t%s",
        id,
        (long)getpid(),
        kitty,
        socket,
        encoded_layer,
        uri);
    g_free(uri);
    g_free(encoded_layer);
    g_free(socket);
    g_free(kitty);
    g_free(id);

    if (!sent) {
        mux_ipc_free(ipc);
        return NULL;
    }

    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    ipc->source = g_unix_fd_add(
        fd,
        G_IO_IN | G_IO_HUP | G_IO_ERR,
        ipc_ready,
        ipc);
    return ipc;
}

void mux_ipc_free(MuxIpc *ipc)
{
    if (!ipc)
        return;
    if (ipc->fd >= 0)
        mux_send_line(ipc->fd, "BYE");
    if (ipc->source)
        g_source_remove(ipc->source);
    ipc_disconnect(ipc);
    g_clear_pointer(&ipc->input, g_byte_array_unref);
    g_free(ipc->id);
    g_free(ipc);
}

void mux_ipc_state(MuxIpc *ipc, const gchar *uri, const gchar *title)
{
    if (!ipc || ipc->fd < 0)
        return;
    gchar *encoded_uri = mux_encode(uri);
    gchar *encoded_title = mux_encode(title);
    mux_send_line(ipc->fd, "STATE\t%s\t%s", encoded_uri, encoded_title);
    g_free(encoded_title);
    g_free(encoded_uri);
}

void mux_ipc_focus(MuxIpc *ipc, gboolean focused)
{
    if (ipc && ipc->fd >= 0)
        mux_send_line(ipc->fd, "FOCUS\t%d", focused ? 1 : 0);
}

void mux_ipc_layer(MuxIpc *ipc, const gchar *layer)
{
    if (!ipc || ipc->fd < 0)
        return;
    gchar *encoded = mux_encode(layer);
    mux_send_line(ipc->fd, "LAYER\t%s", encoded);
    g_free(encoded);
}

void mux_ipc_prompt(MuxIpc *ipc)
{
    if (ipc && ipc->fd >= 0)
        mux_send_line(ipc->fd, "PROMPT");
}

const gchar *mux_ipc_id(MuxIpc *ipc)
{
    return ipc ? ipc->id : NULL;
}
