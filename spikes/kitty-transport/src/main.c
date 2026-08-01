#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAX_RENDER_PIXELS ((size_t)8294400)
#define INTERACTION_BURST_US ((gint64)1500000)
#define STATUS_INTERVAL_US ((gint64)500000)
#define SHM_REAP_US ((gint64)2000000)
#define INPUT_BUFFER_SIZE ((size_t)8192)
#define MAX_DAMAGE_RECTS ((size_t)16)

typedef struct {
    size_t rows;
    size_t columns;
    size_t pixel_width;
    size_t pixel_height;
    size_t cell_height;
    size_t render_width;
    size_t render_height;
} Viewport;

typedef struct {
    size_t x;
    size_t y;
    size_t width;
    size_t height;
} Rect;

typedef struct {
    Rect values[MAX_DAMAGE_RECTS];
    size_t length;
} DamageList;

typedef struct {
    size_t width;
    size_t height;
    uint8_t *pixels;
} Frame;

typedef struct {
    char *name;
    size_t bytes;
    gint64 created_us;
} PendingShm;

typedef struct {
    uint32_t image_id;
    uint32_t placement_id;
    uint64_t sequence;
    GPtrArray *pending;
} KittyRenderer;

typedef struct {
    bool focused;
    bool quit;
    bool stress;
    bool has_mouse;
    size_t mouse_x;
    size_t mouse_y;
    uint64_t event_count;
    char last_event[192];
} InputState;

typedef struct {
    gint64 sample_start_us;
    uint64_t sample_frames;
    size_t sample_bytes;
    size_t sample_damage;
    size_t sample_pixels;
    double fps;
    double megabytes_per_second;
    double damage_percent;
} Stats;

typedef struct {
    unsigned int fps;
    bool stress;
} Options;

typedef struct {
    bool active;
    struct termios original_termios;
    int original_flags;
    uint32_t image_id;
} TerminalState;

static TerminalState terminal_state;
static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t resize_requested;

static bool write_all(int fd, const void *data, size_t length) {
    const uint8_t *cursor = data;
    while (length > 0) {
        ssize_t written = write(fd, cursor, length);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        cursor += (size_t)written;
        length -= (size_t)written;
    }
    return true;
}

static bool write_text(const char *text) {
    return write_all(STDOUT_FILENO, text, strlen(text));
}

static void handle_signal(int signal_number) {
    if (signal_number == SIGWINCH) {
        resize_requested = 1;
    } else {
        stop_requested = 1;
    }
}

static void install_signal_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGWINCH, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
}

static void terminal_cleanup(void) {
    if (!terminal_state.active) {
        return;
    }

    char command[128];
    int length = g_snprintf(
        command,
        sizeof(command),
        "\033_Ga=d,d=I,i=%" PRIu32 ",q=2\033\\",
        terminal_state.image_id
    );
    if (length > 0) {
        write_all(STDOUT_FILENO, command, (size_t)length);
    }
    write_text(
        "\033[<u\033[?1016l\033[?1006l\033[?1003l"
        "\033[?1004l\033[?25h\033[0m\033[?1049l"
    );
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &terminal_state.original_termios);
    fcntl(STDIN_FILENO, F_SETFL, terminal_state.original_flags);
    terminal_state.active = false;
}

static bool terminal_enter(uint32_t image_id) {
    if (getenv("KITTY_WINDOW_ID") == NULL) {
        g_printerr(
            "mux-kitty-transport: run directly inside Kitty via 'nix run .'\n"
        );
        return false;
    }

    if (tcgetattr(STDIN_FILENO, &terminal_state.original_termios) < 0) {
        g_printerr("tcgetattr: %s\n", g_strerror(errno));
        return false;
    }
    terminal_state.original_flags = fcntl(STDIN_FILENO, F_GETFL);
    if (terminal_state.original_flags < 0) {
        g_printerr("fcntl(F_GETFL): %s\n", g_strerror(errno));
        return false;
    }

    struct termios raw = terminal_state.original_termios;
    raw.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t) ~OPOST;
    raw.c_cflag &= (tcflag_t) ~(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
        g_printerr("tcsetattr: %s\n", g_strerror(errno));
        return false;
    }
    if (fcntl(
            STDIN_FILENO,
            F_SETFL,
            terminal_state.original_flags | O_NONBLOCK
        ) < 0) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &terminal_state.original_termios);
        g_printerr("fcntl(F_SETFL): %s\n", g_strerror(errno));
        return false;
    }

    terminal_state.active = true;
    terminal_state.image_id = image_id;
    atexit(terminal_cleanup);

    return write_text(
        "\033[?1049h\033[2J\033[H\033[?25l"
        "\033[?1004h\033[?1003h\033[?1006h\033[?1016h"
        "\033[>1u\033]2;Mux transport spike\a"
    );
}

