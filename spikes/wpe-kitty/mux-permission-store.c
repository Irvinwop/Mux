#define _POSIX_C_SOURCE 200809L

#include "mux-permission-store.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

typedef struct {
    gchar *key;
    gchar *origin;
    gchar *category;
    MuxPermissionDecision decision;
    gint64 updated_us;
    gint64 expires_us;
} PermissionEntry;

struct _MuxPermissionStore {
    grefcount reference_count;
    gchar *profile_directory;
    gchar *path;
    gchar *profile_namespace;
    GHashTable *entries;
    MuxPermissionStoreScope scope;
    gboolean persistent;
};

static void
permission_entry_free(PermissionEntry *entry)
{
    if (!entry)
        return;
    g_free(entry->key);
    g_free(entry->origin);
    g_free(entry->category);
    g_free(entry);
}

static gchar *
permission_key(const gchar *origin, const gchar *category)
{
    return g_strconcat(origin, "\x1f", category, NULL);
}

static PermissionEntry *
permission_entry_new(const gchar *origin,
                     const gchar *category,
                     MuxPermissionDecision decision,
                     gint64 updated_us,
                     gint64 expires_us)
{
    PermissionEntry *entry = g_new0(PermissionEntry, 1);

    entry->key = permission_key(origin, category);
    entry->origin = g_strdup(origin);
    entry->category = g_strdup(category);
    entry->decision = decision;
    entry->updated_us = updated_us;
    entry->expires_us = expires_us;
    return entry;
}

static PermissionEntry *
permission_entry_copy(const PermissionEntry *entry)
{
    return entry ? permission_entry_new(entry->origin,
                                        entry->category,
                                        entry->decision,
                                        entry->updated_us,
                                        entry->expires_us)
                 : NULL;
}

static GHashTable *
permission_entries_copy(GHashTable *entries)
{
    GHashTable *copy = g_hash_table_new_full(
        g_str_hash,
        g_str_equal,
        NULL,
        (GDestroyNotify)permission_entry_free);
    GHashTableIter iterator;
    gpointer value;

    g_hash_table_iter_init(&iterator, entries);
    while (g_hash_table_iter_next(&iterator, NULL, &value)) {
        PermissionEntry *entry = permission_entry_copy(value);

        g_hash_table_insert(copy, entry->key, entry);
    }
    return copy;
}

static gboolean
valid_namespace(const gchar *profile_namespace, GError **error)
{
    const guchar *cursor;
    gsize length;

    if (!profile_namespace || !*profile_namespace) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "permission namespace is empty");
        return FALSE;
    }
    length = strlen(profile_namespace);
    if (length > MUX_PERMISSION_NAMESPACE_MAX) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "permission namespace is too long");
        return FALSE;
    }
    for (cursor = (const guchar *)profile_namespace; *cursor; cursor++) {
        if (!(g_ascii_isalnum(*cursor) || *cursor == '-' ||
              *cursor == '_' || *cursor == '.')) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_ARGUMENT,
                                "permission namespace is invalid");
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean
valid_origin(const gchar *origin, GError **error)
{
    gsize length;

    if (!origin || !*origin) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "permission origin is empty");
        return FALSE;
    }
    length = strlen(origin);
    if (length > MUX_PERMISSION_ORIGIN_MAX ||
        !g_utf8_validate(origin, length, NULL) ||
        strchr(origin, '\x1f')) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "permission origin is invalid");
        return FALSE;
    }
    return TRUE;
}

static gboolean
valid_category(const gchar *category, GError **error)
{
    const guchar *cursor;
    gsize length;

    if (!category || !*category) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "permission category is empty");
        return FALSE;
    }
    length = strlen(category);
    if (length > MUX_PERMISSION_CATEGORY_MAX) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "permission category is too long");
        return FALSE;
    }
    for (cursor = (const guchar *)category; *cursor; cursor++) {
        if (!(g_ascii_isalnum(*cursor) || *cursor == '-' ||
              *cursor == '_' || *cursor == '.')) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_ARGUMENT,
                                "permission category is invalid");
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean
valid_decision(MuxPermissionDecision decision)
{
    return decision == MUX_PERMISSION_DECISION_ASK ||
           decision == MUX_PERMISSION_DECISION_ALLOW ||
           decision == MUX_PERMISSION_DECISION_DENY;
}

