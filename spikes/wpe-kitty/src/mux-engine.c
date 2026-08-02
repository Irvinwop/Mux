#define _GNU_SOURCE

#include "mux-engine-protocol.h"
#include "mux-shortcuts.h"
#include "mux-clipboard-engine-link.h"
#include "mux-ui-engine.h"
#include "mux-download-engine.h"
#include "mux-file-chooser-engine.h"
#include "mux-popup-engine.h"
#include "mux-browser-affordance-engine.h"
#include "mux-browser-store.h"
#include "mux-input-method.h"
#include "mux-notification-engine.h"
#include "mux-navigation-policy.h"
#include "mux-uri.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>

#define MUX_ENGINE_MAX_CLIENTS 128u
#define BEFORE_UNLOAD_STAY_DATA "mux-before-unload-stay"
#define MUX_ENGINE_MAX_VIEWS 32u
#define MUX_ENGINE_MAX_CLIENT_VIEWS 32u
#define MUX_ENGINE_MAX_DIMENSION 8192u
#define MUX_ENGINE_MAX_PIXELS (9u * 1024u * 1024u)
#define MUX_ENGINE_FRAME_BUDGET_BYTES \
    ((gsize)256u * 1024u * 1024u)
#define MUX_ENGINE_MAX_LAYER_BYTES 128u
#define MUX_ENGINE_MAX_KITTY_ID_BYTES 128u
#define MUX_ENGINE_MAX_URI_BYTES (16u * 1024u)
#define MUX_ENGINE_TEXT_SHORTCUT_MODIFIERS \
    (WPE_MODIFIER_KEYBOARD_CONTROL | \
     WPE_MODIFIER_KEYBOARD_ALT | \
     WPE_MODIFIER_KEYBOARD_META)
#define MUX_ENGINE_KEY_LEFT 0xff51u
#define MUX_ENGINE_KEY_RIGHT 0xff53u
#define MUX_ENGINE_PALETTE_BOOKMARKS 32U
#define MUX_ENGINE_PALETTE_CLOSED 32U
#define MUX_ENGINE_PALETTE_HISTORY 96U
#define MUX_ENGINE_ENSURE_TIMEOUT_MS 5000U
#define MUX_ENGINE_ENSURE_POLL_MS 20U
#define MUX_ENGINE_ERROR_DETAIL_CHARACTERS 1024U
#define MUX_ENGINE_MAX_FRAME_RETRIES 2U
#define MUX_ENGINE_BACKPRESSURE_RETRY_MIN_MS 50U
#define MUX_ENGINE_BACKPRESSURE_RETRY_MAX_MS 1000U
#define MUX_POPUP_LAUNCH_TIMEOUT_MS 5000U
#define MUX_ENGINE_REGISTRY_VERSION 1U
#define MUX_ENGINE_MUXD_RETRY_MS 250U
#define MUX_ENGINE_MUXD_HANDSHAKE_MS 1000U
#define MUX_ENGINE_IDLE_EXIT_MS 5000U
#define MUX_ENGINE_MUXD_INPUT_BYTES 4096U
#define MUX_ENGINE_FIND_DEBOUNCE_MS 40U

typedef enum {
    ENGINE_FIND_SHORTCUT_NONE,
    ENGINE_FIND_SHORTCUT_OPEN,
    ENGINE_FIND_SHORTCUT_NEXT,
    ENGINE_FIND_SHORTCUT_PREVIOUS,
} EngineFindShortcut;

static guint
logical_dimension(guint physical, guint scale_milli)
{
    guint64 scaled = (guint64)physical * 1000u;
    guint64 logical = (scaled + scale_milli / 2u) / scale_milli;

    return logical ? (guint)logical : 1u;
}

static gdouble
physical_milli_to_logical(gint32 physical_milli, guint scale_milli)
{
    return (gdouble)physical_milli / (gdouble)scale_milli;
}

static EngineFindShortcut
find_shortcut(guint32 modifiers, guint32 keyval)
{
    const guint keyboard = WPE_MODIFIER_KEYBOARD_CONTROL |
        WPE_MODIFIER_KEYBOARD_SHIFT |
        WPE_MODIFIER_KEYBOARD_ALT |
        WPE_MODIFIER_KEYBOARD_META;
    guint key = keyval >= 'A' && keyval <= 'Z'
        ? keyval + ('a' - 'A')
        : keyval;
    guint exact = modifiers & keyboard;
    gboolean command = exact == WPE_MODIFIER_KEYBOARD_META ||
        exact == WPE_MODIFIER_KEYBOARD_CONTROL;
    gboolean command_shift =
        exact == (WPE_MODIFIER_KEYBOARD_META |
                  WPE_MODIFIER_KEYBOARD_SHIFT) ||
        exact == (WPE_MODIFIER_KEYBOARD_CONTROL |
                  WPE_MODIFIER_KEYBOARD_SHIFT);

    /* Control is an explicit Linux compatibility alias for Command. */
    if (key == 'f' && command)
        return ENGINE_FIND_SHORTCUT_OPEN;
    if (key == 'g' && command)
        return ENGINE_FIND_SHORTCUT_NEXT;
    if (key == 'g' && command_shift)
        return ENGINE_FIND_SHORTCUT_PREVIOUS;
    return ENGINE_FIND_SHORTCUT_NONE;
}

static gboolean
engine_idle_fallback_should_arm(gboolean had_owned_views,
                                guint active_views,
                                gboolean muxd_connected,
                                gboolean shutting_down)
{
    return had_owned_views && active_views == 0 &&
        !muxd_connected && !shutting_down;
}

static gboolean
engine_view_capacity_available(guint active_views, guint pending_views)
{
    return active_views <= MUX_ENGINE_MAX_VIEWS &&
        pending_views <= MUX_ENGINE_MAX_VIEWS - active_views &&
        active_views + pending_views < MUX_ENGINE_MAX_VIEWS;
}

static gboolean
prepare_initial_uri(gboolean popup_claim,
                    const gchar *uri,
                    const gchar *search_url,
                    gchar **normalized_uri,
                    GError **error)
{
    g_return_val_if_fail(normalized_uri != NULL, FALSE);

    *normalized_uri = NULL;
    if (popup_claim)
        return TRUE;
    *normalized_uri = mux_uri_resolve_user_input(uri, search_url, error);
    return *normalized_uri != NULL;
}

static guint
frame_backpressure_retry_delay_ms(guint rejection_count)
{
    guint shift = rejection_count > 0
        ? MIN(rejection_count - 1, 5u)
        : 0;
    guint delay = MUX_ENGINE_BACKPRESSURE_RETRY_MIN_MS << shift;

    return MIN(delay, MUX_ENGINE_BACKPRESSURE_RETRY_MAX_MS);
}

static WebKitNetworkSession *
engine_private_network_session_new(void)
{
    WebKitNetworkSession *session = webkit_network_session_new_ephemeral();

    if (!session)
        return NULL;
    webkit_network_session_set_itp_enabled(session, TRUE);
    webkit_network_session_set_persistent_credential_storage_enabled(session,
                                                                     FALSE);
    return session;
}

#ifdef MUX_ENGINE_LOGIC_TEST

gboolean
mux_engine_test_prepare_initial_uri(gboolean popup_claim,
                                    const gchar *uri,
                                    const gchar *search_url,
                                    gchar **normalized_uri,
                                    GError **error)
{
    return prepare_initial_uri(popup_claim,
                               uri,
                               search_url,
                               normalized_uri,
                               error);
}

gboolean
mux_engine_test_view_capacity(guint active_views, guint pending_views)
{
    return engine_view_capacity_available(active_views, pending_views);
}

guint
mux_engine_test_frame_backpressure_retry_delay_ms(guint rejection_count)
{
    return frame_backpressure_retry_delay_ms(rejection_count);
}

guint
mux_engine_test_logical_dimension(guint physical, guint scale_milli)
{
    return logical_dimension(physical, scale_milli);
}

gdouble
mux_engine_test_physical_milli_to_logical(gint32 physical_milli,
                                           guint scale_milli)
{
    return physical_milli_to_logical(physical_milli, scale_milli);
}

guint
mux_engine_test_find_shortcut(guint32 modifiers, guint32 keyval)
{
    return find_shortcut(modifiers, keyval);
}

WebKitNetworkSession *
mux_engine_test_private_network_session_new(void)
{
    return engine_private_network_session_new();
}

gboolean
mux_engine_test_idle_fallback_should_arm(gboolean had_owned_views,
                                         guint active_views,
                                         gboolean muxd_connected,
                                         gboolean shutting_down)
{
    return engine_idle_fallback_should_arm(had_owned_views,
                                           active_views,
                                           muxd_connected,
                                           shutting_down);
}

#else

typedef struct _Engine Engine;
typedef struct _Client Client;
typedef struct _EngineView EngineView;

typedef struct {
    GSubprocess *process;
    gchar *token;
    guint timeout_id;
    gboolean timed_out;
} PopupLaunch;

typedef enum {
    BROWSER_ACTION_NONE,
    BROWSER_ACTION_BACK,
    BROWSER_ACTION_FORWARD,
    BROWSER_ACTION_RELOAD,
    BROWSER_ACTION_STOP,
    BROWSER_ACTION_TOGGLE_BOOKMARK,
    BROWSER_ACTION_REOPEN_LAST,
    BROWSER_ACTION_LOAD_URI,
} BrowserActionKind;

typedef struct {
    BrowserActionKind kind;
    gchar *uri;
} BrowserPaletteAction;

typedef struct {
    EngineView *view;
    GPtrArray *actions;
} BrowserPalette;

typedef struct {
    guint x;
    guint y;
    guint width;
    guint height;
} DamageRect;

struct _Client {
    Engine *engine;
    int fd;
    guint watch_id;
    pid_t peer_pid;
    uid_t peer_uid;
    gboolean welcomed;
    gboolean failed;
    guint view_count;
    gchar *kitty_window;
    gchar *layer;
    gchar *initial_uri;
    guint width;
    guint height;
    guint scale_milli;
};

struct _EngineView {
    Engine *engine;
    Client *owner;
    guint64 id;
    WebKitWebView *web_view;
    WebKitNetworkSession *network_session;
    MuxDownloadManager *download_manager;
    MuxInputMethodContext *input_method;
    GHashTable *suppressed_text_keys;
    MuxUiEngineBridge *ui_bridge;
    MuxBrowserAffordanceBridge *affordance_bridge;
    MuxNotificationEngine *notification_engine;
    MuxNavigationPolicy *navigation_policy;
    MuxFileChooserBridge *file_chooser_bridge;
    MuxPopupManager *popup_manager;
    WebKitFindController *find_controller;
    GString *find_query;
    guint find_timeout_id;
    guint64 find_generation;
    guint64 find_pending_generation;
    MuxEngineFindStatus find_status;
    guint find_match_count;
    gboolean find_active;
    gboolean fullscreen;
    gchar *layer;
    guint width;
    guint height;
    guint scale_milli;
    gboolean ephemeral;
    gboolean focused;
    gboolean hidden;
    gboolean graphics_failed;
    guint frame_rejection_count;
    guint frame_backpressure_count;
    WPEView *platform_view;
    guint8 *pixels;
    gsize pixels_size;
    guint surface_width;
    guint surface_height;
    DamageRect dirty;
    gboolean dirty_valid;
    gboolean dirty_replaces_pending;
    gboolean root_frame_sent;
    gboolean frame_pending;
    guint64 pending_frame_serial;
    guint64 retired_frame_serial;
    gchar *pending_shm_name;
    gchar *pending_frame_uri;
    gchar *pending_frame_title;
    gsize pending_shm_size;
    guint frame_timeout_id;
    guint frame_retry_id;
    gboolean close_pending;
    gboolean close_ready;
    guint64 pending_close_serial;
    guint64 retired_close_serial;
};

struct _Engine {
    GMainLoop *loop;
    GPtrArray *clients;
    GHashTable *views;
    GHashTable *pending_popups;
    WPEDisplay *display;
    MuxClipboardEngineLink *clipboard_link;
    MuxPermissionStore *permission_store;
    MuxPermissionStore *ephemeral_permission_store;
    MuxDownloadManager *download_manager;
    MuxBrowserStore *browser_store;
    WebKitWebContext *web_context;
    WebKitNetworkSession *persistent_session;
    int listen_fd;
    int lock_fd;
    int smoke_frame_ack_fd;
    guint listen_watch_id;
    guint sigint_watch_id;
    guint sigterm_watch_id;
    int muxd_fd;
    guint muxd_watch_id;
    guint muxd_retry_id;
    guint muxd_handshake_id;
    guint idle_exit_id;
    GString *muxd_input;
    guint64 next_view_id;
    guint64 next_event_serial;
    guint64 clipboard_reply_view_id;
    Client *clipboard_reply_client;
    guint clipboard_tick_id;
    gsize frame_bytes;
    guint device_scale_milli;
    EngineView *pending_view;
    WPEView *(*original_create_view)(WPEDisplay *display);
    WPEClipboard *(*original_get_clipboard)(WPEDisplay *display);
    gchar *profile;
    gchar *socket_path;
    gchar *lock_path;
    gchar *data_directory;
    gchar *cache_directory;
    gboolean socket_bound;
    gboolean muxd_registered;
    gboolean had_owned_views;
    gboolean shutting_down;
};

typedef struct {
    WPEView parent_instance;
    EngineView *engine_view;
} MuxPlatformView;

typedef struct {
    WPEViewClass parent_class;
} MuxPlatformViewClass;

#define MUX_TYPE_PLATFORM_VIEW (mux_platform_view_get_type())

static Engine *display_engine;

static gboolean
smoke_frame_ack_telemetry_open(Engine *engine, GError **error)
{
    const gchar *path = g_getenv("MUX_SMOKE_FRAME_ACK_FILE");
    struct stat status;
    int fd;

    if (!path || !*path)
        return TRUE;
    if (!g_path_is_absolute(path)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "MUX_SMOKE_FRAME_ACK_FILE must be an absolute path");
        return FALSE;
    }

    fd = open(path,
              O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "open MUX_SMOKE_FRAME_ACK_FILE: %s",
                    g_strerror(errno));
        return FALSE;
    }
    if (fstat(fd, &status) < 0) {
        int saved_errno = errno;

        close(fd);
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(saved_errno),
                    "inspect MUX_SMOKE_FRAME_ACK_FILE: %s",
                    g_strerror(saved_errno));
        return FALSE;
    }
    if (!S_ISREG(status.st_mode) ||
        status.st_uid != getuid() ||
        (status.st_mode & 0077) != 0) {
        close(fd);
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_PERMISSION_DENIED,
            "MUX_SMOKE_FRAME_ACK_FILE must be an owner-only regular file");
        return FALSE;
    }

    engine->smoke_frame_ack_fd = fd;
    return TRUE;
}

