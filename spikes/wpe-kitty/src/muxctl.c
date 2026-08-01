#include "mux-protocol.h"

#include <errno.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RESPONSE_TIMEOUT_MS 5000

static void usage(void)
{
    g_printerr(
        "usage:\n"
        "  muxctl list\n"
        "  muxctl status\n"
        "  muxctl stop\n"
        "  muxctl open [view-id] URI\n"
        "  muxctl back|forward|reload|quit [view-id]\n"
        "  muxctl focus VIEW-ID\n"
        "  muxctl layer NAME\n"
        "  muxctl move VIEW-ID LAYER\n"
        "  muxctl watch\n");
}

static gchar *join_arguments(int argc, char **argv, int start)
{
    GString *joined = g_string_new(NULL);
    for (int i = start; i < argc; i++) {
        if (joined->len)
            g_string_append_c(joined, ' ');
        g_string_append(joined, argv[i]);
    }
    return g_string_free(joined, FALSE);
}

static void print_view(gchar **fields, guint count)
{
    if (count < 8)
        return;
    gchar *id = mux_decode(fields[1]);
    gchar *layer = mux_decode(fields[2]);
    gchar *uri = mux_decode(fields[3]);
    gchar *title = mux_decode(fields[4]);
    gchar *kitty = mux_decode(fields[6]);
    g_print(
        "%s%s\tlayer=%s\tpid=%s\tkitty=%s\t%s\t%s\n",
        g_strcmp0(fields[5], "1") == 0 ? "* " : "  ",
        id,
        layer,
        fields[7],
        kitty,
        uri,
        title);
    g_free(kitty);
    g_free(title);
    g_free(uri);
    g_free(layer);
    g_free(id);
}

static void print_protocol_line(const gchar *line)
{
    gchar **fields = g_strsplit(line, "\t", -1);
    guint count = g_strv_length(fields);
    if (count && g_strcmp0(fields[0], "VIEW") == 0) {
        print_view(fields, count);
    } else if (count >= 5 && g_strcmp0(fields[0], "STATUS") == 0) {
        gchar *active = mux_decode(fields[2]);
        gchar *layer = mux_decode(fields[3]);
        g_print(
            "revision=%s active=%s layer=%s views=%s\n",
            fields[1],
            active,
            layer,
            fields[4]);
        g_free(layer);
        g_free(active);
    } else if (count >= 2 && g_strcmp0(fields[0], "ERR") == 0) {
        gchar *message = mux_decode(fields[1]);
        g_printerr("muxctl: %s\n", message);
        g_free(message);
    } else if (g_strcmp0(line, "OK") != 0 &&
               g_strcmp0(line, "END") != 0 &&
               !g_str_has_prefix(line, "BEGIN\t")) {
        g_print("%s\n", line);
    }
    g_strfreev(fields);
}

static gint remaining_ms(gint64 deadline_us)
{
    gint64 remaining_us = deadline_us - g_get_monotonic_time();

    if (remaining_us <= 0)
        return 0;
    return (gint)MIN((remaining_us + 999) / 1000, (gint64)G_MAXINT);
}

static int read_response(int fd, gboolean stream)
{
    int result = EXIT_SUCCESS;
    gint64 deadline_us = stream
        ? 0
        : g_get_monotonic_time() +
              ((gint64)RESPONSE_TIMEOUT_MS * 1000);

    while (TRUE) {
        gint timeout_ms = stream ? -1 : remaining_ms(deadline_us);
        gchar *line;

        if (!stream && timeout_ms == 0) {
            g_printerr("muxctl: timed out waiting for a complete response\n");
            return EXIT_FAILURE;
        }

        line = mux_read_line(fd, timeout_ms);
        if (!line) {
            if (stream)
                break;
            if (remaining_ms(deadline_us) == 0)
                g_printerr("muxctl: timed out waiting for a complete response\n");
            else
                g_printerr("muxctl: connection closed before a complete response\n");
            return EXIT_FAILURE;
        }
        if (g_str_has_prefix(line, "ERR\t"))
            result = EXIT_FAILURE;
        print_protocol_line(line);
        gboolean complete =
            g_strcmp0(line, "OK") == 0 ||
            g_strcmp0(line, "END") == 0 ||
            g_str_has_prefix(line, "ERR\t") ||
            g_str_has_prefix(line, "STATUS\t") ||
            g_str_has_prefix(line, "PONG\t");
        g_free(line);
        if (!stream && complete)
            break;
    }
    return result;
}

