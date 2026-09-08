#include "mux-clipboard-picker.h"

#include <string.h>

#define PICKER_QUERY_MAX_BYTES 512u
#define PICKER_PANEL_MAX_COLUMNS 88u
#define PICKER_PANEL_MAX_ROWS 14u
#define PICKER_PAGE_MAX_ITEMS 6u
#define PICKER_PANEL_STYLE "\033[0;38;5;252;48;5;234m"

struct _MuxClipboardPickerItem {
    gatomicrefcount references;
    guint64 id;
    gint64 created_us;
    gchar *origin;
    guint64 source_view_id;
    gboolean pinned;
    gsize total_size;
    gchar *preview;
    gchar **mime_types;
    gsize mime_type_count;
    gsize format_count;
    gchar *search_text;
};

typedef struct {
    MuxClipboardPickerItem *item;
    gint score;
} MuxClipboardPickerMatch;

struct _MuxClipboardPicker {
    gchar *profile;
    gchar *query;
    gchar *status;
    GPtrArray *items;
    GArray *matches;
    guint selected;
    guint top;
    guint page_rows;
    guint64 preserved_selection_id;
};

static gchar *
normalise_visible_text(const gchar *input)
{
    g_autofree gchar *valid = NULL;
    GString *output;
    const gchar *cursor;
    gboolean pending_space = FALSE;

    valid = g_utf8_make_valid(input != NULL ? input : "", -1);
    output = g_string_new(NULL);

    for (cursor = valid; *cursor != '\0'; cursor = g_utf8_next_char(cursor)) {
        gunichar character = g_utf8_get_char(cursor);

        if (g_unichar_isspace(character) ||
            (!g_unichar_isprint(character) && character != 0x200b)) {
            pending_space = output->len > 0;
            continue;
        }

        if (pending_space) {
            g_string_append_c(output, ' ');
            pending_space = FALSE;
        }
        g_string_append_unichar(output, character);
    }

    return g_string_free(output, FALSE);
}

static gchar *
build_search_text(const MuxClipboardPickerItem *item)
{
    GString *combined = g_string_new(item->preview);
    gchar *folded;
    gsize index;

    g_string_append_c(combined, ' ');
    g_string_append(combined, item->origin);
    for (index = 0; index < item->mime_type_count; index++) {
        g_string_append_c(combined, ' ');
        g_string_append(combined, item->mime_types[index]);
    }

    folded = g_utf8_casefold(combined->str, -1);
    g_string_free(combined, TRUE);
    return folded;
}

MuxClipboardPickerItem *
mux_clipboard_picker_item_new(guint64 id,
                              gint64 created_us,
                              const gchar *origin,
                              guint64 source_view_id,
                              gboolean pinned,
                              gsize total_size,
                              const gchar *preview,
                              const gchar *const *mime_types,
                              gsize mime_type_count)
{
    return mux_clipboard_picker_item_new_full(id,
                                              created_us,
                                              origin,
                                              source_view_id,
                                              pinned,
                                              total_size,
                                              preview,
                                              mime_types,
                                              mime_type_count,
                                              mime_type_count);
}

MuxClipboardPickerItem *
mux_clipboard_picker_item_new_full(guint64 id,
                                   gint64 created_us,
                                   const gchar *origin,
                                   guint64 source_view_id,
                                   gboolean pinned,
                                   gsize total_size,
                                   const gchar *preview,
                                   const gchar *const *mime_types,
                                   gsize mime_type_count,
                                   gsize format_count)
{
    MuxClipboardPickerItem *item;
    gsize index;

    g_return_val_if_fail(id != 0, NULL);
    g_return_val_if_fail(mime_type_count == 0 || mime_types != NULL, NULL);
    g_return_val_if_fail(format_count >= mime_type_count, NULL);

    item = g_new0(MuxClipboardPickerItem, 1);
    g_atomic_ref_count_init(&item->references);
    item->id = id;
    item->created_us = created_us;
    item->origin = normalise_visible_text(origin);
    item->source_view_id = source_view_id;
    item->pinned = pinned;
    item->total_size = total_size;
    item->preview = normalise_visible_text(preview);
    item->mime_type_count = mime_type_count;
    item->format_count = format_count;
    item->mime_types = g_new0(gchar *, mime_type_count + 1);

    for (index = 0; index < mime_type_count; index++)
        item->mime_types[index] = normalise_visible_text(mime_types[index]);
    item->search_text = build_search_text(item);
    return item;
}