static bool checked_multiply(size_t left, size_t right, size_t *result) {
    if (left != 0 && right > SIZE_MAX / left) {
        return false;
    }
    *result = left * right;
    return true;
}

static bool viewport_read(Viewport *viewport) {
    struct winsize size;
    memset(&size, 0, sizeof(size));
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0) {
        g_printerr("TIOCGWINSZ: %s\n", g_strerror(errno));
        return false;
    }

    viewport->rows = MAX((size_t)size.ws_row, (size_t)2);
    viewport->columns = MAX((size_t)size.ws_col, (size_t)1);
    viewport->pixel_width = size.ws_xpixel != 0
        ? (size_t)size.ws_xpixel
        : viewport->columns * 8;
    viewport->pixel_height = size.ws_ypixel != 0
        ? (size_t)size.ws_ypixel
        : viewport->rows * 16;
    viewport->cell_height = MAX(viewport->pixel_height / viewport->rows, (size_t)1);

    size_t content_height = viewport->pixel_height > viewport->cell_height
        ? viewport->pixel_height - viewport->cell_height
        : 1;
    viewport->render_width = viewport->pixel_width;
    viewport->render_height = content_height;

    size_t pixels;
    if (!checked_multiply(
            viewport->render_width,
            viewport->render_height,
            &pixels
        )) {
        pixels = SIZE_MAX;
    }
    if (pixels > MAX_RENDER_PIXELS) {
        double scale = sqrt((double)MAX_RENDER_PIXELS / (double)pixels);
        viewport->render_width = MAX(
            (size_t)((double)viewport->render_width * scale),
            (size_t)1
        );
        viewport->render_height = MAX(
            (size_t)((double)viewport->render_height * scale),
            (size_t)1
        );
    }
    return true;
}

static bool viewport_equal(const Viewport *left, const Viewport *right) {
    return left->rows == right->rows
        && left->columns == right->columns
        && left->pixel_width == right->pixel_width
        && left->pixel_height == right->pixel_height
        && left->render_width == right->render_width
        && left->render_height == right->render_height;
}

static bool viewport_map_mouse(
    const Viewport *viewport,
    const InputState *input,
    size_t *render_x,
    size_t *render_y
) {
    if (!input->has_mouse || input->mouse_y < viewport->cell_height) {
        return false;
    }

    size_t content_height = viewport->pixel_height > viewport->cell_height
        ? viewport->pixel_height - viewport->cell_height
        : 1;
    size_t x = MIN(
        input->mouse_x,
        viewport->pixel_width > 0 ? viewport->pixel_width - 1 : 0
    );
    size_t y = input->mouse_y - viewport->cell_height;
    y = MIN(y, content_height - 1);
    *render_x = x * viewport->render_width / MAX(viewport->pixel_width, (size_t)1);
    *render_y = y * viewport->render_height / content_height;
    *render_x = MIN(*render_x, viewport->render_width - 1);
    *render_y = MIN(*render_y, viewport->render_height - 1);
    return true;
}

static size_t rect_right(Rect rect) {
    return rect.x + rect.width;
}

static size_t rect_bottom(Rect rect) {
    return rect.y + rect.height;
}

static size_t rect_area(Rect rect) {
    size_t area;
    return checked_multiply(rect.width, rect.height, &area) ? area : SIZE_MAX;
}

static bool rect_contains(Rect rect, size_t x, size_t y) {
    return x >= rect.x
        && x < rect_right(rect)
        && y >= rect.y
        && y < rect_bottom(rect);
}

static Rect rect_union(Rect left, Rect right) {
    size_t x = MIN(left.x, right.x);
    size_t y = MIN(left.y, right.y);
    size_t edge = MAX(rect_right(left), rect_right(right));
    size_t bottom = MAX(rect_bottom(left), rect_bottom(right));
    return (Rect) {
        .x = x,
        .y = y,
        .width = edge - x,
        .height = bottom - y,
    };
}

static bool rect_near(Rect left, Rect right) {
    const size_t margin = 4;
    return left.x <= rect_right(right) + margin
        && right.x <= rect_right(left) + margin
        && left.y <= rect_bottom(right) + margin
        && right.y <= rect_bottom(left) + margin;
}

static Rect rect_expand(
    Rect rect,
    size_t amount,
    size_t max_width,
    size_t max_height
) {
    size_t x = rect.x > amount ? rect.x - amount : 0;
    size_t y = rect.y > amount ? rect.y - amount : 0;
    size_t right = MIN(rect_right(rect) + amount, max_width);
    size_t bottom = MIN(rect_bottom(rect) + amount, max_height);
    return (Rect) {
        .x = x,
        .y = y,
        .width = right - x,
        .height = bottom - y,
    };
}

