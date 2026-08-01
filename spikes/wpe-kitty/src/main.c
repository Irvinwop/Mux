#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <glib.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <wpe/headless/wpe-headless.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>

#include "mux-ipc.h"

#define MUX_DEFAULT_URI "https://wpewebkit.org/"
#define MUX_DEFAULT_FPS 60
#define MUX_MIN_VIEW_WIDTH 64
#define MUX_MIN_VIEW_HEIGHT 64

#define MUX_FOURCC(a, b, c, d) \
    ((guint32)(a) | ((guint32)(b) << 8) | ((guint32)(c) << 16) | ((guint32)(d) << 24))
#define MUX_DRM_FORMAT_ARGB8888 MUX_FOURCC('A', 'R', '2', '4')
#define MUX_DRM_FORMAT_XRGB8888 MUX_FOURCC('X', 'R', '2', '4')
#define MUX_DRM_FORMAT_ABGR8888 MUX_FOURCC('A', 'B', '2', '4')
#define MUX_DRM_FORMAT_XBGR8888 MUX_FOURCC('X', 'B', '2', '4')

typedef struct _MuxApp MuxApp;
typedef struct _MuxView MuxView;
typedef struct _MuxViewClass MuxViewClass;

struct _MuxApp {
    GMainLoop *loop;
    WPEDisplay *display;
    WPEToplevel *toplevel;
    WebKitWebView *web_view;
    WPEView *wpe_view;
    MuxIpc *ipc;

    struct termios saved_termios;
    int saved_stdin_flags;
    gboolean terminal_active;
    gboolean quitting;

    guint columns;
    guint rows;
    guint pixel_width;
    guint pixel_height;
    guint bar_height;
    guint content_height;

    guint32 image_id;
    gboolean root_uploaded;
    int root_width;
    int root_height;

    GByteArray *input;
    GString *url_input;
    gchar *uri;
    gchar *status;
    gboolean url_mode;
    gboolean replace_url_on_type;
    gboolean global_bar;

    double pointer_x;
    double pointer_y;
    gboolean pointer_inside;
    WPEModifiers pointer_buttons;
    guint max_fps;
};

struct _MuxView {
    WPEView parent_instance;
    MuxApp *app;
    WPEBuffer *pending_buffer;
    WPERectangle pending_damage;
    guint present_source;
    gint64 last_present_us;
    gint64 frame_interval_us;
};

struct _MuxViewClass {
    WPEViewClass parent_class;
};

static GType mux_view_get_type(void);
G_DEFINE_TYPE(MuxView, mux_view, WPE_TYPE_VIEW)

static MuxApp *global_app;
static guint64 shm_sequence;

static gboolean write_all(int fd, const void *data, gsize length)
{
    const guint8 *cursor = data;

    while (length) {
        ssize_t written = write(fd, cursor, length);
        if (written > 0) {
            cursor += written;
            length -= (gsize)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        return FALSE;
    }

    return TRUE;
}

static gboolean terminal_write(const gchar *text)
{
    return write_all(STDOUT_FILENO, text, strlen(text));
}

static gboolean remove_shm_later(gpointer user_data)
{
    (void)user_data;
    return G_SOURCE_REMOVE;
}

static void unlink_shm(gpointer user_data)
{
    gchar *name = user_data;
    shm_unlink(name);
    g_free(name);
}

static gboolean create_shm_payload(const guint8 *pixels, gsize size, gchar **name_out)
{
    int fd = -1;
    gchar *name = NULL;

    for (guint attempt = 0; attempt < 32; attempt++) {
        g_free(name);
        name = g_strdup_printf(
            "/mux-%ld-%" G_GUINT64_FORMAT,
            (long)getpid(),
            ++shm_sequence);
        fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0)
            break;
        if (errno != EEXIST)
            break;
    }

    if (fd < 0) {
        g_warning("shm_open failed: %s", g_strerror(errno));
        g_free(name);
        return FALSE;
    }

    gboolean ok = ftruncate(fd, (off_t)size) == 0 && write_all(fd, pixels, size);
    int saved_errno = errno;
    close(fd);

    if (!ok) {
        shm_unlink(name);
        g_warning("writing shared frame failed: %s", g_strerror(saved_errno));
        g_free(name);
        return FALSE;
    }

    *name_out = name;
    return TRUE;
}

static void kitty_delete_image(MuxApp *app)
{
    if (!app->image_id)
        return;

    gchar *command = g_strdup_printf(
        "\033_Ga=d,d=I,i=%u,q=2\033\\",
        app->image_id);
    terminal_write(command);
    g_free(command);
    app->root_uploaded = FALSE;
}

static gboolean kitty_send_pixels(
    MuxApp *app,
    const guint8 *pixels,
    int image_width,
    int image_height,
    const WPERectangle *damage)
{
    gsize size = (gsize)damage->width * (gsize)damage->height * 4;
    gchar *shm_name = NULL;
    if (!create_shm_payload(pixels, size, &shm_name))
        return FALSE;

    gchar *encoded_name = g_base64_encode((const guchar *)shm_name, strlen(shm_name));
    gchar *command = NULL;

    gboolean replace_root =
        !app->root_uploaded ||
        app->root_width != image_width ||
        app->root_height != image_height;

    if (replace_root) {
        if (app->root_uploaded)
            kitty_delete_image(app);

        guint content_row = app->bar_height ? 2 : 1;
        guint placement_rows = app->rows - (app->bar_height ? 1 : 0);
        placement_rows = MAX(placement_rows, 1);

        gchar *position = g_strdup_printf("\033[%u;1H", content_row);
        terminal_write(position);
        g_free(position);

        command = g_strdup_printf(
            "\033_Ga=T,f=32,t=s,s=%d,v=%d,S=%" G_GSIZE_FORMAT
            ",i=%u,p=1,c=%u,r=%u,z=-1,C=1,q=2;%s\033\\",
            image_width,
            image_height,
            size,
            app->image_id,
            MAX(app->columns, 1),
            placement_rows,
            encoded_name);
    } else {
        command = g_strdup_printf(
            "\033_Ga=f,f=32,t=s,S=%" G_GSIZE_FORMAT
            ",i=%u,r=1,x=%d,y=%d,s=%d,v=%d,X=1,q=2;%s\033\\",
            size,
            app->image_id,
            damage->x,
            damage->y,
            damage->width,
            damage->height,
            encoded_name);
    }

    gboolean ok = terminal_write(command);
    if (ok) {
        app->root_uploaded = TRUE;
        app->root_width = image_width;
        app->root_height = image_height;
        g_timeout_add_seconds_full(
            G_PRIORITY_LOW,
            5,
            remove_shm_later,
            g_strdup(shm_name),
            unlink_shm);
    } else {
        shm_unlink(shm_name);
    }

    g_free(command);
    g_free(encoded_name);
    g_free(shm_name);
    return ok;
}