static void
smoke_frame_telemetry_write(EngineView *view,
                            const gchar *event,
                            guint64 serial)
{
    Engine *engine = view->engine;
    g_autofree gchar *kitty_hash = NULL;
    g_autofree gchar *uri_hash = NULL;
    g_autofree gchar *title_hash = NULL;
    g_autofree gchar *record = NULL;
    const gchar *cursor;
    gsize remaining;

    if (engine->smoke_frame_ack_fd < 0)
        return;

    kitty_hash = g_compute_checksum_for_string(
        G_CHECKSUM_SHA256,
        view->owner && view->owner->kitty_window
            ? view->owner->kitty_window
            : "",
        -1);
    uri_hash = g_compute_checksum_for_string(
        G_CHECKSUM_SHA256,
        view->pending_frame_uri ? view->pending_frame_uri : "",
        -1);
    title_hash = g_compute_checksum_for_string(
        G_CHECKSUM_SHA256,
        view->pending_frame_title ? view->pending_frame_title : "",
        -1);
    record = g_strdup_printf(
        "%s\tview=%" G_GUINT64_FORMAT
        "\tserial=%" G_GUINT64_FORMAT
        "\tkitty_sha256=%s\turi_sha256=%s\ttitle_sha256=%s\n",
        event,
        view->id,
        serial,
        kitty_hash,
        uri_hash,
        title_hash);

    cursor = record;
    remaining = strlen(record);
    while (remaining) {
        ssize_t written = write(engine->smoke_frame_ack_fd,
                                cursor,
                                remaining);

        if (written > 0) {
            cursor += written;
            remaining -= (gsize)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        close(engine->smoke_frame_ack_fd);
        engine->smoke_frame_ack_fd = -1;
        return;
    }
}

static gboolean mux_platform_render_buffer(WPEView *platform_view,
                                           WPEBuffer *buffer,
                                           const WPERectangle *damage_rects,
                                           guint damage_count,
                                           GError **error);
static void engine_view_send_frame(EngineView *view);
static void engine_view_reset_surface(EngineView *view);
static void engine_view_find_close(EngineView *view, gboolean notify);
static gboolean engine_view_find_initialize(EngineView *view);

G_DEFINE_TYPE(MuxPlatformView, mux_platform_view, WPE_TYPE_VIEW)

static void
mux_platform_view_dispose(GObject *object)
{
    MuxPlatformView *platform_view = (MuxPlatformView *)object;

    if (platform_view->engine_view &&
        platform_view->engine_view->platform_view == WPE_VIEW(object))
        platform_view->engine_view->platform_view = NULL;
    platform_view->engine_view = NULL;
    G_OBJECT_CLASS(mux_platform_view_parent_class)->dispose(object);
}

static void
mux_platform_view_class_init(MuxPlatformViewClass *view_class)
{
    GObjectClass *object_class = G_OBJECT_CLASS(view_class);
    WPEViewClass *wpe_view_class = WPE_VIEW_CLASS(view_class);

    object_class->dispose = mux_platform_view_dispose;
    wpe_view_class->render_buffer = mux_platform_render_buffer;
}

static void
mux_platform_view_init(MuxPlatformView *view)
{
    (void)view;
}

static WPEView *
mux_display_create_view(WPEDisplay *display)
{
    MuxPlatformView *platform_view;
    EngineView *engine_view;

    if (!display_engine || !display_engine->pending_view) {
        if (display_engine && display_engine->original_create_view)
            return display_engine->original_create_view(display);
        return NULL;
    }

    engine_view = display_engine->pending_view;
    platform_view = g_object_new(MUX_TYPE_PLATFORM_VIEW,
                                 "display",
                                 display,
                                 NULL);
    platform_view->engine_view = engine_view;
    engine_view->platform_view = WPE_VIEW(platform_view);
    return WPE_VIEW(platform_view);
}

static void engine_view_free(gpointer data);
static void client_free(gpointer data);
static gboolean client_ready(gint fd, GIOCondition condition, gpointer data);
static void engine_update_idle_fallback(Engine *engine);
static void engine_muxd_schedule_reconnect(Engine *engine);

static gboolean
text_is_valid(const gchar *value, gsize maximum)
{
    gsize length;

    if (!value)
        return FALSE;
    length = strlen(value);
    return length <= maximum && g_utf8_validate(value, length, NULL);
}

static gboolean
profile_is_valid(const gchar *profile)
{
    gsize length;

    if (!profile || !*profile ||
        g_str_equal(profile, ".") || g_str_equal(profile, ".."))
        return FALSE;

    length = strlen(profile);
    if (length > 64)
        return FALSE;

    for (gsize i = 0; i < length; i++) {
        gchar byte = profile[i];
        if (!g_ascii_isalnum(byte) && byte != '-' && byte != '_' && byte != '.')
            return FALSE;
    }
    return TRUE;
}

static gboolean
dimensions_are_valid(guint width, guint height, guint scale_milli)
{
    return width > 0 && width <= MUX_ENGINE_MAX_DIMENSION &&
        height > 0 && height <= MUX_ENGINE_MAX_DIMENSION &&
        scale_milli >= MUX_ENGINE_MIN_SCALE_MILLI &&
        scale_milli <= MUX_ENGINE_MAX_SCALE_MILLI;
}

static gboolean
engine_view_apply_geometry(EngineView *view)
{
    WPEView *wpe_view = view->platform_view
        ? view->platform_view
        : webkit_web_view_get_wpe_view(view->web_view);
    WPEToplevel *toplevel = wpe_view
        ? wpe_view_get_toplevel(wpe_view)
        : NULL;
    WPEScreen *screen;
    guint logical_width;
    guint logical_height;
    int current_width;
    int current_height;
    gdouble scale;

    if (!toplevel)
        return FALSE;
    scale = view->scale_milli / 1000.0;
    logical_width = logical_dimension(view->width, view->scale_milli);
    logical_height = logical_dimension(view->height, view->scale_milli);
    screen = wpe_toplevel_get_screen(toplevel);
    if (screen)
        wpe_screen_set_scale(screen, scale);
    wpe_toplevel_scale_changed(toplevel, scale);
    wpe_toplevel_get_size(toplevel, &current_width, &current_height);
    if (current_width == (int)logical_width &&
        current_height == (int)logical_height)
        return TRUE;
    return wpe_toplevel_resize(toplevel, logical_width, logical_height);
}

static gboolean
ensure_private_directory(const gchar *path, GError **error)
{
    struct stat status;

    if (g_mkdir_with_parents(path, 0700) < 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "create directory %s: %s",
                    path,
                    g_strerror(errno));
        return FALSE;
    }
    if (stat(path, &status) < 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "inspect directory %s: %s",
                    path,
                    g_strerror(errno));
        return FALSE;
    }
    if (!S_ISDIR(status.st_mode) || status.st_uid != getuid()) {
        g_set_error(error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_ACCES,
                    "%s must be a directory owned by uid %u",
                    path,
                    (guint)getuid());
        return FALSE;
    }
    if (chmod(path, 0700) < 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "restrict directory %s: %s",
                    path,
                    g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

static gchar *
runtime_directory(void)
{
    const gchar *xdg_runtime = g_getenv("XDG_RUNTIME_DIR");

    if (xdg_runtime && g_path_is_absolute(xdg_runtime))
        return g_build_filename(xdg_runtime, "mux", NULL);
    return g_strdup_printf("/tmp/mux-%u", (guint)getuid());
}

static gchar *
default_socket_path(const gchar *profile)
{
    gchar *directory = runtime_directory();
    gchar *filename = g_strdup_printf("mux-engine-v%u-%s.sock",
                                      MUX_ENGINE_VERSION,
                                      profile);
    gchar *path = g_build_filename(directory, filename, NULL);

    g_free(filename);
    g_free(directory);
    return path;
}

static gchar *
muxd_socket_path(void)
{
    gchar *directory = runtime_directory();
    gchar *path = g_build_filename(directory, "muxd.sock", NULL);

    g_free(directory);
    return path;
}

static gboolean
engine_muxd_send_bytes(int fd, const gchar *data, gsize length)
{
    while (length > 0) {
        ssize_t written;

        do {
            written = send(fd, data, length, MSG_NOSIGNAL);
        } while (written < 0 && errno == EINTR);
        if (written <= 0)
            return FALSE;
        data += written;
        length -= (gsize)written;
    }
    return TRUE;
}

static gboolean
engine_idle_exit(gpointer data)
{
    Engine *engine = data;
    guint active_views = engine->views
        ? g_hash_table_size(engine->views)
        : 0;

    engine->idle_exit_id = 0;
    if (!engine_idle_fallback_should_arm(engine->had_owned_views,
                                         active_views,
                                         engine->muxd_fd >= 0,
                                         engine->shutting_down))
        return G_SOURCE_REMOVE;
    engine->shutting_down = TRUE;
    g_main_loop_quit(engine->loop);
    return G_SOURCE_REMOVE;
}

static void
engine_update_idle_fallback(Engine *engine)
{
    guint active_views = engine->views
        ? g_hash_table_size(engine->views)
        : 0;
    gboolean arm = engine_idle_fallback_should_arm(
        engine->had_owned_views,
        active_views,
        engine->muxd_fd >= 0,
        engine->shutting_down);

    if (!arm && engine->idle_exit_id) {
        g_source_remove(engine->idle_exit_id);
        engine->idle_exit_id = 0;
    } else if (arm && !engine->idle_exit_id) {
        engine->idle_exit_id = g_timeout_add(MUX_ENGINE_IDLE_EXIT_MS,
                                             engine_idle_exit,
                                             engine);
    }
}

static void
engine_muxd_drop(Engine *engine, gboolean from_watch)
{
    if (!from_watch && engine->muxd_watch_id)
        g_source_remove(engine->muxd_watch_id);
    engine->muxd_watch_id = 0;
    if (engine->muxd_handshake_id) {
        g_source_remove(engine->muxd_handshake_id);
        engine->muxd_handshake_id = 0;
    }
    if (engine->muxd_fd >= 0) {
        close(engine->muxd_fd);
        engine->muxd_fd = -1;
    }
    engine->muxd_registered = FALSE;
    if (engine->muxd_input)
        g_string_truncate(engine->muxd_input, 0);
    if (!engine->shutting_down)
        engine_muxd_schedule_reconnect(engine);
    engine_update_idle_fallback(engine);
}

static gboolean
engine_muxd_handle_line(Engine *engine, const gchar *line)
{
    if (g_strcmp0(line, "ENGINE_OK\t1") == 0) {
        engine->muxd_registered = TRUE;
        if (engine->muxd_handshake_id) {
            g_source_remove(engine->muxd_handshake_id);
            engine->muxd_handshake_id = 0;
        }
        engine_update_idle_fallback(engine);
        return TRUE;
    }
    if (engine->muxd_registered &&
        g_strcmp0(line, "ENGINE_STOP") == 0) {
        static const gchar bye[] = "ENGINE_BYE\n";

        engine->shutting_down = TRUE;
        (void)engine_muxd_send_bytes(engine->muxd_fd,
                                     bye,
                                     sizeof(bye) - 1);
        g_main_loop_quit(engine->loop);
        return TRUE;
    }
    return FALSE;
}

static gboolean
engine_muxd_ready(gint fd, GIOCondition condition, gpointer data)
{
    Engine *engine = data;
    gchar buffer[1024];
    gsize received = 0;

    (void)fd;
    if (!(condition & G_IO_IN)) {
        engine_muxd_drop(engine, TRUE);
        return G_SOURCE_REMOVE;
    }

    while (received < MUX_ENGINE_MUXD_INPUT_BYTES) {
        ssize_t count;

        do {
            count = recv(engine->muxd_fd,
                         buffer,
                         MIN(sizeof(buffer),
                             MUX_ENGINE_MUXD_INPUT_BYTES - received),
                         0);
        } while (count < 0 && errno == EINTR);
        if (count > 0) {
            received += (gsize)count;
            g_string_append_len(engine->muxd_input, buffer, count);
            if (engine->muxd_input->len > MUX_ENGINE_MUXD_INPUT_BYTES) {
                engine_muxd_drop(engine, TRUE);
                return G_SOURCE_REMOVE;
            }
            for (;;) {
                gchar *newline = memchr(engine->muxd_input->str,
                                        '\n',
                                        engine->muxd_input->len);
                gchar *line;
                gsize length;

                if (!newline)
                    break;
                length = (gsize)(newline - engine->muxd_input->str);
                line = g_strndup(engine->muxd_input->str, length);
                g_strchomp(line);
                g_string_erase(engine->muxd_input, 0, length + 1);
                if (!engine_muxd_handle_line(engine, line)) {
                    g_free(line);
                    engine_muxd_drop(engine, TRUE);
                    return G_SOURCE_REMOVE;
                }
                g_free(line);
                if (engine->shutting_down)
                    return G_SOURCE_CONTINUE;
            }
            continue;
        }
        if (count == 0) {
            engine_muxd_drop(engine, TRUE);
            return G_SOURCE_REMOVE;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        engine_muxd_drop(engine, TRUE);
        return G_SOURCE_REMOVE;
    }

    if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        engine_muxd_drop(engine, TRUE);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean
engine_muxd_handshake_timed_out(gpointer data)
{
    Engine *engine = data;

    engine->muxd_handshake_id = 0;
    if (!engine->muxd_registered)
        engine_muxd_drop(engine, FALSE);
    return G_SOURCE_REMOVE;
}

static gboolean
engine_muxd_connect(Engine *engine)
{
    g_autofree gchar *path = muxd_socket_path();
    g_autofree gchar *encoded_profile = NULL;
    g_autofree gchar *registration = NULL;
    struct sockaddr_un address = { 0 };
    struct ucred credentials;
    struct stat status;
    socklen_t credentials_size = sizeof(credentials);
    int fd;
    int flags;

    if (strlen(path) >= sizeof(address.sun_path) ||
        lstat(path, &status) < 0 || !S_ISSOCK(status.st_mode) ||
        status.st_uid != getuid() || (status.st_mode & 0077) != 0)
        return FALSE;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return FALSE;
    address.sun_family = AF_UNIX;
    g_strlcpy(address.sun_path, path, sizeof(address.sun_path));
    if (connect(fd,
                (const struct sockaddr *)&address,
                sizeof(address)) < 0 ||
        getsockopt(fd,
                   SOL_SOCKET,
                   SO_PEERCRED,
                   &credentials,
                   &credentials_size) < 0 ||
        credentials_size != sizeof(credentials) ||
        credentials.uid != getuid()) {
        close(fd);
        return FALSE;
    }

    encoded_profile = g_base64_encode((const guchar *)engine->profile,
                                      strlen(engine->profile));
    registration = g_strdup_printf("ENGINE\t%u\t%s\t%ld\n",
                                   MUX_ENGINE_REGISTRY_VERSION,
                                   encoded_profile,
                                   (long)getpid());
    if (!engine_muxd_send_bytes(fd,
                                registration,
                                strlen(registration))) {
        close(fd);
        return FALSE;
    }
    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return FALSE;
    }

    engine->muxd_fd = fd;
    if (!engine->muxd_input)
        engine->muxd_input = g_string_new(NULL);
    else
        g_string_truncate(engine->muxd_input, 0);
    engine->muxd_watch_id = g_unix_fd_add_full(
        G_PRIORITY_DEFAULT,
        fd,
        G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
        engine_muxd_ready,
        engine,
        NULL);
    engine->muxd_handshake_id = g_timeout_add(
        MUX_ENGINE_MUXD_HANDSHAKE_MS,
        engine_muxd_handshake_timed_out,
        engine);
    engine_update_idle_fallback(engine);
    return TRUE;
}

static gboolean
engine_muxd_retry(gpointer data)
{
    Engine *engine = data;

    engine->muxd_retry_id = 0;
    if (!engine->shutting_down && !engine_muxd_connect(engine))
        engine_muxd_schedule_reconnect(engine);
    return G_SOURCE_REMOVE;
}

static void
engine_muxd_schedule_reconnect(Engine *engine)
{
    if (!engine->shutting_down && engine->muxd_fd < 0 &&
        !engine->muxd_retry_id)
        engine->muxd_retry_id = g_timeout_add(MUX_ENGINE_MUXD_RETRY_MS,
                                              engine_muxd_retry,
                                              engine);
}

static gboolean
prepare_paths(Engine *engine, const gchar *socket_override, GError **error)
{
    const gchar *socket_environment = g_getenv("MUX_ENGINE_SOCKET");
    const gchar *data_environment = g_getenv("MUX_PROFILE_DATA_DIR");
    const gchar *cache_environment = g_getenv("MUX_PROFILE_CACHE_DIR");
    gchar *socket_directory;

    if (socket_override)
        engine->socket_path = g_strdup(socket_override);
    else if (socket_environment && *socket_environment)
        engine->socket_path = g_strdup(socket_environment);
    else
        engine->socket_path = default_socket_path(engine->profile);

    if (!g_path_is_absolute(engine->socket_path)) {
        g_set_error_literal(error,
                            G_FILE_ERROR,
                            G_FILE_ERROR_INVAL,
                            "engine socket path must be absolute");
        return FALSE;
    }
    if (strlen(engine->socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        g_set_error(error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_NAMETOOLONG,
                    "engine socket path is too long: %s",
                    engine->socket_path);
        return FALSE;
    }

    socket_directory = g_path_get_dirname(engine->socket_path);
    if (!ensure_private_directory(socket_directory, error)) {
        g_free(socket_directory);
        return FALSE;
    }
    g_free(socket_directory);

    engine->lock_path = g_strconcat(engine->socket_path, ".lock", NULL);
    engine->data_directory = data_environment && *data_environment
        ? g_strdup(data_environment)
        : g_build_filename(g_get_user_data_dir(),
                           "mux",
                           "profiles",
                           engine->profile,
                           NULL);
    engine->cache_directory = cache_environment && *cache_environment
        ? g_strdup(cache_environment)
        : g_build_filename(g_get_user_cache_dir(),
                           "mux",
                           "profiles",
                           engine->profile,
                           NULL);

    if (!g_path_is_absolute(engine->data_directory) ||
        !g_path_is_absolute(engine->cache_directory)) {
        g_set_error_literal(error,
                            G_FILE_ERROR,
                            G_FILE_ERROR_INVAL,
                            "profile data and cache directories must be absolute");
        return FALSE;
    }
    return TRUE;
}

static gboolean
acquire_engine_lock(Engine *engine, gboolean *already_running, GError **error)
{
    *already_running = FALSE;
    engine->lock_fd = open(engine->lock_path,
                           O_RDWR | O_CREAT | O_CLOEXEC,
                           0600);
    if (engine->lock_fd < 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "open engine lock %s: %s",
                    engine->lock_path,
                    g_strerror(errno));
        return FALSE;
    }
    if (fchmod(engine->lock_fd, 0600) < 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "restrict engine lock %s: %s",
                    engine->lock_path,
                    g_strerror(errno));
        return FALSE;
    }
    if (flock(engine->lock_fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            *already_running = TRUE;
            close(engine->lock_fd);
            engine->lock_fd = -1;
            return TRUE;
        }
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "lock engine profile %s: %s",
                    engine->profile,
                    g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

static gboolean
write_lock_pid(Engine *engine, GError **error)
{
    gchar buffer[64];
    gsize length;
    ssize_t written;

    length = (gsize)g_snprintf(buffer, sizeof(buffer), "%ld\n", (long)getpid());
    if (ftruncate(engine->lock_fd, 0) < 0 ||
        lseek(engine->lock_fd, 0, SEEK_SET) < 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "prepare engine lock pid: %s",
                    g_strerror(errno));
        return FALSE;
    }
    do {
        written = write(engine->lock_fd, buffer, length);
    } while (written < 0 && errno == EINTR);
    if (written < 0 || (gsize)written != length) {
        g_set_error(error,
                    G_FILE_ERROR,
                    written < 0 ? g_file_error_from_errno(errno) : G_FILE_ERROR_FAILED,
                    "write engine lock pid: %s",
                    written < 0 ? g_strerror(errno) : "short write");
        return FALSE;
    }
    return TRUE;
}

static gint
daemonize_engine(pid_t *child_pid, GError **error)
{
    pid_t child = fork();

    *child_pid = -1;
    if (child < 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "fork engine daemon: %s",
                    g_strerror(errno));
        return -1;
    }
    if (child > 0) {
        *child_pid = child;
        return 1;
    }

    *child_pid = 0;

    if (setsid() < 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "create engine daemon session: %s",
                    g_strerror(errno));
        return -1;
    }

    gint null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null_fd < 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "open /dev/null: %s",
                    g_strerror(errno));
        return -1;
    }
    if (dup2(null_fd, STDIN_FILENO) < 0 ||
        dup2(null_fd, STDOUT_FILENO) < 0 ||
        dup2(null_fd, STDERR_FILENO) < 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "redirect engine daemon streams: %s",
                    g_strerror(errno));
        close(null_fd);
        return -1;
    }
    if (null_fd > STDERR_FILENO)
        close(null_fd);
    return 0;
}

static gboolean
engine_listener_reachable(const gchar *socket_path)
{
    struct stat status;
    struct sockaddr_un address = { 0 };
    int fd;
    gboolean reachable;

    if (lstat(socket_path, &status) < 0 ||
        !S_ISSOCK(status.st_mode) ||
        status.st_uid != getuid() ||
        (status.st_mode & 0077) != 0)
        return FALSE;

    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return FALSE;
    address.sun_family = AF_UNIX;
    g_strlcpy(address.sun_path, socket_path, sizeof(address.sun_path));
    reachable = connect(fd,
                        (const struct sockaddr *)&address,
                        offsetof(struct sockaddr_un, sun_path) +
                            strlen(address.sun_path) + 1) == 0;
    close(fd);
    return reachable;
}

static void
remove_owned_engine_socket(const gchar *socket_path)
{
    struct stat status;

    if (lstat(socket_path, &status) == 0 &&
        S_ISSOCK(status.st_mode) &&
        status.st_uid == getuid())
        (void)unlink(socket_path);
}

static void
stop_daemon_child(pid_t child_pid)
{
    int status;

    if (child_pid <= 0)
        return;
    if (kill(child_pid, SIGTERM) < 0 && errno == ESRCH)
        return;
    for (guint attempt = 0; attempt < 50; attempt++) {
        pid_t waited;

        do {
            waited = waitpid(child_pid, &status, WNOHANG);
        } while (waited < 0 && errno == EINTR);
        if (waited == child_pid || (waited < 0 && errno == ECHILD))
            return;
        g_usleep(10000);
    }

    (void)kill(child_pid, SIGKILL);
    while (waitpid(child_pid, &status, 0) < 0 && errno == EINTR)
        continue;
}

static gboolean
wait_for_engine_listener(const gchar *socket_path,
                         pid_t child_pid,
                         GError **error)
{
    gint64 deadline = g_get_monotonic_time() +
        (gint64)MUX_ENGINE_ENSURE_TIMEOUT_MS * 1000;

    for (;;) {
        int status = 0;

        if (engine_listener_reachable(socket_path))
            return TRUE;
        if (child_pid > 0) {
            pid_t waited;

            do {
                waited = waitpid(child_pid, &status, WNOHANG);
            } while (waited < 0 && errno == EINTR);
            if (waited == child_pid) {
                remove_owned_engine_socket(socket_path);
                if (WIFEXITED(status)) {
                    g_set_error(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "engine daemon exited with status %d before its listener was ready",
                                WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    g_set_error(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "engine daemon was terminated by signal %d before its listener was ready",
                                WTERMSIG(status));
                } else {
                    g_set_error_literal(
                        error,
                        G_IO_ERROR,
                        G_IO_ERROR_FAILED,
                        "engine daemon exited before its listener was ready");
                }
                return FALSE;
            }
            if (waited < 0) {
                g_set_error(error,
                            G_IO_ERROR,
                            g_io_error_from_errno(errno),
                            "wait for engine daemon: %s",
                            g_strerror(errno));
                stop_daemon_child(child_pid);
                remove_owned_engine_socket(socket_path);
                return FALSE;
            }
        }
        if (g_get_monotonic_time() >= deadline)
            break;
        g_usleep((gulong)MUX_ENGINE_ENSURE_POLL_MS * 1000);
    }

    if (child_pid > 0) {
        stop_daemon_child(child_pid);
        remove_owned_engine_socket(socket_path);
    }
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_TIMED_OUT,
                "engine listener %s was not reachable within %u ms",
                socket_path,
                MUX_ENGINE_ENSURE_TIMEOUT_MS);
    return FALSE;
}

static gboolean
client_send(Client *client,
            guint16 type,
            guint32 flags,
            guint64 view_id,
            guint64 serial,
            GBytes *payload)
{
    MuxEngineMessage message = {
        .type = type,
        .flags = flags,
        .view_id = view_id,
        .serial = serial,
        .payload = payload,
    };
    GError *error = NULL;

    if (client->failed)
        return FALSE;
    if (mux_engine_send_message(client->fd, &message, &error))
        return TRUE;

    g_warning("engine client %ld send failed: %s",
              (long)client->peer_pid,
              error->message);
    g_clear_error(&error);
    client->failed = TRUE;
    shutdown(client->fd, SHUT_RDWR);
    return FALSE;
}

static EngineView *
clipboard_find_view(Engine *engine, guint64 view_id, Client *owner)
{
    GHashTableIter iterator;
    gpointer value;
    EngineView *fallback = NULL;

    if (!engine || !engine->views)
        return NULL;

    g_hash_table_iter_init(&iterator, engine->views);
    while (g_hash_table_iter_next(&iterator, NULL, &value)) {
        EngineView *view = value;

        if (owner && view->owner != owner)
            continue;
        if (view_id && view->id == view_id)
            return view;
        if (!fallback || view->focused)
            fallback = view;
    }
    return view_id ? NULL : fallback;
}

static gchar *
clipboard_origin(EngineView *view)
{
    const gchar *uri = webkit_web_view_get_uri(view->web_view);
    GUri *parsed;
    const gchar *scheme;
    const gchar *host;
    gchar *origin;
    gint port;

    if (!uri || !*uri)
        return g_strdup("unknown");

    parsed = g_uri_parse(uri, G_URI_FLAGS_PARSE_RELAXED, NULL);
    if (!parsed)
        return g_strdup("unknown");
    scheme = g_uri_get_scheme(parsed);
    host = g_uri_get_host(parsed);
    if (!scheme || !host) {
        g_uri_unref(parsed);
        return g_strdup("unknown");
    }

    port = g_uri_get_port(parsed);
    origin = port > 0
        ? g_strdup_printf("%s://%s:%d", scheme, host, port)
        : g_strdup_printf("%s://%s", scheme, host);
    g_uri_unref(parsed);
    return origin;
}