MuxClipboardPickerItem *
mux_clipboard_picker_item_ref(MuxClipboardPickerItem *item)
{
    g_return_val_if_fail(item != NULL, NULL);
    g_atomic_ref_count_inc(&item->references);
    return item;
}

void
mux_clipboard_picker_item_unref(MuxClipboardPickerItem *item)
{
    if (item == NULL || !g_atomic_ref_count_dec(&item->references))
        return;

    g_free(item->origin);
    g_free(item->preview);
    g_strfreev(item->mime_types);
    g_free(item->search_text);
    g_free(item);
}

guint64
mux_clipboard_picker_item_get_id(const MuxClipboardPickerItem *item)
{
    g_return_val_if_fail(item != NULL, 0);
    return item->id;
}

gboolean
mux_clipboard_picker_item_get_pinned(const MuxClipboardPickerItem *item)
{
    g_return_val_if_fail(item != NULL, FALSE);
    return item->pinned;
}

static gint
fuzzy_score(const gchar *folded_query, const gchar *folded_haystack)
{
    glong query_length;
    glong haystack_length;
    g_autofree gunichar *query =
        g_utf8_to_ucs4_fast(folded_query, -1, &query_length);
    g_autofree gunichar *haystack =
        g_utf8_to_ucs4_fast(folded_haystack, -1, &haystack_length);
    glong query_index;
    glong search_from = 0;
    glong previous = -2;
    gint score = 1000;

    if (query_length == 0)
        return 0;
    if (query_length > haystack_length)
        return G_MININT;

    for (query_index = 0; query_index < query_length; query_index++) {
        glong position;

        for (position = search_from; position < haystack_length; position++) {
            if (query[query_index] == haystack[position])
                break;
        }
        if (position == haystack_length)
            return G_MININT;

        if (position == previous + 1)
            score += 18;
        else
            score -= (gint) MIN(position - previous - 1, 40);

        if (position == 0 || haystack[position - 1] == ' ' ||
            haystack[position - 1] == '/' || haystack[position - 1] == '-' ||
            haystack[position - 1] == '_' || haystack[position - 1] == '.')
            score += 12;

        if (query_index == 0)
            score -= (gint) MIN(position, 80);
        previous = position;
        search_from = position + 1;
    }

    score -= (gint) MIN(haystack_length - query_length, 120) / 4;
    return score;
}

static gint
compare_matches(gconstpointer left_pointer, gconstpointer right_pointer)
{
    const MuxClipboardPickerMatch *left = left_pointer;
    const MuxClipboardPickerMatch *right = right_pointer;

    if (left->score != right->score)
        return left->score > right->score ? -1 : 1;
    if (left->item->pinned != right->item->pinned)
        return left->item->pinned ? -1 : 1;
    if (left->item->created_us != right->item->created_us)
        return left->item->created_us > right->item->created_us ? -1 : 1;
    if (left->item->id == right->item->id)
        return 0;
    return left->item->id > right->item->id ? -1 : 1;
}

static guint64
selected_id(const MuxClipboardPicker *picker)
{
    if (picker->matches->len == 0 || picker->selected >= picker->matches->len)
        return 0;
    return g_array_index(picker->matches,
                         MuxClipboardPickerMatch,
                         picker->selected).item->id;
}

