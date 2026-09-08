#include "mux-pane-diagnostic.h"

static void
append_clear_rows(GString *command, guint rows)
{
    g_string_append(command, "\033[0m");
    for (guint row = 1; row <= rows; row++)
        g_string_append_printf(command, "\033[%u;1H\033[2K", row);
}

static void
append_clipped_text(GString *command, const gchar *text, guint columns)
{
    guint used = 0;
    guint budget = columns - 1u;

    /* Leave the final cell unused so narrow terminals never soft-wrap. */
    while (*text && used < budget) {
        gunichar character = g_utf8_get_char_validated(text, -1);
        guint width;

        if (character == (gunichar)-1 || character == (gunichar)-2) {
            character = '?';
            text++;
        } else {
            text = g_utf8_next_char(text);
            if (!g_unichar_isprint(character))
                character = '?';
        }
        width = g_unichar_iszerowidth(character)
            ? 0u : g_unichar_iswide(character) ? 2u : 1u;
        if (width > budget - used)
            break;
        g_string_append_unichar(command, character);
        used += width;
    }
}

gboolean
mux_pane_diagnostic_show(MuxPaneDiagnostic *diagnostic,
                         guint32 code,
                         const gchar *detail,
                         gboolean reconnecting,
                         guint columns,
                         guint rows,
                         MuxPaneDiagnosticOutput output,
                         gpointer user_data,
                         GError **error)
{
    g_autofree gchar *title = NULL;
    g_autoptr(GString) command = NULL;
    const gchar *lines[MUX_PANE_DIAGNOSTIC_ROWS];
    guint painted_rows;

    g_return_val_if_fail(diagnostic != NULL, FALSE);
    g_return_val_if_fail(output != NULL, FALSE);
    if (!columns || !rows)
        return TRUE;

    title = g_strdup_printf("Mux engine error (%u)", code);
    lines[0] = title;
    lines[1] = detail ? detail : "engine error";
    lines[2] = reconnecting
        ? "The pane is reconnecting with bounded backoff."
        : "Use the URL bar, reload, or close the pane.";
    painted_rows = MIN(rows, MUX_PANE_DIAGNOSTIC_ROWS);
    command = g_string_new("\033[?2026h\033[s");
    append_clear_rows(command, MIN(diagnostic->painted_rows, rows));
    for (guint row = 1; row <= painted_rows; row++) {
        g_string_append_printf(command,
                               "\033[%u;1H\033[38;5;231;48;5;52m\033[2K",
                               row);
        append_clipped_text(command, lines[row - 1], columns);
    }
    g_string_append(command, "\033[0m\033[u\033[?2026l");
    if (!output((const guint8 *)command->str,
                command->len,
                user_data,
                error))
        return FALSE;
    diagnostic->painted_rows = painted_rows;
    if (!++diagnostic->generation)
        diagnostic->generation++;
    diagnostic->recovered = FALSE;
    return TRUE;
}

gboolean
mux_pane_diagnostic_clear(MuxPaneDiagnostic *diagnostic,
                          guint rows,
                          MuxPaneDiagnosticOutput output,
                          gpointer user_data,
                          GError **error)
{
    g_autoptr(GString) command = NULL;

    g_return_val_if_fail(diagnostic != NULL, FALSE);
    g_return_val_if_fail(output != NULL, FALSE);
    if (!diagnostic->painted_rows || !rows)
        return TRUE;

    command = g_string_new("\033[?2026h\033[s");
    append_clear_rows(command, MIN(diagnostic->painted_rows, rows));
    g_string_append(command, "\033[u\033[?2026l");
    if (!output((const guint8 *)command->str,
                command->len,
                user_data,
                error))
        return FALSE;
    diagnostic->painted_rows = 0;
    diagnostic->recovered = FALSE;
    return TRUE;
}

gboolean
mux_pane_diagnostic_recover(MuxPaneDiagnostic *diagnostic,
                            guint64 generation,
                            guint rows,
                            MuxPaneDiagnosticOutput output,
                            gpointer user_data,
                            GError **error)
{
    g_return_val_if_fail(diagnostic != NULL, FALSE);
    if (!diagnostic->painted_rows || generation != diagnostic->generation)
        return TRUE;
    diagnostic->recovered = TRUE;
    return mux_pane_diagnostic_clear(diagnostic, rows,
                                      output, user_data, error);
}
