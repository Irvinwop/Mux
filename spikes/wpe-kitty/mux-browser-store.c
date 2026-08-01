#include "mux-browser-store.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

#define MUX_BROWSER_BOOKMARK_FILE "bookmarks.ini"
#define MUX_BROWSER_BOOKMARK_FILE_LIMIT (2U * 1024U * 1024U)
#define MUX_BROWSER_URI_LIMIT (16U * 1024U)
#define MUX_BROWSER_TITLE_LIMIT 1024U
#define MUX_BROWSER_STORE_VERSION 1

typedef struct {
    gboolean private_profile;
    MuxBrowserEntry *entry;
} CurrentView;

struct _MuxBrowserStore {
    GQueue history[2];
    GQueue recently_closed[2];
    GHashTable *current_views;
    GPtrArray *bookmarks;
    gchar *profile_directory;
    gchar *bookmark_path;
};

static guint
privacy_index(gboolean private_profile)
{
    return private_profile ? 1U : 0U;
}

static gchar *
bounded_utf8_copy(const gchar *value, gsize maximum)
{
    gsize length;

    if (!value || !g_utf8_validate(value, -1, NULL))
        return NULL;
    length = strlen(value);
    if (length <= maximum)
        return g_strdup(value);
    length = maximum;
    while (length && !g_utf8_validate(value, length, NULL))
        length--;
    return g_strndup(value, length);
}

static gboolean
uri_has_allowed_scheme(const gchar *uri, gboolean bookmark)
{
    g_autoptr(GUri) parsed = NULL;
    const gchar *scheme;
    const gchar *host;

    if (!uri || !*uri || strlen(uri) > MUX_BROWSER_URI_LIMIT ||
        !g_utf8_validate(uri, -1, NULL))
        return FALSE;
    if (!bookmark && g_str_equal(uri, "about:blank"))
        return TRUE;

    parsed = g_uri_parse(uri, G_URI_FLAGS_PARSE_RELAXED, NULL);
    if (!parsed || g_uri_get_userinfo(parsed))
        return FALSE;
    scheme = g_uri_get_scheme(parsed);
    host = g_uri_get_host(parsed);
    return scheme && host && *host &&
        (g_ascii_strcasecmp(scheme, "http") == 0 ||
         g_ascii_strcasecmp(scheme, "https") == 0);
}

gboolean
mux_browser_store_uri_is_replayable(const gchar *uri)
{
    return uri_has_allowed_scheme(uri, FALSE);
}

gboolean
mux_browser_store_uri_is_bookmarkable(const gchar *uri)
{
    return uri_has_allowed_scheme(uri, TRUE);
}

static MuxBrowserEntry *
browser_entry_new(guint64 view_id,
                  gboolean private_profile,
                  const gchar *uri,
                  const gchar *title,
                  gint64 visited_us)
{
    MuxBrowserEntry *entry;
    gchar *uri_copy;
    gchar *title_copy;

    if (!mux_browser_store_uri_is_replayable(uri))
        return NULL;
    uri_copy = bounded_utf8_copy(uri, MUX_BROWSER_URI_LIMIT);
    title_copy = bounded_utf8_copy(title && *title ? title : uri,
                                  MUX_BROWSER_TITLE_LIMIT);
    if (!uri_copy || !title_copy) {
        g_free(uri_copy);
        g_free(title_copy);
        return NULL;
    }

    entry = g_new0(MuxBrowserEntry, 1);
    entry->uri = uri_copy;
    entry->title = title_copy;
    entry->visited_us = visited_us > 0 ? visited_us : g_get_real_time();
    entry->view_id = view_id;
    entry->private_profile = private_profile;
    return entry;
}

MuxBrowserEntry *
mux_browser_entry_copy(const MuxBrowserEntry *entry)
{
    MuxBrowserEntry *copy;

    g_return_val_if_fail(entry, NULL);
    copy = g_new0(MuxBrowserEntry, 1);
    copy->uri = g_strdup(entry->uri);
    copy->title = g_strdup(entry->title);
    copy->visited_us = entry->visited_us;
    copy->view_id = entry->view_id;
    copy->private_profile = entry->private_profile;
    return copy;
}

void
mux_browser_entry_free(MuxBrowserEntry *entry)
{
    if (!entry)
        return;
    g_free(entry->uri);
    g_free(entry->title);
    g_free(entry);
}