static gboolean
set_file_error(GError **error, gint saved_errno, const gchar *operation)
{
    g_set_error(error,
                G_FILE_ERROR,
                g_file_error_from_errno(saved_errno),
                "%s: %s",
                operation,
                g_strerror(saved_errno));
    return FALSE;
}

static gboolean
check_owner_only_directory(const gchar *directory, GError **error)
{
    struct stat status;

    if (g_lstat(directory, &status) < 0)
        return set_file_error(error, errno, "stat profile directory");
    if (!S_ISDIR(status.st_mode) || status.st_uid != getuid()) {
        g_set_error_literal(error,
                            G_FILE_ERROR,
                            G_FILE_ERROR_ACCES,
                            "profile directory is not an owned directory");
        return FALSE;
    }
    if (status.st_mode & 077) {
        if (g_chmod(directory, 0700) < 0)
            return set_file_error(
                error, errno, "restrict profile directory permissions");
    }
    return TRUE;
}

static gboolean
ensure_profile_directory(const gchar *directory, GError **error)
{
    if (g_mkdir_with_parents(directory, 0700) < 0)
        return set_file_error(error, errno, "create profile directory");
    return check_owner_only_directory(directory, error);
}

static gboolean
load_file_is_safe(const gchar *path, gboolean *exists, GError **error)
{
    struct stat status;

    *exists = FALSE;
    if (g_lstat(path, &status) < 0) {
        if (errno == ENOENT)
            return TRUE;
        return set_file_error(error, errno, "stat permission store");
    }
    *exists = TRUE;
    if (!S_ISREG(status.st_mode) || status.st_uid != getuid() ||
        (status.st_mode & 077)) {
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_ACCES,
            "permission store must be an owner-only regular file");
        return FALSE;
    }
    if ((guint64)status.st_size > MUX_PERMISSION_STORE_MAX_FILE_BYTES) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NO_SPACE,
                            "permission store exceeds its size limit");
        return FALSE;
    }
    return TRUE;
}

static gboolean
permission_entry_is_expired(const PermissionEntry *entry, gint64 now_us)
{
    return entry->expires_us <= now_us;
}

static guint
prune_expired(MuxPermissionStore *store, gint64 now_us)
{
    GHashTableIter iterator;
    gpointer value;
    guint removed = 0;

    g_hash_table_iter_init(&iterator, store->entries);
    while (g_hash_table_iter_next(&iterator, NULL, &value)) {
        if (!permission_entry_is_expired(value, now_us))
            continue;
        g_hash_table_iter_remove(&iterator);
        removed++;
    }
    return removed;
}

static PermissionEntry *
oldest_entry(MuxPermissionStore *store)
{
    GHashTableIter iterator;
    PermissionEntry *oldest = NULL;
    gpointer value;

    g_hash_table_iter_init(&iterator, store->entries);
    while (g_hash_table_iter_next(&iterator, NULL, &value)) {
        PermissionEntry *entry = value;

        if (!oldest || entry->updated_us < oldest->updated_us ||
            (entry->updated_us == oldest->updated_us &&
             g_strcmp0(entry->key, oldest->key) < 0)) {
            oldest = entry;
        }
    }
    return oldest;
}

static void
enforce_entry_limit(MuxPermissionStore *store)
{
    while (g_hash_table_size(store->entries) >
           MUX_PERMISSION_STORE_MAX_ENTRIES) {
        PermissionEntry *oldest = oldest_entry(store);

        if (!oldest)
            return;
        g_hash_table_remove(store->entries, oldest->key);
    }
}