static WPERectangle normalize_damage(
    int width,
    int height,
    const WPERectangle *rectangles,
    guint n_rectangles)
{
    WPERectangle full = { 0, 0, width, height };
    if (!rectangles || !n_rectangles)
        return full;

    int x1 = width;
    int y1 = height;
    int x2 = 0;
    int y2 = 0;
    gboolean found = FALSE;

    for (guint i = 0; i < n_rectangles; i++) {
        const WPERectangle *rect = &rectangles[i];
        if (rect->width <= 0 || rect->height <= 0)
            continue;

        int left = CLAMP(rect->x, 0, width);
        int top = CLAMP(rect->y, 0, height);
        int right = CLAMP((int)MIN((gint64)width, (gint64)rect->x + rect->width), 0, width);
        int bottom = CLAMP((int)MIN((gint64)height, (gint64)rect->y + rect->height), 0, height);
        if (right <= left || bottom <= top)
            continue;

        x1 = MIN(x1, left);
        y1 = MIN(y1, top);
        x2 = MAX(x2, right);
        y2 = MAX(y2, bottom);
        found = TRUE;
    }

    if (!found)
        return full;

    WPERectangle result = { x1, y1, x2 - x1, y2 - y1 };
    return result;
}

typedef enum {
    PIXELS_BGRA,
    PIXELS_BGRX,
    PIXELS_RGBA,
    PIXELS_RGBX,
} PixelLayout;

static PixelLayout buffer_layout(WPEBuffer *buffer)
{
    if (WPE_IS_BUFFER_DMA_BUF(buffer)) {
        guint32 format = wpe_buffer_dma_buf_get_format(WPE_BUFFER_DMA_BUF(buffer));
        switch (format) {
        case MUX_DRM_FORMAT_XRGB8888:
            return PIXELS_BGRX;
        case MUX_DRM_FORMAT_ABGR8888:
            return PIXELS_RGBA;
        case MUX_DRM_FORMAT_XBGR8888:
            return PIXELS_RGBX;
        case MUX_DRM_FORMAT_ARGB8888:
        default:
            return PIXELS_BGRA;
        }
    }

    return PIXELS_BGRA;
}

static gboolean present_buffer(
    MuxApp *app,
    WPEBuffer *buffer,
    const WPERectangle *requested_damage)
{
#if G_BYTE_ORDER != G_LITTLE_ENDIAN
    g_warning("the WPE pixel adapter currently supports little-endian Linux only");
    return FALSE;
#else
    int width = wpe_buffer_get_width(buffer);
    int height = wpe_buffer_get_height(buffer);
    if (width <= 0 || height <= 0)
        return FALSE;

    GError *error = NULL;
    GBytes *bytes = wpe_buffer_import_to_pixels(buffer, &error);
    if (!bytes) {
        g_warning("WPE buffer import failed: %s", error ? error->message : "unknown error");
        g_clear_error(&error);
        return FALSE;
    }

    gsize byte_count = 0;
    const guint8 *source = g_bytes_get_data(bytes, &byte_count);
    gsize stride = 0;

    if (WPE_IS_BUFFER_SHM(buffer))
        stride = wpe_buffer_shm_get_stride(WPE_BUFFER_SHM(buffer));
    else if (height > 0)
        stride = byte_count / (gsize)height;

    if (!stride || stride < (gsize)width * 4 || stride > byte_count ||
        (gsize)height > byte_count / stride) {
        g_warning("WPE returned an invalid pixel stride");
        return FALSE;
    }

    WPERectangle damage = *requested_damage;
    if (!app->root_uploaded ||
        app->root_width != width ||
        app->root_height != height) {
        damage = (WPERectangle){ 0, 0, width, height };
    }

    if (damage.width <= 0 || damage.height <= 0)
        return TRUE;
    if ((gsize)damage.width > G_MAXSIZE / (gsize)damage.height / 4)
        return FALSE;

    gsize packed_size = (gsize)damage.width * (gsize)damage.height * 4;
    guint8 *packed = g_malloc(packed_size);
    PixelLayout layout = buffer_layout(buffer);

    for (int y = 0; y < damage.height; y++) {
        const guint8 *src =
            source + (gsize)(damage.y + y) * stride + (gsize)damage.x * 4;
        guint8 *dst = packed + (gsize)y * (gsize)damage.width * 4;

        for (int x = 0; x < damage.width; x++, src += 4, dst += 4) {
            if (layout == PIXELS_BGRA || layout == PIXELS_BGRX) {
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
            } else {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
            }
            dst[3] =
                (layout == PIXELS_BGRX || layout == PIXELS_RGBX) ? 255 : src[3];
        }
    }

    gboolean ok = kitty_send_pixels(app, packed, width, height, &damage);
    g_free(packed);
    return ok;
#endif
}

static void mux_view_present(MuxView *self)
{
    WPEBuffer *buffer = self->pending_buffer;
    if (!buffer)
        return;

    self->pending_buffer = NULL;
    if (self->app && self->app->terminal_active)
        present_buffer(self->app, buffer, &self->pending_damage);

    self->last_present_us = g_get_monotonic_time();
    wpe_view_buffer_rendered(WPE_VIEW(self), buffer);
    wpe_view_buffer_released(WPE_VIEW(self), buffer);
    g_object_unref(buffer);
}

static gboolean mux_view_present_timeout(gpointer user_data)
{
    MuxView *self = user_data;
    self->present_source = 0;
    mux_view_present(self);
    return G_SOURCE_REMOVE;
}

static gboolean mux_view_render_buffer(
    WPEView *view,
    WPEBuffer *buffer,
    const WPERectangle *damage_rects,
    guint n_damage_rects,
    GError **error)
{
    (void)error;
    MuxView *self = (MuxView *)view;

    if (self->pending_buffer) {
        if (self->present_source) {
            g_source_remove(self->present_source);
            self->present_source = 0;
        }
        mux_view_present(self);
    }

    self->pending_buffer = g_object_ref(buffer);
    self->pending_damage = normalize_damage(
        wpe_buffer_get_width(buffer),
        wpe_buffer_get_height(buffer),
        damage_rects,
        n_damage_rects);

    gint64 now = g_get_monotonic_time();
    gint64 due = self->last_present_us
        ? self->last_present_us + self->frame_interval_us
        : now;

    if (due <= now) {
        mux_view_present(self);
        return TRUE;
    }

    guint delay_ms = (guint)MAX((due - now + 999) / 1000, 1);
    self->present_source = g_timeout_add_full(
        G_PRIORITY_HIGH,
        delay_ms,
        mux_view_present_timeout,
        g_object_ref(self),
        g_object_unref);
    return TRUE;
}

static gboolean mux_view_can_be_mapped(WPEView *view)
{
    (void)view;
    return TRUE;
}

