#include "mux-pane-overlay.h"

#include <string.h>

typedef struct {
    MuxUiRequest *request;
    GString *input;
    GString *secondary_input;
    gint64 expires_at_us;
    guint selected_choice;
    gboolean auth_password;
} PendingPrompt;

struct _MuxPaneOverlay {
    GQueue pending;
};

static void
pending_prompt_free(PendingPrompt *pending)
{
    if (!pending)
        return;
    mux_ui_request_free(pending->request);
    g_string_free(pending->input, TRUE);
    if (pending->secondary_input) {
        volatile gchar *cursor =
            (volatile gchar *)pending->secondary_input->str;
        gsize length = pending->secondary_input->len;

        while (length--)
            *cursor++ = '\0';
        g_string_free(pending->secondary_input, TRUE);
    }
    g_free(pending);
}

static gboolean
choice_selectable(const MuxUiChoice *choice)
{
    return choice &&
           !(choice->flags &
             (MUX_UI_CHOICE_FLAG_DISABLED | MUX_UI_CHOICE_FLAG_SEPARATOR));
}

static guint
first_selectable_choice(const MuxUiRequest *request)
{
    guint i;

    if (!request->choices)
        return 0;
    for (i = 0; i < request->choices->len; i++) {
        const MuxUiChoice *choice = g_ptr_array_index(request->choices, i);

        if ((choice->flags & MUX_UI_CHOICE_FLAG_SELECTED) &&
            choice_selectable(choice))
            return i;
    }
    for (i = 0; i < request->choices->len; i++) {
        if (choice_selectable(g_ptr_array_index(request->choices, i)))
            return i;
    }
    return 0;
}

static PendingPrompt *
pending_prompt_new(MuxUiRequest *request)
{
    PendingPrompt *pending = g_new0(PendingPrompt, 1);
    guint32 deadline_ms = request->deadline_ms ? request->deadline_ms : 120000;

    pending->request = request;
    pending->input = g_string_new(
        request->kind == MUX_UI_REQUEST_DIALOG_PROMPT ||
                request->kind == MUX_UI_REQUEST_DOWNLOAD_DESTINATION ||
                request->kind == MUX_UI_REQUEST_AUTHENTICATION
            ? request->default_value
            : NULL);
    pending->secondary_input = g_string_new(NULL);
    pending->expires_at_us =
        g_get_monotonic_time() + ((gint64)deadline_ms * 1000);
    pending->selected_choice = first_selectable_choice(request);
    return pending;
}

static PendingPrompt *
active_prompt(const MuxPaneOverlay *overlay)
{
    return overlay ? g_queue_peek_head((GQueue *)&overlay->pending) : NULL;
}

MuxPaneOverlay *
mux_pane_overlay_new(void)
{
    MuxPaneOverlay *overlay = g_new0(MuxPaneOverlay, 1);

    g_queue_init(&overlay->pending);
    return overlay;
}

void
mux_pane_overlay_free(MuxPaneOverlay *overlay)
{
    if (!overlay)
        return;
    mux_pane_overlay_clear(overlay);
    g_free(overlay);
}

gboolean
mux_pane_overlay_contains(const MuxPaneOverlay *overlay, guint64 request_id)
{
    GList *link;

    g_return_val_if_fail(overlay, FALSE);
    for (link = overlay->pending.head; link; link = link->next) {
        const PendingPrompt *pending = link->data;

        if (pending->request->request_id == request_id)
            return TRUE;
    }
    return FALSE;
}