static void
keep_selection_visible(MuxClipboardPicker *picker)
{
    guint page_rows = MAX(picker->page_rows, 1u);

    if (picker->matches->len == 0) {
        picker->selected = 0;
        picker->top = 0;
        return;
    }

    picker->selected = MIN(picker->selected, picker->matches->len - 1);
    if (picker->selected < picker->top)
        picker->top = picker->selected;
    if (picker->selected >= picker->top + page_rows)
        picker->top = picker->selected - page_rows + 1;
    if (picker->top + page_rows > picker->matches->len)
        picker->top = picker->matches->len > page_rows
                          ? picker->matches->len - page_rows
                          : 0;
}

static void
rebuild_matches(MuxClipboardPicker *picker)
{
    g_autofree gchar *normal_query = normalise_visible_text(picker->query);
    g_autofree gchar *folded_query = g_utf8_casefold(normal_query, -1);
    guint64 previous_id = picker->preserved_selection_id != 0
                              ? picker->preserved_selection_id
                              : selected_id(picker);
    guint index;

    picker->preserved_selection_id = 0;
    g_array_set_size(picker->matches, 0);
    for (index = 0; index < picker->items->len; index++) {
        MuxClipboardPickerItem *item = g_ptr_array_index(picker->items, index);
        gint score = fuzzy_score(folded_query, item->search_text);

        if (score != G_MININT) {
            MuxClipboardPickerMatch match = {
                .item = item,
                .score = score,
            };
            g_array_append_val(picker->matches, match);
        }
    }
    g_array_sort(picker->matches, compare_matches);

    picker->selected = 0;
    if (previous_id != 0) {
        for (index = 0; index < picker->matches->len; index++) {
            MuxClipboardPickerMatch *match = &g_array_index(
                picker->matches,
                MuxClipboardPickerMatch,
                index);
            if (match->item->id == previous_id) {
                picker->selected = index;
                break;
            }
        }
    }
    keep_selection_visible(picker);
}

MuxClipboardPicker *
mux_clipboard_picker_new(const gchar *profile)
{
    MuxClipboardPicker *picker = g_new0(MuxClipboardPicker, 1);

    picker->profile = normalise_visible_text(profile);
    picker->query = g_strdup("");
    picker->status = g_strdup("");
    picker->items = g_ptr_array_new_with_free_func(
        (GDestroyNotify) mux_clipboard_picker_item_unref);
    picker->matches = g_array_new(FALSE,
                                  FALSE,
                                  sizeof(MuxClipboardPickerMatch));
    picker->page_rows = 8;
    return picker;
}

void
mux_clipboard_picker_free(MuxClipboardPicker *picker)
{
    if (picker == NULL)
        return;

    g_free(picker->profile);
    g_free(picker->query);
    g_free(picker->status);
    g_ptr_array_unref(picker->items);
    g_array_unref(picker->matches);
    g_free(picker);
}

void
mux_clipboard_picker_set_items(MuxClipboardPicker *picker, GPtrArray *items)
{
    guint index;

    g_return_if_fail(picker != NULL);
    g_return_if_fail(items != NULL);

    picker->preserved_selection_id = selected_id(picker);
    g_array_set_size(picker->matches, 0);
    g_ptr_array_set_size(picker->items, 0);
    for (index = 0; index < items->len; index++) {
        MuxClipboardPickerItem *item = g_ptr_array_index(items, index);
        if (item != NULL)
            g_ptr_array_add(picker->items,
                            mux_clipboard_picker_item_ref(item));
    }
    rebuild_matches(picker);
}

void
mux_clipboard_picker_set_query(MuxClipboardPicker *picker,
                               const gchar *query)
{
    g_autofree gchar *valid = NULL;

    g_return_if_fail(picker != NULL);

    valid = g_utf8_make_valid(query != NULL ? query : "", -1);
    if (strlen(valid) > PICKER_QUERY_MAX_BYTES) {
        gchar *cut = valid + PICKER_QUERY_MAX_BYTES;

        while (cut > valid && (((guchar) *cut & 0xc0u) == 0x80u))
            cut--;
        *cut = '\0';
    }
    g_free(picker->query);
    picker->query = g_steal_pointer(&valid);
    rebuild_matches(picker);
}

