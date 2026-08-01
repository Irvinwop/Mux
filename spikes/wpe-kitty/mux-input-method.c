#include "mux-input-method.h"

#include <string.h>

#define MUX_INPUT_METHOD_MAX_COMMIT (64U * 1024U)

struct _MuxInputMethodContext {
    WebKitInputMethodContext parent_instance;
    gboolean focused;
    gboolean preedit_enabled;
    gint cursor_x;
    gint cursor_y;
    gint cursor_width;
    gint cursor_height;
};

G_DEFINE_TYPE(MuxInputMethodContext,
              mux_input_method_context,
              WEBKIT_TYPE_INPUT_METHOD_CONTEXT)

static void
input_method_set_enable_preedit(WebKitInputMethodContext *base,
                                gboolean enabled)
{
    MuxInputMethodContext *context = MUX_INPUT_METHOD_CONTEXT(base);

    context->preedit_enabled = enabled;
}

static void
input_method_get_preedit(WebKitInputMethodContext *base,
                         gchar **text,
                         GList **underlines,
                         guint *cursor_offset)
{
    (void)base;
    if (text)
        *text = g_strdup("");
    if (underlines)
        *underlines = NULL;
    if (cursor_offset)
        *cursor_offset = 0;
}

static gboolean
input_method_filter_key_event(WebKitInputMethodContext *base,
                              gpointer key_event)
{
    (void)base;
    (void)key_event;
    return FALSE;
}

static void
input_method_notify_focus_in(WebKitInputMethodContext *base)
{
    MUX_INPUT_METHOD_CONTEXT(base)->focused = TRUE;
}

static void
input_method_notify_focus_out(WebKitInputMethodContext *base)
{
    MUX_INPUT_METHOD_CONTEXT(base)->focused = FALSE;
}

static void
input_method_notify_cursor_area(WebKitInputMethodContext *base,
                                gint x,
                                gint y,
                                gint width,
                                gint height)
{
    MuxInputMethodContext *context = MUX_INPUT_METHOD_CONTEXT(base);

    context->cursor_x = x;
    context->cursor_y = y;
    context->cursor_width = width;
    context->cursor_height = height;
}

static void
input_method_notify_surrounding(WebKitInputMethodContext *base,
                                const gchar *text,
                                guint length,
                                guint cursor_index,
                                guint selection_index)
{
    (void)base;
    (void)text;
    (void)length;
    (void)cursor_index;
    (void)selection_index;
}

static void
input_method_reset(WebKitInputMethodContext *base)
{
    MuxInputMethodContext *context = MUX_INPUT_METHOD_CONTEXT(base);

    context->preedit_enabled = FALSE;
}

static void
mux_input_method_context_class_init(MuxInputMethodContextClass *klass)
{
    WebKitInputMethodContextClass *input_class =
        WEBKIT_INPUT_METHOD_CONTEXT_CLASS(klass);

    input_class->set_enable_preedit = input_method_set_enable_preedit;
    input_class->get_preedit = input_method_get_preedit;
    input_class->filter_key_event = input_method_filter_key_event;
    input_class->notify_focus_in = input_method_notify_focus_in;
    input_class->notify_focus_out = input_method_notify_focus_out;
    input_class->notify_cursor_area = input_method_notify_cursor_area;
    input_class->notify_surrounding = input_method_notify_surrounding;
    input_class->reset = input_method_reset;
}

static void
mux_input_method_context_init(MuxInputMethodContext *context)
{
    (void)context;
}

MuxInputMethodContext *
mux_input_method_context_new(void)
{
    return g_object_new(MUX_TYPE_INPUT_METHOD_CONTEXT, NULL);
}

gboolean
mux_input_method_context_commit(MuxInputMethodContext *context,
                                const guint8 *text,
                                gsize length,
                                GError **error)
{
    g_autofree gchar *committed = NULL;

    g_return_val_if_fail(
        MUX_IS_INPUT_METHOD_CONTEXT((gpointer)context), FALSE);
    if (!text || !length || length > MUX_INPUT_METHOD_MAX_COMMIT ||
        memchr(text, '\0', length) ||
        !g_utf8_validate((const gchar *)text, length, NULL)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "committed text is not bounded UTF-8");
        return FALSE;
    }
    if (!context->focused)
        return FALSE;

    committed = g_strndup((const gchar *)text, length);
    g_signal_emit_by_name(context, "committed", committed);
    return TRUE;
}

gboolean
mux_input_method_context_is_focused(
    const MuxInputMethodContext *context)
{
    g_return_val_if_fail(
        MUX_IS_INPUT_METHOD_CONTEXT((gpointer)context), FALSE);
    return context->focused;
}