gboolean
mux_pane_overlay_push(MuxPaneOverlay *overlay,
                      MuxUiRequest *request,
                      GError **error)
{
    g_return_val_if_fail(overlay, FALSE);
    g_return_val_if_fail(request, FALSE);

    if (!request->request_id) {
        g_set_error_literal(error,
                            MUX_UI_ERROR,
                            MUX_UI_ERROR_INVALID,
                            "overlay request ID must be nonzero");
        return FALSE;
    }
    if (overlay->pending.length >= MUX_PANE_OVERLAY_MAX_PENDING) {
        g_set_error_literal(error,
                            MUX_UI_ERROR,
                            MUX_UI_ERROR_TOO_LARGE,
                            "overlay prompt queue is full");
        return FALSE;
    }
    if (mux_pane_overlay_contains(overlay, request->request_id)) {
        g_set_error_literal(error,
                            MUX_UI_ERROR,
                            MUX_UI_ERROR_INVALID,
                            "duplicate overlay request ID");
        return FALSE;
    }
    g_queue_push_tail(&overlay->pending, pending_prompt_new(request));
    return TRUE;
}

gboolean
mux_pane_overlay_cancel(MuxPaneOverlay *overlay, guint64 request_id)
{
    GList *link;

    g_return_val_if_fail(overlay, FALSE);
    for (link = overlay->pending.head; link; link = link->next) {
        PendingPrompt *pending = link->data;

        if (pending->request->request_id == request_id) {
            g_queue_delete_link(&overlay->pending, link);
            pending_prompt_free(pending);
            return TRUE;
        }
    }
    return FALSE;
}

void
mux_pane_overlay_clear(MuxPaneOverlay *overlay)
{
    g_return_if_fail(overlay);
    g_queue_clear_full(&overlay->pending,
                       (GDestroyNotify)pending_prompt_free);
}

gboolean
mux_pane_overlay_is_active(const MuxPaneOverlay *overlay)
{
    return active_prompt(overlay) != NULL;
}

const MuxUiRequest *
mux_pane_overlay_active(const MuxPaneOverlay *overlay)
{
    PendingPrompt *pending = active_prompt(overlay);

    return pending ? pending->request : NULL;
}

guint
mux_pane_overlay_pending_count(const MuxPaneOverlay *overlay)
{
    g_return_val_if_fail(overlay, 0);
    return overlay->pending.length;
}

static MuxUiResponse *
complete_active(MuxPaneOverlay *overlay,
                MuxUiAction action,
                const gchar *value)
{
    PendingPrompt *pending = g_queue_pop_head(&overlay->pending);
    MuxUiResponse *response;

    if (!pending)
        return NULL;
    response = mux_ui_response_new(pending->request->request_id, action);
    response->value = g_strdup(value);
    pending_prompt_free(pending);
    return response;
}

static void
backspace_utf8(GString *input)
{
    gchar *previous;

    if (!input->len)
        return;
    previous = g_utf8_find_prev_char(input->str, input->str + input->len);
    if (previous)
        g_string_truncate(input, previous - input->str);
}

static void
append_unichar(GString *input, gunichar character)
{
    gchar encoded[6];
    gint length;

    if (!g_unichar_validate(character) || !g_unichar_isprint(character))
        return;
    length = g_unichar_to_utf8(character, encoded);
    if (input->len + length > MUX_UI_MAX_VALUE)
        return;
    g_string_append_len(input, encoded, length);
}

static GString *
active_authentication_input(PendingPrompt *pending)
{
    return pending->auth_password ? pending->secondary_input
                                  : pending->input;
}

static void
append_authentication_unichar(PendingPrompt *pending, gunichar character)
{
    GString *input = active_authentication_input(pending);
    gchar encoded[6];
    gint length;

    if (!g_unichar_validate(character) || !g_unichar_isprint(character))
        return;
    length = g_unichar_to_utf8(character, encoded);
    if (pending->input->len + pending->secondary_input->len + length >
        6000U)
        return;
    g_string_append_len(input, encoded, length);
}

static gchar *
authentication_value(const PendingPrompt *pending)
{
    g_autofree gchar *username = g_base64_encode(
        (const guchar *)pending->input->str, pending->input->len);
    g_autofree gchar *password = g_base64_encode(
        (const guchar *)pending->secondary_input->str,
        pending->secondary_input->len);
    gchar *value = g_strdup_printf("v1:%s:%s", username, password);

    if (strlen(value) > MUX_UI_MAX_VALUE) {
        g_free(value);
        return NULL;
    }
    return value;
}