static void mux_view_dispose(GObject *object)
{
    MuxView *self = (MuxView *)object;

    if (self->present_source) {
        g_source_remove(self->present_source);
        self->present_source = 0;
    }

    if (self->pending_buffer) {
        wpe_view_buffer_rendered(WPE_VIEW(self), self->pending_buffer);
        wpe_view_buffer_released(WPE_VIEW(self), self->pending_buffer);
        g_clear_object(&self->pending_buffer);
    }

    G_OBJECT_CLASS(mux_view_parent_class)->dispose(object);
}

static void mux_view_class_init(MuxViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = mux_view_dispose;

    WPEViewClass *view_class = WPE_VIEW_CLASS(klass);
    view_class->render_buffer = mux_view_render_buffer;
    view_class->can_be_mapped = mux_view_can_be_mapped;
}

static void mux_view_init(MuxView *self)
{
    self->frame_interval_us = G_USEC_PER_SEC / MUX_DEFAULT_FPS;
}

static WPEView *mux_display_create_view(WPEDisplay *display)
{
    return WPE_VIEW(g_object_new(mux_view_get_type(), "display", display, NULL));
}

static guint env_uint(const gchar *name, guint fallback)
{
    const gchar *value = g_getenv(name);
    if (!value || !*value)
        return fallback;

    gchar *end = NULL;
    guint64 parsed = g_ascii_strtoull(value, &end, 10);
    if (!end || *end || !parsed || parsed > G_MAXUINT)
        return fallback;
    return (guint)parsed;
}

static void read_terminal_geometry(MuxApp *app)
{
    struct winsize size = { 0 };
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);

    app->columns = size.ws_col ? size.ws_col : env_uint("MUX_COLUMNS", 100);
    app->rows = size.ws_row ? size.ws_row : env_uint("MUX_ROWS", 40);
    app->pixel_width = size.ws_xpixel
        ? size.ws_xpixel
        : env_uint("MUX_WIDTH", app->columns * 10);
    app->pixel_height = size.ws_ypixel
        ? size.ws_ypixel
        : env_uint("MUX_HEIGHT", app->rows * 20);

    app->bar_height = !app->global_bar && app->rows > 1
        ? MAX(app->pixel_height / app->rows, 1)
        : 0;
    app->content_height = app->pixel_height - app->bar_height;
    app->pixel_width = MAX(app->pixel_width, MUX_MIN_VIEW_WIDTH);
    app->content_height = MAX(app->content_height, MUX_MIN_VIEW_HEIGHT);
}

static gchar *ascii_status(const gchar *text)
{
    GString *clean = g_string_new(NULL);
    if (!text)
        return g_string_free(clean, FALSE);

    for (const guchar *cursor = (const guchar *)text; *cursor; cursor++) {
        if (*cursor >= 0x20 && *cursor < 0x7f)
            g_string_append_c(clean, (gchar)*cursor);
        else if (*cursor >= 0x80)
            g_string_append_c(clean, '?');
    }
    return g_string_free(clean, FALSE);
}

static void draw_url_bar(MuxApp *app)
{
    if (!app->terminal_active || !app->bar_height)
        return;

    const gchar *value = app->status
        ? app->status
        : (app->uri ? app->uri : MUX_DEFAULT_URI);
    const gchar *prefix = " mux  ";
    if (app->url_mode) {
        value = app->url_input->str;
        prefix = " > ";
    }

    gchar *clean = ascii_status(value);
    GString *line = g_string_new("\033[1;1H\033[48;2;16;29;26m\033[38;2;188;238;210m");
    guint used = 0;

    for (const gchar *cursor = prefix; *cursor && used < app->columns; cursor++, used++)
        g_string_append_c(line, *cursor);
    for (const gchar *cursor = clean; *cursor && used < app->columns; cursor++, used++)
        g_string_append_c(line, *cursor);
    while (used++ < app->columns)
        g_string_append_c(line, ' ');

    g_string_append(line, "\033[0m");
    terminal_write(line->str);
    g_string_free(line, TRUE);
    g_free(clean);
}

static void set_status(MuxApp *app, const gchar *status)
{
    g_free(app->status);
    app->status = status ? g_strdup(status) : NULL;
    draw_url_bar(app);
}

static void set_terminal_title(const gchar *title)
{
    if (!title)
        title = "Mux";

    GString *safe = g_string_new(NULL);
    for (const guchar *cursor = (const guchar *)title; *cursor; cursor++) {
        if (*cursor != 0x07 && *cursor != 0x1b &&
            (*cursor >= 0x20 || *cursor >= 0x80)) {
            g_string_append_c(safe, (gchar)*cursor);
        }
    }

    gchar *command = g_strdup_printf("\033]2;Mux - %s\007", safe->str);
    terminal_write(command);
    g_free(command);
    g_string_free(safe, TRUE);
}

static gboolean terminal_enable(MuxApp *app)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        g_printerr("mux-view must run directly inside a Kitty pane\n");
        return FALSE;
    }

    if (tcgetattr(STDIN_FILENO, &app->saved_termios) < 0) {
        g_printerr("tcgetattr failed: %s\n", g_strerror(errno));
        return FALSE;
    }

    struct termios raw = app->saved_termios;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
        g_printerr("tcsetattr failed: %s\n", g_strerror(errno));
        return FALSE;
    }

    app->saved_stdin_flags = fcntl(STDIN_FILENO, F_GETFL);
    if (app->saved_stdin_flags >= 0)
        fcntl(STDIN_FILENO, F_SETFL, app->saved_stdin_flags | O_NONBLOCK);

    app->terminal_active = TRUE;
    terminal_write(
        "\033[?1049h"
        "\033[2J\033[H"
        "\033[?25l"
        "\033[?1004h"
        "\033[?1003h"
        "\033[?1006h"
        "\033[?1016h"
        "\033[>31u");
    draw_url_bar(app);
    return TRUE;
}

static void terminal_restore(MuxApp *app)
{
    if (!app || !app->terminal_active)
        return;

    kitty_delete_image(app);
    terminal_write(
        "\033[<u"
        "\033[?1016l"
        "\033[?1006l"
        "\033[?1003l"
        "\033[?1004l"
        "\033[?25h"
        "\033[0m"
        "\033[?1049l");

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &app->saved_termios);
    if (app->saved_stdin_flags >= 0)
        fcntl(STDIN_FILENO, F_SETFL, app->saved_stdin_flags);
    app->terminal_active = FALSE;
}

static void restore_at_exit(void)
{
    terminal_restore(global_app);
}

static gboolean request_quit(gpointer user_data)
{
    MuxApp *app = user_data;
    if (app->quitting)
        return G_SOURCE_REMOVE;

    app->quitting = TRUE;
    if (app->loop)
        g_main_loop_quit(app->loop);
    return G_SOURCE_REMOVE;
}

