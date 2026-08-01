#ifndef MUX_PERMISSION_STORE_H
#define MUX_PERMISSION_STORE_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define MUX_PERMISSION_ORIGIN_MAX 2048U
#define MUX_PERMISSION_CATEGORY_MAX 128U
#define MUX_PERMISSION_NAMESPACE_MAX 128U
#define MUX_PERMISSION_STORE_MAX_ENTRIES 512U
#define MUX_PERMISSION_STORE_MAX_FILE_BYTES (2U * 1024U * 1024U)
#define MUX_PERMISSION_STORE_DEFAULT_TTL_US \
    (G_GINT64_CONSTANT(30) * G_TIME_SPAN_DAY)
#define MUX_PERMISSION_STORE_MAX_TTL_US \
    (G_GINT64_CONSTANT(365) * G_TIME_SPAN_DAY)

typedef struct _MuxPermissionStore MuxPermissionStore;

typedef enum {
    MUX_PERMISSION_DECISION_ASK = 0,
    MUX_PERMISSION_DECISION_ALLOW = 1,
    MUX_PERMISSION_DECISION_DENY = 2,
} MuxPermissionDecision;

typedef enum {
    MUX_PERMISSION_STORE_SCOPE_PERSISTENT = 0,
    MUX_PERMISSION_STORE_SCOPE_PRIVATE = 1,
    MUX_PERMISSION_STORE_SCOPE_EPHEMERAL = 2,
} MuxPermissionStoreScope;

/*
 * profile_directory may be NULL only for a non-persistent store. Persistent
 * stores load permissions.ini from this directory and reject files not owned
 * by the current user or accessible by group/other.
 */
MuxPermissionStore *mux_permission_store_new(
    const gchar *profile_directory,
    gboolean persistent,
    GError **error);

/*
 * Creates a namespace-bound store. Only PERSISTENT reads or writes
 * profile_directory. PRIVATE and EPHEMERAL always remain process-local, even
 * when a non-NULL directory is supplied accidentally.
 */
MuxPermissionStore *mux_permission_store_new_for_namespace(
    const gchar *profile_directory,
    const gchar *profile_namespace,
    MuxPermissionStoreScope scope,
    GError **error);

MuxPermissionStore *mux_permission_store_ref(MuxPermissionStore *store);
void mux_permission_store_free(MuxPermissionStore *store);

gboolean mux_permission_store_is_persistent(
    const MuxPermissionStore *store);
MuxPermissionStoreScope mux_permission_store_get_scope(
    const MuxPermissionStore *store);
const gchar *mux_permission_store_get_namespace(
    const MuxPermissionStore *store);

MuxPermissionDecision mux_permission_store_lookup(
    const MuxPermissionStore *store,
    const gchar *origin,
    const gchar *category);

/*
 * ASK removes a stored decision. Persistent updates are transactional with
 * respect to memory: a failed disk write restores the previous value.
 */
gboolean mux_permission_store_set(MuxPermissionStore *store,
                                  const gchar *origin,
                                  const gchar *category,
                                  MuxPermissionDecision decision,
                                  GError **error);

/* duration_us must be positive and no greater than MAX_TTL_US. */
gboolean mux_permission_store_set_for_duration(
    MuxPermissionStore *store,
    const gchar *origin,
    const gchar *category,
    MuxPermissionDecision decision,
    gint64 duration_us,
    GError **error);

gboolean mux_permission_store_flush(MuxPermissionStore *store,
                                    GError **error);

guint mux_permission_store_size(const MuxPermissionStore *store);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxPermissionStore,
                              mux_permission_store_free)

G_END_DECLS

#endif