static gboolean
clipboard_output(MuxClipboardEngineLink *link,
                 GBytes *packet,
                 gpointer data,
                 GError **error)
{
    Engine *engine = data;
    EngineView *view;

    (void)link;
    view = clipboard_find_view(engine,
                               engine->clipboard_reply_view_id,
                               engine->clipboard_reply_client);
    if (!view)
        view = clipboard_find_view(engine, 0, NULL);
    if (!view) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_CONNECTED,
                            "no pane owns the active clipboard view");
        return FALSE;
    }

    if (client_send(view->owner,
                    MUX_ENGINE_MESSAGE_EXTENSION,
                    MUX_ENGINE_FLAG_NONE,
                    view->id,
                    ++engine->next_event_serial,
                    packet))
        return TRUE;

    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_BROKEN_PIPE,
                        "clipboard packet could not be sent to its pane");
    return FALSE;
}

static void
clipboard_paste(MuxClipboardEngineLink *link,
                guint64 target_view_id,
                const MuxClipboardSnapshot *snapshot,
                gpointer data)
{
    Engine *engine = data;
    EngineView *view = clipboard_find_view(engine, target_view_id, NULL);

    (void)link;
    (void)snapshot;
    if (!view)
        view = clipboard_find_view(engine, 0, NULL);
    if (view)
        webkit_web_view_execute_editing_command(view->web_view, "Paste");
}

static void
clipboard_failure(MuxClipboardEngineLink *link,
                  const gchar *operation,
                  const GError *error,
                  gpointer data)
{
    (void)link;
    (void)data;
    g_warning("clipboard %s failed: %s",
              operation ? operation : "operation",
              error ? error->message : "unknown error");
}

static WPEClipboard *
mux_display_get_clipboard(WPEDisplay *display)
{
    if (display_engine && display_engine->clipboard_link)
        return mux_clipboard_engine_link_get_clipboard(
            display_engine->clipboard_link);
    if (display_engine && display_engine->original_get_clipboard)
        return display_engine->original_get_clipboard(display);
    return NULL;
}

static gboolean
clipboard_tick(gpointer data)
{
    Engine *engine = data;
    GHashTableIter iterator;
    gpointer value;

    if (engine->clipboard_link)
        (void)mux_clipboard_engine_link_tick(engine->clipboard_link,
                                             g_get_monotonic_time());
    if (engine->views) {
        g_hash_table_iter_init(&iterator, engine->views);
        while (g_hash_table_iter_next(&iterator, NULL, &value)) {
            EngineView *view = value;

            if (view->popup_manager)
                (void)mux_popup_manager_tick(view->popup_manager,
                                             g_get_monotonic_time());
        }
    }
    return G_SOURCE_CONTINUE;
}

static gboolean
ui_output(GBytes *payload, gpointer data, GError **error)
{
    EngineView *view = data;

    if (view->owner &&
        client_send(view->owner,
                    MUX_ENGINE_MESSAGE_EXTENSION,
                    MUX_ENGINE_FLAG_NONE,
                    view->id,
                    ++view->engine->next_event_serial,
                    payload))
        return TRUE;

    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_BROKEN_PIPE,
                        "UI request could not be sent to its pane");
    return FALSE;
}

static EngineView *
view_for_web_view(Engine *engine, WebKitWebView *web_view)
{
    GHashTableIter iterator;
    gpointer value;

    if (!engine || !engine->views)
        return NULL;
    g_hash_table_iter_init(&iterator, engine->views);
    while (g_hash_table_iter_next(&iterator, NULL, &value)) {
        EngineView *view = value;

        if (view->web_view == web_view)
            return view;
    }
    return NULL;
}

static gboolean
download_output(WebKitWebView *source_view,
                GBytes *payload,
                gpointer data,
                GError **error)
{
    Engine *engine = data;
    EngineView *view = view_for_web_view(engine, source_view);

    if (!view)
        view = clipboard_find_view(engine, 0, NULL);
    if (view && view->owner &&
        client_send(view->owner,
                    MUX_ENGINE_MESSAGE_EXTENSION,
                    MUX_ENGINE_FLAG_NONE,
                    view->id,
                    ++engine->next_event_serial,
                    payload))
        return TRUE;

    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_BROKEN_PIPE,
                        "download request could not be sent to a pane");
    return FALSE;
}

static gboolean
add_download_clipboard_text(MuxClipboardSnapshot *snapshot,
                            const gchar *mime,
                            const gchar *text,
                            GError **error)
{
    GBytes *bytes = g_bytes_new(text, strlen(text));
    gboolean result =
        mux_clipboard_snapshot_add(snapshot, mime, bytes, error);

    g_bytes_unref(bytes);
    return result;
}

static MuxClipboardSnapshot *
download_clipboard_snapshot(const gchar *path,
                            const gchar *mime_type,
                            GError **error)
{
    g_autofree gchar *uri = g_filename_to_uri(path, NULL, error);
    g_autofree gchar *uri_list = NULL;
    g_autofree gchar *gnome_files = NULL;
    MuxClipboardSnapshot *snapshot;
    const gchar *content_mime =
        mux_clipboard_mime_is_valid(mime_type)
            ? mime_type
            : "application/octet-stream";
    struct stat status;

    if (!uri)
        return NULL;
    uri_list = g_strconcat(uri, "\r\n", NULL);
    gnome_files = g_strconcat("copy\n", uri, "\n", NULL);
    snapshot = mux_clipboard_snapshot_new((guint64)g_get_monotonic_time());
    if (!add_download_clipboard_text(snapshot,
                                     "text/uri-list",
                                     uri_list,
                                     error) ||
        !add_download_clipboard_text(snapshot,
                                     "x-special/gnome-copied-files",
                                     gnome_files,
                                     error) ||
        !add_download_clipboard_text(snapshot,
                                     "application/x-kde4-urilist",
                                     uri_list,
                                     error) ||
        !add_download_clipboard_text(snapshot,
                                     "public.file-url",
                                     uri,
                                     error) ||
        !add_download_clipboard_text(snapshot,
                                     "text/plain;charset=utf-8",
                                     path,
                                     error)) {
        mux_clipboard_snapshot_unref(snapshot);
        return NULL;
    }

    if (stat(path, &status) == 0 && S_ISREG(status.st_mode) &&
        status.st_size >= 0 &&
        (guint64)status.st_size <= MUX_CLIPBOARD_MAX_ITEM_BYTES &&
        !mux_clipboard_snapshot_find(snapshot, content_mime)) {
        GBytes *bytes = NULL;
        GMappedFile *mapped = NULL;

        if (status.st_size == 0) {
            bytes = g_bytes_new_static("", 0);
        } else {
            mapped = g_mapped_file_new(path, FALSE, NULL);
            if (mapped)
                bytes = g_mapped_file_get_bytes(mapped);
        }
        if (bytes && !mux_clipboard_snapshot_add(snapshot,
                                                  content_mime,
                                                  bytes,
                                                  error)) {
            g_bytes_unref(bytes);
            if (mapped)
                g_mapped_file_unref(mapped);
            mux_clipboard_snapshot_unref(snapshot);
            return NULL;
        }
        if (bytes)
            g_bytes_unref(bytes);
        if (mapped)
            g_mapped_file_unref(mapped);
    }
    mux_clipboard_snapshot_seal(snapshot);
    return snapshot;
}

static gboolean
download_clipboard_output(WebKitWebView *source_view,
                          const gchar *path,
                          const gchar *mime_type,
                          gpointer data,
                          GError **error)
{
    Engine *engine = data;
    EngineView *view = view_for_web_view(engine, source_view);
    g_autoptr(MuxClipboardSnapshot) snapshot = NULL;
    g_autoptr(MuxClipboardEngineWrite) write = NULL;
    g_autofree gchar *origin = NULL;

    if (!view || !engine->clipboard_link) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_CONNECTED,
                            "clipboard download source pane is unavailable");
        return FALSE;
    }
    snapshot = download_clipboard_snapshot(path, mime_type, error);
    if (!snapshot)
        return FALSE;
    origin = clipboard_origin(view);
    if (!mux_clipboard_engine_link_set_active_source(engine->clipboard_link,
                                                     view->id,
                                                     origin,
                                                     view->ephemeral,
                                                     error))
        return FALSE;
    write = mux_clipboard_engine_link_begin_write(engine->clipboard_link);
    if (!write ||
        !mux_clipboard_engine_link_complete_write(engine->clipboard_link,
                                                  write,
                                                  snapshot,
                                                  error))
        return FALSE;
    mux_wpe_clipboard_set_external(
        MUX_WPE_CLIPBOARD(mux_clipboard_engine_link_get_clipboard(
            engine->clipboard_link)),
        snapshot);
    return TRUE;
}

static gboolean
download_context_to_clipboard(WebKitWebView *web_view,
                              const gchar *uri,
                              gpointer data,
                              GError **error)
{
    EngineView *view = data;
    MuxDownloadManager *manager = view->ephemeral
        ? view->download_manager
        : view->engine->download_manager;

    if (!manager) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_CONNECTED,
                            "download manager is unavailable");
        return FALSE;
    }
    return mux_download_manager_download_uri_to_clipboard(manager,
                                                          web_view,
                                                          uri,
                                                          error);
}

static gboolean
engine_view_prepare_private_network(EngineView *view, GError **error)
{
    g_return_val_if_fail(view != NULL, FALSE);
    g_return_val_if_fail(view->ephemeral, FALSE);

    view->network_session = engine_private_network_session_new();
    if (!view->network_session) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "private WebKit network session construction failed");
        return FALSE;
    }
    view->download_manager = mux_download_manager_new(view->network_session,
                                                      download_output,
                                                      NULL,
                                                      view->engine,
                                                      NULL);
    if (!view->download_manager) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "private WebKit download manager construction failed");
        return FALSE;
    }
    mux_download_manager_set_clipboard_func(view->download_manager,
                                            download_clipboard_output);
    return TRUE;
}

static gboolean
popup_token_valid(const gchar *token)
{
    if (!token || strlen(token) != MUX_POPUP_TOKEN_LENGTH)
        return FALSE;
    for (guint i = 0; i < MUX_POPUP_TOKEN_LENGTH; i++) {
        if (!g_ascii_isxdigit(token[i]))
            return FALSE;
    }
    return TRUE;
}

static void
popup_launch_free(PopupLaunch *launch)
{
    if (!launch)
        return;
    if (launch->timeout_id)
        g_source_remove(launch->timeout_id);
    g_clear_object(&launch->process);
    g_free(launch->token);
    g_free(launch);
}

static gboolean
popup_launch_timeout(gpointer data)
{
    PopupLaunch *launch = data;

    launch->timeout_id = 0;
    launch->timed_out = TRUE;
    g_warning("popup launcher timed out for token %.8s", launch->token);
    g_subprocess_force_exit(launch->process);
    return G_SOURCE_REMOVE;
}

static void
popup_launch_finished(GObject *source,
                      GAsyncResult *result,
                      gpointer data)
{
    PopupLaunch *launch = data;
    g_autoptr(GError) error = NULL;

    if (launch->timeout_id) {
        g_source_remove(launch->timeout_id);
        launch->timeout_id = 0;
    }
    if (!g_subprocess_wait_check_finish(G_SUBPROCESS(source),
                                        result,
                                        &error) &&
        !launch->timed_out) {
        g_warning("popup launcher failed for token %.8s: %s",
                  launch->token,
                  error ? error->message : "unknown error");
    }
    popup_launch_free(launch);
}

static WebKitWebView *
popup_create(WebKitWebView *parent,
             WebKitNavigationAction *navigation_action,
             gpointer data,
             GError **error)
{
    EngineView *parent_view = data;
    Engine *engine = parent_view->engine;
    EngineView *child = g_new0(EngineView, 1);

    (void)navigation_action;
    if (!engine_view_capacity_available(
            g_hash_table_size(engine->views),
            g_hash_table_size(engine->pending_popups))) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NO_SPACE,
                            "engine view limit reached");
        g_free(child);
        return NULL;
    }
    child->engine = engine;
    child->hidden = TRUE;
    child->layer = g_strdup(parent_view->layer);
    child->width = parent_view->width;
    child->height = parent_view->height;
    child->scale_milli = parent_view->scale_milli;
    child->ephemeral = parent_view->ephemeral;

    if (child->ephemeral &&
        !engine_view_prepare_private_network(child, error)) {
        engine_view_free(child);
        return NULL;
    }

    engine->pending_view = child;
    if (child->ephemeral) {
        child->web_view = WEBKIT_WEB_VIEW(
            g_object_new(WEBKIT_TYPE_WEB_VIEW,
                         "web-context", engine->web_context,
                         "network-session", child->network_session,
                         "display", engine->display,
                         NULL));
    } else {
        child->web_view = WEBKIT_WEB_VIEW(
            g_object_new(WEBKIT_TYPE_WEB_VIEW,
                         "related-view", parent,
                         "display", engine->display,
                         NULL));
    }
    engine->pending_view = NULL;
    if (!child->web_view || !child->platform_view) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "related popup WebView construction failed");
        engine_view_free(child);
        return NULL;
    }
    if (!engine_view_apply_geometry(child)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "related popup WPE geometry setup failed");
        engine_view_free(child);
        return NULL;
    }

    g_hash_table_insert(engine->pending_popups,
                        child->web_view,
                        child);
    return child->web_view;
}

static gboolean
popup_offer(WebKitWebView *parent,
            WebKitWebView *child,
            const gchar *token,
            gpointer data,
            GError **error)
{
    EngineView *parent_view = data;
    const gchar *listen_on = g_getenv("KITTY_LISTEN_ON");
    g_autofree gchar *self = NULL;
    g_autofree gchar *directory = NULL;
    g_autofree gchar *pane_binary = NULL;
    g_autofree gchar *source_match = NULL;
    g_autofree gchar *profile_env = NULL;
    g_autofree gchar *layer_env = NULL;
    g_autofree gchar *token_env = NULL;
    g_autofree gchar *ephemeral_env = NULL;
    g_autoptr(GPtrArray) arguments = g_ptr_array_new();
    GSubprocess *process;
    PopupLaunch *launch;

    (void)parent;
    (void)child;
    if (!listen_on || !*listen_on || !parent_view->owner) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_CONNECTED,
                            "Kitty remote control is unavailable");
        return FALSE;
    }

    self = g_file_read_link("/proc/self/exe", NULL);
    directory = self ? g_path_get_dirname(self) : NULL;
    pane_binary = directory
        ? g_build_filename(directory, "mux-pane", NULL)
        : g_strdup("mux-pane");
    if (parent_view->owner->kitty_window &&
        *parent_view->owner->kitty_window)
        source_match = g_strdup_printf(
            "id:%s", parent_view->owner->kitty_window);
    profile_env = g_strdup_printf("MUX_PROFILE=%s",
                                  parent_view->engine->profile);
    layer_env = g_strdup_printf("MUX_LAYER=%s", parent_view->layer);
    token_env = g_strdup_printf("MUX_POPUP_TOKEN=%s", token);
    ephemeral_env = g_strdup_printf("MUX_EPHEMERAL=%u",
                                    parent_view->ephemeral ? 1 : 0);

    g_ptr_array_add(arguments, (gpointer)"kitten");
    g_ptr_array_add(arguments, (gpointer)"@");
    g_ptr_array_add(arguments, (gpointer)"--to");
    g_ptr_array_add(arguments, (gpointer)listen_on);
    g_ptr_array_add(arguments, (gpointer)"launch");
    g_ptr_array_add(arguments, (gpointer)"--type=window");
    g_ptr_array_add(arguments, (gpointer)"--location=neighbor");
    if (source_match) {
        g_ptr_array_add(arguments, (gpointer)"--source-window");
        g_ptr_array_add(arguments, source_match);
        g_ptr_array_add(arguments, (gpointer)"--next-to");
        g_ptr_array_add(arguments, source_match);
    }
    g_ptr_array_add(arguments, (gpointer)"--cwd=current");
    g_ptr_array_add(arguments, (gpointer)"--copy-env");
    g_ptr_array_add(arguments, (gpointer)"--env");
    g_ptr_array_add(arguments, profile_env);
    g_ptr_array_add(arguments, (gpointer)"--env");
    g_ptr_array_add(arguments, layer_env);
    g_ptr_array_add(arguments, (gpointer)"--env");
    g_ptr_array_add(arguments, token_env);
    g_ptr_array_add(arguments, (gpointer)"--env");
    g_ptr_array_add(arguments, ephemeral_env);
    g_ptr_array_add(arguments, (gpointer)"--title");
    g_ptr_array_add(arguments, (gpointer)"Mux popup");
    g_ptr_array_add(arguments, pane_binary);
    g_ptr_array_add(arguments, (gpointer)"about:blank");
    g_ptr_array_add(arguments, NULL);

    process = g_subprocess_newv(
        (const gchar * const *)arguments->pdata,
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
            G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        error);
    if (!process)
        return FALSE;

    launch = g_new0(PopupLaunch, 1);
    launch->process = process;
    launch->token = g_strdup(token);
    launch->timeout_id = g_timeout_add(MUX_POPUP_LAUNCH_TIMEOUT_MS,
                                       popup_launch_timeout,
                                       launch);
    g_subprocess_wait_check_async(process,
                                  NULL,
                                  popup_launch_finished,
                                  launch);
    return TRUE;
}

static void
popup_destroy(WebKitWebView *child, gpointer data)
{
    EngineView *parent_view = data;
    EngineView *child_view = g_hash_table_lookup(
        parent_view->engine->pending_popups,
        child);

    if (!child_view) {
        g_object_unref(child);
        return;
    }
    g_hash_table_steal(parent_view->engine->pending_popups, child);
    engine_view_free(child_view);
}

static EngineView *
claim_popup(Engine *engine, const gchar *token)
{
    GHashTableIter iterator;
    gpointer value;
    WebKitWebView *child = NULL;
    EngineView *view;

    g_hash_table_iter_init(&iterator, engine->views);
    while (g_hash_table_iter_next(&iterator, NULL, &value)) {
        EngineView *parent_view = value;
        g_autoptr(GError) claim_error = NULL;

        if (!parent_view->popup_manager)
            continue;
        child = mux_popup_manager_claim(parent_view->popup_manager,
                                        token,
                                        &claim_error);
        if (child)
            break;
    }
    if (!child)
        return NULL;

    view = g_hash_table_lookup(engine->pending_popups, child);
    if (!view) {
        g_object_unref(child);
        return NULL;
    }
    g_hash_table_steal(engine->pending_popups, child);
    return view;
}

static gboolean
attach_popup_manager(EngineView *view, GError **error)
{
    view->popup_manager = mux_popup_manager_new(view->web_view,
                                                popup_create,
                                                popup_offer,
                                                popup_destroy,
                                                view,
                                                NULL);
    if (view->popup_manager)
        return TRUE;
    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_FAILED,
                        "popup manager construction failed");
    return FALSE;
}

static gboolean
client_send_empty(Client *client,
                  guint16 type,
                  guint64 view_id,
                  guint64 serial)
{
    GBytes *payload = g_bytes_new(NULL, 0);
    gboolean result = client_send(client,
                                  type,
                                  MUX_ENGINE_FLAG_NONE,
                                  view_id,
                                  serial,
                                  payload);

    g_bytes_unref(payload);
    return result;
}

static gboolean
client_send_error(Client *client,
                  const MuxEngineMessage *request,
                  MuxEngineRemoteError code,
                  const gchar *detail)
{
    MuxEngineBuilder builder;
    GBytes *payload;
    gboolean result;
    const gchar *message = detail ? detail : "engine error";
    g_autofree gchar *bounded_detail = NULL;
    glong character_count;

    if (!g_utf8_validate(message, -1, NULL))
        message = "engine error";
    character_count = g_utf8_strlen(message, -1);
    bounded_detail = g_utf8_substring(
        message,
        0,
        MIN(character_count, (glong)MUX_ENGINE_ERROR_DETAIL_CHARACTERS));

    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u32(&builder, code);
    mux_engine_builder_put_string(&builder, bounded_detail);
    payload = mux_engine_builder_finish(&builder);
    result = client_send(client,
                         MUX_ENGINE_MESSAGE_ERROR,
                         MUX_ENGINE_FLAG_NONE,
                         request ? request->view_id : 0,
                         request ? request->serial : 0,
                         payload);
    g_bytes_unref(payload);
    return result;
}