static gchar *normalize_uri(const gchar *input)
{
    gchar *trimmed = g_strdup(input ? input : "");
    g_strstrip(trimmed);

    if (!*trimmed) {
        g_free(trimmed);
        return g_strdup(MUX_DEFAULT_URI);
    }

    if (g_path_is_absolute(trimmed)) {
        gchar *canonical = g_canonicalize_filename(trimmed, NULL);
        gchar *uri = g_filename_to_uri(canonical, NULL, NULL);
        g_free(canonical);
        g_free(trimmed);
        return uri ? uri : g_strdup(MUX_DEFAULT_URI);
    }

    if (strstr(trimmed, "://") ||
        g_str_has_prefix(trimmed, "about:") ||
        g_str_has_prefix(trimmed, "data:") ||
        g_str_has_prefix(trimmed, "file:")) {
        return trimmed;
    }

    if (strchr(trimmed, ' ')) {
        gchar *escaped = g_uri_escape_string(trimmed, NULL, TRUE);
        gchar *uri = g_strdup_printf("https://duckduckgo.com/?q=%s", escaped);
        g_free(escaped);
        g_free(trimmed);
        return uri;
    }

    gchar *uri = g_strdup_printf("https://%s", trimmed);
    g_free(trimmed);
    return uri;
}

static void enter_url_mode(MuxApp *app)
{
    app->url_mode = TRUE;
    app->replace_url_on_type = TRUE;
    g_string_assign(app->url_input, app->uri ? app->uri : "");
    set_status(app, NULL);
    draw_url_bar(app);
}

static void leave_url_mode(MuxApp *app, gboolean navigate)
{
    if (navigate) {
        gchar *uri = normalize_uri(app->url_input->str);
        webkit_web_view_load_uri(app->web_view, uri);
        g_free(uri);
    }

    app->url_mode = FALSE;
    app->replace_url_on_type = FALSE;
    draw_url_bar(app);
}

static void url_append_codepoint(MuxApp *app, gunichar codepoint)
{
    if (!g_unichar_validate(codepoint) || g_unichar_iscntrl(codepoint))
        return;

    if (app->replace_url_on_type) {
        g_string_truncate(app->url_input, 0);
        app->replace_url_on_type = FALSE;
    }

    gchar encoded[7] = { 0 };
    gint length = g_unichar_to_utf8(codepoint, encoded);
    g_string_append_len(app->url_input, encoded, length);
    draw_url_bar(app);
}

static void url_backspace(MuxApp *app)
{
    if (app->replace_url_on_type) {
        g_string_truncate(app->url_input, 0);
        app->replace_url_on_type = FALSE;
    } else if (app->url_input->len) {
        gchar *previous = g_utf8_find_prev_char(
            app->url_input->str,
            app->url_input->str + app->url_input->len);
        if (previous)
            g_string_truncate(app->url_input, (gsize)(previous - app->url_input->str));
    }
    draw_url_bar(app);
}

static guint32 event_time_ms(void)
{
    return (guint32)(g_get_monotonic_time() / 1000);
}

static WPEModifiers kitty_modifiers(guint kitty_bits)
{
    WPEModifiers modifiers = 0;
    if (kitty_bits & 1)
        modifiers |= WPE_MODIFIER_KEYBOARD_SHIFT;
    if (kitty_bits & 2)
        modifiers |= WPE_MODIFIER_KEYBOARD_ALT;
    if (kitty_bits & 4)
        modifiers |= WPE_MODIFIER_KEYBOARD_CONTROL;
    if (kitty_bits & 8)
        modifiers |= WPE_MODIFIER_KEYBOARD_META;
    if (kitty_bits & 64)
        modifiers |= WPE_MODIFIER_KEYBOARD_CAPS_LOCK;
    return modifiers;
}

static guint unicode_keysym(guint codepoint)
{
    if (codepoint <= 0xff)
        return codepoint;
    if (codepoint <= 0x10ffff)
        return 0x01000000u | codepoint;
    return 0;
}

static guint kitty_key_to_keysym(guint key)
{
    switch (key) {
    case 9:
        return WPE_KEY_Tab;
    case 13:
        return WPE_KEY_Return;
    case 27:
        return WPE_KEY_Escape;
    case 127:
        return WPE_KEY_BackSpace;
    case 57441:
        return WPE_KEY_Shift_L;
    case 57442:
        return WPE_KEY_Control_L;
    case 57443:
        return WPE_KEY_Alt_L;
    case 57444:
        return WPE_KEY_Super_L;
    case 57447:
        return WPE_KEY_Shift_R;
    case 57448:
        return WPE_KEY_Control_R;
    case 57449:
        return WPE_KEY_Alt_R;
    case 57450:
        return WPE_KEY_Super_R;
    default:
        break;
    }

    if (key >= 57376 && key <= 57398)
        return WPE_KEY_F13 + (key - 57376);
    if (key >= 57344 && key <= 63743)
        return 0;
    return unicode_keysym(key);
}

static guint evdev_code_for_key(guint key)
{
    static const guint letter_codes[26] = {
        30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
        49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,
    };

    if (key >= 'a' && key <= 'z')
        return letter_codes[key - 'a'] + 8;
    if (key >= 'A' && key <= 'Z')
        return letter_codes[key - 'A'] + 8;
    if (key >= '1' && key <= '9')
        return (key - '1' + 2) + 8;

    switch (key) {
    case '0': return 11 + 8;
    case '-': return 12 + 8;
    case '=': return 13 + 8;
    case 127: return 14 + 8;
    case 9: return 15 + 8;
    case '[': return 26 + 8;
    case ']': return 27 + 8;
    case 13: return 28 + 8;
    case ';': return 39 + 8;
    case '\'': return 40 + 8;
    case 0x60: return 41 + 8;
    case '\\': return 43 + 8;
    case ',': return 51 + 8;
    case '.': return 52 + 8;
    case '/': return 53 + 8;
    case ' ': return 57 + 8;
    case 27: return 1 + 8;
    default: return 0;
    }
}

static void emit_keyboard_event(
    MuxApp *app,
    WPEEventType type,
    guint keycode,
    guint keyval,
    WPEModifiers modifiers)
{
    if (!keyval)
        return;

    WPEEvent *event = wpe_event_keyboard_new(
        type,
        app->wpe_view,
        WPE_INPUT_SOURCE_KEYBOARD,
        event_time_ms(),
        modifiers,
        keycode,
        keyval);
    wpe_view_event(app->wpe_view, event);
    wpe_event_unref(event);
}