static void
move_choice(PendingPrompt *pending, gint direction)
{
    guint count;
    guint candidate;
    guint attempts;

    if (!pending->request->choices ||
        !(count = pending->request->choices->len))
        return;
    candidate = pending->selected_choice;
    for (attempts = 0; attempts < count; attempts++) {
        if (direction > 0)
            candidate = (candidate + 1) % count;
        else
            candidate = candidate ? candidate - 1 : count - 1;
        if (choice_selectable(
                g_ptr_array_index(pending->request->choices, candidate))) {
            pending->selected_choice = candidate;
            return;
        }
    }
}

static gchar *
selected_choice_value(const PendingPrompt *pending)
{
    const MuxUiChoice *choice;

    if (!pending->request->choices ||
        pending->selected_choice >= pending->request->choices->len)
        return NULL;
    choice = g_ptr_array_index(pending->request->choices,
                               pending->selected_choice);
    if (!choice_selectable(choice))
        return NULL;
    return g_strdup_printf("%u", choice->id);
}

gboolean
mux_pane_overlay_handle_key(MuxPaneOverlay *overlay,
                            MuxPaneOverlayKey key,
                            gunichar text,
                            MuxUiResponse **response_out)
{
    PendingPrompt *pending;
    MuxUiAction action = 0;
    const gchar *value = NULL;
    g_autofree gchar *owned_value = NULL;

    g_return_val_if_fail(overlay, FALSE);
    g_return_val_if_fail(response_out, FALSE);
    *response_out = NULL;
    pending = active_prompt(overlay);
    if (!pending)
        return FALSE;

    switch (pending->request->kind) {
    case MUX_UI_REQUEST_DIALOG_ALERT:
        if (key == MUX_PANE_OVERLAY_KEY_ENTER ||
            key == MUX_PANE_OVERLAY_KEY_ESCAPE ||
            (key == MUX_PANE_OVERLAY_KEY_TEXT && text == ' '))
            action = MUX_UI_ACTION_ACKNOWLEDGE;
        break;
    case MUX_UI_REQUEST_DIALOG_CONFIRM:
        if (key == MUX_PANE_OVERLAY_KEY_ENTER ||
            (key == MUX_PANE_OVERLAY_KEY_TEXT &&
             (text == 'y' || text == 'Y')))
            action = MUX_UI_ACTION_ACCEPT;
        else if (key == MUX_PANE_OVERLAY_KEY_ESCAPE ||
                 (key == MUX_PANE_OVERLAY_KEY_TEXT &&
                  (text == 'n' || text == 'N')))
            action = MUX_UI_ACTION_CANCEL;
        break;
    case MUX_UI_REQUEST_DIALOG_PROMPT:
        if (key == MUX_PANE_OVERLAY_KEY_ENTER) {
            action = MUX_UI_ACTION_SUBMIT;
            value = pending->input->str;
        } else if (key == MUX_PANE_OVERLAY_KEY_ESCAPE) {
            action = MUX_UI_ACTION_CANCEL;
        } else if (key == MUX_PANE_OVERLAY_KEY_BACKSPACE) {
            backspace_utf8(pending->input);
        } else if (key == MUX_PANE_OVERLAY_KEY_TEXT) {
            append_unichar(pending->input, text);
        }
        break;
    case MUX_UI_REQUEST_DIALOG_BEFORE_UNLOAD:
        if (key == MUX_PANE_OVERLAY_KEY_TEXT &&
            (text == 'l' || text == 'L' || text == 'y' || text == 'Y'))
            action = MUX_UI_ACTION_LEAVE;
        else if (key == MUX_PANE_OVERLAY_KEY_ESCAPE ||
                 (key == MUX_PANE_OVERLAY_KEY_TEXT &&
                  (text == 's' || text == 'S' ||
                   text == 'n' || text == 'N')))
            action = MUX_UI_ACTION_STAY;
        break;
    case MUX_UI_REQUEST_PERMISSION:
        if (key == MUX_PANE_OVERLAY_KEY_TEXT && text == 'a')
            action = MUX_UI_ACTION_ALLOW_ONCE;
        else if (key == MUX_PANE_OVERLAY_KEY_TEXT && text == 'A' &&
                 (pending->request->flags &
                  MUX_UI_REQUEST_FLAG_PERSISTENCE_AVAILABLE))
            action = MUX_UI_ACTION_ALLOW_ALWAYS;
        else if (key == MUX_PANE_OVERLAY_KEY_TEXT && text == 'D' &&
                 (pending->request->flags &
                  MUX_UI_REQUEST_FLAG_PERSISTENCE_AVAILABLE))
            action = MUX_UI_ACTION_DENY_ALWAYS;
        else if (key == MUX_PANE_OVERLAY_KEY_ESCAPE ||
                 (key == MUX_PANE_OVERLAY_KEY_TEXT && text == 'd'))
            action = MUX_UI_ACTION_DENY_ONCE;
        break;
    case MUX_UI_REQUEST_CONTEXT_MENU:
    case MUX_UI_REQUEST_OPTION_MENU:
        if (key == MUX_PANE_OVERLAY_KEY_UP)
            move_choice(pending, -1);
        else if (key == MUX_PANE_OVERLAY_KEY_DOWN ||
                 key == MUX_PANE_OVERLAY_KEY_TAB)
            move_choice(pending, 1);
        else if (key == MUX_PANE_OVERLAY_KEY_ESCAPE)
            action = MUX_UI_ACTION_CANCEL;
        else if (key == MUX_PANE_OVERLAY_KEY_ENTER) {
            owned_value = selected_choice_value(pending);
            if (owned_value) {
                action = MUX_UI_ACTION_SELECT;
                value = owned_value;
            }
        }
        break;
    case MUX_UI_REQUEST_DOWNLOAD_DESTINATION:
        if (key == MUX_PANE_OVERLAY_KEY_ENTER) {
            action = MUX_UI_ACTION_SUBMIT;
            value = pending->input->str;
        } else if (key == MUX_PANE_OVERLAY_KEY_ESCAPE) {
            action = MUX_UI_ACTION_CANCEL;
        } else if (key == MUX_PANE_OVERLAY_KEY_BACKSPACE) {
            backspace_utf8(pending->input);
        } else if (key == MUX_PANE_OVERLAY_KEY_TEXT) {
            append_unichar(pending->input, text);
        }
        break;
    case MUX_UI_REQUEST_AUTHENTICATION:
        if (key == MUX_PANE_OVERLAY_KEY_ESCAPE) {
            action = MUX_UI_ACTION_CANCEL;
        } else if (key == MUX_PANE_OVERLAY_KEY_TAB) {
            pending->auth_password = !pending->auth_password;
        } else if (key == MUX_PANE_OVERLAY_KEY_ENTER) {
            if (!pending->auth_password) {
                pending->auth_password = TRUE;
            } else {
                owned_value = authentication_value(pending);
                if (owned_value) {
                    action = MUX_UI_ACTION_SUBMIT;
                    value = owned_value;
                }
            }
        } else if (key == MUX_PANE_OVERLAY_KEY_BACKSPACE) {
            backspace_utf8(active_authentication_input(pending));
        } else if (key == MUX_PANE_OVERLAY_KEY_TEXT) {
            append_authentication_unichar(pending, text);
        }
        break;
    case MUX_UI_REQUEST_FILE_CHOOSER:
        if (key == MUX_PANE_OVERLAY_KEY_ESCAPE)
            action = MUX_UI_ACTION_CANCEL;
        break;
    case MUX_UI_REQUEST_CRASH:
        if (key == MUX_PANE_OVERLAY_KEY_TEXT &&
            (text == 'r' || text == 'R'))
            action = MUX_UI_ACTION_RELOAD;
        else if (key == MUX_PANE_OVERLAY_KEY_ESCAPE ||
                 (key == MUX_PANE_OVERLAY_KEY_TEXT &&
                  (text == 'q' || text == 'Q')))
            action = MUX_UI_ACTION_CLOSE;
        break;
    default:
        action = MUX_UI_ACTION_UNSUPPORTED;
        break;
    }

    if (action)
        *response_out = complete_active(overlay, action, value);
    return TRUE;
}