static Rect moving_rect(size_t width, size_t height, uint64_t tick) {
    size_t box_width = MIN(MAX(width / 5, (size_t)16), width);
    size_t box_height = MIN(MAX(height / 6, (size_t)16), height);
    size_t travel = width - box_width;
    size_t period = MAX(travel * 2, (size_t)1);
    size_t phase = ((size_t)tick * 6) % period;
    size_t x = phase <= travel ? phase : period - phase;
    size_t vertical_travel = height - box_height;
    size_t y = vertical_travel == 0
        ? 0
        : height / 3 + ((size_t)(tick / 3) % (vertical_travel / 3 + 1));
    y = MIN(y, vertical_travel);
    return (Rect) {
        .x = x,
        .y = y,
        .width = box_width,
        .height = box_height,
    };
}

static Rect scanline_rect(size_t width, size_t height, uint64_t tick) {
    size_t line_height = MIN((size_t)4, height);
    size_t travel = MAX(height - line_height, (size_t)1);
    return (Rect) {
        .x = 0,
        .y = ((size_t)tick * 7) % travel,
        .width = width,
        .height = line_height,
    };
}

static Rect pointer_rect(
    size_t width,
    size_t height,
    size_t x,
    size_t y
) {
    Rect point = {
        .x = MIN(x, width - 1),
        .y = MIN(y, height - 1),
        .width = 1,
        .height = 1,
    };
    return rect_expand(point, 13, width, height);
}

static void damage_add(DamageList *damage, Rect rect) {
    if (rect.width == 0 || rect.height == 0) {
        return;
    }
    if (damage->length < MAX_DAMAGE_RECTS) {
        damage->values[damage->length++] = rect;
    } else {
        damage->values[0] = rect_union(damage->values[0], rect);
    }
}

static void damage_coalesce(DamageList *damage) {
    for (size_t index = 0; index < damage->length; index++) {
        size_t other = index + 1;
        while (other < damage->length) {
            if (rect_near(damage->values[index], damage->values[other])) {
                damage->values[index] = rect_union(
                    damage->values[index],
                    damage->values[other]
                );
                damage->values[other] = damage->values[--damage->length];
                other = index + 1;
            } else {
                other++;
            }
        }
    }

    if (damage->length > 8) {
        Rect combined = damage->values[0];
        for (size_t index = 1; index < damage->length; index++) {
            combined = rect_union(combined, damage->values[index]);
        }
        damage->values[0] = combined;
        damage->length = 1;
    }
}

static bool frame_init(Frame *frame, size_t width, size_t height) {
    size_t pixels;
    size_t bytes;
    if (!checked_multiply(width, height, &pixels)
        || !checked_multiply(pixels, (size_t)4, &bytes)) {
        g_printerr("frame dimensions overflow\n");
        return false;
    }

    frame->pixels = g_try_malloc0(bytes);
    if (frame->pixels == NULL) {
        g_printerr("unable to allocate %zu-byte frame\n", bytes);
        return false;
    }
    frame->width = width;
    frame->height = height;
    return true;
}

static void frame_clear(Frame *frame) {
    g_clear_pointer(&frame->pixels, g_free);
    frame->width = 0;
    frame->height = 0;
}

static void frame_render_rect(
    Frame *frame,
    Rect rect,
    uint64_t tick,
    bool has_mouse,
    size_t mouse_x,
    size_t mouse_y
) {
    Rect moving = moving_rect(frame->width, frame->height, tick);
    Rect scanline = scanline_rect(frame->width, frame->height, tick);

    for (size_t y = rect.y; y < rect_bottom(rect); y++) {
        for (size_t x = rect.x; x < rect_right(rect); x++) {
            uint8_t checker = (uint8_t)(((x / 32) + (y / 32)) & 1);
            uint8_t color[4] = {
                (uint8_t)(7 + checker * 6),
                (uint8_t)(17 + (y * 31) / MAX(frame->height, (size_t)1)
                    + checker * 7),
                (uint8_t)(15 + (x * 25) / MAX(frame->width, (size_t)1)
                    + checker * 4),
                255,
            };

            if (rect_contains(moving, x, y)) {
                size_t local_x = x - moving.x;
                size_t local_y = y - moving.y;
                color[0] = (uint8_t)(60 + local_x * 130 / moving.width);
                color[1] = (uint8_t)(170 + local_y * 60 / moving.height);
                color[2] = 145;
            }
            if (rect_contains(scanline, x, y)) {
                color[0] = 255;
                color[1] = 203;
                color[2] = 102;
            }
            if (has_mouse) {
                size_t dx = x > mouse_x ? x - mouse_x : mouse_x - x;
                size_t dy = y > mouse_y ? y - mouse_y : mouse_y - y;
                if ((dx <= 1 && dy <= 11) || (dy <= 1 && dx <= 11)) {
                    color[0] = 255;
                    color[1] = 107;
                    color[2] = 95;
                }
            }

            size_t offset = (y * frame->width + x) * 4;
            memcpy(frame->pixels + offset, color, sizeof(color));
        }
    }
}