static const gchar *
decision_name(MuxPermissionDecision decision)
{
    switch (decision) {
    case MUX_PERMISSION_DECISION_ALLOW:
        return "allow";
    case MUX_PERMISSION_DECISION_DENY:
        return "deny";
    case MUX_PERMISSION_DECISION_ASK:
    default:
        return "ask";
    }
}

static MuxPermissionDecision
decision_from_name(const gchar *name)
{
    if (g_strcmp0(name, "allow") == 0)
        return MUX_PERMISSION_DECISION_ALLOW;
    if (g_strcmp0(name, "deny") == 0)
        return MUX_PERMISSION_DECISION_DENY;
    return MUX_PERMISSION_DECISION_ASK;
}

static gboolean
load_store(MuxPermissionStore *store, GError **error)
{
    g_autoptr(GKeyFile) key_file = g_key_file_new();
    g_auto(GStrv) groups = NULL;
    g_autofree gchar *file_namespace = NULL;
    g_autoptr(GError) metadata_error = NULL;
    gboolean exists;
    gint version;
    gint64 now_us = g_get_real_time();
    gsize count;
    gsize i;

    if (!load_file_is_safe(store->path, &exists, error))
        return FALSE;
    if (!exists)
        return TRUE;
    if (!g_key_file_load_from_file(
            key_file, store->path, G_KEY_FILE_NONE, error))
        return FALSE;

    version = g_key_file_get_integer(
        key_file, "mux", "version", &metadata_error);
    if (metadata_error) {
        g_propagate_error(error, g_steal_pointer(&metadata_error));
        return FALSE;
    }
    if (version == 1) {
        if (g_strcmp0(store->profile_namespace, "default") != 0) {
            g_set_error_literal(
                error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_DATA,
                "legacy permission store belongs to the default namespace");
            return FALSE;
        }
    } else if (version == 2) {
        file_namespace = g_key_file_get_string(
            key_file, "mux", "namespace", &metadata_error);
        if (!file_namespace) {
            g_propagate_error(error, g_steal_pointer(&metadata_error));
            return FALSE;
        }
        if (g_strcmp0(file_namespace, store->profile_namespace) != 0) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_DATA,
                                "permission store namespace does not match");
            return FALSE;
        }
    } else {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            "unsupported permission store version");
        return FALSE;
    }

    groups = g_key_file_get_groups(key_file, &count);
    for (i = 0; i < count; i++) {
        g_autofree gchar *origin = NULL;
        g_autofree gchar *category = NULL;
        g_autofree gchar *decision_text = NULL;
        g_autoptr(GError) entry_error = NULL;
        MuxPermissionDecision decision;
        PermissionEntry *entry;
        gint64 updated_us;
        gint64 expires_us;

        if (!g_str_has_prefix(groups[i], "permission "))
            continue;
        origin = g_key_file_get_string(
            key_file, groups[i], "origin", &entry_error);
        if (!origin)
            continue;
        category = g_key_file_get_string(
            key_file, groups[i], "category", &entry_error);
        if (!category)
            continue;
        decision_text = g_key_file_get_string(
            key_file, groups[i], "decision", &entry_error);
        if (!decision_text)
            continue;
        decision = decision_from_name(decision_text);
        if (decision == MUX_PERMISSION_DECISION_ASK ||
            !valid_origin(origin, NULL) ||
            !valid_category(category, NULL))
            continue;
        if (version == 1) {
            updated_us = now_us;
            expires_us = now_us +
                         MUX_PERMISSION_STORE_DEFAULT_TTL_US;
        } else {
            updated_us = g_key_file_get_int64(
                key_file, groups[i], "updated-us", &entry_error);
            if (entry_error)
                continue;
            expires_us = g_key_file_get_int64(
                key_file, groups[i], "expires-us", &entry_error);
            if (entry_error)
                continue;
            if (updated_us <= 0 || expires_us <= now_us ||
                expires_us <= updated_us ||
                expires_us - updated_us >
                    MUX_PERMISSION_STORE_MAX_TTL_US ||
                expires_us - now_us >
                    MUX_PERMISSION_STORE_MAX_TTL_US) {
                continue;
            }
        }
        entry = permission_entry_new(origin,
                                     category,
                                     decision,
                                     updated_us,
                                     expires_us);
        g_hash_table_replace(store->entries, entry->key, entry);
    }
    enforce_entry_limit(store);
    return TRUE;
}

