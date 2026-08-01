#include "mux-clipboard-history.h"

#include <string.h>

struct _MuxClipboardHistoryEntry {
    guint64 id;
    gint64 created_us;
    gchar *profile;
    gchar *source_origin;
    guint64 source_view_id;
    MuxClipboardSnapshot *snapshot;
    gboolean pinned;
};

struct _MuxClipboardHistory {
    gchar *profile;
    MuxClipboardHistoryMode mode;
    GQueue entries;
    guint64 next_id;
    gsize total_bytes;
};

static void
history_entry_free(MuxClipboardHistoryEntry *entry)
{
    if (entry == NULL)
        return;

    g_free(entry->profile);
    g_free(entry->source_origin);
    mux_clipboard_snapshot_unref(entry->snapshot);
    g_free(entry);
}

static gboolean
valid_profile(const gchar *profile)
{
    gsize length;

    if (profile == NULL || profile[0] == '\0')
        return FALSE;
    length = strlen(profile);
    return length <= MUX_CLIPBOARD_HISTORY_MAX_PROFILE &&
           g_utf8_validate(profile, length, NULL);
}

static gboolean
valid_origin(const gchar *origin)
{
    gsize length;

    if (origin == NULL)
        return TRUE;
    length = strlen(origin);
    return length <= MUX_CLIPBOARD_HISTORY_MAX_ORIGIN &&
           g_utf8_validate(origin, length, NULL);
}

static MuxClipboardSnapshot *
history_snapshot(const MuxClipboardSnapshot *source, GError **error)
{
    MuxClipboardSnapshot *copy;
    guint i;

    copy = mux_clipboard_snapshot_new(
        mux_clipboard_snapshot_get_serial(source));
    for (i = 0; i < mux_clipboard_snapshot_get_count(source); i++) {
        const gchar *mime = NULL;
        GBytes *bytes = NULL;

        mux_clipboard_snapshot_get_item(source, i, &mime, &bytes);
        if (!mux_clipboard_snapshot_add(copy, mime, bytes, error)) {
            mux_clipboard_snapshot_unref(copy);
            return NULL;
        }
    }

    if (mux_clipboard_snapshot_get_count(copy) == 0) {
        mux_clipboard_snapshot_unref(copy);
        return NULL;
    }
    if (mux_clipboard_snapshot_get_total_bytes(copy) >
        MUX_CLIPBOARD_HISTORY_MAX_BYTES) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NO_SPACE,
                            "clipboard history entry exceeds 16 MiB");
        mux_clipboard_snapshot_unref(copy);
        return NULL;
    }

    mux_clipboard_snapshot_seal(copy);
    return copy;
}

static gboolean
snapshots_equal(const MuxClipboardSnapshot *left,
                const MuxClipboardSnapshot *right)
{
    guint i;

    if (mux_clipboard_snapshot_get_count(left) !=
            mux_clipboard_snapshot_get_count(right) ||
        mux_clipboard_snapshot_get_total_bytes(left) !=
            mux_clipboard_snapshot_get_total_bytes(right))
        return FALSE;

    for (i = 0; i < mux_clipboard_snapshot_get_count(left); i++) {
        const gchar *mime = NULL;
        GBytes *left_bytes = NULL;
        GBytes *right_bytes;

        mux_clipboard_snapshot_get_item(left, i, &mime, &left_bytes);
        right_bytes = mux_clipboard_snapshot_find(right, mime);
        if (right_bytes == NULL || !g_bytes_equal(left_bytes, right_bytes))
            return FALSE;
    }
    return TRUE;
}

static GList *
find_link(const MuxClipboardHistory *history, guint64 entry_id)
{
    GList *link;

    for (link = history->entries.head; link != NULL; link = link->next) {
        MuxClipboardHistoryEntry *entry = link->data;

        if (entry->id == entry_id)
            return link;
    }
    return NULL;
}