static void
current_view_free(CurrentView *current)
{
    if (!current)
        return;
    mux_browser_entry_free(current->entry);
    g_free(current);
}

static void
queue_remove_uri(GQueue *queue, const gchar *uri)
{
    GList *link = queue->head;

    while (link) {
        GList *next = link->next;
        MuxBrowserEntry *entry = link->data;

        if (g_strcmp0(entry->uri, uri) == 0) {
            g_queue_delete_link(queue, link);
            mux_browser_entry_free(entry);
        }
        link = next;
    }
}

static void
queue_trim(GQueue *queue, guint limit)
{
    while (queue->length > limit)
        mux_browser_entry_free(g_queue_pop_tail(queue));
}

static GPtrArray *
queue_copy(const GQueue *queue, guint limit)
{
    GPtrArray *entries = g_ptr_array_new_with_free_func(
        (GDestroyNotify)mux_browser_entry_free);
    const GList *link;

    for (link = queue->head;
         link && (!limit || entries->len < limit);
         link = link->next)
        g_ptr_array_add(entries, mux_browser_entry_copy(link->data));
    return entries;
}

static gint
bookmark_index(const GPtrArray *bookmarks, const gchar *uri)
{
    guint i;

    for (i = 0; i < bookmarks->len; i++) {
        const MuxBrowserEntry *entry = g_ptr_array_index(bookmarks, i);

        if (g_strcmp0(entry->uri, uri) == 0)
            return (gint)i;
    }
    return -1;
}

static GPtrArray *
bookmark_array_copy(const GPtrArray *bookmarks)
{
    GPtrArray *copy = g_ptr_array_new_with_free_func(
        (GDestroyNotify)mux_browser_entry_free);
    guint i;

    for (i = 0; i < bookmarks->len; i++)
        g_ptr_array_add(copy,
                        mux_browser_entry_copy(
                            g_ptr_array_index(bookmarks, i)));
    return copy;
}

static gboolean
load_bookmarks(MuxBrowserStore *store, GError **error)
{
    g_autofree gchar *contents = NULL;
    gsize length = 0;
    g_autoptr(GKeyFile) key_file = g_key_file_new();
    gint version;
    gint count;
    gint i;

    if (!g_file_get_contents(store->bookmark_path,
                             &contents,
                             &length,
                             error)) {
        if (error && *error &&
            g_error_matches(*error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_clear_error(error);
            return TRUE;
        }
        return FALSE;
    }
    if (length > MUX_BROWSER_BOOKMARK_FILE_LIMIT) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "bookmark store exceeds its size limit");
        return FALSE;
    }
    if (!g_key_file_load_from_data(key_file,
                                   contents,
                                   length,
                                   G_KEY_FILE_NONE,
                                   error))
        return FALSE;
    version = g_key_file_get_integer(key_file, "store", "version", error);
    if (error && *error)
        return FALSE;
    count = g_key_file_get_integer(key_file, "store", "count", error);
    if (error && *error)
        return FALSE;
    if (version != MUX_BROWSER_STORE_VERSION || count < 0) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "bookmark store has an unsupported format");
        return FALSE;
    }

    count = MIN(count, (gint)MUX_BROWSER_BOOKMARK_LIMIT);
    for (i = 0; i < count; i++) {
        g_autofree gchar *group = g_strdup_printf("bookmark.%d", i);
        g_autofree gchar *uri = NULL;
        g_autofree gchar *title = NULL;
        g_autoptr(GError) item_error = NULL;
        gint64 created_us;
        MuxBrowserEntry *entry;

        uri = g_key_file_get_string(key_file, group, "uri", &item_error);
        title = g_key_file_get_string(key_file, group, "title", &item_error);
        created_us = g_key_file_get_int64(key_file,
                                          group,
                                          "created-us",
                                          &item_error);
        if (item_error || !mux_browser_store_uri_is_bookmarkable(uri) ||
            bookmark_index(store->bookmarks, uri) >= 0)
            continue;
        entry = browser_entry_new(0, FALSE, uri, title, created_us);
        if (entry)
            g_ptr_array_add(store->bookmarks, entry);
    }
    return TRUE;
}