static gboolean browser_shortcut(
    MuxApp *app,
    guint key,
    guint keyval,
    guint kitty_bits,
    guint event_type)
{
    guint normalized = key;
    if (normalized >= 'A' && normalized <= 'Z')
        normalized += 'a' - 'A';

    gboolean control = kitty_bits & 4;
    gboolean alt = kitty_bits & 2;
    gboolean shift = kitty_bits & 1;
    gboolean pressed = event_type != 3;

    if (control && normalized == 'q') {
        if (pressed)
            request_quit(app);
        return TRUE;
    }
    if (control && normalized == 'l') {
        if (pressed) {
            if (app->global_bar && app->ipc)
                mux_ipc_prompt(app->ipc);
            else
                enter_url_mode(app);
        }
        return TRUE;
    }
    if (control && normalized == 'r') {
        if (pressed)
            webkit_web_view_reload(app->web_view);
        return TRUE;
    }
    if (control && shift && normalized == 'i') {
        if (pressed)
            webkit_web_view_toggle_inspector(app->web_view);
        return TRUE;
    }
    if (alt && keyval == WPE_KEY_Left) {
        if (pressed && webkit_web_view_can_go_back(app->web_view))
            webkit_web_view_go_back(app->web_view);
        return TRUE;
    }
    if (alt && keyval == WPE_KEY_Right) {
        if (pressed && webkit_web_view_can_go_forward(app->web_view))
            webkit_web_view_go_forward(app->web_view);
        return TRUE;
    }

    return FALSE;
}

static guint first_text_codepoint(const gchar *text_field)
{
    if (!text_field || !*text_field)
        return 0;

    gchar **parts = g_strsplit(text_field, ":", 2);
    guint value = (guint)g_ascii_strtoull(parts[0], NULL, 10);
    g_strfreev(parts);
    return value;
}

static void append_text_field_to_url(MuxApp *app, const gchar *text_field)
{
    if (!text_field || !*text_field)
        return;

    gchar **parts = g_strsplit(text_field, ":", -1);
    for (guint i = 0; parts[i]; i++) {
        guint value = (guint)g_ascii_strtoull(parts[i], NULL, 10);
        if (value)
            url_append_codepoint(app, value);
    }
    g_strfreev(parts);
}

static gboolean handle_url_key(
    MuxApp *app,
    guint keyval,
    guint kitty_bits,
    guint event_type,
    const gchar *text_field)
{
    if (!app->url_mode)
        return FALSE;
    if (event_type == 3)
        return TRUE;

    if (keyval == WPE_KEY_Escape) {
        leave_url_mode(app, FALSE);
        return TRUE;
    }
    if (keyval == WPE_KEY_Return) {
        leave_url_mode(app, TRUE);
        return TRUE;
    }
    if (keyval == WPE_KEY_BackSpace) {
        url_backspace(app);
        return TRUE;
    }
    if ((kitty_bits & 4) && (keyval == 'u' || keyval == 'U')) {
        g_string_truncate(app->url_input, 0);
        app->replace_url_on_type = FALSE;
        draw_url_bar(app);
        return TRUE;
    }

    if (!(kitty_bits & 4) && text_field && *text_field)
        append_text_field_to_url(app, text_field);
    return TRUE;
}

static void handle_key_event(
    MuxApp *app,
    guint key,
    guint shifted_key,
    guint base_key,
    guint encoded_modifiers,
    guint event_type,
    const gchar *text_field)
{
    guint kitty_bits = encoded_modifiers ? encoded_modifiers - 1 : 0;
    guint text_codepoint = first_text_codepoint(text_field);
    guint keyval = 0;

    if (text_codepoint)
        keyval = unicode_keysym(text_codepoint);
    else if ((kitty_bits & 1) && shifted_key)
        keyval = kitty_key_to_keysym(shifted_key);
    else
        keyval = kitty_key_to_keysym(key);

    guint physical_key = base_key ? base_key : key;
    guint keycode = evdev_code_for_key(physical_key);

    if (browser_shortcut(app, physical_key, keyval, kitty_bits, event_type))
        return;
    if (handle_url_key(app, keyval, kitty_bits, event_type, text_field))
        return;

    WPEEventType type = event_type == 3
        ? WPE_EVENT_KEYBOARD_KEY_UP
        : WPE_EVENT_KEYBOARD_KEY_DOWN;
    emit_keyboard_event(
        app,
        type,
        keycode,
        keyval,
        kitty_modifiers(kitty_bits));
}

static guint parse_uint(const gchar *text, guint fallback)
{
    if (!text || !*text)
        return fallback;
    gchar *end = NULL;
    guint64 value = g_ascii_strtoull(text, &end, 10);
    if (!end || *end || value > G_MAXUINT)
        return fallback;
    return (guint)value;
}

static void handle_csi_u(MuxApp *app, const gchar *parameters)
{
    gchar **fields = g_strsplit(parameters, ";", 3);
    gchar **keys = g_strsplit(fields[0] ? fields[0] : "", ":", 3);
    gchar **modifiers = g_strsplit(fields[1] ? fields[1] : "", ":", 2);

    guint key = parse_uint(keys[0], 0);
    guint shifted = parse_uint(keys[1], 0);
    guint base = parse_uint(keys[2], 0);
    guint mods = parse_uint(modifiers[0], 1);
    guint event_type = parse_uint(modifiers[1], 1);
    const gchar *text = fields[2] ? fields[2] : "";

    handle_key_event(app, key, shifted, base, mods, event_type, text);

    g_strfreev(modifiers);
    g_strfreev(keys);
    g_strfreev(fields);
}

static guint csi_modifier(const gchar *parameters)
{
    gchar **fields = g_strsplit(parameters, ";", -1);
    guint count = g_strv_length(fields);
    guint encoded = count > 1 ? parse_uint(fields[count - 1], 1) : 1;
    g_strfreev(fields);
    return encoded ? encoded - 1 : 0;
}

static void dispatch_function_key(
    MuxApp *app,
    guint keyval,
    guint evdev_code,
    guint kitty_bits)
{
    guint shortcut_key = 0;
    if (browser_shortcut(app, shortcut_key, keyval, kitty_bits, 1))
        return;
    if (handle_url_key(app, keyval, kitty_bits, 1, ""))
        return;

    WPEModifiers modifiers = kitty_modifiers(kitty_bits);
    emit_keyboard_event(
        app,
        WPE_EVENT_KEYBOARD_KEY_DOWN,
        evdev_code ? evdev_code + 8 : 0,
        keyval,
        modifiers);
    emit_keyboard_event(
        app,
        WPE_EVENT_KEYBOARD_KEY_UP,
        evdev_code ? evdev_code + 8 : 0,
        keyval,
        modifiers);
}