static gboolean
reserve_space(MuxClipboardHistory *history,
              gsize incoming_bytes,
              GError **error)
{
    g_autoptr(GPtrArray) victims = g_ptr_array_new();
    guint remaining_count = history->entries.length;
    gsize remaining_bytes = history->total_bytes;
    GList *link;
    guint i;

    if (incoming_bytes > MUX_CLIPBOARD_HISTORY_MAX_BYTES) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NO_SPACE,
                            "clipboard history entry is too large");
        return FALSE;
    }

    for (link = history->entries.tail;
         (remaining_count >= MUX_CLIPBOARD_HISTORY_MAX_ENTRIES ||
          incoming_bytes >
              MUX_CLIPBOARD_HISTORY_MAX_BYTES - remaining_bytes) &&
         link != NULL;
         link = link->prev) {
        MuxClipboardHistoryEntry *entry = link->data;

        if (entry->pinned)
            continue;
        g_ptr_array_add(victims, entry);
        remaining_count--;
        remaining_bytes -=
            mux_clipboard_snapshot_get_total_bytes(entry->snapshot);
    }

    if (remaining_count >= MUX_CLIPBOARD_HISTORY_MAX_ENTRIES ||
        incoming_bytes >
            MUX_CLIPBOARD_HISTORY_MAX_BYTES - remaining_bytes) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NO_SPACE,
                            "pinned clipboard entries occupy the history limit");
        return FALSE;
    }

    for (i = 0; i < victims->len; i++) {
        MuxClipboardHistoryEntry *entry =
            g_ptr_array_index(victims, i);

        history->total_bytes -=
            mux_clipboard_snapshot_get_total_bytes(entry->snapshot);
        g_queue_remove(&history->entries, entry);
        history_entry_free(entry);
    }
    return TRUE;
}

MuxClipboardHistory *
mux_clipboard_history_new(const gchar *profile,
                          MuxClipboardHistoryMode mode)
{
    MuxClipboardHistory *history;

    g_return_val_if_fail(valid_profile(profile), NULL);
    g_return_val_if_fail(mode >= MUX_CLIPBOARD_HISTORY_DISABLED &&
                         mode <= MUX_CLIPBOARD_HISTORY_EPHEMERAL,
                         NULL);

    history = g_new0(MuxClipboardHistory, 1);
    history->profile = g_strdup(profile);
    history->mode = mode;
    g_queue_init(&history->entries);
    return history;
}

void
mux_clipboard_history_free(MuxClipboardHistory *history)
{
    if (history == NULL)
        return;

    g_queue_clear_full(&history->entries,
                       (GDestroyNotify)history_entry_free);
    g_free(history->profile);
    g_free(history);
}

MuxClipboardHistoryAddResult
mux_clipboard_history_add(MuxClipboardHistory *history,
                          const MuxClipboardSnapshot *snapshot,
                          gint64 created_us,
                          const gchar *source_origin,
                          guint64 source_view_id,
                          guint64 *entry_id,
                          GError **error)
{
    MuxClipboardSnapshot *filtered;
    GList *link;
    MuxClipboardHistoryEntry *entry;
    gsize bytes;

    g_return_val_if_fail(history != NULL,
                         MUX_CLIPBOARD_HISTORY_IGNORED);
    g_return_val_if_fail(snapshot != NULL,
                         MUX_CLIPBOARD_HISTORY_IGNORED);
    if (entry_id != NULL)
        *entry_id = 0;

    if (history->mode == MUX_CLIPBOARD_HISTORY_DISABLED)
        return MUX_CLIPBOARD_HISTORY_IGNORED;
    if (!valid_origin(source_origin)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "clipboard source origin is invalid");
        return MUX_CLIPBOARD_HISTORY_IGNORED;
    }

    filtered = history_snapshot(snapshot, error);
    if (filtered == NULL)
        return MUX_CLIPBOARD_HISTORY_IGNORED;

    for (link = history->entries.head; link != NULL; link = link->next) {
        entry = link->data;
        if (!snapshots_equal(entry->snapshot, filtered))
            continue;

        entry->created_us = created_us > 0
                                ? created_us
                                : g_get_monotonic_time();
        g_free(entry->source_origin);
        entry->source_origin = g_strdup(source_origin);
        entry->source_view_id = source_view_id;
        g_queue_unlink(&history->entries, link);
        g_queue_push_head_link(&history->entries, link);
        if (entry_id != NULL)
            *entry_id = entry->id;
        mux_clipboard_snapshot_unref(filtered);
        return MUX_CLIPBOARD_HISTORY_DEDUPLICATED;
    }

    bytes = mux_clipboard_snapshot_get_total_bytes(filtered);
    if (!reserve_space(history, bytes, error)) {
        mux_clipboard_snapshot_unref(filtered);
        return MUX_CLIPBOARD_HISTORY_IGNORED;
    }

    entry = g_new0(MuxClipboardHistoryEntry, 1);
    entry->id = ++history->next_id;
    if (entry->id == 0)
        entry->id = ++history->next_id;
    entry->created_us = created_us > 0
                            ? created_us
                            : g_get_monotonic_time();
    entry->profile = g_strdup(history->profile);
    entry->source_origin = g_strdup(source_origin);
    entry->source_view_id = source_view_id;
    entry->snapshot = filtered;
    g_queue_push_head(&history->entries, entry);
    history->total_bytes += bytes;
    if (entry_id != NULL)
        *entry_id = entry->id;
    return MUX_CLIPBOARD_HISTORY_ADDED;
}