static void frame_render_full(
    Frame *frame,
    uint64_t tick,
    bool has_mouse,
    size_t mouse_x,
    size_t mouse_y
) {
    frame_render_rect(
        frame,
        (Rect) {
            .x = 0,
            .y = 0,
            .width = frame->width,
            .height = frame->height,
        },
        tick,
        has_mouse,
        mouse_x,
        mouse_y
    );
}

static DamageList frame_update(
    Frame *frame,
    uint64_t previous_tick,
    uint64_t tick,
    bool had_mouse,
    size_t previous_mouse_x,
    size_t previous_mouse_y,
    bool has_mouse,
    size_t mouse_x,
    size_t mouse_y
) {
    DamageList damage = {0};
    damage_add(
        &damage,
        rect_expand(
            rect_union(
                moving_rect(frame->width, frame->height, previous_tick),
                moving_rect(frame->width, frame->height, tick)
            ),
            2,
            frame->width,
            frame->height
        )
    );
    damage_add(
        &damage,
        scanline_rect(frame->width, frame->height, previous_tick)
    );
    damage_add(&damage, scanline_rect(frame->width, frame->height, tick));
    if (had_mouse) {
        damage_add(
            &damage,
            pointer_rect(
                frame->width,
                frame->height,
                previous_mouse_x,
                previous_mouse_y
            )
        );
    }
    if (has_mouse) {
        damage_add(
            &damage,
            pointer_rect(frame->width, frame->height, mouse_x, mouse_y)
        );
    }
    damage_coalesce(&damage);

    size_t total_area = frame->width * frame->height;
    size_t damage_area = 0;
    for (size_t index = 0; index < damage.length; index++) {
        damage_area += rect_area(damage.values[index]);
    }
    if (damage_area * 100 > total_area * 60) {
        damage.values[0] = (Rect) {
            .x = 0,
            .y = 0,
            .width = frame->width,
            .height = frame->height,
        };
        damage.length = 1;
    }

    for (size_t index = 0; index < damage.length; index++) {
        frame_render_rect(
            frame,
            damage.values[index],
            tick,
            has_mouse,
            mouse_x,
            mouse_y
        );
    }
    return damage;
}

static void pending_shm_free(gpointer data) {
    PendingShm *shared = data;
    if (shared == NULL) {
        return;
    }
    shm_unlink(shared->name);
    g_free(shared->name);
    g_free(shared);
}

static PendingShm *shared_memory_from_frame(
    const Frame *frame,
    Rect rect,
    uint64_t sequence
) {
    size_t pixels;
    size_t bytes;
    if (!checked_multiply(rect.width, rect.height, &pixels)
        || !checked_multiply(pixels, (size_t)4, &bytes)
        || bytes == 0) {
        g_printerr("invalid shared-memory rectangle\n");
        return NULL;
    }

    char name[192];
    g_snprintf(
        name,
        sizeof(name),
        "/tty-graphics-protocol-mux-%ld-%" PRIu64 "-%" G_GINT64_FORMAT,
        (long)getpid(),
        sequence,
        g_get_monotonic_time()
    );

    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        g_printerr("shm_open: %s\n", g_strerror(errno));
        return NULL;
    }
    if (ftruncate(fd, (off_t)bytes) < 0) {
        g_printerr("ftruncate: %s\n", g_strerror(errno));
        close(fd);
        shm_unlink(name);
        return NULL;
    }

    uint8_t *mapping = mmap(
        NULL,
        bytes,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        fd,
        0
    );
    if (mapping == MAP_FAILED) {
        g_printerr("mmap: %s\n", g_strerror(errno));
        close(fd);
        shm_unlink(name);
        return NULL;
    }

    size_t row_bytes = rect.width * 4;
    for (size_t row = 0; row < rect.height; row++) {
        size_t source_offset =
            ((rect.y + row) * frame->width + rect.x) * 4;
        memcpy(
            mapping + row * row_bytes,
            frame->pixels + source_offset,
            row_bytes
        );
    }
    munmap(mapping, bytes);
    close(fd);

    PendingShm *shared = g_new0(PendingShm, 1);
    shared->name = g_strdup(name);
    shared->bytes = bytes;
    shared->created_us = g_get_monotonic_time();
    return shared;
}

static void renderer_init(KittyRenderer *renderer, uint32_t image_id) {
    renderer->image_id = image_id;
    renderer->placement_id = 1;
    renderer->sequence = 0;
    renderer->pending = g_ptr_array_new_with_free_func(pending_shm_free);
}

static void renderer_clear(KittyRenderer *renderer) {
    g_clear_pointer(&renderer->pending, g_ptr_array_unref);
}

