#ifndef MUX_SESSION_STATE_H
#define MUX_SESSION_STATE_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _MuxSessionState MuxSessionState;

typedef struct {
    guint64 id;
    gchar *profile;
    gchar *layer;
    gchar *uri;
    gchar *title;
} MuxSessionView;

MuxSessionState *mux_session_state_new(void);
void mux_session_state_free(MuxSessionState *state);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxSessionState, mux_session_state_free)

gchar *mux_session_state_default_path(void);
MuxSessionState *mux_session_state_load(const gchar *path, GError **error);
gboolean mux_session_state_save_atomic(const MuxSessionState *state,
                                       const gchar *path,
                                       GError **error);

gchar *mux_session_state_serialize(const MuxSessionState *state,
                                   gsize *length,
                                   GError **error);
MuxSessionState *mux_session_state_deserialize(const gchar *data,
                                               gsize length,
                                               GError **error);

guint64 mux_session_state_get_next_view_id(const MuxSessionState *state);
gboolean mux_session_state_set_next_view_id(MuxSessionState *state,
                                            guint64 next_view_id);

const gchar *mux_session_state_get_active_layer(const MuxSessionState *state);
gboolean mux_session_state_set_active_layer(MuxSessionState *state,
                                            const gchar *layer);
gboolean mux_session_state_add_layer(MuxSessionState *state,
                                     const gchar *layer);
guint mux_session_state_get_layer_count(const MuxSessionState *state);
const gchar *mux_session_state_get_layer(const MuxSessionState *state,
                                         guint index);

gboolean mux_session_state_upsert_view(MuxSessionState *state,
                                       guint64 id,
                                       const gchar *layer,
                                       const gchar *uri,
                                       const gchar *title);
gboolean mux_session_state_upsert_view_with_profile(
    MuxSessionState *state,
    guint64 id,
    const gchar *profile,
    const gchar *layer,
    const gchar *uri,
    const gchar *title);
gboolean mux_session_state_remove_view(MuxSessionState *state, guint64 id);
guint mux_session_state_get_view_count(const MuxSessionState *state);
const MuxSessionView *mux_session_state_get_view(
    const MuxSessionState *state,
    guint index);

G_END_DECLS

#endif