guint
mux_clipboard_history_get_count(const MuxClipboardHistory *history)
{
    g_return_val_if_fail(history != NULL, 0);
    return history->entries.length;
}

gsize
mux_clipboard_history_get_total_bytes(const MuxClipboardHistory *history)
{
    g_return_val_if_fail(history != NULL, 0);
    return history->total_bytes;
}

const gchar *
mux_clipboard_history_get_profile(const MuxClipboardHistory *history)
{
    g_return_val_if_fail(history != NULL, NULL);
    return history->profile;
}

MuxClipboardHistoryMode
mux_clipboard_history_get_mode(const MuxClipboardHistory *history)
{
    g_return_val_if_fail(history != NULL,
                         MUX_CLIPBOARD_HISTORY_DISABLED);
    return history->mode;
}

const MuxClipboardHistoryEntry *
mux_clipboard_history_get(const MuxClipboardHistory *history,
                          guint newest_first_index)
{
    GList *link;

    g_return_val_if_fail(history != NULL, NULL);
    link = g_list_nth(history->entries.head, newest_first_index);
    return link != NULL ? link->data : NULL;
}

const MuxClipboardHistoryEntry *
mux_clipboard_history_lookup(const MuxClipboardHistory *history,
                             guint64 entry_id)
{
    GList *link;

    g_return_val_if_fail(history != NULL, NULL);
    link = find_link(history, entry_id);
    return link != NULL ? link->data : NULL;
}

MuxClipboardSnapshot *
mux_clipboard_history_select(MuxClipboardHistory *history,
                             guint64 entry_id,
                             GError **error)
{
    GList *link;
    MuxClipboardHistoryEntry *entry;

    g_return_val_if_fail(history != NULL, NULL);
    link = find_link(history, entry_id);
    if (link == NULL) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "clipboard history entry was not found");
        return NULL;
    }

    entry = link->data;
    g_queue_unlink(&history->entries, link);
    g_queue_push_head_link(&history->entries, link);
    return mux_clipboard_snapshot_ref(entry->snapshot);
}

gboolean
mux_clipboard_history_set_pinned(MuxClipboardHistory *history,
                                 guint64 entry_id,
                                 gboolean pinned,
                                 GError **error)
{
    GList *link;
    MuxClipboardHistoryEntry *entry;

    g_return_val_if_fail(history != NULL, FALSE);
    link = find_link(history, entry_id);
    if (link == NULL) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "clipboard history entry was not found");
        return FALSE;
    }

    entry = link->data;
    entry->pinned = pinned;
    return TRUE;
}

gboolean
mux_clipboard_history_delete(MuxClipboardHistory *history,
                             guint64 entry_id,
                             GError **error)
{
    GList *link;
    MuxClipboardHistoryEntry *entry;

    g_return_val_if_fail(history != NULL, FALSE);
    link = find_link(history, entry_id);
    if (link == NULL) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "clipboard history entry was not found");
        return FALSE;
    }

    entry = link->data;
    history->total_bytes -=
        mux_clipboard_snapshot_get_total_bytes(entry->snapshot);
    g_queue_delete_link(&history->entries, link);
    history_entry_free(entry);
    return TRUE;
}

guint
mux_clipboard_history_clear(MuxClipboardHistory *history,
                            gboolean include_pinned)
{
    GList *link;
    guint removed = 0;

    g_return_val_if_fail(history != NULL, 0);
    link = history->entries.head;
    while (link != NULL) {
        GList *next = link->next;
        MuxClipboardHistoryEntry *entry = link->data;

        if (include_pinned || !entry->pinned) {
            history->total_bytes -=
                mux_clipboard_snapshot_get_total_bytes(entry->snapshot);
            g_queue_delete_link(&history->entries, link);
            history_entry_free(entry);
            removed++;
        }
        link = next;
    }
    return removed;
}

guint64
mux_clipboard_history_entry_get_id(const MuxClipboardHistoryEntry *entry)
{
    g_return_val_if_fail(entry != NULL, 0);
    return entry->id;
}

gint64
mux_clipboard_history_entry_get_created_us(
    const MuxClipboardHistoryEntry *entry)
{
    g_return_val_if_fail(entry != NULL, 0);
    return entry->created_us;
}

