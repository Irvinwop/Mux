#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
    const gchar *title;
    const gchar *uri;
    const gchar *edit;
    guint columns;
    guint rows;
    gboolean editing;
    gboolean replace_on_type;
} MuxBarRenderState;

/* Pure presentation: never modifies the address or performs navigation. */
gchar *mux_bar_render(const MuxBarRenderState *state);

G_END_DECLS
