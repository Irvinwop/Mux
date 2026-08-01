#ifndef MUX_EXTENSION_ROUTER_H
#define MUX_EXTENSION_ROUTER_H

#include "mux-extension-protocol.h"

G_BEGIN_DECLS

typedef struct _MuxExtensionRouter MuxExtensionRouter;

typedef gboolean (*MuxExtensionOutputFunc)(MuxExtensionRouter *router,
                                           GBytes *packet,
                                           gpointer user_data,
                                           GError **error);

typedef gboolean (*MuxExtensionHandlerFunc)(MuxExtensionRouter *router,
                                            guint16 channel,
                                            GBytes *payload,
                                            gpointer user_data,
                                            GError **error);

MuxExtensionRouter *mux_extension_router_new(
    MuxExtensionOutputFunc output_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);
MuxExtensionRouter *mux_extension_router_ref(MuxExtensionRouter *router);
void mux_extension_router_unref(MuxExtensionRouter *router);

gboolean mux_extension_router_set_handler(
    MuxExtensionRouter *router,
    guint16 channel,
    MuxExtensionHandlerFunc handler_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy,
    GError **error);

gboolean mux_extension_router_send(MuxExtensionRouter *router,
                                   guint16 channel,
                                   GBytes *payload,
                                   GError **error);

gboolean mux_extension_router_dispatch(MuxExtensionRouter *router,
                                       const guint8 *packet,
                                       gsize packet_length,
                                       GError **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxExtensionRouter,
                              mux_extension_router_unref)

G_END_DECLS

#endif