void
mux_clipboard_picker_set_status(MuxClipboardPicker *picker,
                                const gchar *status)
{
    g_return_if_fail(picker != NULL);

    g_free(picker->status);
    picker->status = normalise_visible_text(status);
}

const gchar *
mux_clipboard_picker_get_query(const MuxClipboardPicker *picker)
{
    g_return_val_if_fail(picker != NULL, NULL);
    return picker->query;
}

guint
mux_clipboard_picker_get_match_count(const MuxClipboardPicker *picker)
{
    g_return_val_if_fail(picker != NULL, 0);
    return picker->matches->len;
}

const MuxClipboardPickerItem *
mux_clipboard_picker_get_selected(const MuxClipboardPicker *picker)
{
    g_return_val_if_fail(picker != NULL, NULL);

    if (picker->matches->len == 0 || picker->selected >= picker->matches->len)
        return NULL;
    return g_array_index(picker->matches,
                         MuxClipboardPickerMatch,
                         picker->selected).item;
}

static void
delete_last_character(MuxClipboardPicker *picker)
{
    gchar *end;

    if (*picker->query == '\0')
        return;
    end = g_utf8_find_prev_char(picker->query,
                                picker->query + strlen(picker->query));
    if (end != NULL)
        *end = '\0';
}

static void
delete_last_word(MuxClipboardPicker *picker)
{
    gchar *cursor = picker->query + strlen(picker->query);
    gchar *previous;

    while ((previous = g_utf8_find_prev_char(picker->query, cursor)) != NULL &&
           g_unichar_isspace(g_utf8_get_char(previous)))
        cursor = previous;
    while ((previous = g_utf8_find_prev_char(picker->query, cursor)) != NULL &&
           !g_unichar_isspace(g_utf8_get_char(previous)))
        cursor = previous;
    *cursor = '\0';
}

static gboolean
history_has_unpinned_items(const MuxClipboardPicker *picker)
{
    for (guint index = 0; index < picker->items->len; index++) {
        const MuxClipboardPickerItem *item =
            g_ptr_array_index(picker->items, index);

        if (!item->pinned)
            return TRUE;
    }
    return FALSE;
}