static MuxUiAction
timeout_action(MuxUiRequestKind kind)
{
    switch (kind) {
    case MUX_UI_REQUEST_DIALOG_ALERT:
        return MUX_UI_ACTION_ACKNOWLEDGE;
    case MUX_UI_REQUEST_DIALOG_BEFORE_UNLOAD:
        return MUX_UI_ACTION_STAY;
    case MUX_UI_REQUEST_PERMISSION:
        return MUX_UI_ACTION_DENY_ONCE;
    case MUX_UI_REQUEST_CRASH:
        return MUX_UI_ACTION_CLOSE;
    case MUX_UI_REQUEST_CONTEXT_MENU:
    case MUX_UI_REQUEST_OPTION_MENU:
    case MUX_UI_REQUEST_DIALOG_CONFIRM:
    case MUX_UI_REQUEST_DIALOG_PROMPT:
    case MUX_UI_REQUEST_AUTHENTICATION:
    case MUX_UI_REQUEST_FILE_CHOOSER:
    case MUX_UI_REQUEST_DOWNLOAD_DESTINATION:
        return MUX_UI_ACTION_CANCEL;
    default:
        return MUX_UI_ACTION_UNSUPPORTED;
    }
}

gboolean
mux_pane_overlay_tick(MuxPaneOverlay *overlay,
                      gint64 monotonic_us,
                      MuxUiResponse **response_out)
{
    PendingPrompt *pending;

    g_return_val_if_fail(overlay, FALSE);
    g_return_val_if_fail(response_out, FALSE);
    *response_out = NULL;
    pending = active_prompt(overlay);
    if (!pending || monotonic_us < pending->expires_at_us)
        return FALSE;
    *response_out = complete_active(
        overlay, timeout_action(pending->request->kind), NULL);
    return TRUE;
}

