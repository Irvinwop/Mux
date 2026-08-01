#include "mux-clipboard-broker.h"

#include <string.h>

typedef struct {
    gchar *profile;
    MuxClipboardHistoryMode mode;
    MuxClipboardHistory *history;
    MuxClipboardSnapshot *current;
} ProfileClipboard;

struct _MuxClipboardBroker {
    GHashTable *profiles;
};

static void
profile_clipboard_free(ProfileClipboard *profile)
{
    if (profile == NULL)
        return;
    g_free(profile->profile);
    mux_clipboard_history_free(profile->history);
    g_clear_pointer(&profile->current, mux_clipboard_snapshot_unref);
    g_free(profile);
}

static gboolean
valid_profile_name(const gchar *profile)
{
    gsize length;

    if (profile == NULL || profile[0] == '\0')
        return FALSE;
    length = strlen(profile);
    return length <= MUX_CLIPBOARD_HISTORY_MAX_PROFILE &&
           g_utf8_validate(profile, length, NULL);
}

static ProfileClipboard *
lookup_profile(MuxClipboardBroker *broker,
               const gchar *profile,
               GError **error)
{
    ProfileClipboard *state;

    if (!valid_profile_name(profile)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "clipboard profile name is invalid");
        return NULL;
    }
    state = g_hash_table_lookup(broker->profiles, profile);
    if (state == NULL)
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "clipboard profile is not registered");
    return state;
}

MuxClipboardBroker *
mux_clipboard_broker_new(void)
{
    MuxClipboardBroker *broker = g_new0(MuxClipboardBroker, 1);

    broker->profiles = g_hash_table_new_full(
        g_str_hash,
        g_str_equal,
        g_free,
        (GDestroyNotify)profile_clipboard_free);
    return broker;
}

void
mux_clipboard_broker_free(MuxClipboardBroker *broker)
{
    if (broker == NULL)
        return;
    g_hash_table_unref(broker->profiles);
    g_free(broker);
}

gboolean
mux_clipboard_broker_set_profile_mode(MuxClipboardBroker *broker,
                                      const gchar *profile,
                                      MuxClipboardHistoryMode mode,
                                      GError **error)
{
    ProfileClipboard *state;
    MuxClipboardHistory *history;

    g_return_val_if_fail(broker != NULL, FALSE);
    if (!valid_profile_name(profile) ||
        mode < MUX_CLIPBOARD_HISTORY_DISABLED ||
        mode > MUX_CLIPBOARD_HISTORY_EPHEMERAL) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "clipboard profile registration is invalid");
        return FALSE;
    }

    state = g_hash_table_lookup(broker->profiles, profile);
    if (state != NULL && state->mode == mode)
        return TRUE;
    if (state != NULL) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_EXISTS,
                            "clipboard profile is already registered with a different mode");
        return FALSE;
    }

    history = mux_clipboard_history_new(profile, mode);
    if (history == NULL) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "clipboard profile history could not be created");
        return FALSE;
    }

    if (g_hash_table_size(broker->profiles) >=
        MUX_CLIPBOARD_BROKER_MAX_PROFILES) {
        mux_clipboard_history_free(history);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NO_SPACE,
                            "too many clipboard profiles are registered");
        return FALSE;
    }

    state = g_new0(ProfileClipboard, 1);
    state->profile = g_strdup(profile);
    state->mode = mode;
    state->history = history;
    g_hash_table_insert(broker->profiles, g_strdup(profile), state);
    return TRUE;
}

gboolean
mux_clipboard_broker_remove_profile(MuxClipboardBroker *broker,
                                    const gchar *profile)
{
    g_return_val_if_fail(broker != NULL, FALSE);
    if (!valid_profile_name(profile))
        return FALSE;
    return g_hash_table_remove(broker->profiles, profile);
}

MuxClipboardHistoryAddResult
mux_clipboard_broker_observe(MuxClipboardBroker *broker,
                             const gchar *profile,
                             const MuxClipboardSnapshot *snapshot,
                             gint64 created_us,
                             const gchar *source_origin,
                             guint64 source_view_id,
                             guint64 *history_entry_id,
                             GError **error)
{
    ProfileClipboard *state;
    MuxClipboardSnapshot *copy;

    g_return_val_if_fail(broker != NULL,
                         MUX_CLIPBOARD_HISTORY_IGNORED);
    g_return_val_if_fail(snapshot != NULL,
                         MUX_CLIPBOARD_HISTORY_IGNORED);
    state = lookup_profile(broker, profile, error);
    if (state == NULL)
        return MUX_CLIPBOARD_HISTORY_IGNORED;

    copy = mux_clipboard_snapshot_dup_sealed(snapshot);
    if (copy == NULL) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "clipboard snapshot could not be copied");
        return MUX_CLIPBOARD_HISTORY_IGNORED;
    }
    g_clear_pointer(&state->current, mux_clipboard_snapshot_unref);
    state->current = copy;

    return mux_clipboard_history_add(state->history,
                                     snapshot,
                                     created_us,
                                     source_origin,
                                     source_view_id,
                                     history_entry_id,
                                     error);
}