const gchar *
mux_clipboard_history_entry_get_profile(
    const MuxClipboardHistoryEntry *entry)
{
    g_return_val_if_fail(entry != NULL, NULL);
    return entry->profile;
}

const gchar *
mux_clipboard_history_entry_get_source_origin(
    const MuxClipboardHistoryEntry *entry)
{
    g_return_val_if_fail(entry != NULL, NULL);
    return entry->source_origin;
}

guint64
mux_clipboard_history_entry_get_source_view_id(
    const MuxClipboardHistoryEntry *entry)
{
    g_return_val_if_fail(entry != NULL, 0);
    return entry->source_view_id;
}

gboolean
mux_clipboard_history_entry_get_pinned(
    const MuxClipboardHistoryEntry *entry)
{
    g_return_val_if_fail(entry != NULL, FALSE);
    return entry->pinned;
}

const MuxClipboardSnapshot *
mux_clipboard_history_entry_get_snapshot(
    const MuxClipboardHistoryEntry *entry)
{
    g_return_val_if_fail(entry != NULL, NULL);
    return entry->snapshot;
}

static gboolean
mime_has_base(const gchar *mime, const gchar *base)
{
    gsize length = strlen(base);

    return g_ascii_strncasecmp(mime, base, length) == 0 &&
           (mime[length] == '\0' || mime[length] == ';');
}

static GBytes *
find_preview_text(const MuxClipboardSnapshot *snapshot)
{
    static const gchar *const preferred[] = {
        "text/plain",
        "text/uri-list",
        NULL
    };
    guint preference;
    guint i;

    for (preference = 0; preferred[preference] != NULL; preference++) {
        for (i = 0; i < mux_clipboard_snapshot_get_count(snapshot); i++) {
            const gchar *mime = NULL;
            GBytes *bytes = NULL;

            mux_clipboard_snapshot_get_item(snapshot, i, &mime, &bytes);
            if (mime_has_base(mime, preferred[preference]))
                return bytes;
        }
    }
    return NULL;
}

static gchar *
safe_text_preview(GBytes *bytes, guint max_characters)
{
    const gchar *data;
    const gchar *cursor;
    const gchar *end;
    gsize length;
    GString *preview;
    guint characters = 0;
    gboolean pending_space = FALSE;
    gboolean truncated = FALSE;

    data = g_bytes_get_data(bytes, &length);
    if (length == 0)
        return g_strdup("(empty text)");
    if (memchr(data, '\0', length) != NULL ||
        !g_utf8_validate(data, length, NULL))
        return NULL;

    preview = g_string_new(NULL);
    cursor = data;
    end = data + length;
    while (cursor < end) {
        gunichar character = g_utf8_get_char(cursor);

        cursor = g_utf8_next_char(cursor);
        if (g_unichar_isspace(character)) {
            pending_space = preview->len > 0;
            continue;
        }
        if (g_unichar_iscntrl(character))
            continue;
        if (characters >= max_characters) {
            truncated = TRUE;
            break;
        }
        if (pending_space) {
            g_string_append_c(preview, ' ');
            pending_space = FALSE;
        }
        g_string_append_unichar(preview, character);
        characters++;
    }

    if (truncated)
        g_string_append(preview, "...");
    if (preview->len == 0)
        g_string_assign(preview, "(empty text)");
    return g_string_free(preview, FALSE);
}

gchar *
mux_clipboard_history_entry_dup_preview(
    const MuxClipboardHistoryEntry *entry,
    guint max_characters)
{
    GBytes *text;
    gchar *preview;
    const gchar *mime = "unknown";
    GBytes *bytes = NULL;
    g_autofree gchar *size = NULL;
    guint count;

    g_return_val_if_fail(entry != NULL, NULL);
    if (max_characters == 0)
        max_characters = 80;

    text = find_preview_text(entry->snapshot);
    if (text != NULL) {
        preview = safe_text_preview(text, max_characters);
        if (preview != NULL)
            return preview;
    }

    count = mux_clipboard_snapshot_get_count(entry->snapshot);
    if (count > 0)
        mux_clipboard_snapshot_get_item(entry->snapshot, 0, &mime, &bytes);
    size = g_format_size(
        mux_clipboard_snapshot_get_total_bytes(entry->snapshot));
    return g_strdup_printf("[%s, %s, %u format%s]",
                           mime,
                           size,
                           count,
                           count == 1 ? "" : "s");
}
