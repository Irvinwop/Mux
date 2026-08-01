#ifndef MUX_IPC_H
#define MUX_IPC_H

#include <glib.h>

typedef struct _MuxIpc MuxIpc;

typedef void (*MuxIpcCommandFunc)(
    MuxIpc *ipc,
    const gchar *command,
    const gchar *argument,
    gpointer user_data);

MuxIpc *mux_ipc_connect(
    const gchar *layer,
    const gchar *initial_uri,
    MuxIpcCommandFunc command_func,
    gpointer user_data);
void mux_ipc_free(MuxIpc *ipc);

void mux_ipc_state(MuxIpc *ipc, const gchar *uri, const gchar *title);
void mux_ipc_focus(MuxIpc *ipc, gboolean focused);
void mux_ipc_layer(MuxIpc *ipc, const gchar *layer);
void mux_ipc_prompt(MuxIpc *ipc);

const gchar *mux_ipc_id(MuxIpc *ipc);

#endif