static gboolean
client_send_ack(Client *client, const MuxEngineMessage *request)
{
    return client_send_empty(client,
                             MUX_ENGINE_MESSAGE_ACK,
                             request->view_id,
                             request->serial);
}

#define DRM_FOURCC(a, b, c, d) \
    ((guint32)(a) | ((guint32)(b) << 8) | \
     ((guint32)(c) << 16) | ((guint32)(d) << 24))
#define DRM_FORMAT_XRGB8888 DRM_FOURCC('X', 'R', '2', '4')

static DamageRect
damage_full(guint width, guint height)
{
    DamageRect rectangle = { 0, 0, width, height };
    return rectangle;
}

static gboolean
damage_is_full(const DamageRect *rectangle, guint width, guint height)
{
    return rectangle->x == 0 && rectangle->y == 0 &&
        rectangle->width == width && rectangle->height == height;
}

static void
damage_union(DamageRect *destination, const DamageRect *source)
{
    guint64 right = MAX((guint64)destination->x + destination->width,
                        (guint64)source->x + source->width);
    guint64 bottom = MAX((guint64)destination->y + destination->height,
                         (guint64)source->y + source->height);

    destination->x = MIN(destination->x, source->x);
    destination->y = MIN(destination->y, source->y);
    destination->width = (guint)(right - destination->x);
    destination->height = (guint)(bottom - destination->y);
}

static DamageRect
coalesce_damage(const WPERectangle *rectangles,
                guint rectangle_count,
                guint width,
                guint height)
{
    DamageRect result = { 0 };
    gboolean found = FALSE;

    for (guint i = 0; i < rectangle_count; i++) {
        gint64 left = CLAMP((gint64)rectangles[i].x, 0, (gint64)width);
        gint64 top = CLAMP((gint64)rectangles[i].y, 0, (gint64)height);
        gint64 right = CLAMP((gint64)rectangles[i].x +
                                 rectangles[i].width,
                             0,
                             (gint64)width);
        gint64 bottom = CLAMP((gint64)rectangles[i].y +
                                  rectangles[i].height,
                              0,
                              (gint64)height);
        DamageRect rectangle;

        if (right <= left || bottom <= top)
            continue;
        rectangle.x = (guint)left;
        rectangle.y = (guint)top;
        rectangle.width = (guint)(right - left);
        rectangle.height = (guint)(bottom - top);
        if (found)
            damage_union(&result, &rectangle);
        else {
            result = rectangle;
            found = TRUE;
        }
    }
    return found ? result : damage_full(width, height);
}

static void
engine_frame_bytes_release(Engine *engine, gsize bytes)
{
    if (!bytes)
        return;
    g_return_if_fail(bytes <= engine->frame_bytes);
    engine->frame_bytes -= bytes;
}

static gboolean
engine_frame_bytes_reserve(Engine *engine, gsize bytes)
{
    if (engine->frame_bytes > MUX_ENGINE_FRAME_BUDGET_BYTES ||
        bytes > MUX_ENGINE_FRAME_BUDGET_BYTES - engine->frame_bytes)
        return FALSE;
    engine->frame_bytes += bytes;
    return TRUE;
}

static void
engine_view_clear_pending_frame(EngineView *view)
{
    if (view->frame_pending &&
        view->pending_frame_serial > view->retired_frame_serial)
        view->retired_frame_serial = view->pending_frame_serial;
    if (view->frame_timeout_id) {
        g_source_remove(view->frame_timeout_id);
        view->frame_timeout_id = 0;
    }
    if (view->pending_shm_name) {
        shm_unlink(view->pending_shm_name);
        g_clear_pointer(&view->pending_shm_name, g_free);
    }
    g_clear_pointer(&view->pending_frame_uri, g_free);
    g_clear_pointer(&view->pending_frame_title, g_free);
    if (view->pending_shm_size) {
        engine_frame_bytes_release(view->engine,
                                   view->pending_shm_size);
        view->pending_shm_size = 0;
    }
    view->frame_pending = FALSE;
    view->pending_frame_serial = 0;
}

static gboolean
frame_timeout(gpointer data)
{
    EngineView *view = data;
    guint64 serial = view->pending_frame_serial;

    view->frame_timeout_id = 0;
    engine_view_clear_pending_frame(view);
    if (view->owner && !view->owner->failed) {
        g_warning("Kitty did not acknowledge frame %" G_GUINT64_FORMAT
                  " for view %" G_GUINT64_FORMAT,
                  serial,
                  view->id);
        view->owner->failed = TRUE;
        shutdown(view->owner->fd, SHUT_RDWR);
    }
    return G_SOURCE_REMOVE;
}

static gchar *
create_frame_shm(EngineView *view,
                 const DamageRect *rectangle,
                 gsize *size_out)
{
    gsize row_bytes = (gsize)rectangle->width * 4;
    gsize size = row_bytes * rectangle->height;
    gchar *name = NULL;
    int fd = -1;
    guint8 *mapping;

    if (!engine_frame_bytes_reserve(view->engine, size))
        return NULL;
    for (guint attempt = 0; attempt < 16; attempt++) {
        g_free(name);
        name = g_strdup_printf(
            "/mux-tty-graphics-protocol-%u-%ld-%" G_GUINT64_FORMAT "-%08x",
            (guint)getuid(),
            (long)getpid(),
            view->id,
            g_random_int());
        fd = shm_open(name,
                      O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC,
                      0600);
        if (fd >= 0)
            break;
        if (errno != EEXIST)
            break;
    }
    if (fd < 0) {
        g_warning("create Kitty frame shared memory: %s",
                  g_strerror(errno));
        engine_frame_bytes_release(view->engine, size);
        g_free(name);
        return NULL;
    }
    if (ftruncate(fd, (off_t)size) < 0) {
        g_warning("size Kitty frame shared memory: %s",
                  g_strerror(errno));
        close(fd);
        shm_unlink(name);
        engine_frame_bytes_release(view->engine, size);
        g_free(name);
        return NULL;
    }

    mapping = mmap(NULL,
                   size,
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED,
                   fd,
                   0);
    close(fd);
    if (mapping == MAP_FAILED) {
        g_warning("map Kitty frame shared memory: %s",
                  g_strerror(errno));
        shm_unlink(name);
        engine_frame_bytes_release(view->engine, size);
        g_free(name);
        return NULL;
    }

    for (guint row = 0; row < rectangle->height; row++) {
        const guint8 *source =
            view->pixels +
            ((gsize)rectangle->y + row) * view->surface_width * 4 +
            (gsize)rectangle->x * 4;
        memcpy(mapping + (gsize)row * row_bytes, source, row_bytes);
    }
    munmap(mapping, size);
    *size_out = size;
    return name;
}

static void
engine_view_send_frame(EngineView *view)
{
    DamageRect rectangle;
    gchar *shm_name;
    gsize shm_size;
    guint32 flags = 0;
    guint64 serial;
    MuxEngineBuilder builder;
    GBytes *payload;

    if (view->hidden || view->frame_pending || !view->dirty_valid ||
        !view->pixels || !view->owner || view->owner->failed)
        return;

    rectangle = view->dirty;
    if (!view->root_frame_sent) {
        rectangle = damage_full(view->surface_width,
                                view->surface_height);
        flags |= MUX_ENGINE_FLAG_FULL_DAMAGE;
    } else if (damage_is_full(&rectangle,
                              view->surface_width,
                              view->surface_height))
        flags |= MUX_ENGINE_FLAG_FULL_DAMAGE;
    if (view->dirty_replaces_pending)
        flags |= MUX_ENGINE_FLAG_REPLACES_PENDING;

    shm_name = create_frame_shm(view, &rectangle, &shm_size);
    if (!shm_name)
        return;

    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u32(&builder, view->surface_width);
    mux_engine_builder_put_u32(&builder, view->surface_height);
    mux_engine_builder_put_u32(&builder, rectangle.width * 4);
    mux_engine_builder_put_u32(&builder, MUX_ENGINE_PIXEL_RGBA8888);
    mux_engine_builder_put_u32(&builder, 1);
    mux_engine_builder_put_u32(&builder, rectangle.x);
    mux_engine_builder_put_u32(&builder, rectangle.y);
    mux_engine_builder_put_u32(&builder, rectangle.width);
    mux_engine_builder_put_u32(&builder, rectangle.height);
    mux_engine_builder_put_u64(&builder, shm_size);
    mux_engine_builder_put_string(&builder, shm_name);
    payload = mux_engine_builder_finish(&builder);
    serial = ++view->engine->next_event_serial;
    if (!client_send(view->owner,
                     MUX_ENGINE_MESSAGE_FRAME,
                     flags,
                     view->id,
                     serial,
                     payload)) {
        g_bytes_unref(payload);
        shm_unlink(shm_name);
        engine_frame_bytes_release(view->engine, shm_size);
        g_free(shm_name);
        return;
    }
    g_bytes_unref(payload);

    if (view->frame_retry_id) {
        g_source_remove(view->frame_retry_id);
        view->frame_retry_id = 0;
    }
    view->frame_pending = TRUE;
    view->pending_frame_serial = serial;
    view->pending_shm_name = shm_name;
    view->pending_shm_size = shm_size;
    if (view->engine->smoke_frame_ack_fd >= 0) {
        view->pending_frame_uri = g_strdup(
            webkit_web_view_get_uri(view->web_view));
        view->pending_frame_title = g_strdup(
            webkit_web_view_get_title(view->web_view));
        smoke_frame_telemetry_write(view,
                                    "KITTY_FRAME_SENT",
                                    serial);
    }
    view->frame_timeout_id = g_timeout_add_seconds(30,
                                                   frame_timeout,
                                                   view);
    view->dirty_valid = FALSE;
    view->dirty_replaces_pending = FALSE;
    view->root_frame_sent = TRUE;
}

static gboolean
frame_retry(gpointer data)
{
    EngineView *view = data;

    view->frame_retry_id = 0;
    engine_view_send_frame(view);
    return G_SOURCE_REMOVE;
}

static void
engine_view_schedule_frame_retry(EngineView *view)
{
    if (view->frame_retry_id || view->hidden || view->graphics_failed)
        return;
    view->frame_retry_id = g_timeout_add(
        frame_backpressure_retry_delay_ms(view->frame_backpressure_count),
        frame_retry,
        view);
}

static gboolean
engine_view_resize_pixels(EngineView *view, gsize new_size)
{
    gsize old_size = view->pixels_size;
    gsize reserved = 0;
    guint8 *pixels;

    if (new_size > old_size) {
        reserved = new_size - old_size;
        if (!engine_frame_bytes_reserve(view->engine, reserved))
            return FALSE;
    }
    pixels = g_try_realloc(view->pixels, new_size);
    if (!pixels) {
        engine_frame_bytes_release(view->engine, reserved);
        return FALSE;
    }
    if (new_size < old_size)
        engine_frame_bytes_release(view->engine, old_size - new_size);
    view->pixels = pixels;
    view->pixels_size = new_size;
    return TRUE;
}

static gboolean
engine_view_update_pixels(EngineView *view,
                          WPEBuffer *buffer,
                          const WPERectangle *damage_rects,
                          guint damage_count)
{
    GError *error = NULL;
    GBytes *bytes = NULL;
    const guint8 *source;
    gsize source_size;
    guint source_stride;
    guint width;
    guint height;
    gboolean has_alpha = TRUE;
    gboolean resized;
    DamageRect damage;

    width = (guint)wpe_buffer_get_width(buffer);
    height = (guint)wpe_buffer_get_height(buffer);
    if (!width || !height ||
        width > MUX_ENGINE_MAX_DIMENSION ||
        height > MUX_ENGINE_MAX_DIMENSION ||
        (guint64)width * height > MUX_ENGINE_MAX_PIXELS)
        return FALSE;

    bytes = wpe_buffer_import_to_pixels(buffer, &error);
    if (!bytes) {
        g_warning("import WPE buffer for Kitty: %s",
                  error ? error->message : "unsupported buffer");
        g_clear_error(&error);
        return FALSE;
    }
    source = g_bytes_get_data(bytes, &source_size);
    if (WPE_IS_BUFFER_SHM(buffer))
        source_stride =
            wpe_buffer_shm_get_stride(WPE_BUFFER_SHM(buffer));
    else
        source_stride = height ? (guint)(source_size / height) : 0;
    if (WPE_IS_BUFFER_DMA_BUF(buffer) &&
        wpe_buffer_dma_buf_get_format(WPE_BUFFER_DMA_BUF(buffer)) ==
            DRM_FORMAT_XRGB8888)
        has_alpha = FALSE;
    if (source_stride < width * 4 ||
        source_size < (gsize)source_stride * height)
        return FALSE;

    resized = view->surface_width != width ||
        view->surface_height != height || !view->pixels;
    if (resized) {
        gsize pixels_size = (gsize)width * height * 4;

        if (!engine_view_resize_pixels(view, pixels_size))
            return FALSE;
        view->surface_width = width;
        view->surface_height = height;
        view->root_frame_sent = FALSE;
        view->dirty_valid = FALSE;
        damage = damage_full(width, height);
    } else
        damage = coalesce_damage(damage_rects,
                                 damage_count,
                                 width,
                                 height);

    for (guint row = damage.y;
         row < damage.y + damage.height;
         row++) {
        const guint8 *source_row =
            source + (gsize)row * source_stride;
        guint8 *destination_row =
            view->pixels + (gsize)row * width * 4;

        for (guint column = damage.x;
             column < damage.x + damage.width;
             column++) {
            const guint8 *pixel = source_row + (gsize)column * 4;
            guint8 *destination =
                destination_row + (gsize)column * 4;
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
            destination[0] = pixel[2];
            destination[1] = pixel[1];
            destination[2] = pixel[0];
            destination[3] = has_alpha ? pixel[3] : 255;
#else
            destination[0] = pixel[1];
            destination[1] = pixel[2];
            destination[2] = pixel[3];
            destination[3] = has_alpha ? pixel[0] : 255;
#endif
        }
    }

    if (view->dirty_valid)
        damage_union(&view->dirty, &damage);
    else {
        view->dirty = damage;
        view->dirty_valid = TRUE;
    }
    if (view->frame_pending)
        view->dirty_replaces_pending = TRUE;
    engine_view_send_frame(view);
    return TRUE;
}

static gboolean
mux_platform_render_buffer(WPEView *platform_view,
                           WPEBuffer *buffer,
                           const WPERectangle *damage_rects,
                           guint damage_count,
                           GError **error)
{
    MuxPlatformView *mux_view = (MuxPlatformView *)platform_view;

    (void)error;
    if (mux_view->engine_view &&
        !mux_view->engine_view->hidden &&
        !mux_view->engine_view->graphics_failed)
        engine_view_update_pixels(mux_view->engine_view,
                                  buffer,
                                  damage_rects,
                                  damage_count);
    wpe_view_buffer_rendered(platform_view, buffer);
    wpe_view_buffer_released(platform_view, buffer);
    return TRUE;
}

static void
engine_view_reset_surface(EngineView *view)
{
    engine_view_clear_pending_frame(view);
    if (view->frame_retry_id) {
        g_source_remove(view->frame_retry_id);
        view->frame_retry_id = 0;
    }
    if (view->pixels_size)
        engine_frame_bytes_release(view->engine, view->pixels_size);
    g_clear_pointer(&view->pixels, g_free);
    view->pixels_size = 0;
    view->surface_width = 0;
    view->surface_height = 0;
    view->dirty_valid = FALSE;
    view->dirty_replaces_pending = FALSE;
    view->root_frame_sent = FALSE;
    view->frame_backpressure_count = 0;
}

static void
metadata_capture_flags(guint32 *flags,
                       WebKitMediaCaptureState state,
                       guint32 active_flag,
                       guint32 muted_flag)
{
    if (state == WEBKIT_MEDIA_CAPTURE_STATE_ACTIVE)
        *flags |= active_flag;
    else if (state == WEBKIT_MEDIA_CAPTURE_STATE_MUTED)
        *flags |= muted_flag;
}

static guint32
view_metadata_flags(EngineView *view)
{
    guint32 flags = 0;

    if (webkit_web_view_is_playing_audio(view->web_view))
        flags |= MUX_ENGINE_METADATA_AUDIO_PLAYING;
    if (webkit_web_view_get_is_muted(view->web_view))
        flags |= MUX_ENGINE_METADATA_AUDIO_MUTED;
    metadata_capture_flags(
        &flags,
        webkit_web_view_get_camera_capture_state(view->web_view),
        MUX_ENGINE_METADATA_CAMERA_ACTIVE,
        MUX_ENGINE_METADATA_CAMERA_MUTED);
    metadata_capture_flags(
        &flags,
        webkit_web_view_get_microphone_capture_state(view->web_view),
        MUX_ENGINE_METADATA_MICROPHONE_ACTIVE,
        MUX_ENGINE_METADATA_MICROPHONE_MUTED);
    metadata_capture_flags(
        &flags,
        webkit_web_view_get_display_capture_state(view->web_view),
        MUX_ENGINE_METADATA_DISPLAY_ACTIVE,
        MUX_ENGINE_METADATA_DISPLAY_MUTED);
    if (view->fullscreen)
        flags |= MUX_ENGINE_METADATA_FULLSCREEN;
    return flags;
}

static void
view_send_metadata(EngineView *view)
{
    MuxEngineBuilder builder;
    GBytes *payload;
    const gchar *uri;
    const gchar *title;
    gdouble progress;
    guint progress_milli;

    if (!view->owner || view->owner->failed)
        return;

    uri = webkit_web_view_get_uri(view->web_view);
    title = webkit_web_view_get_title(view->web_view);
    progress = webkit_web_view_get_estimated_load_progress(view->web_view);
    progress_milli = (guint)(CLAMP(progress, 0.0, 1.0) * 1000.0);

    mux_engine_builder_init(&builder);
    mux_engine_builder_put_string(&builder, uri ? uri : "");
    mux_engine_builder_put_string(&builder, title ? title : "");
    mux_engine_builder_put_string(&builder, view->layer);
    mux_engine_builder_put_u32(&builder,
                               webkit_web_view_is_loading(view->web_view));
    mux_engine_builder_put_u32(&builder,
                               webkit_web_view_can_go_back(view->web_view));
    mux_engine_builder_put_u32(&builder,
                               webkit_web_view_can_go_forward(view->web_view));
    mux_engine_builder_put_u32(&builder, progress_milli);
    mux_engine_builder_put_u32(&builder, view->width);
    mux_engine_builder_put_u32(&builder, view->height);
    mux_engine_builder_put_u32(&builder, view->scale_milli);
    mux_engine_builder_put_u32(&builder, view_metadata_flags(view));
    payload = mux_engine_builder_finish(&builder);
    client_send(view->owner,
                MUX_ENGINE_MESSAGE_METADATA,
                view->ephemeral ? MUX_ENGINE_FLAG_EPHEMERAL : 0,
                view->id,
                ++view->engine->next_event_serial,
                payload);
    g_bytes_unref(payload);
}

static void
view_record_navigation(EngineView *view)
{
    const gchar *uri;
    const gchar *title;

    if (view->ephemeral || !view->engine->browser_store ||
        !view->web_view || !view->id)
        return;
    uri = webkit_web_view_get_uri(view->web_view);
    title = webkit_web_view_get_title(view->web_view);
    mux_browser_store_record_navigation(view->engine->browser_store,
                                        view->id,
                                        view->ephemeral,
                                        uri,
                                        title);
}

static void
view_property_changed(GObject *object, GParamSpec *spec, gpointer data)
{
    EngineView *view = data;

    (void)object;
    if (g_str_equal(g_param_spec_get_name(spec), "uri") ||
        g_str_equal(g_param_spec_get_name(spec), "title"))
        view_record_navigation(view);
    view_send_metadata(view);
}

static void
view_load_changed(WebKitWebView *web_view,
                  WebKitLoadEvent load_event,
                  gpointer data)
{
    EngineView *view = data;

    (void)web_view;
    if (load_event == WEBKIT_LOAD_STARTED) {
        view->fullscreen = FALSE;
        engine_view_find_close(view, TRUE);
    }
    if (load_event == WEBKIT_LOAD_COMMITTED ||
        load_event == WEBKIT_LOAD_FINISHED)
        view_record_navigation(view);
    view_send_metadata(view);
}

static gboolean
view_enter_fullscreen(WebKitWebView *web_view, gpointer data)
{
    EngineView *view = data;

    (void)web_view;
    view->fullscreen = TRUE;
    view_send_metadata(view);
    return FALSE;
}