gboolean
mux_clipboard_picker_handle_key(MuxClipboardPicker *picker,
                                MuxClipboardPickerKey key,
                                gunichar text,
                                MuxClipboardPickerAction *out_action)
{
    gboolean query_changed = FALSE;
    const MuxClipboardPickerItem *selected;
    gchar encoded[7] = { 0 };
    gint encoded_size;

    g_return_val_if_fail(picker != NULL, FALSE);
    g_return_val_if_fail(out_action != NULL, FALSE);

    *out_action = (MuxClipboardPickerAction) {
        .kind = MUX_CLIPBOARD_PICKER_ACTION_NONE,
    };

    switch (key) {
    case MUX_CLIPBOARD_PICKER_KEY_TEXT:
        if (!g_unichar_isprint(text) || g_unichar_iscntrl(text))
            return FALSE;
        encoded_size = g_unichar_to_utf8(text, encoded);
        if (strlen(picker->query) + (gsize) encoded_size >
            PICKER_QUERY_MAX_BYTES)
            return FALSE;
        {
            gchar *updated = g_strconcat(picker->query, encoded, NULL);
            g_free(picker->query);
            picker->query = updated;
        }
        query_changed = TRUE;
        break;
    case MUX_CLIPBOARD_PICKER_KEY_BACKSPACE:
        delete_last_character(picker);
        query_changed = TRUE;
        break;
    case MUX_CLIPBOARD_PICKER_KEY_DELETE_WORD:
        delete_last_word(picker);
        query_changed = TRUE;
        break;
    case MUX_CLIPBOARD_PICKER_KEY_CLEAR_QUERY:
        if (*picker->query != '\0') {
            *picker->query = '\0';
            query_changed = TRUE;
        }
        break;
    case MUX_CLIPBOARD_PICKER_KEY_UP:
        if (picker->selected > 0)
            picker->selected--;
        break;
    case MUX_CLIPBOARD_PICKER_KEY_DOWN:
        if (picker->selected + 1 < picker->matches->len)
            picker->selected++;
        break;
    case MUX_CLIPBOARD_PICKER_KEY_PAGE_UP:
        picker->selected = picker->selected > picker->page_rows
                               ? picker->selected - picker->page_rows
                               : 0;
        break;
    case MUX_CLIPBOARD_PICKER_KEY_PAGE_DOWN:
        if (picker->matches->len > 0)
            picker->selected = MIN(picker->selected + picker->page_rows,
                                   picker->matches->len - 1);
        break;
    case MUX_CLIPBOARD_PICKER_KEY_HOME:
        picker->selected = 0;
        break;
    case MUX_CLIPBOARD_PICKER_KEY_END:
        if (picker->matches->len > 0)
            picker->selected = picker->matches->len - 1;
        break;
    case MUX_CLIPBOARD_PICKER_KEY_ENTER:
        selected = mux_clipboard_picker_get_selected(picker);
        if (selected == NULL)
            return FALSE;
        out_action->kind = MUX_CLIPBOARD_PICKER_ACTION_SELECT;
        out_action->entry_id = selected->id;
        break;
    case MUX_CLIPBOARD_PICKER_KEY_ESCAPE:
        out_action->kind = MUX_CLIPBOARD_PICKER_ACTION_CLOSE;
        break;
    case MUX_CLIPBOARD_PICKER_KEY_TOGGLE_PIN:
        selected = mux_clipboard_picker_get_selected(picker);
        if (selected == NULL)
            return FALSE;
        out_action->kind = MUX_CLIPBOARD_PICKER_ACTION_SET_PINNED;
        out_action->entry_id = selected->id;
        out_action->pinned = !selected->pinned;
        break;
    case MUX_CLIPBOARD_PICKER_KEY_DELETE_ENTRY:
        selected = mux_clipboard_picker_get_selected(picker);
        if (selected == NULL)
            return FALSE;
        out_action->kind = MUX_CLIPBOARD_PICKER_ACTION_DELETE;
        out_action->entry_id = selected->id;
        break;
    case MUX_CLIPBOARD_PICKER_KEY_CLEAR_HISTORY:
        if (!history_has_unpinned_items(picker))
            return FALSE;
        out_action->kind = MUX_CLIPBOARD_PICKER_ACTION_CLEAR;
        break;
    default:
        return FALSE;
    }

    if (query_changed)
        rebuild_matches(picker);
    else
        keep_selection_visible(picker);
    return TRUE;
}

static guint
character_width(gunichar character)
{
    if (g_unichar_combining_class(character) != 0)
        return 0;
    return g_unichar_iswide(character) ? 2u : 1u;
}

static guint
text_width(const gchar *text)
{
    const gchar *cursor;
    guint width = 0;

    for (cursor = text; *cursor != '\0'; cursor = g_utf8_next_char(cursor))
        width += character_width(g_utf8_get_char(cursor));
    return width;
}

static gchar *
clip_text(const gchar *text, guint maximum_width)
{
    const gchar *cursor;
    GString *output;
    guint width = text_width(text);
    guint used = 0;
    guint content_limit;

    if (width <= maximum_width)
        return g_strdup(text);
    if (maximum_width == 0)
        return g_strdup("");

    content_limit = maximum_width > 3 ? maximum_width - 3 : maximum_width;
    output = g_string_new(NULL);
    for (cursor = text; *cursor != '\0'; cursor = g_utf8_next_char(cursor)) {
        gunichar character = g_utf8_get_char(cursor);
        guint next_width = character_width(character);

        if (used + next_width > content_limit)
            break;
        g_string_append_unichar(output, character);
        used += next_width;
    }
    if (maximum_width > 3)
        g_string_append(output, "...");
    return g_string_free(output, FALSE);
}