static PendingShm *renderer_create_shm(
    KittyRenderer *renderer,
    const Frame *frame,
    Rect rect
) {
    renderer->sequence++;
    return shared_memory_from_frame(frame, rect, renderer->sequence);
}

static bool renderer_send_full(
    KittyRenderer *renderer,
    const Frame *frame,
    const Viewport *viewport,
    size_t *bytes_sent
) {
    Rect full = {
        .x = 0,
        .y = 0,
        .width = frame->width,
        .height = frame->height,
    };
    PendingShm *shared = renderer_create_shm(renderer, frame, full);
    if (shared == NULL) {
        return false;
    }

    char *payload = g_base64_encode(
        (const guchar *)shared->name,
        strlen(shared->name)
    );
    char *command = g_strdup_printf(
        "\033[2;1H\033_Ga=T,f=32,t=s,s=%zu,v=%zu,S=%zu,"
        "i=%" PRIu32 ",p=%" PRIu32 ",c=%zu,r=%zu,C=1,q=2;%s\033\\",
        frame->width,
        frame->height,
        shared->bytes,
        renderer->image_id,
        renderer->placement_id,
        viewport->columns,
        MAX(viewport->rows - 1, (size_t)1),
        payload
    );
    bool success = write_text(command);
    g_free(command);
    g_free(payload);
    if (!success) {
        pending_shm_free(shared);
        return false;
    }

    *bytes_sent = shared->bytes;
    g_ptr_array_add(renderer->pending, shared);
    return true;
}

static bool renderer_send_damage(
    KittyRenderer *renderer,
    const Frame *frame,
    const DamageList *damage,
    size_t *bytes_sent
) {
    *bytes_sent = 0;
    for (size_t index = 0; index < damage->length; index++) {
        Rect rect = damage->values[index];
        PendingShm *shared = renderer_create_shm(renderer, frame, rect);
        if (shared == NULL) {
            return false;
        }

        char *payload = g_base64_encode(
            (const guchar *)shared->name,
            strlen(shared->name)
        );
        char *command = g_strdup_printf(
            "\033_Ga=f,f=32,t=s,s=%zu,v=%zu,S=%zu,"
            "i=%" PRIu32 ",r=1,x=%zu,y=%zu,X=1,q=2;%s\033\\",
            rect.width,
            rect.height,
            shared->bytes,
            renderer->image_id,
            rect.x,
            rect.y,
            payload
        );
        bool success = write_text(command);
        g_free(command);
        g_free(payload);
        if (!success) {
            pending_shm_free(shared);
            return false;
        }

        *bytes_sent += shared->bytes;
        g_ptr_array_add(renderer->pending, shared);
    }
    return true;
}

static void renderer_reap(KittyRenderer *renderer, gint64 now_us) {
    size_t index = 0;
    while (index < renderer->pending->len) {
        PendingShm *shared = g_ptr_array_index(renderer->pending, index);
        if (now_us - shared->created_us >= SHM_REAP_US) {
            g_ptr_array_remove_index_fast(renderer->pending, (guint)index);
        } else {
            index++;
        }
    }
}

static void input_event(InputState *input, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    g_vsnprintf(
        input->last_event,
        sizeof(input->last_event),
        format,
        arguments
    );
    va_end(arguments);
    input->event_count++;
}

static bool bytes_start_with(
    const GByteArray *bytes,
    const uint8_t *prefix,
    size_t length
) {
    return bytes->len >= length && memcmp(bytes->data, prefix, length) == 0;
}

static void visible_bytes(
    const uint8_t *bytes,
    size_t length,
    char *output,
    size_t output_size
) {
    size_t cursor = 0;
    for (size_t index = 0; index < length && cursor + 1 < output_size; index++) {
        int written;
        if (bytes[index] == 0x1b) {
            written = g_snprintf(
                output + cursor,
                output_size - cursor,
                "<ESC>"
            );
        } else if (g_ascii_isgraph(bytes[index]) || bytes[index] == ' ') {
            output[cursor] = (char)bytes[index];
            output[cursor + 1] = '\0';
            written = 1;
        } else {
            written = g_snprintf(
                output + cursor,
                output_size - cursor,
                "<%02x>",
                bytes[index]
            );
        }
        if (written < 0) {
            break;
        }
        cursor += MIN((size_t)written, output_size - cursor - 1);
    }
    output[cursor] = '\0';
}