static void handle_legacy_csi(
    MuxApp *app,
    const gchar *parameters,
    gchar final)
{
    guint keyval = 0;
    guint evdev = 0;
    guint bits = csi_modifier(parameters);

    switch (final) {
    case 'A': keyval = WPE_KEY_Up; evdev = 103; break;
    case 'B': keyval = WPE_KEY_Down; evdev = 108; break;
    case 'C': keyval = WPE_KEY_Right; evdev = 106; break;
    case 'D': keyval = WPE_KEY_Left; evdev = 105; break;
    case 'H': keyval = WPE_KEY_Home; evdev = 102; break;
    case 'F': keyval = WPE_KEY_End; evdev = 107; break;
    case 'P': keyval = WPE_KEY_F1; evdev = 59; break;
    case 'Q': keyval = WPE_KEY_F2; evdev = 60; break;
    case 'S': keyval = WPE_KEY_F4; evdev = 62; break;
    case 'Z':
        keyval = WPE_KEY_Tab;
        evdev = 15;
        bits |= 1;
        break;
    default:
        return;
    }

    dispatch_function_key(app, keyval, evdev, bits);
}

static void handle_tilde_key(MuxApp *app, const gchar *parameters)
{
    gchar **fields = g_strsplit(parameters, ";", -1);
    guint code = parse_uint(fields[0], 0);
    guint count = g_strv_length(fields);
    guint encoded = count > 1 ? parse_uint(fields[count - 1], 1) : 1;
    guint bits = encoded ? encoded - 1 : 0;
    guint keyval = 0;
    guint evdev = 0;

    switch (code) {
    case 2: keyval = WPE_KEY_Insert; evdev = 110; break;
    case 3: keyval = WPE_KEY_Delete; evdev = 111; break;
    case 5: keyval = WPE_KEY_Page_Up; evdev = 104; break;
    case 6: keyval = WPE_KEY_Page_Down; evdev = 109; break;
    case 7: keyval = WPE_KEY_Home; evdev = 102; break;
    case 8: keyval = WPE_KEY_End; evdev = 107; break;
    case 11: keyval = WPE_KEY_F1; evdev = 59; break;
    case 12: keyval = WPE_KEY_F2; evdev = 60; break;
    case 13: keyval = WPE_KEY_F3; evdev = 61; break;
    case 14: keyval = WPE_KEY_F4; evdev = 62; break;
    case 15: keyval = WPE_KEY_F5; evdev = 63; break;
    case 17: keyval = WPE_KEY_F6; evdev = 64; break;
    case 18: keyval = WPE_KEY_F7; evdev = 65; break;
    case 19: keyval = WPE_KEY_F8; evdev = 66; break;
    case 20: keyval = WPE_KEY_F9; evdev = 67; break;
    case 21: keyval = WPE_KEY_F10; evdev = 68; break;
    case 23: keyval = WPE_KEY_F11; evdev = 87; break;
    case 24: keyval = WPE_KEY_F12; evdev = 88; break;
    default: break;
    }

    if (keyval)
        dispatch_function_key(app, keyval, evdev, bits);
    g_strfreev(fields);
}

static WPEModifiers mouse_keyboard_modifiers(guint code)
{
    WPEModifiers modifiers = 0;
    if (code & 4)
        modifiers |= WPE_MODIFIER_KEYBOARD_SHIFT;
    if (code & 8)
        modifiers |= WPE_MODIFIER_KEYBOARD_ALT;
    if (code & 16)
        modifiers |= WPE_MODIFIER_KEYBOARD_CONTROL;
    return modifiers;
}

static WPEModifiers pointer_modifier_for_button(guint button)
{
    switch (button) {
    case WPE_BUTTON_PRIMARY: return WPE_MODIFIER_POINTER_BUTTON1;
    case WPE_BUTTON_MIDDLE: return WPE_MODIFIER_POINTER_BUTTON2;
    case WPE_BUTTON_SECONDARY: return WPE_MODIFIER_POINTER_BUTTON3;
    default: return 0;
    }
}

static void send_pointer_motion(
    MuxApp *app,
    WPEEventType type,
    double x,
    double y,
    WPEModifiers modifiers)
{
    WPEEvent *event = wpe_event_pointer_move_new(
        type,
        app->wpe_view,
        WPE_INPUT_SOURCE_MOUSE,
        event_time_ms(),
        modifiers,
        x,
        y,
        x - app->pointer_x,
        y - app->pointer_y);
    wpe_view_event(app->wpe_view, event);
    wpe_event_unref(event);
    app->pointer_x = x;
    app->pointer_y = y;
}

static void handle_mouse(
    MuxApp *app,
    const gchar *parameters,
    gboolean release)
{
    guint code = 0;
    int terminal_x = 0;
    int terminal_y = 0;
    if (sscanf(parameters, "<%u;%d;%d", &code, &terminal_x, &terminal_y) != 3)
        return;

    double x = MAX(terminal_x - 1, 0);
    double pane_y = MAX(terminal_y - 1, 0);

    if (app->bar_height && pane_y < app->bar_height) {
        if (!release && !(code & 64))
            enter_url_mode(app);
        return;
    }

    double y = pane_y - app->bar_height;
    x = CLAMP(x, 0, (double)MAX((int)app->pixel_width - 1, 0));
    y = CLAMP(y, 0, (double)MAX((int)app->content_height - 1, 0));

    WPEModifiers modifiers =
        mouse_keyboard_modifiers(code) | app->pointer_buttons;

    if (code & 128) {
        if (app->pointer_inside) {
            send_pointer_motion(app, WPE_EVENT_POINTER_LEAVE, x, y, modifiers);
            app->pointer_inside = FALSE;
        }
        return;
    }

    if (!app->pointer_inside) {
        send_pointer_motion(app, WPE_EVENT_POINTER_ENTER, x, y, modifiers);
        app->pointer_inside = TRUE;
    }

    if (code & 64) {
        double delta_x = 0;
        double delta_y = 0;
        switch (code & 3) {
        case 0: delta_y = -53; break;
        case 1: delta_y = 53; break;
        case 2: delta_x = -53; break;
        case 3: delta_x = 53; break;
        }

        WPEEvent *event = wpe_event_scroll_new(
            app->wpe_view,
            WPE_INPUT_SOURCE_MOUSE,
            event_time_ms(),
            modifiers,
            delta_x,
            delta_y,
            FALSE,
            FALSE,
            x,
            y);
        wpe_view_event(app->wpe_view, event);
        wpe_event_unref(event);
        return;
    }

    if ((code & 32) && !release) {
        send_pointer_motion(app, WPE_EVENT_POINTER_MOVE, x, y, modifiers);
        return;
    }

    guint button = 0;
    switch (code & 3) {
    case 0: button = WPE_BUTTON_PRIMARY; break;
    case 1: button = WPE_BUTTON_MIDDLE; break;
    case 2: button = WPE_BUTTON_SECONDARY; break;
    default: return;
    }

    WPEModifiers button_modifier = pointer_modifier_for_button(button);
    WPEEventType type = release ? WPE_EVENT_POINTER_UP : WPE_EVENT_POINTER_DOWN;
    if (!release) {
        app->pointer_buttons |= button_modifier;
        modifiers |= button_modifier;
        wpe_view_focus_in(app->wpe_view);
    }

    guint32 time = event_time_ms();
    guint press_count = release
        ? 0
        : wpe_view_compute_press_count(app->wpe_view, x, y, button, time);
    WPEEvent *event = wpe_event_pointer_button_new(
        type,
        app->wpe_view,
        WPE_INPUT_SOURCE_MOUSE,
        time,
        modifiers,
        button,
        x,
        y,
        press_count);
    wpe_view_event(app->wpe_view, event);
    wpe_event_unref(event);

    if (release)
        app->pointer_buttons &= ~button_modifier;
    app->pointer_x = x;
    app->pointer_y = y;
}