static gboolean
view_leave_fullscreen(WebKitWebView *web_view, gpointer data)
{
    EngineView *view = data;

    (void)web_view;
    view->fullscreen = FALSE;
    view_send_metadata(view);
    return FALSE;
}

static void
view_close(WebKitWebView *web_view, gpointer data)
{
    EngineView *view = data;
    guint64 serial;

    (void)web_view;
    if (!view->close_pending || view->close_ready || !view->owner ||
        view->owner->failed)
        return;

    serial = view->pending_close_serial;
    view->close_pending = FALSE;
    view->close_ready = TRUE;
    if (!client_send_empty(view->owner,
                           MUX_ENGINE_MESSAGE_CLOSE_READY,
                           view->id,
                           serial)) {
        view->close_ready = FALSE;
        view->pending_close_serial = 0;
    }
}

static void
view_finish_close_cancellation(EngineView *view)
{
    guint64 serial;

    if (!g_object_steal_data(G_OBJECT(view->web_view),
                             BEFORE_UNLOAD_STAY_DATA) ||
        !view->close_pending || view->close_ready ||
        !view->pending_close_serial)
        return;

    serial = view->pending_close_serial;
    view->close_pending = FALSE;
    view->close_ready = FALSE;
    view->pending_close_serial = 0;
    if (serial > view->retired_close_serial)
        view->retired_close_serial = serial;
    if (view->owner && !view->owner->failed)
        client_send_empty(view->owner,
                          MUX_ENGINE_MESSAGE_CLOSE_CANCELLED,
                          view->id,
                          serial);
}

static void
engine_view_free(gpointer data)
{
    EngineView *view = data;

    if (!view)
        return;
    if (view->owner && view->owner->view_count)
        view->owner->view_count--;
    if (!view->ephemeral && view->engine &&
        view->engine->browser_store && view->id)
        mux_browser_store_close_view(view->engine->browser_store,
                                     view->id);
    if (view->engine && view->web_view) {
        MuxDownloadManager *download_manager = view->ephemeral
            ? view->download_manager
            : view->engine->download_manager;

        if (download_manager)
            mux_download_manager_cancel_view(download_manager,
                                             view->web_view);
    }
    g_clear_pointer(&view->download_manager, mux_download_manager_free);
    engine_view_reset_surface(view);
    g_clear_pointer(&view->popup_manager, mux_popup_manager_free);
    g_clear_pointer(&view->file_chooser_bridge,
                    mux_file_chooser_bridge_free);
    g_clear_pointer(&view->affordance_bridge,
                    mux_browser_affordance_bridge_free);
    g_clear_pointer(&view->notification_engine,
                    mux_notification_engine_free);
    g_clear_pointer(&view->navigation_policy,
                    mux_navigation_policy_free);
    g_clear_pointer(&view->ui_bridge, mux_ui_engine_bridge_free);
    if (view->find_timeout_id)
        g_source_remove(view->find_timeout_id);
    if (view->find_controller) {
        webkit_find_controller_search_finish(view->find_controller);
        g_signal_handlers_disconnect_by_data(view->find_controller, view);
    }
    g_clear_object(&view->find_controller);
    if (view->find_query)
        g_string_free(view->find_query, TRUE);
    if (view->web_view && view->input_method)
        webkit_web_view_set_input_method_context(view->web_view, NULL);
    g_clear_object(&view->input_method);
    g_clear_pointer(&view->suppressed_text_keys, g_hash_table_unref);
    g_clear_object(&view->web_view);
    g_clear_object(&view->network_session);
    g_free(view->layer);
    g_free(view);
}

static EngineView *
lookup_owned_view(Client *client, const MuxEngineMessage *request)
{
    EngineView *view;

    view = g_hash_table_lookup(client->engine->views, &request->view_id);
    if (!view) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_NOT_FOUND,
                          "view not found");
        return NULL;
    }
    if (view->owner != client) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_NOT_OWNER,
                          "view belongs to another pane");
        return NULL;
    }
    return view;
}

static gboolean
handle_hello(Client *client, const MuxEngineMessage *request)
{
    MuxEngineCursor cursor;
    guint32 claimed_pid;
    guint32 width;
    guint32 height;
    guint32 scale_milli;
    gchar *kitty_window = NULL;
    gchar *layer = NULL;
    gchar *initial_uri = NULL;
    MuxEngineBuilder builder;
    GBytes *payload;
    gboolean sent;

    if (client->welcomed) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_STATE,
                          "HELLO was already received");
        return FALSE;
    }
    if (request->view_id) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          "HELLO must use view id zero");
        return FALSE;
    }

    mux_engine_cursor_init(&cursor, request->payload);
    if (!mux_engine_cursor_get_u32(&cursor, &claimed_pid) ||
        !mux_engine_cursor_get_string(&cursor, &kitty_window) ||
        !mux_engine_cursor_get_string(&cursor, &layer) ||
        !mux_engine_cursor_get_u32(&cursor, &width) ||
        !mux_engine_cursor_get_u32(&cursor, &height) ||
        !mux_engine_cursor_get_u32(&cursor, &scale_milli) ||
        !mux_engine_cursor_get_string(&cursor, &initial_uri) ||
        !mux_engine_cursor_done(&cursor)) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          "invalid HELLO payload");
        g_free(kitty_window);
        g_free(layer);
        g_free(initial_uri);
        return FALSE;
    }
    if ((pid_t)claimed_pid != client->peer_pid ||
        !text_is_valid(kitty_window, MUX_ENGINE_MAX_KITTY_ID_BYTES) ||
        !text_is_valid(layer, MUX_ENGINE_MAX_LAYER_BYTES) ||
        !text_is_valid(initial_uri, MUX_ENGINE_MAX_URI_BYTES) ||
        !dimensions_are_valid(width, height, scale_milli) ||
        scale_milli != client->engine->device_scale_milli) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          "HELLO identity, text, or dimensions are invalid");
        g_free(kitty_window);
        g_free(layer);
        g_free(initial_uri);
        return FALSE;
    }

    client->kitty_window = kitty_window;
    client->layer = *layer ? layer : g_strdup("main");
    if (!*layer)
        g_free(layer);
    client->initial_uri = initial_uri;
    client->width = width;
    client->height = height;
    client->scale_milli = scale_milli;
    client->welcomed = TRUE;

    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u32(&builder, (guint32)getpid());
    mux_engine_builder_put_string(&builder, client->engine->profile);
    mux_engine_builder_put_string(&builder, client->engine->socket_path);
    payload = mux_engine_builder_finish(&builder);
    sent = client_send(client,
                       MUX_ENGINE_MESSAGE_WELCOME,
                       MUX_ENGINE_FLAG_NONE,
                       0,
                       request->serial,
                       payload);
    g_bytes_unref(payload);
    return sent;
}

static gboolean
parse_create_payload(Client *client,
                     GBytes *payload,
                     guint *width,
                     guint *height,
                     guint *scale_milli,
                     gchar **layer,
                     gchar **uri,
                     gchar **popup_token)
{
    MuxEngineCursor cursor;
    guint32 parsed_width;
    guint32 parsed_height;
    guint32 parsed_scale;
    gchar *parsed_layer = NULL;
    gchar *parsed_uri = NULL;
    gchar *parsed_popup_token = NULL;

    *width = client->width;
    *height = client->height;
    *scale_milli = client->scale_milli;
    *layer = g_strdup(client->layer);
    *uri = g_strdup(client->initial_uri);
    *popup_token = NULL;

    if (!g_bytes_get_size(payload))
        return TRUE;

    mux_engine_cursor_init(&cursor, payload);
    if (!mux_engine_cursor_get_u32(&cursor, &parsed_width) ||
        !mux_engine_cursor_get_u32(&cursor, &parsed_height) ||
        !mux_engine_cursor_get_u32(&cursor, &parsed_scale) ||
        !mux_engine_cursor_get_string(&cursor, &parsed_layer) ||
        !mux_engine_cursor_get_string(&cursor, &parsed_uri)) {
        g_free(parsed_layer);
        g_free(parsed_uri);
        return FALSE;
    }
    if (!mux_engine_cursor_done(&cursor) &&
        !mux_engine_cursor_get_string(&cursor, &parsed_popup_token)) {
        g_free(parsed_layer);
        g_free(parsed_uri);
        return FALSE;
    }
    if (!mux_engine_cursor_done(&cursor)) {
        g_free(parsed_layer);
        g_free(parsed_uri);
        g_free(parsed_popup_token);
        return FALSE;
    }

    g_free(*layer);
    g_free(*uri);
    *width = parsed_width;
    *height = parsed_height;
    *scale_milli = parsed_scale;
    *layer = parsed_layer;
    *uri = parsed_uri;
    *popup_token = parsed_popup_token;
    return TRUE;
}

static gboolean
handle_create_view(Client *client, const MuxEngineMessage *request)
{
    Engine *engine = client->engine;
    EngineView *view = NULL;
    WebKitNetworkSession *session;
    WPEView *wpe_view;
    WPEToplevel *toplevel;
    guint width;
    guint height;
    guint scale_milli;
    gchar *layer = NULL;
    gchar *uri = NULL;
    g_autofree gchar *popup_token = NULL;
    g_autofree gchar *normalized_uri = NULL;
    GError *uri_error = NULL;
    guint64 *key;
    MuxEngineBuilder builder;
    GBytes *payload;
    gboolean popup_claim;
    gboolean requested_ephemeral;
    g_autoptr(GError) private_network_error = NULL;

    if (request->view_id) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          "CREATE_VIEW must use view id zero");
        return TRUE;
    }
    if (client->view_count >= MUX_ENGINE_MAX_CLIENT_VIEWS ||
        g_hash_table_size(engine->views) >= MUX_ENGINE_MAX_VIEWS) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_LIMIT,
                          "engine view limit reached");
        return TRUE;
    }
    if (!parse_create_payload(client,
                              request->payload,
                              &width,
                              &height,
                              &scale_milli,
                              &layer,
                              &uri,
                              &popup_token) ||
        !dimensions_are_valid(width, height, scale_milli) ||
        scale_milli != engine->device_scale_milli ||
        !text_is_valid(layer, MUX_ENGINE_MAX_LAYER_BYTES) ||
        !text_is_valid(uri, MUX_ENGINE_MAX_URI_BYTES) ||
        (popup_token && *popup_token &&
         !popup_token_valid(popup_token))) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          "invalid CREATE_VIEW payload");
        g_free(layer);
        g_free(uri);
        return TRUE;
    }

    popup_claim = popup_token && *popup_token;
    requested_ephemeral =
        (request->flags & MUX_ENGINE_FLAG_EPHEMERAL) != 0;
    if (!popup_claim &&
        !engine_view_capacity_available(
            g_hash_table_size(engine->views),
            g_hash_table_size(engine->pending_popups))) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_LIMIT,
                          "engine view limit reached");
        g_free(layer);
        g_free(uri);
        return TRUE;
    }
    if (!prepare_initial_uri(popup_claim,
                             uri,
                             g_getenv(MUX_URI_SEARCH_ENV),
                             &normalized_uri,
                             &uri_error)) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          uri_error->message);
        g_clear_error(&uri_error);
        g_free(layer);
        g_free(uri);
        return TRUE;
    }
    if (popup_claim)
        view = claim_popup(engine, popup_token);
    if (popup_claim && !view) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_STATE,
                          "popup token is unavailable or expired");
        g_free(layer);
        g_free(uri);
        return TRUE;
    }
    if (view && view->ephemeral != requested_ephemeral) {
        engine_view_free(view);
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_STATE,
                          "popup crossed a profile privacy boundary");
        g_free(layer);
        g_free(uri);
        return TRUE;
    }

    if (!view) {
        view = g_new0(EngineView, 1);
        view->engine = engine;
        view->hidden = TRUE;
        view->owner = client;
        view->id = engine->next_view_id++;
        view->layer = *layer ? layer : g_strdup("main");
        if (!*layer)
            g_free(layer);
        view->width = width;
        view->height = height;
        view->scale_milli = scale_milli;
        view->ephemeral = requested_ephemeral;
        if (view->ephemeral &&
            !engine_view_prepare_private_network(
                view, &private_network_error)) {
            client_send_error(client,
                              request,
                              MUX_ENGINE_REMOTE_ERROR_INTERNAL,
                              private_network_error->message);
            engine_view_free(view);
            g_free(uri);
            return TRUE;
        }
        session = view->ephemeral
            ? view->network_session
            : engine->persistent_session;

        engine->pending_view = view;
        view->web_view = WEBKIT_WEB_VIEW(
            g_object_new(WEBKIT_TYPE_WEB_VIEW,
                         "web-context", engine->web_context,
                         "network-session", session,
                         "display", engine->display,
                         NULL));
        engine->pending_view = NULL;
    } else {
        view->owner = client;
        view->id = engine->next_view_id++;
        g_free(view->layer);
        view->layer = *layer ? layer : g_strdup("main");
        if (!*layer)
            g_free(layer);
        view->width = width;
        view->height = height;
        view->scale_milli = scale_milli;
    }
    if (!view->web_view || !view->platform_view) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_INTERNAL,
                          "WebKitWebView construction failed");
        engine_view_free(view);
        g_free(uri);
        return TRUE;
    }
    if (!engine_view_find_initialize(view)) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_INTERNAL,
                          "WebView find controller construction failed");
        engine_view_free(view);
        g_free(uri);
        return TRUE;
    }
    view->input_method = mux_input_method_context_new();
    view->suppressed_text_keys =
        g_hash_table_new(g_direct_hash, g_direct_equal);
    if (!view->input_method || !view->suppressed_text_keys) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_INTERNAL,
                          "WebView input method construction failed");
        engine_view_free(view);
        g_free(uri);
        return TRUE;
    }
    webkit_web_view_set_input_method_context(
        view->web_view,
        WEBKIT_INPUT_METHOD_CONTEXT(view->input_method));
    view->ui_bridge = mux_ui_engine_bridge_new(view->web_view,
                                               ui_output,
                                               view,
                                               NULL);
    if (!view->ui_bridge) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_INTERNAL,
                          "WebView UI bridge construction failed");
        engine_view_free(view);
        g_free(uri);
        return TRUE;
    }
    mux_ui_engine_bridge_set_permission_store(
        view->ui_bridge,
        view->ephemeral ? engine->ephemeral_permission_store
                        : engine->permission_store);
    view->affordance_bridge = mux_browser_affordance_bridge_new(
        view->web_view,
        view->ephemeral,
        ui_output,
        view,
        NULL);
    if (!view->affordance_bridge) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_INTERNAL,
                          "WebView browser affordance bridge construction failed");
        engine_view_free(view);
        g_free(uri);
        return TRUE;
    }
    mux_browser_affordance_bridge_set_download_func(
        view->affordance_bridge,
        download_context_to_clipboard);
    view->notification_engine = mux_notification_engine_new(
        view->web_view,
        view->ephemeral,
        ui_output,
        view,
        NULL);
    if (!view->notification_engine) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_INTERNAL,
                          "WebView notification bridge construction failed");
        engine_view_free(view);
        g_free(uri);
        return TRUE;
    }
    view->navigation_policy = mux_navigation_policy_new(
        view->web_view,
        view->ephemeral,
        ui_output,
        view,
        NULL);
    if (!view->navigation_policy) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_INTERNAL,
                          "WebView navigation policy construction failed");
        engine_view_free(view);
        g_free(uri);
        return TRUE;
    }
    view->file_chooser_bridge = mux_file_chooser_bridge_new(
        view->web_view,
        ui_output,
        view,
        NULL);
    if (!view->file_chooser_bridge) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_INTERNAL,
                          "WebView file chooser bridge construction failed");
        engine_view_free(view);
        g_free(uri);
        return TRUE;
    }
    {
        GError *popup_error = NULL;

        if (!attach_popup_manager(view, &popup_error)) {
            client_send_error(client,
                              request,
                              MUX_ENGINE_REMOTE_ERROR_INTERNAL,
                              popup_error->message);
            g_clear_error(&popup_error);
            engine_view_free(view);
            g_free(uri);
            return TRUE;
        }
    }

    wpe_view = webkit_web_view_get_wpe_view(view->web_view);
    toplevel = wpe_view ? wpe_view_get_toplevel(wpe_view) : NULL;
    if (!toplevel || !engine_view_apply_geometry(view)) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_INTERNAL,
                          "WebView WPE geometry setup failed");
        engine_view_free(view);
        g_free(uri);
        return TRUE;
    }

    key = g_new(guint64, 1);
    *key = view->id;
    client->view_count++;
    g_hash_table_insert(engine->views, key, view);
    engine->had_owned_views = TRUE;
    engine_update_idle_fallback(engine);

    g_signal_connect(view->web_view,
                     "notify::uri",
                     G_CALLBACK(view_property_changed),
                     view);
    g_signal_connect(view->web_view,
                     "notify::title",
                     G_CALLBACK(view_property_changed),
                     view);
    g_signal_connect(view->web_view,
                     "notify::estimated-load-progress",
                     G_CALLBACK(view_property_changed),
                     view);
    g_signal_connect(view->web_view,
                     "notify::is-playing-audio",
                     G_CALLBACK(view_property_changed),
                     view);
    g_signal_connect(view->web_view,
                     "notify::is-muted",
                     G_CALLBACK(view_property_changed),
                     view);
    g_signal_connect(view->web_view,
                     "notify::camera-capture-state",
                     G_CALLBACK(view_property_changed),
                     view);
    g_signal_connect(view->web_view,
                     "notify::microphone-capture-state",
                     G_CALLBACK(view_property_changed),
                     view);
    g_signal_connect(view->web_view,
                     "notify::display-capture-state",
                     G_CALLBACK(view_property_changed),
                     view);
    g_signal_connect(view->web_view,
                     "enter-fullscreen",
                     G_CALLBACK(view_enter_fullscreen),
                     view);
    g_signal_connect(view->web_view,
                     "leave-fullscreen",
                     G_CALLBACK(view_leave_fullscreen),
                     view);
    g_signal_connect(view->web_view,
                     "close",
                     G_CALLBACK(view_close),
                     view);
    g_signal_connect(view->web_view,
                     "load-changed",
                     G_CALLBACK(view_load_changed),
                     view);
    view_record_navigation(view);

    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u32(&builder, width);
    mux_engine_builder_put_u32(&builder, height);
    mux_engine_builder_put_u32(&builder, scale_milli);
    mux_engine_builder_put_string(&builder, view->layer);
    payload = mux_engine_builder_finish(&builder);
    if (!client_send(client,
                     MUX_ENGINE_MESSAGE_VIEW_CREATED,
                     view->ephemeral ? MUX_ENGINE_FLAG_EPHEMERAL : 0,
                     view->id,
                     request->serial,
                     payload)) {
        g_bytes_unref(payload);
        g_hash_table_remove(engine->views, &view->id);
        engine_update_idle_fallback(engine);
        g_free(uri);
        return FALSE;
    }
    g_bytes_unref(payload);

    if (!popup_claim)
        webkit_web_view_load_uri(view->web_view, normalized_uri);
    g_free(uri);
    view_send_metadata(view);
    engine_view_send_frame(view);
    return TRUE;
}

static gboolean
handle_destroy_view(Client *client, const MuxEngineMessage *request)
{
    EngineView *view = lookup_owned_view(client, request);

    if (!view)
        return TRUE;
    client_send_ack(client, request);
    g_hash_table_remove(client->engine->views, &view->id);
    engine_update_idle_fallback(client->engine);
    return !client->failed;
}

static gboolean
handle_request_close(Client *client, const MuxEngineMessage *request)
{
    EngineView *view = lookup_owned_view(client, request);

    if (!view)
        return TRUE;
    if (request->flags != MUX_ENGINE_FLAG_NONE ||
        !request->serial ||
        g_bytes_get_size(request->payload) != 0) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          "invalid REQUEST_CLOSE record");
        return TRUE;
    }
    if (view->close_pending || view->close_ready) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_STATE,
                          "a close request is already in progress");
        return TRUE;
    }

    view->close_pending = TRUE;
    view->pending_close_serial = request->serial;
    webkit_web_view_try_close(view->web_view);
    view_finish_close_cancellation(view);
    return !client->failed;
}