static void parse_mouse(
    const uint8_t *sequence,
    size_t length,
    InputState *input
) {
    if (length < 7 || length - 4 >= 128) {
        return;
    }

    char parameters[128];
    memcpy(parameters, sequence + 3, length - 4);
    parameters[length - 4] = '\0';
    unsigned int button;
    size_t x;
    size_t y;
    if (sscanf(parameters, "%u;%zu;%zu", &button, &x, &y) != 3) {
        return;
    }

    input->has_mouse = true;
    input->mouse_x = x > 0 ? x - 1 : 0;
    input->mouse_y = y > 0 ? y - 1 : 0;

    const char *kind;
    if (sequence[length - 1] == 'm') {
        kind = "release";
    } else if ((button & 64U) != 0) {
        kind = (button & 1U) == 0 ? "wheel-up" : "wheel-down";
    } else if ((button & 32U) != 0) {
        kind = "motion";
    } else {
        kind = "press";
    }
    input_event(
        input,
        "mouse %s b=%u px=%zu,%zu",
        kind,
        button,
        input->mouse_x,
        input->mouse_y
    );
}

static void input_parse(GByteArray *pending, InputState *input) {
    static const uint8_t focus_in[] = "\033[I";
    static const uint8_t focus_out[] = "\033[O";
    static const uint8_t mouse_prefix[] = "\033[<";
    static const uint8_t graphics_prefix[] = "\033_G";

    while (pending->len > 0) {
        if (pending->data[0] != 0x1b) {
            uint8_t byte = pending->data[0];
            g_byte_array_remove_index(pending, 0);
            if (byte == 'q' || byte == 0x03) {
                input->quit = true;
                input_event(input, "quit");
            } else if (byte == 's') {
                input->stress = !input->stress;
                input_event(
                    input,
                    "%s mode enabled",
                    input->stress ? "stress" : "adaptive"
                );
            } else if (g_ascii_isgraph(byte) || byte == ' ') {
                input_event(input, "text '%c'", byte);
            } else {
                input_event(input, "byte 0x%02x", byte);
            }
            continue;
        }

        if (bytes_start_with(pending, focus_in, sizeof(focus_in) - 1)) {
            g_byte_array_remove_range(pending, 0, sizeof(focus_in) - 1);
            input->focused = true;
            input_event(input, "focus in");
            continue;
        }
        if (bytes_start_with(pending, focus_out, sizeof(focus_out) - 1)) {
            g_byte_array_remove_range(pending, 0, sizeof(focus_out) - 1);
            input->focused = false;
            input_event(input, "focus out");
            continue;
        }
        if (bytes_start_with(
                pending,
                mouse_prefix,
                sizeof(mouse_prefix) - 1
            )) {
            size_t end = 3;
            while (end < pending->len
                && pending->data[end] != 'M'
                && pending->data[end] != 'm') {
                end++;
            }
            if (end == pending->len) {
                return;
            }
            parse_mouse(pending->data, end + 1, input);
            g_byte_array_remove_range(pending, 0, (guint)(end + 1));
            continue;
        }
        if (bytes_start_with(
                pending,
                graphics_prefix,
                sizeof(graphics_prefix) - 1
            )) {
            size_t end = 0;
            bool found = false;
            while (end + 1 < pending->len) {
                if (pending->data[end] == 0x1b
                    && pending->data[end + 1] == '\\') {
                    found = true;
                    break;
                }
                end++;
            }
            if (!found) {
                return;
            }
            g_byte_array_remove_range(pending, 0, (guint)(end + 2));
            continue;
        }
        if (pending->len >= 2 && pending->data[1] == '[') {
            size_t end = 2;
            while (end < pending->len
                && (pending->data[end] < 0x40
                    || pending->data[end] > 0x7e)) {
                end++;
            }
            if (end == pending->len) {
                return;
            }

            static const uint8_t ctrl_c[] = "\033[99;5u";
            if (end + 1 == sizeof(ctrl_c) - 1
                && memcmp(pending->data, ctrl_c, sizeof(ctrl_c) - 1) == 0) {
                input->quit = true;
                input_event(input, "ctrl-c");
            } else {
                char visible[128];
                visible_bytes(pending->data, end + 1, visible, sizeof(visible));
                input_event(input, "key %s", visible);
            }
            g_byte_array_remove_range(pending, 0, (guint)(end + 1));
            continue;
        }
        if (pending->len == 1) {
            return;
        }

        char visible[64];
        visible_bytes(pending->data, 2, visible, sizeof(visible));
        input_event(input, "escape %s", visible);
        g_byte_array_remove_range(pending, 0, 2);
    }
}