MuxPermissionStore *
mux_permission_store_new(const gchar *profile_directory,
                         gboolean persistent,
                         GError **error)
{
    return mux_permission_store_new_for_namespace(
        profile_directory,
        "default",
        persistent ? MUX_PERMISSION_STORE_SCOPE_PERSISTENT
                   : MUX_PERMISSION_STORE_SCOPE_EPHEMERAL,
        error);
}

MuxPermissionStore *
mux_permission_store_new_for_namespace(
    const gchar *profile_directory,
    const gchar *profile_namespace,
    MuxPermissionStoreScope scope,
    GError **error)
{
    MuxPermissionStore *store;

    if (scope < MUX_PERMISSION_STORE_SCOPE_PERSISTENT ||
        scope > MUX_PERMISSION_STORE_SCOPE_EPHEMERAL) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "invalid permission store scope");
        return NULL;
    }
    if (!valid_namespace(profile_namespace, error))
        return NULL;
    if (scope == MUX_PERMISSION_STORE_SCOPE_PERSISTENT &&
        (!profile_directory || !*profile_directory)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "persistent permission store needs a profile path");
        return NULL;
    }
    store = g_new0(MuxPermissionStore, 1);
    g_ref_count_init(&store->reference_count);
    store->scope = scope;
    store->persistent =
        scope == MUX_PERMISSION_STORE_SCOPE_PERSISTENT;
    store->profile_namespace = g_strdup(profile_namespace);
    store->entries = g_hash_table_new_full(
        g_str_hash,
        g_str_equal,
        NULL,
        (GDestroyNotify)permission_entry_free);
    if (!store->persistent)
        return store;

    store->profile_directory = g_strdup(profile_directory);
    store->path =
        g_build_filename(profile_directory, "permissions.ini", NULL);
    if (!ensure_profile_directory(profile_directory, error) ||
        !load_store(store, error)) {
        mux_permission_store_free(store);
        return NULL;
    }
    return store;
}

MuxPermissionStore *
mux_permission_store_ref(MuxPermissionStore *store)
{
    g_return_val_if_fail(store, NULL);
    g_ref_count_inc(&store->reference_count);
    return store;
}

void
mux_permission_store_free(MuxPermissionStore *store)
{
    if (!store)
        return;
    if (!g_ref_count_dec(&store->reference_count))
        return;
    g_clear_pointer(&store->entries, g_hash_table_unref);
    g_free(store->path);
    g_free(store->profile_directory);
    g_free(store->profile_namespace);
    g_free(store);
}

gboolean
mux_permission_store_is_persistent(const MuxPermissionStore *store)
{
    g_return_val_if_fail(store, FALSE);
    return store->persistent;
}

MuxPermissionStoreScope
mux_permission_store_get_scope(const MuxPermissionStore *store)
{
    g_return_val_if_fail(store,
                         MUX_PERMISSION_STORE_SCOPE_EPHEMERAL);
    return store->scope;
}

const gchar *
mux_permission_store_get_namespace(const MuxPermissionStore *store)
{
    g_return_val_if_fail(store, NULL);
    return store->profile_namespace;
}