static gboolean
handle_cancel_close(Client *client, const MuxEngineMessage *request)
{
    EngineView *view = lookup_owned_view(client, request);
    MuxEngineCursor cursor;
    guint64 close_serial;

    if (!view)
        return TRUE;
    mux_engine_cursor_init(&cursor, request->payload);
    if (request->flags != MUX_ENGINE_FLAG_NONE ||
        !request->serial ||
        !mux_engine_cursor_get_u64(&cursor, &close_serial) ||
        !close_serial ||
        !mux_engine_cursor_done(&cursor)) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          "invalid CANCEL_CLOSE record");
        return TRUE;
    }
    if (close_serial <= view->retired_close_serial) {
        client_send_ack(client, request);
        return !client->failed;
    }
    if ((!view->close_pending && !view->close_ready) ||
        close_serial != view->pending_close_serial) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_STATE,
                          "CANCEL_CLOSE does not match the pending close");
        return TRUE;
    }

    view->close_pending = FALSE;
    view->close_ready = FALSE;
    view->pending_close_serial = 0;
    view->retired_close_serial = close_serial;
    client_send_ack(client, request);
    return !client->failed;
}

static gboolean
handle_resize(Client *client, const MuxEngineMessage *request)
{
    EngineView *view = lookup_owned_view(client, request);
    MuxEngineCursor cursor;
    guint32 width;
    guint32 height;
    guint32 scale_milli;
    WPEView *wpe_view;
    WPEToplevel *toplevel;

    if (!view)
        return TRUE;
    mux_engine_cursor_init(&cursor, request->payload);
    if (!mux_engine_cursor_get_u32(&cursor, &width) ||
        !mux_engine_cursor_get_u32(&cursor, &height) ||
        !mux_engine_cursor_get_u32(&cursor, &scale_milli) ||
        !mux_engine_cursor_done(&cursor) ||
        !dimensions_are_valid(width, height, scale_milli) ||
        scale_milli != client->engine->device_scale_milli) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          "invalid RESIZE payload");
        return TRUE;
    }

    view->width = width;
    view->height = height;
    view->scale_milli = scale_milli;
    engine_view_reset_surface(view);
    wpe_view = webkit_web_view_get_wpe_view(view->web_view);
    toplevel = wpe_view ? wpe_view_get_toplevel(wpe_view) : NULL;
    if (!toplevel || !engine_view_apply_geometry(view)) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_INTERNAL,
                          "WPE resize failed");
        return TRUE;
    }
    client_send_ack(client, request);
    view_send_metadata(view);
    return !client->failed;
}

static gboolean
handle_navigate(Client *client, const MuxEngineMessage *request)
{
    EngineView *view = lookup_owned_view(client, request);
    MuxEngineCursor cursor;
    guint16 action;
    gchar *uri = NULL;
    gchar *normalized_uri;
    GError *uri_error = NULL;

    if (!view)
        return TRUE;
    mux_engine_cursor_init(&cursor, request->payload);
    if (!mux_engine_cursor_get_u16(&cursor, &action)) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          "NAVIGATE action is missing");
        return TRUE;
    }

    if (action == MUX_ENGINE_NAVIGATE_LOAD) {
        if (!mux_engine_cursor_get_string(&cursor, &uri) ||
            !mux_engine_cursor_done(&cursor) ||
            !text_is_valid(uri, MUX_ENGINE_MAX_URI_BYTES)) {
            client_send_error(client,
                              request,
                              MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                              "invalid navigation URI");
            g_free(uri);
            return TRUE;
        }
        normalized_uri = mux_uri_resolve_user_input(
            uri,
            g_getenv(MUX_URI_SEARCH_ENV),
            &uri_error);
        if (!normalized_uri) {
            client_send_error(client,
                              request,
                              MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                              uri_error->message);
            g_clear_error(&uri_error);
            g_free(uri);
            return TRUE;
        }
        webkit_web_view_load_uri(view->web_view, normalized_uri);
        g_free(normalized_uri);
        g_free(uri);
    } else {
        if (!mux_engine_cursor_done(&cursor)) {
            client_send_error(client,
                              request,
                              MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                              "unexpected NAVIGATE payload");
            return TRUE;
        }
        switch (action) {
        case MUX_ENGINE_NAVIGATE_BACK:
            if (webkit_web_view_can_go_back(view->web_view))
                webkit_web_view_go_back(view->web_view);
            break;
        case MUX_ENGINE_NAVIGATE_FORWARD:
            if (webkit_web_view_can_go_forward(view->web_view))
                webkit_web_view_go_forward(view->web_view);
            break;
        case MUX_ENGINE_NAVIGATE_RELOAD:
            webkit_web_view_reload(view->web_view);
            break;
        case MUX_ENGINE_NAVIGATE_STOP:
            webkit_web_view_stop_loading(view->web_view);
            break;
        default:
            client_send_error(client,
                              request,
                              MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                              "unknown navigation action");
            return TRUE;
        }
    }

    client_send_ack(client, request);
    return !client->failed;
}

static void
browser_palette_action_free(BrowserPaletteAction *action)
{
    if (!action)
        return;
    g_free(action->uri);
    g_free(action);
}

static void
browser_palette_free(BrowserPalette *palette)
{
    if (!palette)
        return;
    g_ptr_array_unref(palette->actions);
    g_free(palette);
}

static gchar *
browser_label_part(const gchar *value, gsize maximum)
{
    g_autofree gchar *valid = g_utf8_make_valid(value ? value : "", -1);
    gsize length = strlen(valid);

    if (length <= maximum)
        return g_steal_pointer(&valid);
    length = maximum;
    while (length && !g_utf8_validate(valid, length, NULL))
        length--;
    return g_strndup(valid, length);
}

static gchar *
browser_entry_label(const gchar *category, const MuxBrowserEntry *entry)
{
    g_autofree gchar *title = browser_label_part(entry->title, 160);
    g_autofree gchar *uri = browser_label_part(entry->uri, 300);

    if (!*title || g_str_equal(title, uri))
        return g_strdup_printf("%s  %s", category, uri);
    return g_strdup_printf("%s  %s | %s", category, title, uri);
}

static gboolean
browser_palette_add(BrowserPalette *palette,
                    GPtrArray *choices,
                    BrowserActionKind kind,
                    const gchar *uri,
                    guint32 flags,
                    const gchar *label)
{
    BrowserPaletteAction *action = g_new0(BrowserPaletteAction, 1);
    guint32 id;

    if (choices->len >= MUX_UI_MAX_CHOICES) {
        g_free(action);
        return FALSE;
    }
    id = palette->actions->len;

    action->kind = kind;
    action->uri = g_strdup(uri);
    g_ptr_array_add(palette->actions, action);
    g_ptr_array_add(choices, mux_ui_choice_new(id, flags, label));
    return TRUE;
}

static guint
browser_palette_section_budget(const GPtrArray *choices,
                               const GPtrArray *entries)
{
    guint remaining;

    if (!entries->len || choices->len >= MUX_UI_MAX_CHOICES - 1)
        return 0;
    remaining = MUX_UI_MAX_CHOICES - choices->len - 1;
    return MIN(entries->len, remaining);
}

static gboolean
browser_load_user_input(EngineView *view,
                        const gchar *input,
                        GError **error)
{
    g_autofree gchar *normalized = mux_uri_resolve_user_input(
        input,
        g_getenv(MUX_URI_SEARCH_ENV),
        error);

    if (!normalized)
        return FALSE;
    webkit_web_view_load_uri(view->web_view, normalized);
    return TRUE;
}

static gboolean
browser_palette_selected(guint32 choice_id,
                         gpointer user_data,
                         GError **error)
{
    BrowserPalette *palette = user_data;
    EngineView *view = palette->view;
    BrowserPaletteAction *action;

    if (choice_id >= palette->actions->len) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "browser command choice is out of range");
        return FALSE;
    }
    action = g_ptr_array_index(palette->actions, choice_id);
    switch (action->kind) {
    case BROWSER_ACTION_BACK:
        if (webkit_web_view_can_go_back(view->web_view))
            webkit_web_view_go_back(view->web_view);
        return TRUE;
    case BROWSER_ACTION_FORWARD:
        if (webkit_web_view_can_go_forward(view->web_view))
            webkit_web_view_go_forward(view->web_view);
        return TRUE;
    case BROWSER_ACTION_RELOAD:
        webkit_web_view_reload(view->web_view);
        return TRUE;
    case BROWSER_ACTION_STOP:
        webkit_web_view_stop_loading(view->web_view);
        return TRUE;
    case BROWSER_ACTION_TOGGLE_BOOKMARK: {
        const gchar *uri = webkit_web_view_get_uri(view->web_view);
        const gchar *title = webkit_web_view_get_title(view->web_view);
        gboolean bookmarked = mux_browser_store_is_bookmarked(
            view->engine->browser_store,
            view->ephemeral,
            uri);

        return mux_browser_store_set_bookmarked(
            view->engine->browser_store,
            view->ephemeral,
            uri,
            title,
            !bookmarked,
            error);
    }
    case BROWSER_ACTION_REOPEN_LAST: {
        g_autoptr(MuxBrowserEntry) entry =
            mux_browser_store_take_recently_closed(
                view->engine->browser_store,
                view->ephemeral);

        if (!entry) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_NOT_FOUND,
                                "there is no recently closed page");
            return FALSE;
        }
        return browser_load_user_input(view, entry->uri, error);
    }
    case BROWSER_ACTION_LOAD_URI:
        return browser_load_user_input(view, action->uri, error);
    case BROWSER_ACTION_NONE:
    default:
        break;
    }
    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_INVALID_ARGUMENT,
                        "browser command is unavailable");
    return FALSE;
}

static gboolean
browser_show_command_surface(EngineView *view, GError **error)
{
    BrowserPalette *palette = g_new0(BrowserPalette, 1);
    g_autoptr(GPtrArray) choices = g_ptr_array_new_with_free_func(
        (GDestroyNotify)mux_ui_choice_free);
    g_autoptr(GPtrArray) bookmarks = NULL;
    g_autoptr(GPtrArray) closed = NULL;
    g_autoptr(GPtrArray) history = NULL;
    const gchar *uri = webkit_web_view_get_uri(view->web_view);
    gboolean bookmarkable = !view->ephemeral &&
        mux_browser_store_uri_is_bookmarkable(uri);
    gboolean bookmarked = bookmarkable &&
        mux_browser_store_is_bookmarked(view->engine->browser_store,
                                        FALSE,
                                        uri);
    guint section_budget;
    guint i;

    palette->view = view;
    palette->actions = g_ptr_array_new_with_free_func(
        (GDestroyNotify)browser_palette_action_free);
    (void)browser_palette_add(
        palette,
        choices,
        BROWSER_ACTION_BACK,
        NULL,
        webkit_web_view_can_go_back(view->web_view)
            ? 0
            : MUX_UI_CHOICE_FLAG_DISABLED,
        "Command  Back");
    (void)browser_palette_add(
        palette,
        choices,
        BROWSER_ACTION_FORWARD,
        NULL,
        webkit_web_view_can_go_forward(view->web_view)
            ? 0
            : MUX_UI_CHOICE_FLAG_DISABLED,
        "Command  Forward");
    (void)browser_palette_add(
        palette,
        choices,
        webkit_web_view_is_loading(view->web_view)
            ? BROWSER_ACTION_STOP
            : BROWSER_ACTION_RELOAD,
        NULL,
        0,
        webkit_web_view_is_loading(view->web_view)
            ? "Command  Stop loading"
            : "Command  Reload");
    (void)browser_palette_add(
        palette,
        choices,
        BROWSER_ACTION_TOGGLE_BOOKMARK,
        NULL,
        bookmarkable ? 0 : MUX_UI_CHOICE_FLAG_DISABLED,
        view->ephemeral
            ? "Command  Bookmarks unavailable in private panes"
            : bookmarked ? "Command  Remove bookmark"
                         : "Command  Add bookmark");
    (void)browser_palette_add(
        palette,
        choices,
        BROWSER_ACTION_REOPEN_LAST,
        NULL,
        mux_browser_store_recently_closed_count(
            view->engine->browser_store,
            view->ephemeral)
            ? 0
            : MUX_UI_CHOICE_FLAG_DISABLED,
        "Command  Reopen most recently closed page");

    bookmarks = mux_browser_store_copy_bookmarks(
        view->engine->browser_store,
        view->ephemeral,
        MUX_ENGINE_PALETTE_BOOKMARKS);
    closed = mux_browser_store_copy_recently_closed(
        view->engine->browser_store,
        view->ephemeral,
        MUX_ENGINE_PALETTE_CLOSED);
    history = mux_browser_store_copy_history(
        view->engine->browser_store,
        view->ephemeral,
        MUX_ENGINE_PALETTE_HISTORY);

    section_budget = browser_palette_section_budget(choices, bookmarks);
    if (section_budget) {
        (void)browser_palette_add(palette,
                                  choices,
                                  BROWSER_ACTION_NONE,
                                  NULL,
                                  MUX_UI_CHOICE_FLAG_DISABLED |
                                      MUX_UI_CHOICE_FLAG_SEPARATOR,
                                  "Bookmarks");
        for (i = 0; i < section_budget; i++) {
            const MuxBrowserEntry *entry = g_ptr_array_index(bookmarks, i);
            g_autofree gchar *label = browser_entry_label("Bookmark", entry);

            (void)browser_palette_add(palette,
                                      choices,
                                      BROWSER_ACTION_LOAD_URI,
                                      entry->uri,
                                      0,
                                      label);
        }
    }
    section_budget = browser_palette_section_budget(choices, closed);
    if (section_budget) {
        (void)browser_palette_add(palette,
                                  choices,
                                  BROWSER_ACTION_NONE,
                                  NULL,
                                  MUX_UI_CHOICE_FLAG_DISABLED |
                                      MUX_UI_CHOICE_FLAG_SEPARATOR,
                                  "Recently closed");
        for (i = 0; i < section_budget; i++) {
            const MuxBrowserEntry *entry = g_ptr_array_index(closed, i);
            g_autofree gchar *label = browser_entry_label("Closed", entry);

            (void)browser_palette_add(palette,
                                      choices,
                                      BROWSER_ACTION_LOAD_URI,
                                      entry->uri,
                                      0,
                                      label);
        }
    }
    section_budget = browser_palette_section_budget(choices, history);
    if (section_budget) {
        (void)browser_palette_add(palette,
                                  choices,
                                  BROWSER_ACTION_NONE,
                                  NULL,
                                  MUX_UI_CHOICE_FLAG_DISABLED |
                                      MUX_UI_CHOICE_FLAG_SEPARATOR,
                                  "Recent navigation");
        for (i = 0; i < section_budget; i++) {
            const MuxBrowserEntry *entry = g_ptr_array_index(history, i);
            g_autofree gchar *label = browser_entry_label("History", entry);

            (void)browser_palette_add(palette,
                                      choices,
                                      BROWSER_ACTION_LOAD_URI,
                                      entry->uri,
                                      0,
                                      label);
        }
    }

    return mux_browser_affordance_bridge_show_command_surface(
        view->affordance_bridge,
        "Mux commands and history",
        "Use Up/Down to choose, then press Enter to run or open.",
        choices,
        browser_palette_selected,
        palette,
        (GDestroyNotify)browser_palette_free,
        error);
}

static gboolean
browser_handle_shortcut(EngineView *view,
                        guint16 event_type,
                        guint32 modifiers,
                        guint32 keyval)
{
    MuxShortcut shortcut = mux_shortcut_match_engine(modifiers, keyval);
    gboolean execute = FALSE;
    g_autoptr(GError) error = NULL;

    if (!mux_shortcut_handle_event(shortcut, event_type, &execute))
        return FALSE;
    if (!execute)
        return TRUE;

    if (shortcut == MUX_SHORTCUT_COMMAND_PALETTE) {
        if (!browser_show_command_surface(view, &error))
            g_warning("browser command surface failed: %s", error->message);
    } else if (shortcut == MUX_SHORTCUT_BOOKMARK) {
        const gchar *uri = webkit_web_view_get_uri(view->web_view);
        const gchar *title = webkit_web_view_get_title(view->web_view);
        gboolean current = mux_browser_store_is_bookmarked(
            view->engine->browser_store,
            view->ephemeral,
            uri);

        if (!mux_browser_store_set_bookmarked(view->engine->browser_store,
                                              view->ephemeral,
                                              uri,
                                              title,
                                              !current,
                                              &error))
            g_warning("bookmark shortcut failed: %s", error->message);
    } else if (shortcut == MUX_SHORTCUT_HISTORY_BACK) {
        if (webkit_web_view_can_go_back(view->web_view))
            webkit_web_view_go_back(view->web_view);
    } else if (shortcut == MUX_SHORTCUT_HISTORY_FORWARD &&
               webkit_web_view_can_go_forward(view->web_view)) {
        webkit_web_view_go_forward(view->web_view);
    }
    return TRUE;
}

static void
engine_view_find_advance(EngineView *view)
{
    view->find_generation++;
    if (!view->find_generation)
        view->find_generation++;
}

static void
engine_view_find_send_state(EngineView *view)
{
    MuxEngineBuilder builder;
    GBytes *payload;

    if (!view->owner || view->owner->failed)
        return;
    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u32(&builder, view->find_active);
    mux_engine_builder_put_u32(
        &builder,
        view->find_active ? view->find_status : MUX_ENGINE_FIND_CLOSED);
    mux_engine_builder_put_u32(
        &builder,
        view->find_active ? view->find_match_count : 0);
    mux_engine_builder_put_u64(&builder, view->find_generation);
    mux_engine_builder_put_string(
        &builder,
        view->find_active && view->find_query ? view->find_query->str : "");
    payload = mux_engine_builder_finish(&builder);
    client_send(view->owner,
                MUX_ENGINE_MESSAGE_FIND_STATE,
                MUX_ENGINE_FLAG_NONE,
                view->id,
                ++view->engine->next_event_serial,
                payload);
    g_bytes_unref(payload);
}

static void
engine_view_find_cancel_search(EngineView *view)
{
    if (view->find_timeout_id) {
        g_source_remove(view->find_timeout_id);
        view->find_timeout_id = 0;
    }
    view->find_pending_generation = 0;
    if (view->find_controller)
        webkit_find_controller_search_finish(view->find_controller);
}

static void
engine_view_find_found(WebKitFindController *controller,
                       guint match_count,
                       gpointer data)
{
    EngineView *view = data;

    if (controller != view->find_controller || !view->find_active ||
        !view->find_pending_generation ||
        view->find_pending_generation != view->find_generation)
        return;
    view->find_pending_generation = 0;
    view->find_status = match_count
        ? MUX_ENGINE_FIND_FOUND
        : MUX_ENGINE_FIND_NOT_FOUND;
    view->find_match_count = MIN(match_count, MUX_ENGINE_MAX_FIND_MATCHES);
    engine_view_find_send_state(view);
}

static void
engine_view_find_failed(WebKitFindController *controller, gpointer data)
{
    EngineView *view = data;

    if (controller != view->find_controller || !view->find_active ||
        !view->find_pending_generation ||
        view->find_pending_generation != view->find_generation)
        return;
    view->find_pending_generation = 0;
    view->find_status = MUX_ENGINE_FIND_NOT_FOUND;
    view->find_match_count = 0;
    engine_view_find_send_state(view);
}

static gboolean
engine_view_find_search(gpointer data)
{
    EngineView *view = data;

    view->find_timeout_id = 0;
    if (!view->find_active || !view->find_controller ||
        !view->find_query || !view->find_query->len)
        return G_SOURCE_REMOVE;
    view->find_pending_generation = view->find_generation;
    webkit_find_controller_search(
        view->find_controller,
        view->find_query->str,
        WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE |
            WEBKIT_FIND_OPTIONS_WRAP_AROUND,
        MUX_ENGINE_MAX_FIND_MATCHES);
    return G_SOURCE_REMOVE;
}

static void
engine_view_find_schedule(EngineView *view)
{
    engine_view_find_cancel_search(view);
    engine_view_find_advance(view);
    view->find_match_count = 0;
    if (!view->find_query->len) {
        view->find_status = MUX_ENGINE_FIND_IDLE;
        engine_view_find_send_state(view);
        return;
    }
    view->find_status = MUX_ENGINE_FIND_PENDING;
    engine_view_find_send_state(view);
    view->find_timeout_id = g_timeout_add(MUX_ENGINE_FIND_DEBOUNCE_MS,
                                          engine_view_find_search,
                                          view);
}

static gboolean
engine_view_find_text_is_printable(const gchar *text, gsize length)
{
    const gchar *cursor = text;
    const gchar *end = text + length;

    while (cursor < end) {
        gunichar character = g_utf8_get_char(cursor);

        if (!g_unichar_isprint(character))
            return FALSE;
        cursor = g_utf8_next_char(cursor);
    }
    return TRUE;
}