static gboolean
save_bookmarks(const MuxBrowserStore *store,
               const GPtrArray *bookmarks,
               GError **error)
{
    g_autoptr(GKeyFile) key_file = g_key_file_new();
    g_autofree gchar *contents = NULL;
    gsize length;
    guint i;

    if (g_mkdir_with_parents(store->profile_directory, 0700) < 0 ||
        g_chmod(store->profile_directory, 0700) < 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "prepare bookmark directory %s: %s",
                    store->profile_directory,
                    g_strerror(errno));
        return FALSE;
    }

    g_key_file_set_integer(key_file,
                           "store",
                           "version",
                           MUX_BROWSER_STORE_VERSION);
    g_key_file_set_integer(key_file,
                           "store",
                           "count",
                           (gint)bookmarks->len);
    for (i = 0; i < bookmarks->len; i++) {
        const MuxBrowserEntry *entry = g_ptr_array_index(bookmarks, i);
        g_autofree gchar *group = g_strdup_printf("bookmark.%u", i);

        g_key_file_set_string(key_file, group, "uri", entry->uri);
        g_key_file_set_string(key_file, group, "title", entry->title);
        g_key_file_set_int64(key_file,
                             group,
                             "created-us",
                             entry->visited_us);
    }
    contents = g_key_file_to_data(key_file, &length, error);
    if (!contents)
        return FALSE;
    return g_file_set_contents_full(
        store->bookmark_path,
        contents,
        (gssize)length,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE,
        0600,
        error);
}

MuxBrowserStore *
mux_browser_store_new(const gchar *profile_directory, GError **error)
{
    MuxBrowserStore *store;
    g_autoptr(GError) load_error = NULL;

    if (!profile_directory || !g_path_is_absolute(profile_directory)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "browser profile directory must be absolute");
        return NULL;
    }

    store = g_new0(MuxBrowserStore, 1);
    store->profile_directory = g_strdup(profile_directory);
    store->bookmark_path = g_build_filename(profile_directory,
                                             MUX_BROWSER_BOOKMARK_FILE,
                                             NULL);
    store->current_views = g_hash_table_new_full(
        g_int64_hash,
        g_int64_equal,
        g_free,
        (GDestroyNotify)current_view_free);
    store->bookmarks = g_ptr_array_new_with_free_func(
        (GDestroyNotify)mux_browser_entry_free);
    if (!load_bookmarks(store, &load_error)) {
        g_warning("bookmark store ignored: %s", load_error->message);
        g_ptr_array_set_size(store->bookmarks, 0);
    }
    return store;
}

void
mux_browser_store_free(MuxBrowserStore *store)
{
    guint i;

    if (!store)
        return;
    for (i = 0; i < G_N_ELEMENTS(store->history); i++) {
        g_queue_clear_full(&store->history[i],
                           (GDestroyNotify)mux_browser_entry_free);
        g_queue_clear_full(&store->recently_closed[i],
                           (GDestroyNotify)mux_browser_entry_free);
    }
    g_hash_table_unref(store->current_views);
    g_ptr_array_unref(store->bookmarks);
    g_free(store->profile_directory);
    g_free(store->bookmark_path);
    g_free(store);
}

void
mux_browser_store_record_navigation(MuxBrowserStore *store,
                                    guint64 view_id,
                                    gboolean private_profile,
                                    const gchar *uri,
                                    const gchar *title)
{
    MuxBrowserEntry *entry;
    CurrentView *current;
    guint64 *key;
    guint index;

    g_return_if_fail(store);
    if (!view_id)
        return;
    entry = browser_entry_new(view_id,
                              private_profile,
                              uri,
                              title,
                              g_get_real_time());
    if (!entry)
        return;

    index = privacy_index(private_profile);
    queue_remove_uri(&store->history[index], entry->uri);
    g_queue_push_head(&store->history[index],
                      mux_browser_entry_copy(entry));
    queue_trim(&store->history[index], MUX_BROWSER_HISTORY_LIMIT);

    current = g_new0(CurrentView, 1);
    current->private_profile = private_profile;
    current->entry = entry;
    key = g_new(guint64, 1);
    *key = view_id;
    g_hash_table_replace(store->current_views, key, current);
}

void
mux_browser_store_close_view(MuxBrowserStore *store, guint64 view_id)
{
    CurrentView *current;
    guint index;

    g_return_if_fail(store);
    current = g_hash_table_lookup(store->current_views, &view_id);
    if (!current)
        return;
    index = privacy_index(current->private_profile);
    queue_remove_uri(&store->recently_closed[index], current->entry->uri);
    g_queue_push_head(&store->recently_closed[index],
                      mux_browser_entry_copy(current->entry));
    queue_trim(&store->recently_closed[index],
               MUX_BROWSER_RECENTLY_CLOSED_LIMIT);
    g_hash_table_remove(store->current_views, &view_id);
}