MuxPermissionDecision
mux_permission_store_lookup(const MuxPermissionStore *store,
                            const gchar *origin,
                            const gchar *category)
{
    g_autofree gchar *key = NULL;
    const PermissionEntry *entry;

    g_return_val_if_fail(store, MUX_PERMISSION_DECISION_ASK);
    if (!valid_origin(origin, NULL) || !valid_category(category, NULL))
        return MUX_PERMISSION_DECISION_ASK;
    key = permission_key(origin, category);
    entry = g_hash_table_lookup(store->entries, key);
    return entry && !permission_entry_is_expired(entry, g_get_real_time())
               ? entry->decision
               : MUX_PERMISSION_DECISION_ASK;
}

static gint
entry_compare(gconstpointer first, gconstpointer second)
{
    const PermissionEntry *a = *(PermissionEntry *const *)first;
    const PermissionEntry *b = *(PermissionEntry *const *)second;

    return g_strcmp0(a->key, b->key);
}

static gboolean
write_all(gint descriptor,
          const guint8 *data,
          gsize length,
          GError **error)
{
    while (length) {
        ssize_t written = write(descriptor, data, length);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            return set_file_error(error, errno, "write permission store");
        }
        if (!written) {
            g_set_error_literal(error,
                                G_FILE_ERROR,
                                G_FILE_ERROR_FAILED,
                                "short write to permission store");
            return FALSE;
        }
        data += written;
        length -= written;
    }
    return TRUE;
}

static gboolean
sync_parent_directory(const gchar *directory, GError **error)
{
    gint descriptor = open(directory, O_RDONLY | O_CLOEXEC);
    gint saved_errno;

    if (descriptor < 0)
        return set_file_error(
            error, errno, "open profile directory for sync");
    if (fsync(descriptor) == 0) {
        close(descriptor);
        return TRUE;
    }
    saved_errno = errno;
    close(descriptor);
    return set_file_error(
        error, saved_errno, "sync profile directory");
}

gboolean
mux_permission_store_flush(MuxPermissionStore *store, GError **error)
{
    g_autoptr(GKeyFile) key_file = NULL;
    g_autoptr(GPtrArray) ordered = NULL;
    g_autofree gchar *contents = NULL;
    g_autofree gchar *temporary = NULL;
    GHashTableIter iterator;
    gpointer value;
    gsize length;
    guint i;
    gint descriptor = -1;
    gboolean success = FALSE;

    g_return_val_if_fail(store, FALSE);
    if (!store->persistent)
        return TRUE;
    prune_expired(store, g_get_real_time());
    if (!ensure_profile_directory(store->profile_directory, error))
        return FALSE;

    key_file = g_key_file_new();
    g_key_file_set_integer(key_file, "mux", "version", 2);
    g_key_file_set_string(key_file,
                          "mux",
                          "namespace",
                          store->profile_namespace);
    ordered = g_ptr_array_new();
    g_hash_table_iter_init(&iterator, store->entries);
    while (g_hash_table_iter_next(&iterator, NULL, &value))
        g_ptr_array_add(ordered, value);
    g_ptr_array_sort(ordered, entry_compare);

    for (i = 0; i < ordered->len; i++) {
        const PermissionEntry *entry = g_ptr_array_index(ordered, i);
        g_autofree gchar *digest =
            g_compute_checksum_for_string(G_CHECKSUM_SHA256,
                                          entry->key,
                                          -1);
        g_autofree gchar *group =
            g_strconcat("permission ", digest, NULL);

        g_key_file_set_string(
            key_file, group, "origin", entry->origin);
        g_key_file_set_string(
            key_file, group, "category", entry->category);
        g_key_file_set_string(
            key_file,
            group,
            "decision",
            decision_name(entry->decision));
        g_key_file_set_int64(key_file,
                             group,
                             "updated-us",
                             entry->updated_us);
        g_key_file_set_int64(key_file,
                             group,
                             "expires-us",
                             entry->expires_us);
    }
    contents = g_key_file_to_data(key_file, &length, error);
    if (!contents)
        return FALSE;
    if (length > MUX_PERMISSION_STORE_MAX_FILE_BYTES) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NO_SPACE,
                            "permission store exceeds its size limit");
        return FALSE;
    }

    temporary = g_strconcat(store->path, ".tmp.XXXXXX", NULL);
    descriptor = g_mkstemp_full(
        temporary, O_RDWR | O_CLOEXEC, 0600);
    if (descriptor < 0)
        return set_file_error(
            error, errno, "create temporary permission store");
    if (!write_all(descriptor,
                   (const guint8 *)contents,
                   length,
                   error))
        goto out;
    if (fsync(descriptor) < 0) {
        set_file_error(error, errno, "sync permission store");
        goto out;
    }
    if (close(descriptor) < 0) {
        descriptor = -1;
        set_file_error(error, errno, "close permission store");
        goto out;
    }
    descriptor = -1;
    if (g_rename(temporary, store->path) < 0) {
        set_file_error(error, errno, "replace permission store");
        goto out;
    }
    success = TRUE;
    {
        g_autoptr(GError) sync_error = NULL;

        /*
         * rename() is the logical commit point. A directory fsync failure can
         * weaken crash durability, but reporting the update as uncommitted
         * would roll memory back while the new file is already visible.
         */
        if (!sync_parent_directory(store->profile_directory, &sync_error))
            g_warning("permission store directory sync failed: %s",
                      sync_error->message);
    }