static gboolean send_target_command(
    int fd,
    const gchar *command,
    const gchar *target,
    const gchar *argument)
{
    gchar *encoded_target = mux_encode(target ? target : "-");
    gchar *encoded_argument = mux_encode(argument ? argument : "");
    gboolean ok;
    if (argument)
        ok = mux_send_line(
            fd,
            "CTL\t%s\t%s\t%s",
            command,
            encoded_target,
            encoded_argument);
    else
        ok = mux_send_line(fd, "CTL\t%s\t%s", command, encoded_target);
    g_free(encoded_argument);
    g_free(encoded_target);
    return ok;
}

int main(int argc, char **argv)
{
    const gchar *command;

    if (argc < 2) {
        usage();
        return EXIT_FAILURE;
    }

    command = argv[1];
    if (g_strcmp0(command, "stop") == 0 && argc != 2) {
        usage();
        return EXIT_FAILURE;
    }

    int fd = mux_connect_socket();
    if (fd < 0) {
        if (g_strcmp0(command, "stop") == 0 &&
            (errno == ENOENT || errno == ECONNREFUSED)) {
            g_print("muxd is not running\n");
            return EXIT_SUCCESS;
        }
        g_printerr("muxctl: muxd is unavailable: %s\n", g_strerror(errno));
        return EXIT_FAILURE;
    }

    gboolean sent = FALSE;
    gboolean stream = FALSE;

    if (g_strcmp0(command, "list") == 0) {
        sent = mux_send_line(fd, "CTL\tLIST");
    } else if (g_strcmp0(command, "status") == 0) {
        sent = mux_send_line(fd, "CTL\tSTATUS");
    } else if (g_strcmp0(command, "stop") == 0) {
        sent = mux_send_line(fd, "CTL\tSTOP");
    } else if (g_strcmp0(command, "watch") == 0) {
        sent = mux_send_line(fd, "SUB");
        stream = TRUE;
    } else if (g_strcmp0(command, "layer") == 0 && argc == 3) {
        gchar *layer = mux_encode(argv[2]);
        sent = mux_send_line(fd, "CTL\tLAYER\t%s", layer);
        g_free(layer);
    } else if (g_strcmp0(command, "focus") == 0 && argc == 3) {
        gchar *target = mux_encode(argv[2]);
        sent = mux_send_line(fd, "CTL\tFOCUS\t%s", target);
        g_free(target);
    } else if (g_strcmp0(command, "move") == 0 && argc == 4) {
        gchar *target = mux_encode(argv[2]);
        gchar *layer = mux_encode(argv[3]);
        sent = mux_send_line(fd, "CTL\tMOVE\t%s\t%s", target, layer);
        g_free(layer);
        g_free(target);
    } else if (g_strcmp0(command, "open") == 0 && argc >= 3) {
        const gchar *target = "-";
        int uri_start = 2;
        if (argc >= 4) {
            target = argv[2];
            uri_start = 3;
        }
        gchar *uri = join_arguments(argc, argv, uri_start);
        sent = send_target_command(fd, "OPEN", target, uri);
        g_free(uri);
    } else if ((g_strcmp0(command, "back") == 0 ||
                g_strcmp0(command, "forward") == 0 ||
                g_strcmp0(command, "reload") == 0 ||
                g_strcmp0(command, "quit") == 0) &&
               argc <= 3) {
        gchar *upper = g_ascii_strup(command, -1);
        sent = send_target_command(
            fd,
            upper,
            argc == 3 ? argv[2] : "-",
            NULL);
        g_free(upper);
    } else {
        usage();
        close(fd);
        return EXIT_FAILURE;
    }

    if (!sent) {
        g_printerr("muxctl: send failed: %s\n", g_strerror(errno));
        close(fd);
        return EXIT_FAILURE;
    }

    int result = read_response(fd, stream);
    close(fd);
    if (result == EXIT_SUCCESS && g_strcmp0(command, "stop") == 0)
        g_print("muxd stopped\n");
    return result;
}