static void
engine_view_find_commit(EngineView *view,
                        const gchar *text,
                        gsize length)
{
    if (!view->find_active || !view->find_query || !length ||
        length > MUX_ENGINE_MAX_FIND_TEXT_BYTES - view->find_query->len ||
        !engine_view_find_text_is_printable(text, length))
        return;
    g_string_append_len(view->find_query, text, length);
    engine_view_find_schedule(view);
}

static void
engine_view_find_close(EngineView *view, gboolean notify)
{
    if (!view || !view->find_active)
        return;
    engine_view_find_cancel_search(view);
    view->find_active = FALSE;
    view->find_status = MUX_ENGINE_FIND_CLOSED;
    view->find_match_count = 0;
    engine_view_find_advance(view);
    if (notify)
        engine_view_find_send_state(view);
}

static void
engine_view_find_open(EngineView *view, gboolean clear_query)
{
    engine_view_find_cancel_search(view);
    view->find_active = TRUE;
    if (clear_query)
        g_string_truncate(view->find_query, 0);
    engine_view_find_advance(view);
    view->find_status = MUX_ENGINE_FIND_IDLE;
    view->find_match_count = 0;
    engine_view_find_send_state(view);
}

static gboolean
engine_view_find_initialize(EngineView *view)
{
    WebKitFindController *controller;

    if (view->find_controller)
        return TRUE;
    controller = webkit_web_view_get_find_controller(view->web_view);
    if (!controller)
        return FALSE;
    view->find_controller = g_object_ref(controller);
    view->find_query = g_string_sized_new(64);
    g_signal_connect(view->find_controller,
                     "found-text",
                     G_CALLBACK(engine_view_find_found),
                     view);
    g_signal_connect(view->find_controller,
                     "failed-to-find-text",
                     G_CALLBACK(engine_view_find_failed),
                     view);
    return TRUE;
}

static gboolean
engine_view_find_handle_key(EngineView *view,
                            guint16 event_type,
                            guint32 modifiers,
                            guint32 keyval)
{
    EngineFindShortcut shortcut = find_shortcut(modifiers, keyval);

    if (shortcut != ENGINE_FIND_SHORTCUT_NONE) {
        if (event_type == MUX_ENGINE_KEY_PRESS) {
            if (shortcut == ENGINE_FIND_SHORTCUT_OPEN) {
                engine_view_find_open(view, TRUE);
            } else if (!view->find_active) {
                engine_view_find_open(view, FALSE);
                if (view->find_query->len)
                    engine_view_find_schedule(view);
            } else if (view->find_query->len) {
                if (shortcut == ENGINE_FIND_SHORTCUT_PREVIOUS)
                    webkit_find_controller_search_previous(
                        view->find_controller);
                else
                    webkit_find_controller_search_next(
                        view->find_controller);
            }
        }
        return TRUE;
    }
    if (!view->find_active)
        return FALSE;
    if (event_type == MUX_ENGINE_KEY_RELEASE)
        return TRUE;
    if (!(modifiers & (WPE_MODIFIER_KEYBOARD_CONTROL |
                       WPE_MODIFIER_KEYBOARD_ALT |
                       WPE_MODIFIER_KEYBOARD_META))) {
        if (keyval == 0xff1bu) {
            engine_view_find_close(view, TRUE);
        } else if (keyval == 0xff08u && view->find_query->len) {
            gchar *previous = g_utf8_find_prev_char(
                view->find_query->str,
                view->find_query->str + view->find_query->len);

            g_string_truncate(view->find_query,
                              previous
                                  ? (gsize)(previous - view->find_query->str)
                                  : 0);
            engine_view_find_schedule(view);
        } else if (keyval == 0xff0du && view->find_query->len) {
            if (modifiers & WPE_MODIFIER_KEYBOARD_SHIFT)
                webkit_find_controller_search_previous(
                    view->find_controller);
            else
                webkit_find_controller_search_next(view->find_controller);
        }
    }
    return TRUE;
}

static gboolean
handle_input_key(Client *client, const MuxEngineMessage *request)
{
    EngineView *view = lookup_owned_view(client, request);
    MuxEngineCursor cursor;
    guint16 event_type;
    guint32 time;
    guint32 modifiers;
    guint32 keycode;
    guint32 keyval;
    WPEEvent *event;

    if (!view)
        return TRUE;
    mux_engine_cursor_init(&cursor, request->payload);
    if (!mux_engine_cursor_get_u16(&cursor, &event_type) ||
        !mux_engine_cursor_get_u32(&cursor, &time) ||
        !mux_engine_cursor_get_u32(&cursor, &modifiers) ||
        !mux_engine_cursor_get_u32(&cursor, &keycode) ||
        !mux_engine_cursor_get_u32(&cursor, &keyval) ||
        !mux_engine_cursor_done(&cursor) ||
        event_type < MUX_ENGINE_KEY_PRESS ||
        event_type > MUX_ENGINE_KEY_RELEASE) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          "invalid INPUT_KEY payload");
        return TRUE;
    }

    if (engine_view_find_handle_key(view,
                                    event_type,
                                    modifiers,
                                    keyval) ||
        browser_handle_shortcut(view,
                                event_type,
                                modifiers,
                                keyval))
        return TRUE;

    if (view->input_method && view->suppressed_text_keys) {
        if (!mux_input_method_context_is_focused(view->input_method)) {
            g_hash_table_remove_all(view->suppressed_text_keys);
        } else if (!(modifiers & MUX_ENGINE_TEXT_SHORTCUT_MODIFIERS) &&
                   g_hash_table_contains(view->suppressed_text_keys,
                                         GUINT_TO_POINTER(keyval))) {
            if (event_type == MUX_ENGINE_KEY_RELEASE)
                g_hash_table_remove(view->suppressed_text_keys,
                                    GUINT_TO_POINTER(keyval));
            return TRUE;
        }
    }

    event = wpe_event_keyboard_new(
        event_type == MUX_ENGINE_KEY_RELEASE
            ? WPE_EVENT_KEYBOARD_KEY_UP
            : WPE_EVENT_KEYBOARD_KEY_DOWN,
        view->platform_view,
        WPE_INPUT_SOURCE_KEYBOARD,
        time,
        (WPEModifiers)modifiers,
        keycode,
        keyval);
    if (event) {
        wpe_view_event(view->platform_view, event);
        wpe_event_unref(event);
    }
    return TRUE;
}

static gboolean
handle_text_commit(Client *client, const MuxEngineMessage *request)
{
    EngineView *view = lookup_owned_view(client, request);
    MuxEngineCursor cursor;
    guint32 keyval;
    guint32 text_length;
    const guint8 *text;
    g_autoptr(GError) error = NULL;

    if (!view)
        return TRUE;
    mux_engine_cursor_init(&cursor, request->payload);
    if (!mux_engine_cursor_get_u32(&cursor, &keyval) || !keyval ||
        !mux_engine_cursor_get_u32(&cursor, &text_length) ||
        !text_length || text_length > MUX_ENGINE_MAX_TEXT_BYTES ||
        !mux_engine_cursor_get_bytes(&cursor, text_length, &text) ||
        !mux_engine_cursor_done(&cursor) ||
        memchr(text, '\0', text_length) ||
        !g_utf8_validate((const gchar *)text, text_length, NULL)) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          "invalid TEXT_COMMIT payload");
        return TRUE;
    }

    if (view->find_active) {
        engine_view_find_commit(view, (const gchar *)text, text_length);
        return TRUE;
    }

    if (view->input_method &&
        mux_input_method_context_commit(view->input_method,
                                        text,
                                        text_length,
                                        &error))
        g_hash_table_add(view->suppressed_text_keys,
                         GUINT_TO_POINTER(keyval));
    else if (error)
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          error->message);
    return TRUE;
}

static gboolean
handle_input_pointer(Client *client, const MuxEngineMessage *request)
{
    EngineView *view = lookup_owned_view(client, request);
    MuxEngineCursor cursor;
    guint16 event_type;
    guint32 time;
    guint32 modifiers;
    guint32 button;
    guint32 encoded_x;
    guint32 encoded_y;
    guint32 encoded_delta_x;
    guint32 encoded_delta_y;
    guint32 precise;
    guint32 stop;
    gdouble x;
    gdouble y;
    gdouble delta_x;
    gdouble delta_y;
    WPEEvent *event = NULL;

    if (!view)
        return TRUE;
    mux_engine_cursor_init(&cursor, request->payload);
    if (!mux_engine_cursor_get_u16(&cursor, &event_type) ||
        !mux_engine_cursor_get_u32(&cursor, &time) ||
        !mux_engine_cursor_get_u32(&cursor, &modifiers) ||
        !mux_engine_cursor_get_u32(&cursor, &button) ||
        !mux_engine_cursor_get_u32(&cursor, &encoded_x) ||
        !mux_engine_cursor_get_u32(&cursor, &encoded_y) ||
        !mux_engine_cursor_get_u32(&cursor, &encoded_delta_x) ||
        !mux_engine_cursor_get_u32(&cursor, &encoded_delta_y) ||
        !mux_engine_cursor_get_u32(&cursor, &precise) ||
        !mux_engine_cursor_get_u32(&cursor, &stop) ||
        !mux_engine_cursor_done(&cursor) ||
        event_type < MUX_ENGINE_POINTER_MOVE ||
        event_type > MUX_ENGINE_POINTER_SCROLL ||
        precise > 1 || stop > 1) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          "invalid INPUT_POINTER payload");
        return TRUE;
    }

    x = physical_milli_to_logical((gint32)encoded_x,
                                  view->scale_milli);
    y = physical_milli_to_logical((gint32)encoded_y,
                                  view->scale_milli);
    delta_x = physical_milli_to_logical((gint32)encoded_delta_x,
                                        view->scale_milli);
    delta_y = physical_milli_to_logical((gint32)encoded_delta_y,
                                        view->scale_milli);
    switch (event_type) {
    case MUX_ENGINE_POINTER_MOVE:
    case MUX_ENGINE_POINTER_ENTER:
    case MUX_ENGINE_POINTER_LEAVE:
        event = wpe_event_pointer_move_new(
            event_type == MUX_ENGINE_POINTER_ENTER
                ? WPE_EVENT_POINTER_ENTER
                : event_type == MUX_ENGINE_POINTER_LEAVE
                    ? WPE_EVENT_POINTER_LEAVE
                    : WPE_EVENT_POINTER_MOVE,
            view->platform_view,
            WPE_INPUT_SOURCE_MOUSE,
            time,
            (WPEModifiers)modifiers,
            x,
            y,
            delta_x,
            delta_y);
        break;
    case MUX_ENGINE_POINTER_DOWN:
    case MUX_ENGINE_POINTER_UP: {
        guint press_count = event_type == MUX_ENGINE_POINTER_DOWN
            ? wpe_view_compute_press_count(view->platform_view,
                                           x,
                                           y,
                                           button,
                                           time)
            : 1;
        event = wpe_event_pointer_button_new(
            event_type == MUX_ENGINE_POINTER_DOWN
                ? WPE_EVENT_POINTER_DOWN
                : WPE_EVENT_POINTER_UP,
            view->platform_view,
            WPE_INPUT_SOURCE_MOUSE,
            time,
            (WPEModifiers)modifiers,
            button,
            x,
            y,
            press_count);
        break;
    }
    case MUX_ENGINE_POINTER_SCROLL:
        event = wpe_event_scroll_new(view->platform_view,
                                     WPE_INPUT_SOURCE_MOUSE,
                                     time,
                                     (WPEModifiers)modifiers,
                                     delta_x,
                                     delta_y,
                                     precise,
                                     stop,
                                     x,
                                     y);
        break;
    default:
        break;
    }
    if (event) {
        wpe_view_event(view->platform_view, event);
        wpe_event_unref(event);
    }
    return TRUE;
}

static gboolean
handle_frame_ack(Client *client, const MuxEngineMessage *request)
{
    EngineView *view = lookup_owned_view(client, request);

    if (!view)
        return TRUE;
    if (g_bytes_get_size(request->payload)) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          "FRAME_ACK must have an empty payload");
        return TRUE;
    }
    if (request->serial &&
        request->serial <= view->retired_frame_serial)
        return TRUE;
    if (!view->frame_pending ||
        request->serial != view->pending_frame_serial) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_STATE,
                          "FRAME_ACK does not match the pending frame");
        return TRUE;
    }

    smoke_frame_telemetry_write(view,
                                "KITTY_FRAME_ACK",
                                request->serial);
    engine_view_clear_pending_frame(view);
    view->frame_rejection_count = 0;
    view->frame_backpressure_count = 0;
    view->graphics_failed = FALSE;
    engine_view_send_frame(view);
    return TRUE;
}

static gboolean
handle_frame_rejected(Client *client, const MuxEngineMessage *request)
{
    EngineView *view = lookup_owned_view(client, request);
    MuxEngineCursor cursor;
    guint32 reason;
    g_autofree gchar *detail = NULL;
    g_autofree gchar *escaped = NULL;

    if (!view)
        return TRUE;
    mux_engine_cursor_init(&cursor, request->payload);
    if (request->flags != MUX_ENGINE_FLAG_NONE ||
        !mux_engine_cursor_get_u32(&cursor, &reason) ||
        reason < MUX_ENGINE_FRAME_REJECTED_KITTY ||
        reason > MUX_ENGINE_FRAME_REJECTED_BACKPRESSURE ||
        !mux_engine_cursor_get_string(&cursor, &detail) ||
        !mux_engine_cursor_done(&cursor) ||
        !text_is_valid(detail,
                       MUX_ENGINE_MAX_FRAME_REJECTION_BYTES)) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          "invalid FRAME_REJECTED payload");
        return TRUE;
    }
    if (request->serial &&
        request->serial <= view->retired_frame_serial)
        return TRUE;
    if (!view->frame_pending ||
        request->serial != view->pending_frame_serial) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_STATE,
                          "FRAME_REJECTED does not match the pending frame");
        return TRUE;
    }

    engine_view_clear_pending_frame(view);
    if (reason == MUX_ENGINE_FRAME_REJECTED_NOT_VISIBLE ||
        view->hidden) {
        view->dirty_valid = FALSE;
        return TRUE;
    }
    escaped = g_strescape(detail, NULL);
    if (reason == MUX_ENGINE_FRAME_REJECTED_BACKPRESSURE) {
        if (view->frame_backpressure_count < G_MAXUINT)
            view->frame_backpressure_count++;
        if (view->frame_backpressure_count == 1 ||
            (view->frame_backpressure_count &
             (view->frame_backpressure_count - 1)) == 0) {
            g_warning("pane rejected frame for backpressure: %s",
                      escaped ? escaped : "unspecified");
        }
        if (view->pixels && view->surface_width && view->surface_height) {
            view->root_frame_sent = FALSE;
            view->dirty = damage_full(view->surface_width,
                                      view->surface_height);
            view->dirty_valid = TRUE;
            view->dirty_replaces_pending = FALSE;
            engine_view_schedule_frame_retry(view);
        } else {
            view->dirty_valid = FALSE;
        }
        return TRUE;
    }

    view->frame_backpressure_count = 0;
    view->frame_rejection_count++;
    if (view->frame_rejection_count <= MUX_ENGINE_MAX_FRAME_RETRIES &&
        view->pixels && view->surface_width && view->surface_height) {
        view->root_frame_sent = FALSE;
        view->dirty = damage_full(view->surface_width,
                                  view->surface_height);
        view->dirty_valid = TRUE;
        view->dirty_replaces_pending = FALSE;
        engine_view_send_frame(view);
        return TRUE;
    }

    view->graphics_failed = TRUE;
    view->dirty_valid = FALSE;
    g_warning("Kitty rejected browser graphics after %u attempts: %s",
              view->frame_rejection_count,
              escaped ? escaped : "unspecified");
    return TRUE;
}

static gboolean
handle_set_focus(Client *client, const MuxEngineMessage *request)
{
    EngineView *view = lookup_owned_view(client, request);
    MuxEngineCursor cursor;
    guint32 focused;

    if (!view)
        return TRUE;
    mux_engine_cursor_init(&cursor, request->payload);
    if (!mux_engine_cursor_get_u32(&cursor, &focused) ||
        !mux_engine_cursor_done(&cursor) ||
        focused > 1) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          "invalid SET_FOCUS payload");
        return TRUE;
    }
    view->focused = focused != 0;
    if (focused) {
        if (client->engine->clipboard_link) {
            GError *clipboard_error = NULL;
            gchar *origin = clipboard_origin(view);

            if (!mux_clipboard_engine_link_set_active_source(
                    client->engine->clipboard_link,
                    view->id,
                    origin,
                    view->ephemeral,
                    &clipboard_error)) {
                clipboard_failure(client->engine->clipboard_link,
                                  "focus-source",
                                  clipboard_error,
                                  client->engine);
                g_clear_error(&clipboard_error);
            }
            g_free(origin);
        }
        wpe_view_focus_in(view->platform_view);
    } else {
        engine_view_find_close(view, TRUE);
        wpe_view_focus_out(view->platform_view);
    }
    client_send_ack(client, request);
    return !client->failed;
}

static gboolean
handle_set_visibility(Client *client, const MuxEngineMessage *request)
{
    EngineView *view = lookup_owned_view(client, request);
    MuxEngineCursor cursor;
    guint32 visible;
    WPEView *wpe_view;
    WPEToplevel *toplevel;

    if (!view)
        return TRUE;
    mux_engine_cursor_init(&cursor, request->payload);
    if (!mux_engine_cursor_get_u32(&cursor, &visible) ||
        !mux_engine_cursor_done(&cursor) ||
        visible > 1) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          "invalid SET_VISIBILITY payload");
        return TRUE;
    }
    if (view->hidden == (visible == 0)) {
        client_send_ack(client, request);
        return !client->failed;
    }

    view->hidden = visible == 0;
    if (view->hidden)
        engine_view_find_close(view, TRUE);
    engine_view_reset_surface(view);
    wpe_view = view->platform_view
        ? view->platform_view
        : webkit_web_view_get_wpe_view(view->web_view);
    if (wpe_view) {
        if (view->hidden) {
            wpe_view_focus_out(wpe_view);
            wpe_view_set_visible(wpe_view, FALSE);
            wpe_view_unmap(wpe_view);
        } else {
            view->graphics_failed = FALSE;
            view->frame_rejection_count = 0;
            wpe_view_set_visible(wpe_view, TRUE);
            wpe_view_map(wpe_view);
            if (view->focused)
                wpe_view_focus_in(wpe_view);
        }
    }
    if (!view->hidden) {
        toplevel = wpe_view ? wpe_view_get_toplevel(wpe_view) : NULL;
        if (toplevel && !engine_view_apply_geometry(view))
            g_warning("restore visible WPE geometry failed");
    }
    client_send_ack(client, request);
    return !client->failed;
}

static gboolean
handle_set_layer(Client *client, const MuxEngineMessage *request)
{
    EngineView *view = lookup_owned_view(client, request);
    MuxEngineCursor cursor;
    g_autofree gchar *layer = NULL;

    if (!view)
        return TRUE;
    mux_engine_cursor_init(&cursor, request->payload);
    if (request->flags != MUX_ENGINE_FLAG_NONE ||
        !mux_engine_cursor_get_string(&cursor, &layer) ||
        !mux_engine_cursor_done(&cursor) ||
        !*layer ||
        !text_is_valid(layer, MUX_ENGINE_MAX_LAYER_BYTES)) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          "invalid SET_LAYER payload");
        return TRUE;
    }
    if (g_strcmp0(view->layer, layer) != 0) {
        g_free(view->layer);
        view->layer = g_steal_pointer(&layer);
        view_send_metadata(view);
    }
    client_send_ack(client, request);
    return !client->failed;
}