static gchar *
format_age(gint64 created_us)
{
    gint64 elapsed = MAX((g_get_monotonic_time() - created_us) /
                             G_USEC_PER_SEC,
                         (gint64) 0);

    if (elapsed < 60)
        return g_strdup_printf("%2" G_GINT64_FORMAT "s", elapsed);
    if (elapsed < 60 * 60)
        return g_strdup_printf("%2" G_GINT64_FORMAT "m", elapsed / 60);
    if (elapsed < 24 * 60 * 60)
        return g_strdup_printf("%2" G_GINT64_FORMAT "h", elapsed / 3600);
    if (elapsed < 7 * 24 * 60 * 60)
        return g_strdup_printf("%2" G_GINT64_FORMAT "d", elapsed / 86400);
    return g_strdup_printf("%2" G_GINT64_FORMAT "w",
                           MIN(elapsed / (7 * 86400), (gint64) 99));
}

static gchar *
format_size(gsize size)
{
    if (size < 1024)
        return g_strdup_printf("%" G_GSIZE_FORMAT "B", size);
    if (size < 1024u * 1024u)
        return g_strdup_printf("%.1fK", (gdouble) size / 1024.0);
    return g_strdup_printf("%.1fM", (gdouble) size / (1024.0 * 1024.0));
}

static void
append_panel_line(GString *output,
                  const gchar *content,
                  guint panel_width,
                  gboolean framed,
                  const gchar *style)
{
    guint inside_width = framed ? panel_width - 4 : panel_width;
    g_autofree gchar *clipped = clip_text(content, inside_width);
    guint padding = inside_width - text_width(clipped);

    if (output->len > 0)
        g_string_append(output, "\r\n");
    g_string_append(output, PICKER_PANEL_STYLE);
    if (framed)
        g_string_append(output, "\033[38;5;37m| " PICKER_PANEL_STYLE);
    if (style != NULL)
        g_string_append(output, style);
    g_string_append(output, clipped);
    while (padding-- > 0)
        g_string_append_c(output, ' ');
    if (framed)
        g_string_append(output, PICKER_PANEL_STYLE "\033[38;5;37m |");
    g_string_append(output, "\033[0m");
}

static void
append_border(GString *output, guint panel_width)
{
    if (output->len > 0)
        g_string_append(output, "\r\n");
    g_string_append(output, PICKER_PANEL_STYLE "\033[38;5;37m+");
    for (guint index = 0; index < panel_width - 2; index++)
        g_string_append_c(output, '-');
    g_string_append(output, "+\033[0m");
}