GPtrArray *
mux_browser_store_copy_history(const MuxBrowserStore *store,
                               gboolean private_profile,
                               guint limit)
{
    g_return_val_if_fail(store, NULL);
    return queue_copy(&store->history[privacy_index(private_profile)], limit);
}

GPtrArray *
mux_browser_store_copy_recently_closed(const MuxBrowserStore *store,
                                       gboolean private_profile,
                                       guint limit)
{
    g_return_val_if_fail(store, NULL);
    return queue_copy(
        &store->recently_closed[privacy_index(private_profile)], limit);
}

MuxBrowserEntry *
mux_browser_store_take_recently_closed(MuxBrowserStore *store,
                                       gboolean private_profile)
{
    g_return_val_if_fail(store, NULL);
    return g_queue_pop_head(
        &store->recently_closed[privacy_index(private_profile)]);
}

guint
mux_browser_store_history_count(const MuxBrowserStore *store,
                                gboolean private_profile)
{
    g_return_val_if_fail(store, 0);
    return store->history[privacy_index(private_profile)].length;
}

guint
mux_browser_store_recently_closed_count(const MuxBrowserStore *store,
                                        gboolean private_profile)
{
    g_return_val_if_fail(store, 0);
    return store->recently_closed[privacy_index(private_profile)].length;
}

gboolean
mux_browser_store_is_bookmarked(const MuxBrowserStore *store,
                                gboolean private_profile,
                                const gchar *uri)
{
    g_return_val_if_fail(store, FALSE);
    return !private_profile && uri &&
        bookmark_index(store->bookmarks, uri) >= 0;
}

gboolean
mux_browser_store_set_bookmarked(MuxBrowserStore *store,
                                 gboolean private_profile,
                                 const gchar *uri,
                                 const gchar *title,
                                 gboolean bookmarked,
                                 GError **error)
{
    g_autoptr(GPtrArray) candidate = NULL;
    gint index;

    g_return_val_if_fail(store, FALSE);
    if (private_profile) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            "bookmarks are unavailable in private panes");
        return FALSE;
    }
    if (!mux_browser_store_uri_is_bookmarkable(uri)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "only HTTP and HTTPS pages can be bookmarked");
        return FALSE;
    }

    index = bookmark_index(store->bookmarks, uri);
    if (!bookmarked && index < 0)
        return TRUE;
    candidate = bookmark_array_copy(store->bookmarks);
    if (index >= 0)
        g_ptr_array_remove_index(candidate, (guint)index);
    if (bookmarked) {
        MuxBrowserEntry *entry = browser_entry_new(0,
                                                   FALSE,
                                                   uri,
                                                   title,
                                                   g_get_real_time());

        if (!entry) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_ARGUMENT,
                                "bookmark data is invalid");
            return FALSE;
        }
        g_ptr_array_insert(candidate, 0, entry);
        if (candidate->len > MUX_BROWSER_BOOKMARK_LIMIT)
            g_ptr_array_remove_index(candidate, candidate->len - 1);
    }
    if (!save_bookmarks(store, candidate, error))
        return FALSE;

    g_ptr_array_unref(store->bookmarks);
    store->bookmarks = g_steal_pointer(&candidate);
    return TRUE;
}

GPtrArray *
mux_browser_store_copy_bookmarks(const MuxBrowserStore *store,
                                 gboolean private_profile,
                                 guint limit)
{
    GPtrArray *copy;
    guint i;

    g_return_val_if_fail(store, NULL);
    copy = g_ptr_array_new_with_free_func(
        (GDestroyNotify)mux_browser_entry_free);
    if (private_profile)
        return copy;
    for (i = 0; i < store->bookmarks->len && (!limit || i < limit); i++)
        g_ptr_array_add(copy,
                        mux_browser_entry_copy(
                            g_ptr_array_index(store->bookmarks, i)));
    return copy;
}

guint
mux_browser_store_bookmark_count(const MuxBrowserStore *store,
                                 gboolean private_profile)
{
    g_return_val_if_fail(store, 0);
    return private_profile ? 0 : store->bookmarks->len;
}
