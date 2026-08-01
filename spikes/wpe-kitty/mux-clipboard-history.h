#ifndef MUX_CLIPBOARD_HISTORY_H
#define MUX_CLIPBOARD_HISTORY_H

#include "mux-clipboard.h"

G_BEGIN_DECLS

#define MUX_CLIPBOARD_HISTORY_MAX_ENTRIES 25U
#define MUX_CLIPBOARD_HISTORY_MAX_BYTES (16U * 1024U * 1024U)
#define MUX_CLIPBOARD_HISTORY_MAX_ITEM_BYTES (8U * 1024U * 1024U)
#define MUX_CLIPBOARD_HISTORY_MAX_ORIGIN 2048U
#define MUX_CLIPBOARD_HISTORY_MAX_PROFILE 128U
#define MUX_CLIPBOARD_HISTORY_MAX_NAMESPACE 128U

typedef enum {
    MUX_CLIPBOARD_HISTORY_DISABLED,
    MUX_CLIPBOARD_HISTORY_MEMORY,
    MUX_CLIPBOARD_HISTORY_EPHEMERAL
} MuxClipboardHistoryMode;

typedef enum {
    MUX_CLIPBOARD_HISTORY_SCOPE_PERSISTENT = 0,
    MUX_CLIPBOARD_HISTORY_SCOPE_PRIVATE = 1,
    MUX_CLIPBOARD_HISTORY_SCOPE_EPHEMERAL = 2,
} MuxClipboardHistoryScope;

typedef enum {
    MUX_CLIPBOARD_HISTORY_IGNORED,
    MUX_CLIPBOARD_HISTORY_ADDED,
    MUX_CLIPBOARD_HISTORY_DEDUPLICATED
} MuxClipboardHistoryAddResult;

typedef struct _MuxClipboardHistory MuxClipboardHistory;
typedef struct _MuxClipboardHistoryEntry MuxClipboardHistoryEntry;

MuxClipboardHistory *mux_clipboard_history_new(
    const gchar *profile,
    MuxClipboardHistoryMode mode);

/*
 * All modes are memory-only. The scope and namespace form an internal,
 * collision-free storage identity; private and ephemeral identities never
 * alias persistent identities with the same display profile.
 */
MuxClipboardHistory *mux_clipboard_history_new_for_namespace(
    const gchar *profile,
    const gchar *profile_namespace,
    MuxClipboardHistoryScope scope,
    MuxClipboardHistoryMode mode);
void mux_clipboard_history_free(MuxClipboardHistory *history);

MuxClipboardHistoryAddResult mux_clipboard_history_add(
    MuxClipboardHistory *history,
    const MuxClipboardSnapshot *snapshot,
    gint64 created_us,
    const gchar *source_origin,
    guint64 source_view_id,
    guint64 *entry_id,
    GError **error);

guint mux_clipboard_history_get_count(const MuxClipboardHistory *history);
gsize mux_clipboard_history_get_total_bytes(
    const MuxClipboardHistory *history);
const gchar *mux_clipboard_history_get_profile(
    const MuxClipboardHistory *history);
MuxClipboardHistoryMode mux_clipboard_history_get_mode(
    const MuxClipboardHistory *history);
MuxClipboardHistoryScope mux_clipboard_history_get_scope(
    const MuxClipboardHistory *history);
const gchar *mux_clipboard_history_get_namespace(
    const MuxClipboardHistory *history);

/* Returned entries are borrowed and invalidated by history mutations. */
const MuxClipboardHistoryEntry *mux_clipboard_history_get(
    const MuxClipboardHistory *history,
    guint newest_first_index);
const MuxClipboardHistoryEntry *mux_clipboard_history_lookup(
    const MuxClipboardHistory *history,
    guint64 entry_id);

MuxClipboardSnapshot *mux_clipboard_history_select(
    MuxClipboardHistory *history,
    guint64 entry_id,
    GError **error);

gboolean mux_clipboard_history_set_pinned(MuxClipboardHistory *history,
                                          guint64 entry_id,
                                          gboolean pinned,
                                          GError **error);
gboolean mux_clipboard_history_delete(MuxClipboardHistory *history,
                                      guint64 entry_id,
                                      GError **error);
guint mux_clipboard_history_clear(MuxClipboardHistory *history,
                                  gboolean include_pinned);

guint64 mux_clipboard_history_entry_get_id(
    const MuxClipboardHistoryEntry *entry);
gint64 mux_clipboard_history_entry_get_created_us(
    const MuxClipboardHistoryEntry *entry);
const gchar *mux_clipboard_history_entry_get_profile(
    const MuxClipboardHistoryEntry *entry);
const gchar *mux_clipboard_history_entry_get_namespace(
    const MuxClipboardHistoryEntry *entry);
const gchar *mux_clipboard_history_entry_get_source_origin(
    const MuxClipboardHistoryEntry *entry);
guint64 mux_clipboard_history_entry_get_source_view_id(
    const MuxClipboardHistoryEntry *entry);
gboolean mux_clipboard_history_entry_get_pinned(
    const MuxClipboardHistoryEntry *entry);
const MuxClipboardSnapshot *mux_clipboard_history_entry_get_snapshot(
    const MuxClipboardHistoryEntry *entry);

/* Returns terminal-safe text with control bytes removed. */
gchar *mux_clipboard_history_entry_dup_preview(
    const MuxClipboardHistoryEntry *entry,
    guint max_characters);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxClipboardHistory,
                              mux_clipboard_history_free)

G_END_DECLS

#endif