static const gchar *
default_heading(MuxUiRequestKind kind)
{
    switch (kind) {
    case MUX_UI_REQUEST_DIALOG_ALERT:
        return "Page alert";
    case MUX_UI_REQUEST_DIALOG_CONFIRM:
        return "Page confirmation";
    case MUX_UI_REQUEST_DIALOG_PROMPT:
        return "Page prompt";
    case MUX_UI_REQUEST_DIALOG_BEFORE_UNLOAD:
        return "Leave this page?";
    case MUX_UI_REQUEST_PERMISSION:
        return "Permission request";
    case MUX_UI_REQUEST_AUTHENTICATION:
        return "Authentication";
    case MUX_UI_REQUEST_FILE_CHOOSER:
        return "Choose files";
    case MUX_UI_REQUEST_DOWNLOAD_DESTINATION:
        return "Download";
    case MUX_UI_REQUEST_CONTEXT_MENU:
        return "Context menu";
    case MUX_UI_REQUEST_OPTION_MENU:
        return "Choose an option";
    case MUX_UI_REQUEST_CRASH:
        return "Web process terminated";
    default:
        return "Browser request";
    }
}

static const gchar *
action_hint(const PendingPrompt *pending)
{
    switch (pending->request->kind) {
    case MUX_UI_REQUEST_DIALOG_ALERT:
        return "[Enter] acknowledge";
    case MUX_UI_REQUEST_DIALOG_CONFIRM:
        return "[y/Enter] accept  [n/Esc] cancel";
    case MUX_UI_REQUEST_DIALOG_PROMPT:
        return "[Enter] submit  [Esc] cancel";
    case MUX_UI_REQUEST_DIALOG_BEFORE_UNLOAD:
        return "[l] leave  [s/Esc] stay";
    case MUX_UI_REQUEST_PERMISSION:
        if (pending->request->flags &
            MUX_UI_REQUEST_FLAG_PERSISTENCE_AVAILABLE)
            return "[a] allow once  [A] always  [d] deny  [D] deny always";
        return "[a] allow once  [d/Esc] deny";
    case MUX_UI_REQUEST_AUTHENTICATION:
        return pending->auth_password
                   ? "Password: [Enter] submit  [Tab] username  [Esc] cancel"
                   : "Username: [Enter/Tab] password  [Esc] cancel";
    case MUX_UI_REQUEST_FILE_CHOOSER:
        return "Opening chooser...  [Esc] cancel";
    case MUX_UI_REQUEST_DOWNLOAD_DESTINATION:
        return "[Enter] download  [Esc] cancel";
    case MUX_UI_REQUEST_CONTEXT_MENU:
    case MUX_UI_REQUEST_OPTION_MENU:
        return "[Up/Down] move  [Enter] select  [Esc] cancel";
    case MUX_UI_REQUEST_CRASH:
        return "[r] reload  [q/Esc] close";
    default:
        return "[Esc] dismiss";
    }
}

