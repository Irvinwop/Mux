#ifndef MUX_PROTOCOL_H
#define MUX_PROTOCOL_H

#include <glib.h>

#define MUX_PROTOCOL_VERSION 1
#define MUX_TOR_POLICY_VERSION 1

gchar *mux_socket_path(void);
int mux_connect_socket(void);

/* Probe on the same authenticated connection before sending private metadata.
 * Success leaves the connection available for VIEW/ENGINE/BAR registration. */
gboolean mux_require_tor_policy(int fd);

gboolean mux_send_line(int fd, const gchar *format, ...)
    G_GNUC_PRINTF(2, 3);
gchar *mux_read_line(int fd, int timeout_ms);

gchar *mux_encode(const gchar *value);
gchar *mux_decode(const gchar *value);

#endif