static void handle_csi(
    MuxApp *app,
    const guint8 *data,
    gsize length,
    gchar final)
{
    gchar *parameters = g_strndup((const gchar *)data, length);

    if (final == 'u' && parameters[0] != '?' &&
        parameters[0] != '>' && parameters[0] != '<') {
        handle_csi_u(app, parameters);
    } else if ((final == 'M' || final == 'm') && parameters[0] == '<') {
        handle_mouse(app, parameters, final == 'm');
    } else if (final == 'I') {
        wpe_view_focus_in(app->wpe_view);
    } else if (final == 'O') {
        wpe_view_focus_out(app->wpe_view);
    } else if (final == '~') {
        handle_tilde_key(app, parameters);
    } else {
        handle_legacy_csi(app, parameters, final);
    }

    g_free(parameters);
}

static void handle_raw_codepoint(MuxApp *app, gunichar codepoint)
{
    if (app->url_mode) {
        url_append_codepoint(app, codepoint);
        return;
    }

    guint keyval = unicode_keysym(codepoint);
    emit_keyboard_event(
        app,
        WPE_EVENT_KEYBOARD_KEY_DOWN,
        evdev_code_for_key(codepoint),
        keyval,
        0);
    emit_keyboard_event(
        app,
        WPE_EVENT_KEYBOARD_KEY_UP,
        evdev_code_for_key(codepoint),
        keyval,
        0);
}

static gboolean parse_one_input(MuxApp *app)
{
    GByteArray *input = app->input;
    if (!input->len)
        return FALSE;

    if (input->data[0] == 0x1b) {
        if (input->len == 1)
            return FALSE;

        if (input->data[1] == '[') {
            for (guint i = 2; i < input->len; i++) {
                guint8 byte = input->data[i];
                if (byte < 0x40 || byte > 0x7e)
                    continue;

                handle_csi(app, input->data + 2, i - 2, (gchar)byte);
                g_byte_array_remove_range(input, 0, i + 1);
                return TRUE;
            }
            return FALSE;
        }

        if (input->data[1] == 'O') {
            if (input->len < 3)
                return FALSE;
            handle_legacy_csi(app, "", (gchar)input->data[2]);
            g_byte_array_remove_range(input, 0, 3);
            return TRUE;
        }

        g_byte_array_remove_range(input, 0, 1);
        return TRUE;
    }

    gunichar codepoint = g_utf8_get_char_validated(
        (const gchar *)input->data,
        input->len);
    if (codepoint == (gunichar)-2)
        return FALSE;
    if (codepoint == (gunichar)-1) {
        g_byte_array_remove_range(input, 0, 1);
        return TRUE;
    }

    guint length = (guint)(g_utf8_next_char((const gchar *)input->data) -
        (const gchar *)input->data);
    handle_raw_codepoint(app, codepoint);
    g_byte_array_remove_range(input, 0, length);
    return TRUE;
}

static gboolean stdin_ready(gint fd, GIOCondition condition, gpointer user_data)
{
    MuxApp *app = user_data;
    if (condition & (G_IO_HUP | G_IO_ERR)) {
        request_quit(app);
        return G_SOURCE_REMOVE;
    }

    guint8 buffer[4096];
    while (TRUE) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            g_byte_array_append(app->input, buffer, (guint)count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (count == 0) {
            request_quit(app);
            return G_SOURCE_REMOVE;
        }
        break;
    }

    while (parse_one_input(app))
        ;
    return G_SOURCE_CONTINUE;
}

static gboolean terminal_resized(gpointer user_data)
{
    MuxApp *app = user_data;
    guint previous_width = app->pixel_width;
    guint previous_height = app->content_height;
    guint previous_columns = app->columns;
    guint previous_rows = app->rows;

    read_terminal_geometry(app);
    if (app->pixel_width == previous_width &&
        app->content_height == previous_height &&
        app->columns == previous_columns &&
        app->rows == previous_rows) {
        return G_SOURCE_CONTINUE;
    }

    kitty_delete_image(app);
    terminal_write("\033[2J\033[H");
    draw_url_bar(app);
    wpe_toplevel_resize(
        app->toplevel,
        (int)app->pixel_width,
        (int)app->content_height);
    return G_SOURCE_CONTINUE;
}

static void ipc_command(
    MuxIpc *ipc,
    const gchar *command,
    const gchar *argument,
    gpointer user_data)
{
    (void)ipc;
    MuxApp *app = user_data;

    if (g_strcmp0(command, "OPEN") == 0) {
        gchar *uri = normalize_uri(argument);
        webkit_web_view_load_uri(app->web_view, uri);
        g_free(uri);
    } else if (g_strcmp0(command, "BACK") == 0) {
        if (webkit_web_view_can_go_back(app->web_view))
            webkit_web_view_go_back(app->web_view);
    } else if (g_strcmp0(command, "FORWARD") == 0) {
        if (webkit_web_view_can_go_forward(app->web_view))
            webkit_web_view_go_forward(app->web_view);
    } else if (g_strcmp0(command, "RELOAD") == 0) {
        webkit_web_view_reload(app->web_view);
    } else if (g_strcmp0(command, "QUIT") == 0) {
        request_quit(app);
    }
}

static void uri_changed(WebKitWebView *web_view, GParamSpec *spec, gpointer user_data)
{
    (void)spec;
    MuxApp *app = user_data;
    const gchar *uri = webkit_web_view_get_uri(web_view);
    if (!uri)
        return;

    g_free(app->uri);
    app->uri = g_strdup(uri);
    set_status(app, NULL);
    mux_ipc_state(
        app->ipc,
        app->uri,
        webkit_web_view_get_title(app->web_view));
}

static void title_changed(WebKitWebView *web_view, GParamSpec *spec, gpointer user_data)
{
    (void)spec;
    MuxApp *app = user_data;
    set_terminal_title(webkit_web_view_get_title(web_view));
    mux_ipc_state(
        app->ipc,
        webkit_web_view_get_uri(web_view),
        webkit_web_view_get_title(web_view));
}

static void focus_changed(WPEView *view, GParamSpec *spec, gpointer user_data)
{
    (void)spec;
    MuxApp *app = user_data;
    mux_ipc_focus(app->ipc, wpe_view_get_has_focus(view));
}