out:
    if (descriptor >= 0)
        close(descriptor);
    if (!success)
        g_unlink(temporary);
    return success;
}

gboolean
mux_permission_store_set(MuxPermissionStore *store,
                         const gchar *origin,
                         const gchar *category,
                         MuxPermissionDecision decision,
                         GError **error)
{
    return mux_permission_store_set_for_duration(
        store,
        origin,
        category,
        decision,
        MUX_PERMISSION_STORE_DEFAULT_TTL_US,
        error);
}

gboolean
mux_permission_store_set_for_duration(
    MuxPermissionStore *store,
    const gchar *origin,
    const gchar *category,
    MuxPermissionDecision decision,
    gint64 duration_us,
    GError **error)
{
    g_autofree gchar *key = NULL;
    GHashTable *previous_entries;
    PermissionEntry *replacement = NULL;
    gint64 now_us;

    g_return_val_if_fail(store, FALSE);
    if (!valid_origin(origin, error) ||
        !valid_category(category, error)) {
        return FALSE;
    }
    if (!valid_decision(decision)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "invalid permission decision");
        return FALSE;
    }
    if (decision != MUX_PERMISSION_DECISION_ASK &&
        (duration_us <= 0 ||
         duration_us > MUX_PERMISSION_STORE_MAX_TTL_US)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "permission duration is outside its safety limit");
        return FALSE;
    }

    previous_entries = permission_entries_copy(store->entries);
    now_us = g_get_real_time();
    prune_expired(store, now_us);
    key = permission_key(origin, category);
    if (decision == MUX_PERMISSION_DECISION_ASK) {
        g_hash_table_remove(store->entries, key);
    } else {
        replacement = permission_entry_new(origin,
                                           category,
                                           decision,
                                           now_us,
                                           now_us + duration_us);
        g_hash_table_replace(
            store->entries, replacement->key, replacement);
    }
    enforce_entry_limit(store);

    if (!mux_permission_store_flush(store, error)) {
        g_hash_table_unref(store->entries);
        store->entries = previous_entries;
        return FALSE;
    }
    g_hash_table_unref(previous_entries);
    return TRUE;
}

guint
mux_permission_store_size(const MuxPermissionStore *store)
{
    GHashTableIter iterator;
    gpointer value;
    gint64 now_us;
    guint count = 0;

    g_return_val_if_fail(store, 0);
    now_us = g_get_real_time();
    g_hash_table_iter_init(&iterator, store->entries);
    while (g_hash_table_iter_next(&iterator, NULL, &value)) {
        if (!permission_entry_is_expired(value, now_us))
            count++;
    }
    return count;
}