gchar *
mux_clipboard_picker_render_full(MuxClipboardPicker *picker,
                                 guint terminal_columns,
                                 guint terminal_rows,
                                 gboolean ready)
{
    static const gchar *empty_history[] = {
        "No clipboard history in this profile yet.",
        "Copy in this profile to add an item.",
        "History from other profiles stays separate.",
    };
    static const gchar *no_results[] = {
        "No matches for this search.",
        "Try a shorter query, or clear the search.",
    };
    const MuxClipboardPickerItem *selected;
    GString *output;
    GString *footer;
    guint panel_width;
    guint maximum_rows;
    guint inside_width;
    guint content_budget;
    guint header_rows;
    guint footer_rows;
    guint body_rows;
    guint list_rows;
    gboolean framed;
    gboolean detailed_actions;
    gboolean query_present;
    gboolean has_status;
    g_autofree gchar *title = NULL;
    g_autofree gchar *profile = NULL;
    g_autofree gchar *visible_query = NULL;
    g_autofree gchar *query = NULL;

    g_return_val_if_fail(picker != NULL, NULL);

    if (terminal_columns == 0 || terminal_rows == 0)
        return g_strdup("");

    panel_width = MIN(terminal_columns, PICKER_PANEL_MAX_COLUMNS);
    maximum_rows = MIN(terminal_rows, PICKER_PANEL_MAX_ROWS);
    framed = panel_width >= 32 && maximum_rows >= 8;
    inside_width = framed ? panel_width - 4 : panel_width;
    content_budget = maximum_rows - (framed ? 2u : 0u);
    header_rows = content_budget >= 5 ? 3 : content_budget >= 3 ? 2 : 1;
    selected = mux_clipboard_picker_get_selected(picker);
    detailed_actions = ready && selected != NULL &&
                       inside_width >= 40 && content_budget >= 7;
    footer_rows = detailed_actions ? 2 : content_budget >= 2 ? 1 : 0;
    body_rows = content_budget - header_rows - footer_rows;
    query_present = picker->query[0] != '\0';
    has_status = picker->status[0] != '\0';
    list_rows = MIN(picker->matches->len, PICKER_PAGE_MAX_ITEMS);
    list_rows = MIN(list_rows, body_rows - (has_status && body_rows > 0 ? 1u : 0u));
    picker->page_rows = MAX(list_rows, 1u);
    keep_selection_visible(picker);

    visible_query = normalise_visible_text(picker->query);
    query = g_strdup_printf("Search: %s",
                             query_present ? visible_query : "type to filter");
    if (content_budget <= 2 && has_status) {
        title = g_strdup_printf("Clipboard: %s", picker->status);
    } else if (content_budget == 1 && query_present) {
        title = g_strdup(query);
    } else if (content_budget <= 2 && picker->matches->len == 0) {
        title = g_strdup(picker->items->len == 0
                             ? "Clipboard: empty"
                             : "Clipboard: no matches");
    } else if (picker->matches->len > 0) {
        title = g_strdup_printf("Clipboard history  |  %u of %u",
                                 picker->selected + 1,
                                 picker->matches->len);
    } else {
        title = g_strdup("Clipboard history");
    }
    profile = g_strdup_printf("Profile: %s  |  %u item%s",
                               picker->profile[0] != '\0'
                                   ? picker->profile : "default",
                               picker->items->len,
                               picker->items->len == 1 ? "" : "s");

    output = g_string_new(NULL);
    if (framed)
        append_border(output, panel_width);
    append_panel_line(output, title, panel_width, framed, "\033[1;38;5;223m");
    if (header_rows >= 3)
        append_panel_line(output, profile, panel_width, framed,
                           "\033[38;5;245m");
    if (header_rows >= 2)
        append_panel_line(output, query, panel_width, framed,
                           query_present ? "\033[1;38;5;81m"
                                         : "\033[38;5;245m");

    if (has_status && body_rows > 0) {
        append_panel_line(output, picker->status, panel_width, framed,
                           "\033[38;5;223m");
        body_rows--;
    }
    if (picker->matches->len > 0) {
        for (guint row = 0; row < list_rows; row++) {
            guint match_index = picker->top + row;
            MuxClipboardPickerMatch *match = &g_array_index(
                picker->matches, MuxClipboardPickerMatch, match_index);
            MuxClipboardPickerItem *item = match->item;
            g_autofree gchar *age = format_age(item->created_us);
            g_autofree gchar *size = format_size(item->total_size);
            g_autofree gchar *line = g_strdup_printf(
                "%c %s %-8s %s | %s | %" G_GSIZE_FORMAT " fmt",
                item->pinned ? '*' : ' ',
                age,
                size,
                item->origin[0] != '\0' ? item->origin : "unknown",
                item->preview[0] != '\0' ? item->preview : "[binary data]",
                item->format_count);

            append_panel_line(output, line, panel_width, framed,
                               match_index == picker->selected
                                   ? "\033[1;30;48;5;223m" : NULL);
        }
    } else if (has_status) {
        if (body_rows > 0)
            append_panel_line(output,
                               "History from other profiles stays separate.",
                               panel_width, framed, "\033[38;5;245m");
    } else {
        const gchar *const *messages = picker->items->len == 0
            ? empty_history : no_results;
        guint message_count = picker->items->len == 0
            ? G_N_ELEMENTS(empty_history) : G_N_ELEMENTS(no_results);

        for (guint row = 0; row < MIN(body_rows, message_count); row++)
            append_panel_line(output, messages[row], panel_width, framed,
                               row == 0 ? NULL : "\033[38;5;245m");
    }

    footer = g_string_new("Esc close");
    if (detailed_actions) {
        g_autofree gchar *actions = g_strdup_printf(
            "Enter paste  Alt+P %s  Alt+D delete",
            selected->pinned ? "unpin" : "pin");

        append_panel_line(output, actions, panel_width, framed,
                           "\033[38;5;245m");
    } else if (ready && selected != NULL) {
        g_string_append(footer, "  Enter paste");
    }
    if (ready && query_present)
        g_string_append(footer, inside_width >= 40
                                   ? "  Ctrl+U clear search"
                                   : "  Ctrl+U reset");
    if (ready && history_has_unpinned_items(picker) &&
        (detailed_actions || selected == NULL))
        g_string_append(footer, "  Alt+C clear unpinned");
    if (footer_rows > 0)
        append_panel_line(output, footer->str, panel_width, framed,
                           "\033[38;5;245m");
    g_string_free(footer, TRUE);
    if (framed)
        append_border(output, panel_width);
    return g_string_free(output, FALSE);
}