static void load_changed(WebKitWebView *web_view, WebKitLoadEvent event, gpointer user_data)
{
    (void)web_view;
    MuxApp *app = user_data;

    if (event == WEBKIT_LOAD_STARTED)
        set_status(app, "loading...");
    else if (event == WEBKIT_LOAD_FINISHED)
        set_status(app, NULL);
}

static gboolean load_failed(
    WebKitWebView *web_view,
    WebKitLoadEvent event,
    const gchar *failing_uri,
    GError *error,
    gpointer user_data)
{
    (void)web_view;
    (void)event;
    (void)failing_uri;
    MuxApp *app = user_data;
    gchar *message = g_strdup_printf("load failed: %s", error->message);
    set_status(app, message);
    g_free(message);
    return FALSE;
}

static gboolean initialize_wpe(MuxApp *app, GError **error)
{
    app->display = wpe_display_headless_new();
    if (!app->display) {
        g_set_error_literal(
            error,
            WPE_DISPLAY_ERROR,
            WPE_DISPLAY_ERROR_CONNECTION_FAILED,
            "could not create the WPE headless display");
        return FALSE;
    }

    WPEDisplayClass *display_class = WPE_DISPLAY_GET_CLASS(app->display);
    display_class->create_view = mux_display_create_view;
    wpe_display_set_available_input_devices(
        app->display,
        WPE_AVAILABLE_INPUT_DEVICE_MOUSE |
            WPE_AVAILABLE_INPUT_DEVICE_KEYBOARD);

    if (!wpe_display_connect(app->display, error))
        return FALSE;

    app->web_view = WEBKIT_WEB_VIEW(g_object_new(
        WEBKIT_TYPE_WEB_VIEW,
        "display",
        app->display,
        NULL));
    if (!app->web_view) {
        g_set_error_literal(
            error,
            WPE_DISPLAY_ERROR,
            WPE_DISPLAY_ERROR_CONNECTION_FAILED,
            "could not create WebKitWebView");
        return FALSE;
    }

    app->wpe_view = webkit_web_view_get_wpe_view(app->web_view);
    if (!app->wpe_view ||
        !g_type_is_a(G_OBJECT_TYPE(app->wpe_view), mux_view_get_type())) {
        g_set_error_literal(
            error,
            WPE_DISPLAY_ERROR,
            WPE_DISPLAY_ERROR_CONNECTION_FAILED,
            "WebKit did not create the Mux WPEView");
        return FALSE;
    }

    MuxView *mux_view = (MuxView *)app->wpe_view;
    mux_view->app = app;
    mux_view->frame_interval_us = G_USEC_PER_SEC / MAX(app->max_fps, 1);

    app->toplevel = wpe_display_create_toplevel(app->display, 1);
    if (!app->toplevel) {
        g_set_error_literal(
            error,
            WPE_DISPLAY_ERROR,
            WPE_DISPLAY_ERROR_CONNECTION_FAILED,
            "could not create the WPE headless toplevel");
        return FALSE;
    }

    wpe_toplevel_resize(
        app->toplevel,
        (int)app->pixel_width,
        (int)app->content_height);
    wpe_view_set_toplevel(app->wpe_view, app->toplevel);
    wpe_view_resized(
        app->wpe_view,
        (int)app->pixel_width,
        (int)app->content_height);
    wpe_view_set_visible(app->wpe_view, TRUE);
    wpe_view_map(app->wpe_view);
    wpe_view_focus_in(app->wpe_view);

    WebKitSettings *settings = webkit_web_view_get_settings(app->web_view);
    webkit_settings_set_enable_developer_extras(settings, TRUE);
    return TRUE;
}

static void destroy_app(MuxApp *app)
{
    terminal_restore(app);
    g_clear_pointer(&app->ipc, mux_ipc_free);
    g_clear_pointer(&app->status, g_free);
    g_clear_pointer(&app->uri, g_free);
    g_clear_pointer(&app->input, g_byte_array_unref);
    if (app->url_input)
        g_string_free(app->url_input, TRUE);
    app->url_input = NULL;
    g_clear_object(&app->web_view);
    g_clear_object(&app->toplevel);
    g_clear_object(&app->display);
    if (app->loop)
        g_main_loop_unref(app->loop);
    app->loop = NULL;
}

int main(int argc, char **argv)
{
    MuxApp app = { 0 };
    app.saved_stdin_flags = -1;
    app.global_bar = g_strcmp0(g_getenv("MUX_GLOBAL_BAR"), "1") == 0;
    app.max_fps = CLAMP(env_uint("MUX_MAX_FPS", MUX_DEFAULT_FPS), 1, 240);
    app.image_id = ((guint32)getpid() & 0x7fffffffu) + 1;
    app.input = g_byte_array_new();
    app.url_input = g_string_new(NULL);
    app.loop = g_main_loop_new(NULL, FALSE);

    global_app = &app;
    atexit(restore_at_exit);
    signal(SIGPIPE, SIG_IGN);
    read_terminal_geometry(&app);

    GError *error = NULL;
    if (!initialize_wpe(&app, &error)) {
        g_printerr("WPE initialization failed: %s\n", error->message);
        g_clear_error(&error);
        destroy_app(&app);
        global_app = NULL;
        return EXIT_FAILURE;
    }

    if (!terminal_enable(&app)) {
        destroy_app(&app);
        global_app = NULL;
        return EXIT_FAILURE;
    }

    g_signal_connect(app.web_view, "notify::uri", G_CALLBACK(uri_changed), &app);
    g_signal_connect(app.web_view, "notify::title", G_CALLBACK(title_changed), &app);
    g_signal_connect(app.web_view, "load-changed", G_CALLBACK(load_changed), &app);
    g_signal_connect(app.web_view, "load-failed", G_CALLBACK(load_failed), &app);
    g_signal_connect(app.wpe_view, "notify::has-focus", G_CALLBACK(focus_changed), &app);

    g_unix_fd_add(
        STDIN_FILENO,
        G_IO_IN | G_IO_HUP | G_IO_ERR,
        stdin_ready,
        &app);
    g_unix_signal_add(SIGWINCH, terminal_resized, &app);
    g_unix_signal_add(SIGINT, request_quit, &app);
    g_unix_signal_add(SIGTERM, request_quit, &app);

    gchar *initial_uri = normalize_uri(argc > 1 ? argv[1] : MUX_DEFAULT_URI);
    const gchar *initial_layer = g_getenv("MUX_LAYER");
    app.ipc = mux_ipc_connect(
        initial_layer ? initial_layer : "main",
        initial_uri,
        ipc_command,
        &app);
    app.uri = g_strdup(initial_uri);
    draw_url_bar(&app);
    webkit_web_view_load_uri(app.web_view, initial_uri);
    g_free(initial_uri);

    g_main_loop_run(app.loop);
    destroy_app(&app);
    global_app = NULL;
    return EXIT_SUCCESS;
}