MuxClipboardSnapshot *
mux_clipboard_broker_get_current(MuxClipboardBroker *broker,
                                 const gchar *profile,
                                 GError **error)
{
    ProfileClipboard *state;

    g_return_val_if_fail(broker != NULL, NULL);
    state = lookup_profile(broker, profile, error);
    if (state == NULL)
        return NULL;
    if (state->current == NULL) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "clipboard profile has no current snapshot");
        return NULL;
    }
    return mux_clipboard_snapshot_ref(state->current);
}

void
mux_clipboard_broker_summary_free(MuxClipboardBrokerSummary *summary)
{
    if (summary == NULL)
        return;
    g_free(summary->source_origin);
    g_free(summary->preview);
    g_free(summary);
}

GPtrArray *
mux_clipboard_broker_list(MuxClipboardBroker *broker,
                          const gchar *profile,
                          GError **error)
{
    ProfileClipboard *state;
    GPtrArray *summaries;
    guint i;

    g_return_val_if_fail(broker != NULL, NULL);
    state = lookup_profile(broker, profile, error);
    if (state == NULL)
        return NULL;

    summaries = g_ptr_array_new_with_free_func(
        (GDestroyNotify)mux_clipboard_broker_summary_free);
    for (i = 0; i < mux_clipboard_history_get_count(state->history); i++) {
        const MuxClipboardHistoryEntry *entry =
            mux_clipboard_history_get(state->history, i);
        const MuxClipboardSnapshot *snapshot =
            mux_clipboard_history_entry_get_snapshot(entry);
        MuxClipboardBrokerSummary *summary =
            g_new0(MuxClipboardBrokerSummary, 1);

        summary->id = mux_clipboard_history_entry_get_id(entry);
        summary->created_us =
            mux_clipboard_history_entry_get_created_us(entry);
        summary->source_origin = g_strdup(
            mux_clipboard_history_entry_get_source_origin(entry));
        summary->source_view_id =
            mux_clipboard_history_entry_get_source_view_id(entry);
        summary->pinned =
            mux_clipboard_history_entry_get_pinned(entry);
        summary->format_count =
            mux_clipboard_snapshot_get_count(snapshot);
        summary->total_bytes =
            mux_clipboard_snapshot_get_total_bytes(snapshot);
        summary->preview =
            mux_clipboard_history_entry_dup_preview(entry, 120);
        g_ptr_array_add(summaries, summary);
    }
    return summaries;
}

MuxClipboardSnapshot *
mux_clipboard_broker_select(MuxClipboardBroker *broker,
                            const gchar *profile,
                            guint64 entry_id,
                            GError **error)
{
    ProfileClipboard *state;
    MuxClipboardSnapshot *snapshot;

    g_return_val_if_fail(broker != NULL, NULL);
    state = lookup_profile(broker, profile, error);
    if (state == NULL)
        return NULL;

    snapshot = mux_clipboard_history_select(state->history,
                                            entry_id,
                                            error);
    if (snapshot == NULL)
        return NULL;
    g_clear_pointer(&state->current, mux_clipboard_snapshot_unref);
    state->current = mux_clipboard_snapshot_ref(snapshot);
    return snapshot;
}

gboolean
mux_clipboard_broker_set_pinned(MuxClipboardBroker *broker,
                                const gchar *profile,
                                guint64 entry_id,
                                gboolean pinned,
                                GError **error)
{
    ProfileClipboard *state;

    g_return_val_if_fail(broker != NULL, FALSE);
    state = lookup_profile(broker, profile, error);
    return state != NULL &&
           mux_clipboard_history_set_pinned(state->history,
                                            entry_id,
                                            pinned,
                                            error);
}

gboolean
mux_clipboard_broker_delete(MuxClipboardBroker *broker,
                            const gchar *profile,
                            guint64 entry_id,
                            GError **error)
{
    ProfileClipboard *state;

    g_return_val_if_fail(broker != NULL, FALSE);
    state = lookup_profile(broker, profile, error);
    return state != NULL &&
           mux_clipboard_history_delete(state->history,
                                        entry_id,
                                        error);
}

guint
mux_clipboard_broker_clear(MuxClipboardBroker *broker,
                           const gchar *profile,
                           gboolean include_pinned,
                           GError **error)
{
    ProfileClipboard *state;

    g_return_val_if_fail(broker != NULL, 0);
    state = lookup_profile(broker, profile, error);
    return state != NULL
               ? mux_clipboard_history_clear(state->history,
                                             include_pinned)
               : 0;
}