static gboolean
handle_extension(Client *client, const MuxEngineMessage *request)
{
    Engine *engine = client->engine;
    EngineView *view = lookup_owned_view(client, request);
    GError *error = NULL;
    GError *probe_error = NULL;
    const guint8 *packet;
    gsize packet_size;
    gchar *origin;
    gboolean handled;
    MuxUiRecordType record_type;

    if (!view)
        return TRUE;
    if (g_bytes_get_size(request->payload) == 0) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          "empty engine extension packet");
        return TRUE;
    }

    packet = g_bytes_get_data(request->payload, &packet_size);
    if (view->ui_bridge &&
        mux_ui_record_type(packet,
                           packet_size,
                           &record_type,
                           &probe_error)) {
        handled = mux_ui_engine_bridge_handle_payload(view->ui_bridge,
                                                      packet,
                                                      packet_size,
                                                      &error);
        view_finish_close_cancellation(view);
        if (handled && view->affordance_bridge)
            handled = mux_browser_affordance_bridge_handle_payload(
                view->affordance_bridge,
                packet,
                packet_size,
                &error);
        if (handled && view->notification_engine)
            handled = mux_notification_engine_handle_payload(
                view->notification_engine,
                packet,
                packet_size,
                &error);
        if (handled && view->navigation_policy)
            handled = mux_navigation_policy_handle_payload(
                view->navigation_policy,
                packet,
                packet_size,
                &error);
        if (handled && view->file_chooser_bridge)
            handled = mux_file_chooser_bridge_handle_payload(
                view->file_chooser_bridge,
                packet,
                packet_size,
                &error);
        if (handled) {
            MuxDownloadManager *download_manager = view->ephemeral
                ? view->download_manager
                : engine->download_manager;

            if (download_manager)
                handled = mux_download_manager_handle_payload(
                    download_manager,
                    packet,
                    packet_size,
                    &error);
        }
        if (!handled) {
            client_send_error(client,
                              request,
                              MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                              error ? error->message :
                                      "UI response was rejected");
            g_clear_error(&error);
        }
        return !client->failed;
    }
    g_clear_error(&probe_error);

    if (!engine->clipboard_link) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_STATE,
                          "clipboard bridge is unavailable");
        return TRUE;
    }

    origin = clipboard_origin(view);
    if (!mux_clipboard_engine_link_set_active_source(engine->clipboard_link,
                                                     view->id,
                                                     origin,
                                                     view->ephemeral,
                                                     &error)) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          error->message);
        g_clear_error(&error);
        g_free(origin);
        return TRUE;
    }
    g_free(origin);

    engine->clipboard_reply_client = client;
    engine->clipboard_reply_view_id = view->id;
    handled = mux_clipboard_engine_link_handle_packet(engine->clipboard_link,
                                                      packet,
                                                      packet_size,
                                                      &error);
    engine->clipboard_reply_client = NULL;
    engine->clipboard_reply_view_id = 0;

    if (!handled) {
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          error ? error->message :
                                  "clipboard packet was rejected");
        g_clear_error(&error);
    }
    return !client->failed;
}

static gboolean
handle_message(Client *client, const MuxEngineMessage *request)
{
    if (request->type == MUX_ENGINE_MESSAGE_PING)
        return client_send(client,
                           MUX_ENGINE_MESSAGE_PONG,
                           request->flags,
                           request->view_id,
                           request->serial,
                           request->payload);

    if (!client->welcomed) {
        if (request->type == MUX_ENGINE_MESSAGE_HELLO)
            return handle_hello(client, request);
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_STATE,
                          "HELLO is required before other messages");
        return FALSE;
    }

    switch (request->type) {
    case MUX_ENGINE_MESSAGE_HELLO:
        return handle_hello(client, request);
    case MUX_ENGINE_MESSAGE_CREATE_VIEW:
        return handle_create_view(client, request);
    case MUX_ENGINE_MESSAGE_DESTROY_VIEW:
        return handle_destroy_view(client, request);
    case MUX_ENGINE_MESSAGE_REQUEST_CLOSE:
        return handle_request_close(client, request);
    case MUX_ENGINE_MESSAGE_CANCEL_CLOSE:
        return handle_cancel_close(client, request);
    case MUX_ENGINE_MESSAGE_RESIZE:
        return handle_resize(client, request);
    case MUX_ENGINE_MESSAGE_NAVIGATE:
        return handle_navigate(client, request);
    case MUX_ENGINE_MESSAGE_INPUT_KEY:
        return handle_input_key(client, request);
    case MUX_ENGINE_MESSAGE_TEXT_COMMIT:
        return handle_text_commit(client, request);
    case MUX_ENGINE_MESSAGE_INPUT_POINTER:
        return handle_input_pointer(client, request);
    case MUX_ENGINE_MESSAGE_SET_FOCUS:
        return handle_set_focus(client, request);
    case MUX_ENGINE_MESSAGE_FRAME_ACK:
        return handle_frame_ack(client, request);
    case MUX_ENGINE_MESSAGE_FRAME_REJECTED:
        return handle_frame_rejected(client, request);
    case MUX_ENGINE_MESSAGE_SET_VISIBILITY:
        return handle_set_visibility(client, request);
    case MUX_ENGINE_MESSAGE_SET_LAYER:
        return handle_set_layer(client, request);
    case MUX_ENGINE_MESSAGE_EXTENSION:
        return handle_extension(client, request);
    default:
        client_send_error(client,
                          request,
                          MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE,
                          "message type is not valid from an engine client");
        return TRUE;
    }
}

static void
remove_client_views(Client *client)
{
    GHashTableIter iterator;
    gpointer key;
    gpointer value;

    g_hash_table_iter_init(&iterator, client->engine->views);
    while (g_hash_table_iter_next(&iterator, &key, &value)) {
        EngineView *view = value;
        if (view->owner == client)
            g_hash_table_iter_remove(&iterator);
    }
    engine_update_idle_fallback(client->engine);
}

static void
drop_client(Client *client)
{
    Engine *engine = client->engine;

    client->watch_id = 0;
    remove_client_views(client);
    for (guint i = 0; i < engine->clients->len; i++) {
        if (g_ptr_array_index(engine->clients, i) == client) {
            g_ptr_array_remove_index_fast(engine->clients, i);
            return;
        }
    }
}

static gboolean
client_ready(gint fd, GIOCondition condition, gpointer data)
{
    Client *client = data;
    MuxEngineMessage message = { 0 };
    GError *error = NULL;
    gboolean keep;

    (void)fd;
    if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        drop_client(client);
        return G_SOURCE_REMOVE;
    }
    if (!(condition & G_IO_IN))
        return G_SOURCE_CONTINUE;

    if (!mux_engine_receive_message(client->fd, &message, &error)) {
        if (!g_error_matches(error,
                             MUX_ENGINE_ERROR,
                             MUX_ENGINE_ERROR_CLOSED))
            g_warning("engine client %ld receive failed: %s",
                      (long)client->peer_pid,
                      error->message);
        g_clear_error(&error);
        drop_client(client);
        return G_SOURCE_REMOVE;
    }

    keep = handle_message(client, &message);
    mux_engine_message_clear(&message);
    if (!keep || client->failed) {
        drop_client(client);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void
client_free(gpointer data)
{
    Client *client = data;

    if (!client)
        return;
    if (client->watch_id)
        g_source_remove(client->watch_id);
    if (client->fd >= 0)
        close(client->fd);
    g_free(client->kitty_window);
    g_free(client->layer);
    g_free(client->initial_uri);
    g_free(client);
}

static gboolean
peer_is_allowed(int fd, struct ucred *credentials)
{
    socklen_t length = sizeof(*credentials);

    if (getsockopt(fd,
                   SOL_SOCKET,
                   SO_PEERCRED,
                   credentials,
                   &length) < 0 ||
        length != sizeof(*credentials))
        return FALSE;
    return credentials->uid == getuid();
}

static gboolean
listener_ready(gint fd, GIOCondition condition, gpointer data)
{
    Engine *engine = data;

    if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        g_main_loop_quit(engine->loop);
        return G_SOURCE_REMOVE;
    }

    for (;;) {
        struct ucred credentials;
        int client_fd = accept4(fd,
                                NULL,
                                NULL,
                                SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            g_warning("accept engine client: %s", g_strerror(errno));
            break;
        }
        if (engine->clients->len >= MUX_ENGINE_MAX_CLIENTS ||
            !peer_is_allowed(client_fd, &credentials)) {
            close(client_fd);
            continue;
        }

        Client *client = g_new0(Client, 1);
        client->engine = engine;
        client->fd = client_fd;
        client->peer_pid = credentials.pid;
        client->peer_uid = credentials.uid;
        g_ptr_array_add(engine->clients, client);
        client->watch_id = g_unix_fd_add_full(
            G_PRIORITY_DEFAULT,
            client_fd,
            G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
            client_ready,
            client,
            NULL);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean
start_listener(Engine *engine, GError **error)
{
    struct sockaddr_un address = { 0 };

    engine->listen_fd = socket(AF_UNIX,
                               SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK,
                               0);
    if (engine->listen_fd < 0) {
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(errno),
                    "create engine socket: %s",
                    g_strerror(errno));
        return FALSE;
    }

    address.sun_family = AF_UNIX;
    g_strlcpy(address.sun_path,
              engine->socket_path,
              sizeof(address.sun_path));
    if (unlink(engine->socket_path) < 0 && errno != ENOENT) {
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(errno),
                    "remove stale engine socket %s: %s",
                    engine->socket_path,
                    g_strerror(errno));
        return FALSE;
    }
    if (bind(engine->listen_fd,
             (const struct sockaddr *)&address,
             offsetof(struct sockaddr_un, sun_path) +
                 strlen(address.sun_path) + 1) < 0) {
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(errno),
                    "bind engine socket %s: %s",
                    engine->socket_path,
                    g_strerror(errno));
        return FALSE;
    }
    engine->socket_bound = TRUE;
    if (chmod(engine->socket_path, 0600) < 0) {
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(errno),
                    "restrict engine socket %s: %s",
                    engine->socket_path,
                    g_strerror(errno));
        return FALSE;
    }
    if (listen(engine->listen_fd, 64) < 0) {
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(errno),
                    "listen on engine socket %s: %s",
                    engine->socket_path,
                    g_strerror(errno));
        return FALSE;
    }

    engine->listen_watch_id = g_unix_fd_add_full(
        G_PRIORITY_DEFAULT,
        engine->listen_fd,
        G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
        listener_ready,
        engine,
        NULL);
    return TRUE;
}

static gboolean
initialize_browser(Engine *engine, GError **error)
{
    WPEDisplay *display;
    WPEDisplayClass *display_class;

    if (!ensure_private_directory(engine->data_directory, error) ||
        !ensure_private_directory(engine->cache_directory, error))
        return FALSE;

    engine->browser_store = mux_browser_store_new(engine->data_directory,
                                                  error);
    if (!engine->browser_store)
        return FALSE;
    engine->permission_store = mux_permission_store_new_for_namespace(
        engine->data_directory,
        engine->profile,
        MUX_PERMISSION_STORE_SCOPE_PERSISTENT,
        error);
    if (!engine->permission_store)
        return FALSE;
    engine->ephemeral_permission_store =
        mux_permission_store_new_for_namespace(
        NULL,
        engine->profile,
        MUX_PERMISSION_STORE_SCOPE_PRIVATE,
        error);
    if (!engine->ephemeral_permission_store)
        return FALSE;

    if (!g_getenv("WPE_DISPLAY"))
        g_setenv("WPE_DISPLAY", "wpe-display-headless", FALSE);
    display = wpe_display_get_default();
    if (!display) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            "WPE headless display is unavailable");
        return FALSE;
    }

    engine->display = g_object_ref(display);
    display_class = WPE_DISPLAY_GET_CLASS(display);
    engine->original_create_view = display_class->create_view;
    engine->original_get_clipboard = display_class->get_clipboard;
    engine->clipboard_link = mux_clipboard_engine_link_new(
        display,
        engine->profile,
        FALSE,
        clipboard_output,
        clipboard_paste,
        clipboard_failure,
        engine,
        NULL);
    if (!engine->clipboard_link) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "WPE clipboard bridge construction failed");
        return FALSE;
    }
    display_engine = engine;
    display_class->create_view = mux_display_create_view;
    display_class->get_clipboard = mux_display_get_clipboard;
    engine->web_context =
        g_object_ref(webkit_web_context_get_default());
    engine->persistent_session =
        webkit_network_session_new(engine->data_directory,
                                   engine->cache_directory);
    if (!engine->persistent_session) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "WebKit network session construction failed");
        return FALSE;
    }
    engine->download_manager = mux_download_manager_new(
        engine->persistent_session,
        download_output,
        NULL,
        engine,
        NULL);
    if (!engine->download_manager) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "WebKit download manager construction failed");
        return FALSE;
    }
    mux_download_manager_set_clipboard_func(engine->download_manager,
                                            download_clipboard_output);

    webkit_network_session_set_itp_enabled(engine->persistent_session, TRUE);
    return TRUE;
}

static gboolean
quit_signal(gpointer data)
{
    Engine *engine = data;

    engine->shutting_down = TRUE;
    g_main_loop_quit(engine->loop);
    return G_SOURCE_CONTINUE;
}

static void
engine_clear(Engine *engine)
{
    engine->shutting_down = TRUE;
    if (engine->idle_exit_id)
        g_source_remove(engine->idle_exit_id);
    if (engine->muxd_retry_id)
        g_source_remove(engine->muxd_retry_id);
    if (engine->muxd_handshake_id)
        g_source_remove(engine->muxd_handshake_id);
    if (engine->muxd_watch_id)
        g_source_remove(engine->muxd_watch_id);
    if (engine->muxd_fd >= 0)
        close(engine->muxd_fd);
    if (engine->muxd_input)
        g_string_free(engine->muxd_input, TRUE);
    if (engine->listen_watch_id)
        g_source_remove(engine->listen_watch_id);
    if (engine->sigint_watch_id)
        g_source_remove(engine->sigint_watch_id);
    if (engine->sigterm_watch_id)
        g_source_remove(engine->sigterm_watch_id);
    if (engine->clipboard_tick_id)
        g_source_remove(engine->clipboard_tick_id);

    g_clear_pointer(&engine->download_manager,
                    mux_download_manager_free);

    if (engine->views)
        g_hash_table_destroy(engine->views);
    if (engine->pending_popups) {
        GHashTableIter iterator;
        gpointer value;

        g_hash_table_iter_init(&iterator, engine->pending_popups);
        while (g_hash_table_iter_next(&iterator, NULL, &value))
            engine_view_free(value);
        g_hash_table_unref(engine->pending_popups);
    }
    if (engine->clients)
        g_ptr_array_free(engine->clients, TRUE);
    g_clear_pointer(&engine->browser_store, mux_browser_store_free);
    if (engine->listen_fd >= 0)
        close(engine->listen_fd);
    if (engine->smoke_frame_ack_fd >= 0)
        close(engine->smoke_frame_ack_fd);
    if (engine->socket_bound)
        unlink(engine->socket_path);
    if (engine->lock_fd >= 0)
        close(engine->lock_fd);

    if (engine->display &&
        WPE_DISPLAY_GET_CLASS(engine->display)->create_view ==
            mux_display_create_view)
        WPE_DISPLAY_GET_CLASS(engine->display)->create_view =
            engine->original_create_view;
    if (engine->display &&
        WPE_DISPLAY_GET_CLASS(engine->display)->get_clipboard ==
            mux_display_get_clipboard)
        WPE_DISPLAY_GET_CLASS(engine->display)->get_clipboard =
            engine->original_get_clipboard;
    if (display_engine == engine)
        display_engine = NULL;
    g_clear_pointer(&engine->clipboard_link,
                    mux_clipboard_engine_link_free);
    g_clear_pointer(&engine->ephemeral_permission_store,
                    mux_permission_store_free);
    g_clear_pointer(&engine->permission_store,
                    mux_permission_store_free);
    g_clear_object(&engine->persistent_session);
    g_clear_object(&engine->web_context);
    g_clear_object(&engine->display);
    g_clear_pointer(&engine->loop, g_main_loop_unref);
    g_free(engine->profile);
    g_free(engine->socket_path);
    g_free(engine->lock_path);
    g_free(engine->data_directory);
    g_free(engine->cache_directory);
}

int
main(int argc, char **argv)
{
    gboolean ensure = FALSE;
    gchar *profile_option = NULL;
    gchar *socket_option = NULL;
    GOptionEntry options[] = {
        {
            "ensure",
            0,
            0,
            G_OPTION_ARG_NONE,
            &ensure,
            "Exit successfully if running; otherwise start in the background",
            NULL,
        },
        {
            "profile",
            'p',
            0,
            G_OPTION_ARG_STRING,
            &profile_option,
            "Profile name",
            "NAME",
        },
        {
            "socket",
            's',
            0,
            G_OPTION_ARG_FILENAME,
            &socket_option,
            "Engine socket path",
            "PATH",
        },
        { 0 },
    };
    GOptionContext *option_context;
    GError *error = NULL;
    gboolean already_running = FALSE;
    gint daemon_result;
    pid_t daemon_pid = 0;
    int result = 1;
    Engine engine = {
        .listen_fd = -1,
        .lock_fd = -1,
        .smoke_frame_ack_fd = -1,
        .muxd_fd = -1,
        .next_view_id = 1,
    };

    g_set_prgname("mux-engine");
    signal(SIGPIPE, SIG_IGN);

    option_context = g_option_context_new(
        "- central WPE engine for Mux Kitty panes");
    g_option_context_add_main_entries(option_context, options, NULL);
    if (!g_option_context_parse(option_context, &argc, &argv, &error)) {
        g_printerr("mux-engine: %s\n", error->message);
        g_clear_error(&error);
        g_option_context_free(option_context);
        return 2;
    }
    g_option_context_free(option_context);

    engine.profile = g_strdup(
        profile_option ? profile_option :
        (g_getenv("MUX_PROFILE") ? g_getenv("MUX_PROFILE") : "default"));
    g_free(profile_option);
    if (!profile_is_valid(engine.profile)) {
        g_printerr("mux-engine: invalid profile name\n");
        g_free(socket_option);
        engine_clear(&engine);
        return 2;
    }
    if (!mux_engine_parse_device_scale(g_getenv(MUX_DEVICE_SCALE_ENV),
                                       &engine.device_scale_milli,
                                       &error)) {
        g_printerr("mux-engine: %s\n", error->message);
        g_clear_error(&error);
        g_free(socket_option);
        engine_clear(&engine);
        return 2;
    }
    if (!prepare_paths(&engine, socket_option, &error)) {
        g_printerr("mux-engine: %s\n", error->message);
        g_clear_error(&error);
        g_free(socket_option);
        engine_clear(&engine);
        return 1;
    }
    g_free(socket_option);

    if (!acquire_engine_lock(&engine, &already_running, &error)) {
        g_printerr("mux-engine: %s\n", error->message);
        g_clear_error(&error);
        engine_clear(&engine);
        return 1;
    }
    if (already_running) {
        if (!ensure) {
            g_printerr("mux-engine: profile %s is already running\n",
                       engine.profile);
            engine_clear(&engine);
            return 1;
        }
        if (!wait_for_engine_listener(engine.socket_path, 0, &error)) {
            g_printerr("mux-engine: %s\n", error->message);
            g_clear_error(&error);
            engine_clear(&engine);
            return 1;
        }
        engine_clear(&engine);
        return 0;
    }

    if (ensure) {
        daemon_result = daemonize_engine(&daemon_pid, &error);
        if (daemon_result < 0) {
            g_printerr("mux-engine: %s\n", error->message);
            g_clear_error(&error);
            engine_clear(&engine);
            return 1;
        }
        if (daemon_result > 0) {
            gboolean ready = wait_for_engine_listener(engine.socket_path,
                                                       daemon_pid,
                                                       &error);

            if (!ready) {
                g_printerr("mux-engine: %s\n", error->message);
                g_clear_error(&error);
            }
            engine_clear(&engine);
            return ready ? 0 : 1;
        }
    }

    if (!write_lock_pid(&engine, &error) ||
        !smoke_frame_ack_telemetry_open(&engine, &error) ||
        !initialize_browser(&engine, &error)) {
        g_printerr("mux-engine: %s\n", error->message);
        g_clear_error(&error);
        engine_clear(&engine);
        return 1;
    }

    engine.loop = g_main_loop_new(NULL, FALSE);
    engine.clients = g_ptr_array_new_with_free_func(client_free);
    engine.views = g_hash_table_new_full(g_int64_hash,
                                         g_int64_equal,
                                         g_free,
                                         engine_view_free);
    engine.pending_popups =
        g_hash_table_new(g_direct_hash, g_direct_equal);
    engine.clipboard_tick_id = g_timeout_add(250,
                                             clipboard_tick,
                                             &engine);
    if (!start_listener(&engine, &error)) {
        g_printerr("mux-engine: %s\n", error->message);
        g_clear_error(&error);
        engine_clear(&engine);
        return 1;
    }
    if (!engine_muxd_connect(&engine))
        engine_muxd_schedule_reconnect(&engine);

    engine.sigint_watch_id = g_unix_signal_add(SIGINT,
                                                quit_signal,
                                                &engine);
    engine.sigterm_watch_id = g_unix_signal_add(SIGTERM,
                                                 quit_signal,
                                                 &engine);
    g_main_loop_run(engine.loop);
    result = 0;
    engine_clear(&engine);
    return result;
}

#endif