static gchar *
sanitize_line(const gchar *input)
{
    g_autofree gchar *valid = g_utf8_make_valid(input ? input : "", -1);
    GString *output = g_string_sized_new(strlen(valid));
    const gchar *cursor;

    for (cursor = valid; *cursor; cursor = g_utf8_next_char(cursor)) {
        gunichar character = g_utf8_get_char(cursor);

        if (g_unichar_iscntrl(character))
            g_string_append_c(output, ' ');
        else
            g_string_append_unichar(output, character);
    }
    return g_string_free(output, FALSE);
}

static gchar *
fit_line(const gchar *input, guint width)
{
    g_autofree gchar *safe = sanitize_line(input);
    GString *output;
    const gchar *cursor;
    guint used = 0;

    if (!width)
        return g_strdup("");
    output = g_string_sized_new(MIN(strlen(safe), width));
    for (cursor = safe; *cursor && used < width; cursor = g_utf8_next_char(cursor)) {
        const gchar *next = g_utf8_next_char(cursor);

        if (used + 1 == width && *next) {
            if (width >= 3) {
                while (output->len && used > width - 3) {
                    gchar *previous =
                        g_utf8_find_prev_char(output->str,
                                              output->str + output->len);
                    g_string_truncate(output, previous - output->str);
                    used--;
                }
                g_string_append(output, "...");
            }
            break;
        }
        g_string_append_len(output, cursor, next - cursor);
        used++;
    }
    return g_string_free(output, FALSE);
}

static void
render_row(GString *output,
           guint row,
           guint columns,
           const gchar *style,
           const gchar *text)
{
    g_autofree gchar *fitted =
        fit_line(text, columns > 2 ? columns - 2 : columns);

    g_string_append_printf(output,
                           "\x1b[%u;1H\x1b[2K%s",
                           row,
                           style ? style : "");
    if (columns > 1)
        g_string_append_c(output, ' ');
    g_string_append(output, fitted);
}

static gchar *
choice_line(const PendingPrompt *pending)
{
    const MuxUiChoice *choice;

    if (!pending->request->choices ||
        pending->selected_choice >= pending->request->choices->len)
        return g_strdup("No selectable entries");
    choice = g_ptr_array_index(pending->request->choices,
                               pending->selected_choice);
    if (!choice)
        return g_strdup("No selectable entries");
    return g_strdup_printf("%u/%u  %s",
                           pending->selected_choice + 1,
                           pending->request->choices->len,
                           choice->label ? choice->label : "");
}

