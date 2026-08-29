#ifndef MUX_BROWSER_STORE_H
#define MUX_BROWSER_STORE_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define MUX_BROWSER_HISTORY_LIMIT 256U
#define MUX_BROWSER_RECENTLY_CLOSED_LIMIT 32U
#define MUX_BROWSER_BOOKMARK_LIMIT 256U

typedef struct _MuxBrowserStore MuxBrowserStore;

typedef struct {
    gchar *uri;
    gchar *title;
    gint64 visited_us;
    guint64 view_id;
    gboolean private_profile;
} MuxBrowserEntry;

MuxBrowserEntry *mux_browser_entry_copy(const MuxBrowserEntry *entry);
void mux_browser_entry_free(MuxBrowserEntry *entry);

MuxBrowserStore *mux_browser_store_new(const gchar *profile_directory,
                                       GError **error);
void mux_browser_store_free(MuxBrowserStore *store);

void mux_browser_store_record_navigation(MuxBrowserStore *store,
                                         guint64 view_id,
                                         gboolean private_profile,
                                         const gchar *uri,
                                         const gchar *title);
void mux_browser_store_close_view(MuxBrowserStore *store, guint64 view_id);

GPtrArray *mux_browser_store_copy_history(const MuxBrowserStore *store,
                                          gboolean private_profile,
                                          guint limit);
GPtrArray *mux_browser_store_copy_recently_closed(
    const MuxBrowserStore *store,
    gboolean private_profile,
    guint limit);
MuxBrowserEntry *mux_browser_store_take_recently_closed(
    MuxBrowserStore *store,
    gboolean private_profile);

guint mux_browser_store_history_count(const MuxBrowserStore *store,
                                      gboolean private_profile);
guint mux_browser_store_recently_closed_count(const MuxBrowserStore *store,
                                              gboolean private_profile);

gboolean mux_browser_store_uri_is_replayable(const gchar *uri);
gboolean mux_browser_store_uri_is_bookmarkable(const gchar *uri);
gboolean mux_browser_store_is_bookmarked(const MuxBrowserStore *store,
                                         gboolean private_profile,
                                         const gchar *uri);
gboolean mux_browser_store_set_bookmarked(MuxBrowserStore *store,
                                          gboolean private_profile,
                                          const gchar *uri,
                                          const gchar *title,
                                          gboolean bookmarked,
                                          GError **error);
GPtrArray *mux_browser_store_copy_bookmarks(const MuxBrowserStore *store,
                                            gboolean private_profile,
                                            guint limit);
guint mux_browser_store_bookmark_count(const MuxBrowserStore *store,
                                       gboolean private_profile);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxBrowserEntry, mux_browser_entry_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxBrowserStore, mux_browser_store_free)

G_END_DECLS

#endif