gchar *
mux_clipboard_picker_render(MuxClipboardPicker *picker,
                            guint terminal_columns,
                            guint terminal_rows)
{
    return mux_clipboard_picker_render_full(picker, terminal_columns,
                                            terminal_rows, TRUE);
}

static void
append_surface_clear(GString *output,
                      MuxClipboardPickerSurface *surface,
                      guint terminal_columns,
                      guint terminal_rows)
{
    for (guint row = 1; row <= MIN(surface->painted_rows, terminal_rows); row++)
        g_string_append_printf(output, "\033[%u;1H\033[0m\033[2K", row);
    /* A smaller viewport cannot erase offscreen cells. Keep that footprint
     * until a later resize exposes it, including when the picker has closed. */
    if (terminal_rows >= surface->painted_rows &&
        terminal_columns >= surface->painted_columns) {
        surface->painted_rows = 0;
        surface->painted_columns = 0;
    }
}

gchar *
mux_clipboard_picker_surface_present(MuxClipboardPickerSurface *surface,
                                     const gchar *panel,
                                     guint terminal_columns,
                                     guint terminal_rows)
{
    GString *output;
    guint panel_width;
    guint left;
    guint top;
    guint line_count;
    g_auto(GStrv) lines = NULL;

    g_return_val_if_fail(surface != NULL, NULL);

    if (terminal_columns == 0 || terminal_rows == 0)
        return g_strdup("");
    if (panel == NULL || panel[0] == '\0')
        return mux_clipboard_picker_surface_clear(surface, terminal_columns,
                                                   terminal_rows);

    panel_width = MIN(terminal_columns, PICKER_PANEL_MAX_COLUMNS);
    left = (terminal_columns - panel_width) / 2 + 1;
    top = terminal_rows >= PICKER_PANEL_MAX_ROWS + 2 ? 2 : 1;
    lines = g_strsplit(panel, "\r\n", -1);
    line_count = MIN(g_strv_length(lines), terminal_rows - top + 1);
    output = g_string_new("\0337");
    append_surface_clear(output, surface, terminal_columns, terminal_rows);
    for (guint row = 0; row < line_count; row++) {
        g_string_append_printf(output, "\033[%u;%uH", top + row, left);
        g_string_append(output, lines[row]);
    }
    surface->painted_rows = MAX(surface->painted_rows, top + line_count - 1);
    surface->painted_columns = MAX(surface->painted_columns,
                                   left + panel_width - 1);
    g_string_append(output, "\033[0m\0338");
    return g_string_free(output, FALSE);
}

gchar *
mux_clipboard_picker_surface_clear(MuxClipboardPickerSurface *surface,
                                   guint terminal_columns,
                                   guint terminal_rows)
{
    GString *output;

    g_return_val_if_fail(surface != NULL, NULL);

    if (surface->painted_rows == 0 ||
        terminal_columns == 0 || terminal_rows == 0)
        return g_strdup("");
    output = g_string_new("\0337");
    append_surface_clear(output, surface, terminal_columns, terminal_rows);
    g_string_append(output, "\0338");
    return g_string_free(output, FALSE);
}
