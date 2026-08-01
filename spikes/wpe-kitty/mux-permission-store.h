#ifndef MUX_PERMISSION_STORE_H
#define MUX_PERMISSION_STORE_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define MUX_PERMISSION_ORIGIN_MAX 2048U
#define MUX_PERMISSION_CATEGORY_MAX 128U

typedef struct _MuxPermissionStore MuxPermissionStore;

typedef enum {
    MUX_PERMISSION_DECISION_ASK = 0,
    MUX_PERMISSION_DECISION_ALLOW = 1,
    MUX_PERMISSION_DECISION_DENY = 2,
} MuxPermissionDecision;

/*
 * profile_directory may be NULL only for a non-persistent store. Persistent
 * stores load permissions.ini from this directory and reject files not owned
 * by the current user or accessible by group/other.
 */
MuxPermissionStore *mux_permission_store_new(
    const gchar *profile_directory,
    gboolean persistent,
    GError **error);

MuxPermissionStore *mux_permission_store_ref(MuxPermissionStore *store);
void mux_permission_store_free(MuxPermissionStore *store);

gboolean mux_permission_store_is_persistent(
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

gboolean mux_permission_store_flush(MuxPermissionStore *store,
                                    GError **error);

guint mux_permission_store_size(const MuxPermissionStore *store);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxPermissionStore,
                              mux_permission_store_free)

G_END_DECLS

#endif