gchar *
mux_pane_overlay_render(const MuxPaneOverlay *overlay,
                        guint columns,
                        guint rows,
                        gint64 monotonic_us)
{
    PendingPrompt *pending = active_prompt(overlay);
    GString *output;
    guint panel_rows;
    guint start;
    gint64 remaining_ms;
    g_autofree gchar *heading = NULL;
    g_autofree gchar *title = NULL;
    g_autofree gchar *origin = NULL;
    g_autofree gchar *body = NULL;
    g_autofree gchar *detail = NULL;

    if (!pending || !columns || !rows)
        return g_strdup("");

    panel_rows = MIN(rows, MUX_PANE_OVERLAY_ROWS);
    start = rows - panel_rows + 1;
    remaining_ms = MAX((gint64)0,
                       (pending->expires_at_us - monotonic_us + 999) / 1000);
    heading = sanitize_line(
        pending->request->heading && *pending->request->heading
            ? pending->request->heading
            : default_heading(pending->request->kind));
    title = g_strdup_printf("MUX  %s  (%" G_GINT64_FORMAT "s)",
                            heading,
                            (remaining_ms + 999) / 1000);
    origin = g_strdup_printf("Origin: %s",
                             pending->request->origin &&
                                     *pending->request->origin
                                 ? pending->request->origin
                                 : "browser");
    body = sanitize_line(pending->request->message);

    switch (pending->request->kind) {
    case MUX_UI_REQUEST_DIALOG_PROMPT:
    case MUX_UI_REQUEST_DOWNLOAD_DESTINATION:
        detail = g_strdup_printf("> %s", pending->input->str);
        break;
    case MUX_UI_REQUEST_AUTHENTICATION:
        if (pending->auth_password) {
            g_autofree gchar *masked = g_strnfill(
                g_utf8_strlen(pending->secondary_input->str, -1), '*');

            detail = g_strdup_printf("Password: %s", masked);
        } else {
            detail = g_strdup_printf("Username: %s", pending->input->str);
        }
        break;
    case MUX_UI_REQUEST_CONTEXT_MENU:
    case MUX_UI_REQUEST_OPTION_MENU:
        detail = choice_line(pending);
        break;
    default:
        detail = g_strdup("");
        break;
    }

    output = g_string_new("\x1b[?2026h\x1b[s\x1b[?25l");
    render_row(output,
               start,
               columns,
               "\x1b[0;1;38;5;231;48;5;166m",
               title);
    if (panel_rows >= 2)
        render_row(output,
                   start + 1,
                   columns,
                   "\x1b[0;38;5;87;48;5;236m",
                   origin);
    if (panel_rows >= 3)
        render_row(output,
                   start + 2,
                   columns,
                   "\x1b[0;38;5;255;48;5;236m",
                   body);
    if (panel_rows >= 4)
        render_row(output,
                   start + 3,
                   columns,
                   "\x1b[0;1;38;5;231;48;5;238m",
                   detail);
    if (panel_rows >= 5)
        render_row(output,
                   start + 4,
                   columns,
                   "\x1b[0;38;5;221;48;5;236m",
                   action_hint(pending));
    g_string_append(output, "\x1b[0m\x1b[u\x1b[?2026l");
    return g_string_free(output, FALSE);
}

gchar *
mux_pane_overlay_render_clear(guint rows)
{
    GString *output;
    guint panel_rows;
    guint start;
    guint i;

    if (!rows)
        return g_strdup("");
    panel_rows = MIN(rows, MUX_PANE_OVERLAY_ROWS);
    start = rows - panel_rows + 1;
    output = g_string_new("\x1b[?2026h\x1b[s\x1b[0m");
    for (i = 0; i < panel_rows; i++)
        g_string_append_printf(output, "\x1b[%u;1H\x1b[2K", start + i);
    g_string_append(output, "\x1b[u\x1b[?25h\x1b[?2026l");
    return g_string_free(output, FALSE);
}