static bool input_read(GByteArray *pending, InputState *input) {
    uint8_t buffer[INPUT_BUFFER_SIZE];
    for (;;) {
        ssize_t length = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (length > 0) {
            g_byte_array_append(pending, buffer, (guint)length);
            continue;
        }
        if (length == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        g_printerr("read: %s\n", g_strerror(errno));
        return false;
    }
    input_parse(pending, input);
    return true;
}

static void stats_init(Stats *stats, gint64 now_us) {
    memset(stats, 0, sizeof(*stats));
    stats->sample_start_us = now_us;
}

static void stats_record(
    Stats *stats,
    size_t bytes,
    size_t damage,
    size_t pixels
) {
    stats->sample_frames++;
    stats->sample_bytes += bytes;
    stats->sample_damage += damage;
    stats->sample_pixels += pixels;
}

static bool stats_refresh(Stats *stats, gint64 now_us) {
    gint64 elapsed_us = now_us - stats->sample_start_us;
    if (elapsed_us < STATUS_INTERVAL_US) {
        return false;
    }

    double seconds = MAX((double)elapsed_us / 1000000.0, 0.001);
    stats->fps = (double)stats->sample_frames / seconds;
    stats->megabytes_per_second =
        (double)stats->sample_bytes / seconds / 1000000.0;
    stats->damage_percent = stats->sample_pixels == 0
        ? 0.0
        : (double)stats->sample_damage
            / (double)stats->sample_pixels
            * 100.0;
    stats->sample_start_us = now_us;
    stats->sample_frames = 0;
    stats->sample_bytes = 0;
    stats->sample_damage = 0;
    stats->sample_pixels = 0;
    return true;
}

static bool write_status(
    const Viewport *viewport,
    const Stats *stats,
    const InputState *input
) {
    char status[1024];
    g_snprintf(
        status,
        sizeof(status),
        " MUX/KITTY  %4.1f fps  %5.1f MB/s  damage %4.1f%%  "
        "%zux%zu px  %s  %s  %s  q:quit s:mode ",
        stats->fps,
        stats->megabytes_per_second,
        stats->damage_percent,
        viewport->pixel_width,
        viewport->pixel_height,
        input->stress ? "stress" : "adaptive",
        input->focused ? "focused" : "hidden",
        input->last_event
    );
    size_t maximum = viewport->columns > 0 ? viewport->columns - 1 : 0;
    if (strlen(status) > maximum) {
        status[maximum] = '\0';
    }

    return write_text("\033[1;1H\033[2K\033[1;38;2;76;215;165m")
        && write_text(status)
        && write_text("\033[0m");
}

static bool parse_options(int argc, char **argv, Options *options) {
    options->fps = 60;
    options->stress = false;

    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--stress") == 0) {
            options->stress = true;
        } else if (strcmp(argv[index], "--fps") == 0) {
            if (++index >= argc) {
                g_printerr("--fps requires a value\n");
                return false;
            }
            char *end = NULL;
            errno = 0;
            unsigned long value = strtoul(argv[index], &end, 10);
            if (errno != 0 || end == argv[index] || *end != '\0'
                || value < 1 || value > 120) {
                g_printerr("--fps must be between 1 and 120\n");
                return false;
            }
            options->fps = (unsigned int)value;
        } else if (strcmp(argv[index], "--help") == 0
            || strcmp(argv[index], "-h") == 0) {
            g_print(
                "Mux Kitty transport spike\n\n"
                "q                  quit\n"
                "s                  toggle adaptive/stress mode\n"
                "Ctrl-Shift-Enter   open another split\n"
                "Ctrl-Shift-T       open another layer\n"
                "--fps N            interaction ceiling (1-120)\n"
                "--stress           animate continuously\n"
            );
            exit(EXIT_SUCCESS);
        } else {
            g_printerr("unknown argument: %s\n", argv[index]);
            return false;
        }
    }
    return true;
}

static int poll_timeout_ms(
    bool should_animate,
    gint64 now_us,
    gint64 next_frame_us
) {
    if (!should_animate) {
        return 500;
    }
    gint64 remaining_us = MAX(next_frame_us - now_us, (gint64)0);
    return (int)MIN((remaining_us + 999) / 1000, (gint64)16);
}

