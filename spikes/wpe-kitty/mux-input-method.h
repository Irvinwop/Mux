#ifndef MUX_INPUT_METHOD_H
#define MUX_INPUT_METHOD_H

#include <wpe/webkit.h>

G_BEGIN_DECLS

#define MUX_TYPE_INPUT_METHOD_CONTEXT \
    (mux_input_method_context_get_type())

G_DECLARE_FINAL_TYPE(MuxInputMethodContext,
                     mux_input_method_context,
                     MUX,
                     INPUT_METHOD_CONTEXT,
                     WebKitInputMethodContext)

MuxInputMethodContext *mux_input_method_context_new(void);

gboolean mux_input_method_context_commit(MuxInputMethodContext *context,
                                         const guint8 *text,
                                         gsize length,
                                         GError **error);

gboolean mux_input_method_context_is_focused(
    const MuxInputMethodContext *context);

G_END_DECLS

#endif