int main(int argc, char **argv) {
    int result = EXIT_FAILURE;
    Options options;
    Frame frame = {0};
    KittyRenderer renderer = {0};
    GByteArray *pending_input = NULL;

    if (!parse_options(argc, argv, &options)) {
        return EXIT_FAILURE;
    }
    install_signal_handlers();

    uint32_t image_id =
        UINT32_C(0x4d580000) | ((uint32_t)getpid() & UINT32_C(0xffff));
    if (!terminal_enter(image_id)) {
        terminal_cleanup();
        return EXIT_FAILURE;
    }

    Viewport viewport;
    if (!viewport_read(&viewport)
        || !frame_init(
            &frame,
            viewport.render_width,
            viewport.render_height
        )) {
        goto cleanup;
    }

    renderer_init(&renderer, image_id);
    pending_input = g_byte_array_new();
    InputState input = {
        .focused = true,
        .stress = options.stress,
    };
    g_strlcpy(
        input.last_event,
        "startup interaction burst",
        sizeof(input.last_event)
    );

    size_t mouse_x = 0;
    size_t mouse_y = 0;
    bool has_mouse = viewport_map_mouse(
        &viewport,
        &input,
        &mouse_x,
        &mouse_y
    );
    size_t previous_mouse_x = mouse_x;
    size_t previous_mouse_y = mouse_y;
    bool had_mouse = has_mouse;
    uint64_t tick = 0;
    gint64 now_us = g_get_monotonic_time();
    gint64 active_until_us = now_us + INTERACTION_BURST_US;
    gint64 next_frame_us = now_us;
    uint64_t observed_events = 0;
    uint64_t status_events = 0;
    Stats stats;
    stats_init(&stats, now_us);

    frame_render_full(&frame, tick, has_mouse, mouse_x, mouse_y);
    size_t bytes_sent;
    if (!renderer_send_full(
            &renderer,
            &frame,
            &viewport,
            &bytes_sent
        )) {
        goto cleanup;
    }
    size_t frame_pixels = frame.width * frame.height;
    stats_record(&stats, bytes_sent, frame_pixels, frame_pixels);
    if (!write_status(&viewport, &stats, &input)) {
        goto cleanup;
    }

    while (!stop_requested && !input.quit) {
        now_us = g_get_monotonic_time();
        bool should_animate =
            input.focused && (input.stress || now_us < active_until_us);
        struct pollfd descriptor = {
            .fd = STDIN_FILENO,
            .events = POLLIN,
        };
        int poll_result = poll(
            &descriptor,
            1,
            poll_timeout_ms(should_animate, now_us, next_frame_us)
        );
        if (poll_result < 0 && errno != EINTR) {
            g_printerr("poll: %s\n", g_strerror(errno));
            goto cleanup;
        }
        if ((descriptor.revents & POLLIN) != 0
            && !input_read(pending_input, &input)) {
            goto cleanup;
        }
        if (input.event_count != observed_events) {
            observed_events = input.event_count;
            active_until_us = g_get_monotonic_time() + INTERACTION_BURST_US;
        }
        if (input.quit) {
            break;
        }

        bool resized = false;
        Viewport latest_viewport;
        if (resize_requested || poll_result == 0) {
            resize_requested = 0;
            if (!viewport_read(&latest_viewport)) {
                goto cleanup;
            }
            if (!viewport_equal(&viewport, &latest_viewport)) {
                viewport = latest_viewport;
                frame_clear(&frame);
                if (!frame_init(
                        &frame,
                        viewport.render_width,
                        viewport.render_height
                    )) {
                    goto cleanup;
                }
                tick++;
                has_mouse = viewport_map_mouse(
                    &viewport,
                    &input,
                    &mouse_x,
                    &mouse_y
                );
                had_mouse = has_mouse;
                previous_mouse_x = mouse_x;
                previous_mouse_y = mouse_y;
                frame_render_full(
                    &frame,
                    tick,
                    has_mouse,
                    mouse_x,
                    mouse_y
                );
                if (!renderer_send_full(
                        &renderer,
                        &frame,
                        &viewport,
                        &bytes_sent
                    )) {
                    goto cleanup;
                }
                frame_pixels = frame.width * frame.height;
                stats_record(
                    &stats,
                    bytes_sent,
                    frame_pixels,
                    frame_pixels
                );
                resized = true;
                next_frame_us = g_get_monotonic_time()
                    + 1000000 / options.fps;
            }
        }

        now_us = g_get_monotonic_time();
        should_animate =
            input.focused && (input.stress || now_us < active_until_us);
        if (!resized && should_animate && now_us >= next_frame_us) {
            has_mouse = viewport_map_mouse(
                &viewport,
                &input,
                &mouse_x,
                &mouse_y
            );
            uint64_t next_tick = tick + 1;
            DamageList damage = frame_update(
                &frame,
                tick,
                next_tick,
                had_mouse,
                previous_mouse_x,
                previous_mouse_y,
                has_mouse,
                mouse_x,
                mouse_y
            );
            tick = next_tick;
            had_mouse = has_mouse;
            previous_mouse_x = mouse_x;
            previous_mouse_y = mouse_y;

            if (!renderer_send_damage(
                    &renderer,
                    &frame,
                    &damage,
                    &bytes_sent
                )) {
                goto cleanup;
            }
            size_t damage_pixels = 0;
            for (size_t index = 0; index < damage.length; index++) {
                damage_pixels += rect_area(damage.values[index]);
            }
            stats_record(
                &stats,
                bytes_sent,
                damage_pixels,
                frame.width * frame.height
            );
            next_frame_us = now_us + 1000000 / options.fps;
        }

        bool refreshed = stats_refresh(&stats, now_us);
        if (refreshed || status_events != input.event_count || resized) {
            status_events = input.event_count;
            if (!write_status(&viewport, &stats, &input)) {
                goto cleanup;
            }
        }
        renderer_reap(&renderer, now_us);
    }

    result = EXIT_SUCCESS;

cleanup:
    g_clear_pointer(&pending_input, g_byte_array_unref);
    renderer_clear(&renderer);
    frame_clear(&frame);
    terminal_cleanup();
    return result;
}
