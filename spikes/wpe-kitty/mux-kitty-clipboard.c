#include "mux-kitty-clipboard.h"

#include <string.h>
#include <unistd.h>

typedef enum {
    READ_STAGE_CONTENT,
    READ_STAGE_TARGETS
} ReadStage;

typedef struct {
    gchar *id;
    MuxOsc5522Location location;
    gboolean is_paste;
    ReadStage stage;
    gchar *password;
    gchar *human_name;
    gboolean acknowledged;
    GPtrArray *order;
    GHashTable *buffers;
    gsize total_bytes;
    gint64 deadline_us;
} ReadTransaction;

typedef struct {
    MuxOsc5522Location location;
    GPtrArray *mimes;
    gchar *password;
    gchar *human_name;
    gint64 deadline_us;
} PasteOffer;

typedef struct {
    MuxOsc5522Location location;
    MuxClipboardSnapshot *snapshot;
} PendingWrite;

typedef enum {
    WRITE_STAGE_BEGIN,
    WRITE_STAGE_DATA,
    WRITE_STAGE_END,
    WRITE_STAGE_WAIT_DONE
} WriteStage;

typedef struct {
    gchar *id;
    MuxOsc5522Location location;
    MuxClipboardSnapshot *snapshot;
    WriteStage stage;
    guint item_index;
    gsize item_offset;
    gint64 deadline_us;
} WriteTransaction;

struct _MuxKittyClipboard {
    gint reference_count;
    MuxKittyClipboardOutputFunc output_func;
    MuxKittyClipboardReceiveFunc receive_func;
    MuxKittyClipboardFailureFunc failure_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
    MuxOsc5522Support support;
    gboolean enabled;
    guint32 nonce;
    guint64 next_id;
    guint64 next_serial;
    ReadTransaction *read;
    PasteOffer *offer;
    PasteOffer *pending_offer;
    WriteTransaction *write;
    PendingWrite *queued_write;
};

static gint64
new_deadline(void)
{
    return g_get_monotonic_time() +
           ((gint64)MUX_KITTY_CLIPBOARD_TIMEOUT_MS * 1000);
}

static void
read_transaction_free(ReadTransaction *transaction)
{
    if (transaction == NULL)
        return;

    g_free(transaction->id);
    g_free(transaction->password);
    g_free(transaction->human_name);
    g_ptr_array_unref(transaction->order);
    g_hash_table_unref(transaction->buffers);
    g_free(transaction);
}

static void
paste_offer_free(PasteOffer *offer)
{
    if (offer == NULL)
        return;

    g_ptr_array_unref(offer->mimes);
    g_free(offer->password);
    g_free(offer->human_name);
    g_free(offer);
}

static void
pending_write_free(PendingWrite *write)
{
    if (write == NULL)
        return;

    mux_clipboard_snapshot_unref(write->snapshot);
    g_free(write);
}

static void
write_transaction_free(WriteTransaction *write)
{
    if (write == NULL)
        return;

    g_free(write->id);
    mux_clipboard_snapshot_unref(write->snapshot);
    g_free(write);
}

static gchar *
new_request_id(MuxKittyClipboard *clipboard)
{
    return g_strdup_printf("mux.%08x.%u.%" G_GUINT64_FORMAT,
                           clipboard->nonce,
                           (guint)getpid(),
                           ++clipboard->next_id);
}

static gboolean
emit_packet(MuxKittyClipboard *clipboard,
            GBytes *packet,
            GError **error)
{
    gboolean result;

    if (packet == NULL)
        return FALSE;
    result = clipboard->output_func(clipboard,
                                    packet,
                                    clipboard->user_data,
                                    error);
    g_bytes_unref(packet);
    return result;
}

static void
report_failure(MuxKittyClipboard *clipboard,
               const gchar *operation,
               const GError *error)
{
    if (clipboard->failure_func != NULL)
        clipboard->failure_func(clipboard,
                                operation,
                                error,
                                clipboard->user_data);
}

static void
report_literal(MuxKittyClipboard *clipboard,
               const gchar *operation,
               GIOErrorEnum code,
               const gchar *message)
{
    g_autoptr(GError) error = g_error_new_literal(G_IO_ERROR, code, message);

    report_failure(clipboard, operation, error);
}

static ReadTransaction *
read_transaction_new(gchar *id,
                     MuxOsc5522Location location,
                     gboolean is_paste,
                     ReadStage stage,
                     const gchar *password,
                     const gchar *human_name)
{
    ReadTransaction *transaction = g_new0(ReadTransaction, 1);

    transaction->id = id;
    transaction->location = location;
    transaction->is_paste = is_paste;
    transaction->stage = stage;
    transaction->password = g_strdup(password);
    transaction->human_name = g_strdup(human_name);
    transaction->order = g_ptr_array_new_with_free_func(g_free);
    transaction->buffers = g_hash_table_new_full(
        g_str_hash,
        g_str_equal,
        g_free,
        (GDestroyNotify)g_byte_array_unref);
    transaction->deadline_us = new_deadline();
    return transaction;
}

static gboolean
start_read(MuxKittyClipboard *clipboard,
           MuxOsc5522Location location,
           const gchar *const *mime_types,
           const gchar *password,
           const gchar *human_name,
           gboolean is_paste,
           ReadStage stage,
           GError **error)
{
    gchar *id;
    GBytes *packet;

    if (clipboard->read != NULL) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_BUSY,
                            "a Kitty clipboard read is already pending");
        return FALSE;
    }

    id = new_request_id(clipboard);
    packet = mux_osc5522_read_request(id,
                                      location,
                                      mime_types,
                                      password,
                                      human_name,
                                      error);
    if (packet == NULL) {
        g_free(id);
        return FALSE;
    }

    clipboard->read = read_transaction_new(id,
                                           location,
                                           is_paste,
                                           stage,
                                           password,
                                           human_name);
    if (!emit_packet(clipboard, packet, error)) {
        g_clear_pointer(&clipboard->read, read_transaction_free);
        return FALSE;
    }
    return TRUE;
}

static gboolean
read_add_data(ReadTransaction *transaction,
              const gchar *mime,
              GBytes *bytes,
              GError **error)
{
    GByteArray *buffer;
    const guint8 *data;
    gsize length;

    buffer = g_hash_table_lookup(transaction->buffers, mime);
    if (buffer == NULL) {
        if (transaction->order->len >= MUX_CLIPBOARD_MAX_ITEMS) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_NO_SPACE,
                                "Kitty clipboard returned too many MIME types");
            return FALSE;
        }
        buffer = g_byte_array_new();
        g_hash_table_insert(transaction->buffers,
                            g_strdup(mime),
                            buffer);
        g_ptr_array_add(transaction->order, g_strdup(mime));
    }

    data = g_bytes_get_data(bytes, &length);
    if (length > MUX_CLIPBOARD_MAX_ITEM_BYTES - buffer->len ||
        length > MUX_CLIPBOARD_MAX_TOTAL_BYTES - transaction->total_bytes) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NO_SPACE,
                            "Kitty clipboard data exceeds its byte limit");
        return FALSE;
    }

    if (length > 0)
        g_byte_array_append(buffer, data, length);
    transaction->total_bytes += length;
    transaction->deadline_us = new_deadline();
    return TRUE;
}

static MuxClipboardSnapshot *
finish_snapshot(MuxKittyClipboard *clipboard,
                ReadTransaction *transaction,
                GError **error)
{
    MuxClipboardSnapshot *snapshot =
        mux_clipboard_snapshot_new(++clipboard->next_serial);
    guint i;

    for (i = 0; i < transaction->order->len; i++) {
        const gchar *mime = g_ptr_array_index(transaction->order, i);
        GByteArray *buffer =
            g_hash_table_lookup(transaction->buffers, mime);
        GBytes *bytes;

        if (buffer->len == 0)
            bytes = g_bytes_new_static("", 0);
        else
            bytes = g_bytes_new(buffer->data, buffer->len);
        if (!mux_clipboard_snapshot_add(snapshot, mime, bytes, error)) {
            g_bytes_unref(bytes);
            mux_clipboard_snapshot_unref(snapshot);
            return NULL;
        }
        g_bytes_unref(bytes);
    }

    mux_clipboard_snapshot_seal(snapshot);
    return snapshot;
}

static gboolean launch_pending_offer(MuxKittyClipboard *clipboard);
static void activate_queued_write(MuxKittyClipboard *clipboard);

static GPtrArray *
parse_discovered_mimes(ReadTransaction *transaction, GError **error)
{
    GPtrArray *mimes = g_ptr_array_new_with_free_func(g_free);
    GByteArray *buffer;
    gchar **tokens = NULL;
    gchar *payload = NULL;
    guint i;

    if (transaction->order->len > 1 ||
        (transaction->order->len == 1 &&
         !g_str_equal(g_ptr_array_index(transaction->order, 0), "."))) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "Kitty MIME discovery returned an unexpected type");
        goto fail;
    }

    buffer = g_hash_table_lookup(transaction->buffers, ".");
    if (buffer == NULL || buffer->len == 0)
        goto done;
    if (memchr(buffer->data, '\0', buffer->len) != NULL ||
        !g_utf8_validate((const gchar *)buffer->data, buffer->len, NULL)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "Kitty MIME discovery returned invalid text");
        goto fail;
    }

    payload = g_strndup((const gchar *)buffer->data, buffer->len);
    tokens = g_strsplit_set(payload, " \t\r\n", -1);
    for (i = 0; tokens[i] != NULL; i++) {
        guint j;
        gboolean duplicate = FALSE;

        if (tokens[i][0] == '\0')
            continue;
        if (g_str_equal(tokens[i], ".") ||
            !mux_clipboard_mime_is_valid(tokens[i])) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_DATA,
                                "Kitty MIME discovery returned an invalid type");
            goto fail;
        }
        for (j = 0; j < mimes->len; j++) {
            if (g_str_equal(g_ptr_array_index(mimes, j), tokens[i])) {
                duplicate = TRUE;
                break;
            }
        }
        if (duplicate)
            continue;
        if (mimes->len >= MUX_CLIPBOARD_MAX_ITEMS) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_NO_SPACE,
                                "Kitty MIME discovery returned too many types");
            goto fail;
        }
        g_ptr_array_add(mimes, g_strdup(tokens[i]));
    }

done:
    g_ptr_array_add(mimes, NULL);
    g_strfreev(tokens);
    g_free(payload);
    return mimes;

fail:
    g_strfreev(tokens);
    g_free(payload);
    g_ptr_array_unref(mimes);
    return NULL;
}

static gboolean
complete_read(MuxKittyClipboard *clipboard, GError **error)
{
    ReadTransaction *transaction = clipboard->read;
    MuxClipboardSnapshot *snapshot;
    MuxOsc5522Location location;
    gboolean is_paste;

    if (transaction->stage == READ_STAGE_TARGETS) {
        GPtrArray *mimes = parse_discovered_mimes(transaction, error);
        g_autofree gchar *password = g_strdup(transaction->password);
        g_autofree gchar *human_name = g_strdup(transaction->human_name);
        gboolean result;

        clipboard->read = NULL;
        location = transaction->location;
        is_paste = transaction->is_paste;
        read_transaction_free(transaction);
        if (mimes == NULL) {
            launch_pending_offer(clipboard);
            return FALSE;
        }
        result = start_read(clipboard,
                            location,
                            (const gchar *const *)mimes->pdata,
                            password,
                            human_name,
                            is_paste,
                            READ_STAGE_CONTENT,
                            error);
        g_ptr_array_unref(mimes);
        if (!result)
            launch_pending_offer(clipboard);
        return result;
    }

    clipboard->read = NULL;
    snapshot = finish_snapshot(clipboard, transaction, error);
    location = transaction->location;
    is_paste = transaction->is_paste;
    read_transaction_free(transaction);
    if (snapshot == NULL) {
        launch_pending_offer(clipboard);
        return FALSE;
    }

    if (clipboard->receive_func != NULL)
        clipboard->receive_func(clipboard,
                                location,
                                snapshot,
                                is_paste,
                                clipboard->user_data);
    mux_clipboard_snapshot_unref(snapshot);
    launch_pending_offer(clipboard);
    return TRUE;
}

static gboolean
offer_set_text(gchar **slot,
               const gchar *value,
               const gchar *label,
               GError **error)
{
    if (value == NULL)
        return TRUE;
    if (*slot == NULL) {
        *slot = g_strdup(value);
        return TRUE;
    }
    if (g_str_equal(*slot, value))
        return TRUE;

    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_DATA,
                "Kitty paste offer changed its %s",
                label);
    return FALSE;
}

static gboolean
offer_add_mime(PasteOffer *offer, const gchar *mime, GError **error)
{
    guint i;

    for (i = 0; i < offer->mimes->len; i++) {
        if (g_str_equal(g_ptr_array_index(offer->mimes, i), mime))
            return TRUE;
    }
    if (offer->mimes->len >= MUX_CLIPBOARD_MAX_ITEMS) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NO_SPACE,
                            "Kitty paste offer has too many MIME types");
        return FALSE;
    }

    g_ptr_array_add(offer->mimes, g_strdup(mime));
    return TRUE;
}

static PasteOffer *
paste_offer_new(const MuxOsc5522Event *event)
{
    PasteOffer *offer = g_new0(PasteOffer, 1);

    offer->location = event->location;
    offer->mimes = g_ptr_array_new_with_free_func(g_free);
    offer->password = g_strdup(event->password);
    offer->human_name = g_strdup(event->human_name);
    offer->deadline_us = new_deadline();
    return offer;
}

static gboolean
start_offer_read(MuxKittyClipboard *clipboard,
                 PasteOffer *offer,
                 GError **error)
{
    const gchar **mimes;
    gboolean result;
    guint i;

    if (offer->mimes->len == 0) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "Kitty paste offer contains no MIME types");
        return FALSE;
    }

    mimes = g_new0(const gchar *, offer->mimes->len + 1);
    for (i = 0; i < offer->mimes->len; i++)
        mimes[i] = g_ptr_array_index(offer->mimes, i);
    result = start_read(clipboard,
                        offer->location,
                        mimes,
                        offer->password,
                        offer->human_name,
                        TRUE,
                        READ_STAGE_CONTENT,
                        error);
    g_free(mimes);
    return result;
}

static gboolean
launch_pending_offer(MuxKittyClipboard *clipboard)
{
    PasteOffer *offer;
    g_autoptr(GError) error = NULL;

    if (clipboard->read != NULL || clipboard->pending_offer == NULL)
        return FALSE;

    offer = clipboard->pending_offer;
    clipboard->pending_offer = NULL;
    if (!start_offer_read(clipboard, offer, &error))
        report_failure(clipboard, "paste-read", error);
    paste_offer_free(offer);
    return error == NULL;
}

static GIOErrorEnum
remote_error_code(MuxOsc5522RemoteError remote_error)
{
    switch (remote_error) {
    case MUX_OSC5522_REMOTE_ERROR_PERMISSION:
        return G_IO_ERROR_PERMISSION_DENIED;
    case MUX_OSC5522_REMOTE_ERROR_BUSY:
        return G_IO_ERROR_BUSY;
    case MUX_OSC5522_REMOTE_ERROR_UNSUPPORTED:
        return G_IO_ERROR_NOT_SUPPORTED;
    case MUX_OSC5522_REMOTE_ERROR_INVALID:
        return G_IO_ERROR_INVALID_DATA;
    case MUX_OSC5522_REMOTE_ERROR_IO:
    case MUX_OSC5522_REMOTE_ERROR_NONE:
    default:
        return G_IO_ERROR_FAILED;
    }
}

static const gchar *
remote_error_name(MuxOsc5522RemoteError remote_error)
{
    switch (remote_error) {
    case MUX_OSC5522_REMOTE_ERROR_IO:
        return "EIO";
    case MUX_OSC5522_REMOTE_ERROR_INVALID:
        return "EINVAL";
    case MUX_OSC5522_REMOTE_ERROR_UNSUPPORTED:
        return "ENOSYS";
    case MUX_OSC5522_REMOTE_ERROR_PERMISSION:
        return "EPERM";
    case MUX_OSC5522_REMOTE_ERROR_BUSY:
        return "EBUSY";
    case MUX_OSC5522_REMOTE_ERROR_NONE:
    default:
        return "EUNKNOWN";
    }
}

static void
handle_remote_error(MuxKittyClipboard *clipboard,
                    const MuxOsc5522Event *event)
{
    g_autoptr(GError) error = g_error_new(
        G_IO_ERROR,
        remote_error_code(event->remote_error),
        "Kitty rejected OSC 5522 clipboard transaction%s%s with %s",
        event->id != NULL ? " " : "",
        event->id != NULL ? event->id : "",
        remote_error_name(event->remote_error));

    if (event->id != NULL && clipboard->read != NULL &&
        g_str_equal(event->id, clipboard->read->id)) {
        g_clear_pointer(&clipboard->read, read_transaction_free);
        report_failure(clipboard, "clipboard-read", error);
        launch_pending_offer(clipboard);
        return;
    }
    if (event->id != NULL && clipboard->write != NULL &&
        g_str_equal(event->id, clipboard->write->id)) {
        g_clear_pointer(&clipboard->write, write_transaction_free);
        activate_queued_write(clipboard);
        report_failure(clipboard, "clipboard-write", error);
        return;
    }
    if (event->id == NULL) {
        g_clear_pointer(&clipboard->offer, paste_offer_free);
        report_failure(clipboard, "paste-offer", error);
    }
}

static WriteTransaction *
write_transaction_new(MuxKittyClipboard *clipboard,
                      MuxOsc5522Location location,
                      MuxClipboardSnapshot *snapshot)
{
    WriteTransaction *write = g_new0(WriteTransaction, 1);

    write->id = new_request_id(clipboard);
    write->location = location;
    write->snapshot = snapshot;
    write->stage = WRITE_STAGE_BEGIN;
    write->deadline_us = new_deadline();
    return write;
}

static void
activate_queued_write(MuxKittyClipboard *clipboard)
{
    PendingWrite *pending;

    if (clipboard->write != NULL || clipboard->queued_write == NULL)
        return;

    pending = clipboard->queued_write;
    clipboard->queued_write = NULL;
    clipboard->write = write_transaction_new(clipboard,
                                             pending->location,
                                             pending->snapshot);
    pending->snapshot = NULL;
    pending_write_free(pending);
}

static gboolean
flush_write_budget(MuxKittyClipboard *clipboard, GError **error)
{
    guint packet_count = 0;
    gsize byte_count = 0;

    while (clipboard->write != NULL &&
           clipboard->write->stage != WRITE_STAGE_WAIT_DONE &&
           packet_count < MUX_KITTY_CLIPBOARD_WRITE_PACKETS_PER_TICK &&
           byte_count < MUX_KITTY_CLIPBOARD_WRITE_BYTES_PER_TICK) {
        WriteTransaction *write = clipboard->write;
        WriteStage next_stage = write->stage;
        guint next_item_index = write->item_index;
        gsize next_item_offset = write->item_offset;
        WriteStage previous_stage = write->stage;
        guint previous_item_index = write->item_index;
        gsize previous_item_offset = write->item_offset;
        gint64 previous_deadline_us = write->deadline_us;
        GBytes *packet = NULL;
        gsize packet_size;

        switch (write->stage) {
        case WRITE_STAGE_BEGIN:
            packet = mux_osc5522_write_begin(write->id,
                                             write->location,
                                             NULL,
                                             "Mux browser",
                                             error);
            next_stage = mux_clipboard_snapshot_get_count(write->snapshot) > 0
                             ? WRITE_STAGE_DATA
                             : WRITE_STAGE_END;
            break;
        case WRITE_STAGE_DATA: {
            const gchar *mime = NULL;
            GBytes *bytes = NULL;
            const guint8 *data;
            gsize length;
            gsize amount;

            if (write->item_index >=
                mux_clipboard_snapshot_get_count(write->snapshot)) {
                write->stage = WRITE_STAGE_END;
                continue;
            }

            mux_clipboard_snapshot_get_item(write->snapshot,
                                            write->item_index,
                                            &mime,
                                            &bytes);
            data = g_bytes_get_data(bytes, &length);
            if (write->item_offset > length) {
                g_set_error_literal(error,
                                    G_IO_ERROR,
                                    G_IO_ERROR_INVALID_DATA,
                                    "clipboard write cursor exceeds item size");
                break;
            }

            amount = MIN((gsize)MUX_OSC5522_MAX_CHUNK,
                         length - write->item_offset);
            packet = mux_osc5522_write_data(
                mime,
                amount > 0 ? data + write->item_offset : NULL,
                amount,
                error);
            if (length == 0 || amount == length - write->item_offset) {
                next_item_index++;
                next_item_offset = 0;
                if (next_item_index >=
                    mux_clipboard_snapshot_get_count(write->snapshot))
                    next_stage = WRITE_STAGE_END;
            } else {
                next_item_offset += amount;
            }
            break;
        }
        case WRITE_STAGE_END:
            packet = mux_osc5522_write_end(error);
            next_stage = WRITE_STAGE_WAIT_DONE;
            break;
        case WRITE_STAGE_WAIT_DONE:
        default:
            break;
        }

        if (packet == NULL) {
            if (error != NULL && *error == NULL) {
                g_set_error_literal(error,
                                    G_IO_ERROR,
                                    G_IO_ERROR_FAILED,
                                    "could not encode Kitty clipboard packet");
            }
            if (clipboard->write == write)
                g_clear_pointer(&clipboard->write, write_transaction_free);
            activate_queued_write(clipboard);
            return FALSE;
        }

        packet_size = g_bytes_get_size(packet);
        if (packet_count > 0 &&
            packet_size >
                MUX_KITTY_CLIPBOARD_WRITE_BYTES_PER_TICK - byte_count) {
            g_bytes_unref(packet);
            break;
        }

        write->stage = next_stage;
        write->item_index = next_item_index;
        write->item_offset = next_item_offset;
        write->deadline_us = new_deadline();
        packet_count++;
        byte_count += packet_size;

        if (!emit_packet(clipboard, packet, error)) {
            if (error != NULL && *error != NULL &&
                g_error_matches(*error,
                                G_IO_ERROR,
                                G_IO_ERROR_WOULD_BLOCK)) {
                if (clipboard->write == write) {
                    write->stage = previous_stage;
                    write->item_index = previous_item_index;
                    write->item_offset = previous_item_offset;
                    write->deadline_us = previous_deadline_us;
                }
                g_clear_error(error);
                return TRUE;
            }
            if (error != NULL && *error == NULL) {
                g_set_error_literal(error,
                                    G_IO_ERROR,
                                    G_IO_ERROR_FAILED,
                                    "could not write Kitty clipboard packet");
            }
            if (clipboard->write == write)
                g_clear_pointer(&clipboard->write, write_transaction_free);
            if (clipboard->queued_write != NULL) {
                PendingWrite *queued = clipboard->queued_write;

                clipboard->queued_write = NULL;
                if (error != NULL && *error != NULL)
                    report_failure(clipboard,
                                   "queued-clipboard-write",
                                   *error);
                else
                    report_literal(clipboard,
                                   "queued-clipboard-write",
                                   G_IO_ERROR_BROKEN_PIPE,
                                   "terminal output failed");
                pending_write_free(queued);
            }
            return FALSE;
        }

        /* The output callback may synchronously complete or reject the write. */
        if (clipboard->write != write)
            break;
    }

    return TRUE;
}

MuxKittyClipboard *
mux_kitty_clipboard_new(MuxKittyClipboardOutputFunc output_func,
                        MuxKittyClipboardReceiveFunc receive_func,
                        MuxKittyClipboardFailureFunc failure_func,
                        gpointer user_data,
                        GDestroyNotify user_data_destroy)
{
    MuxKittyClipboard *clipboard;

    g_return_val_if_fail(output_func != NULL, NULL);

    clipboard = g_new0(MuxKittyClipboard, 1);
    clipboard->reference_count = 1;
    clipboard->output_func = output_func;
    clipboard->receive_func = receive_func;
    clipboard->failure_func = failure_func;
    clipboard->user_data = user_data;
    clipboard->user_data_destroy = user_data_destroy;
    clipboard->support = MUX_OSC5522_SUPPORT_UNKNOWN;
    clipboard->nonce = g_random_int();
    return clipboard;
}

MuxKittyClipboard *
mux_kitty_clipboard_ref(MuxKittyClipboard *clipboard)
{
    g_return_val_if_fail(clipboard != NULL, NULL);
    g_atomic_int_inc(&clipboard->reference_count);
    return clipboard;
}

void
mux_kitty_clipboard_unref(MuxKittyClipboard *clipboard)
{
    if (clipboard == NULL)
        return;
    if (!g_atomic_int_dec_and_test(&clipboard->reference_count))
        return;

    g_clear_pointer(&clipboard->read, read_transaction_free);
    g_clear_pointer(&clipboard->offer, paste_offer_free);
    g_clear_pointer(&clipboard->pending_offer, paste_offer_free);
    g_clear_pointer(&clipboard->write, write_transaction_free);
    g_clear_pointer(&clipboard->queued_write, pending_write_free);
    if (clipboard->user_data_destroy != NULL)
        clipboard->user_data_destroy(clipboard->user_data);
    g_free(clipboard);
}

gboolean
mux_kitty_clipboard_set_enabled(MuxKittyClipboard *clipboard,
                                gboolean enabled,
                                GError **error)
{
    MuxKittyClipboard *guard;
    gboolean result = FALSE;

    g_return_val_if_fail(clipboard != NULL, FALSE);
    guard = mux_kitty_clipboard_ref(clipboard);

    if (enabled == clipboard->enabled) {
        result = TRUE;
        goto out;
    }
    if (enabled &&
        !emit_packet(clipboard, mux_osc5522_query_support(), error))
        goto out;
    if (!emit_packet(clipboard,
                     mux_osc5522_set_paste_events(enabled),
                     error))
        goto out;

    clipboard->enabled = enabled;
    if (!enabled) {
        g_clear_pointer(&clipboard->read, read_transaction_free);
        g_clear_pointer(&clipboard->offer, paste_offer_free);
        g_clear_pointer(&clipboard->pending_offer, paste_offer_free);
    }
    result = TRUE;

out:
    mux_kitty_clipboard_unref(guard);
    return result;
}

gboolean
mux_kitty_clipboard_request(MuxKittyClipboard *clipboard,
                            MuxOsc5522Location location,
                            const gchar *const *mime_types,
                            const gchar *password,
                            const gchar *human_name,
                            gboolean is_paste,
                            GError **error)
{
    MuxKittyClipboard *guard;
    gboolean result;

    g_return_val_if_fail(clipboard != NULL, FALSE);
    guard = mux_kitty_clipboard_ref(clipboard);
    result = start_read(clipboard,
                        location,
                        mime_types,
                        password,
                        human_name,
                        is_paste,
                        READ_STAGE_CONTENT,
                        error);
    mux_kitty_clipboard_unref(guard);
    return result;
}

gboolean
mux_kitty_clipboard_request_all(MuxKittyClipboard *clipboard,
                                MuxOsc5522Location location,
                                const gchar *password,
                                const gchar *human_name,
                                gboolean is_paste,
                                GError **error)
{
    static const gchar *const targets[] = { ".", NULL };
    MuxKittyClipboard *guard;
    gboolean result;

    g_return_val_if_fail(clipboard != NULL, FALSE);
    guard = mux_kitty_clipboard_ref(clipboard);
    result = start_read(clipboard,
                        location,
                        targets,
                        password,
                        human_name,
                        is_paste,
                        READ_STAGE_TARGETS,
                        error);
    mux_kitty_clipboard_unref(guard);
    return result;
}

void
mux_kitty_clipboard_cancel_read(MuxKittyClipboard *clipboard)
{
    MuxKittyClipboard *guard;

    g_return_if_fail(clipboard != NULL);
    guard = mux_kitty_clipboard_ref(clipboard);
    g_clear_pointer(&clipboard->read, read_transaction_free);
    launch_pending_offer(clipboard);
    mux_kitty_clipboard_unref(guard);
}

gboolean
mux_kitty_clipboard_publish(MuxKittyClipboard *clipboard,
                            MuxOsc5522Location location,
                            const MuxClipboardSnapshot *snapshot,
                            GError **error)
{
    MuxKittyClipboard *guard;
    MuxClipboardSnapshot *copy;
    gboolean result = TRUE;

    g_return_val_if_fail(clipboard != NULL, FALSE);
    g_return_val_if_fail(snapshot != NULL, FALSE);
    guard = mux_kitty_clipboard_ref(clipboard);

    copy = mux_clipboard_snapshot_dup_sealed(snapshot);
    if (copy == NULL) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "clipboard snapshot could not be copied");
        result = FALSE;
        goto out;
    }

    if (clipboard->write != NULL) {
        PendingWrite *write = g_new0(PendingWrite, 1);

        write->location = location;
        write->snapshot = copy;
        g_clear_pointer(&clipboard->queued_write, pending_write_free);
        clipboard->queued_write = write;
        copy = NULL;
    } else {
        clipboard->write = write_transaction_new(clipboard, location, copy);
        copy = NULL;
        result = flush_write_budget(clipboard, error);
    }

    if (copy != NULL)
        mux_clipboard_snapshot_unref(copy);

out:
    mux_kitty_clipboard_unref(guard);
    return result;
}

gboolean
mux_kitty_clipboard_handle_support(MuxKittyClipboard *clipboard,
                                   const guint8 *sequence,
                                   gsize length,
                                   GError **error)
{
    MuxOsc5522Support support;

    g_return_val_if_fail(clipboard != NULL, FALSE);
    if (!mux_osc5522_parse_support(sequence, length, &support, error))
        return FALSE;
    clipboard->support = support;
    return TRUE;
}

static gboolean
handle_offer_event(MuxKittyClipboard *clipboard,
                   MuxOsc5522Event *event,
                   GError **error)
{
    gsize data_length = 0;

    switch (event->type) {
    case MUX_OSC5522_EVENT_READ_OK:
        g_clear_pointer(&clipboard->offer, paste_offer_free);
        clipboard->offer = paste_offer_new(event);
        return TRUE;
    case MUX_OSC5522_EVENT_READ_DATA:
        if (clipboard->offer == NULL) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_DATA,
                                "Kitty paste data arrived before its offer");
            return FALSE;
        }
        g_bytes_get_data(event->data, &data_length);
        if (data_length != 0 ||
            event->location != clipboard->offer->location ||
            !offer_set_text(&clipboard->offer->password,
                            event->password,
                            "password",
                            error) ||
            !offer_set_text(&clipboard->offer->human_name,
                            event->human_name,
                            "name",
                            error) ||
            !offer_add_mime(clipboard->offer, event->mime, error)) {
            g_clear_pointer(&clipboard->offer, paste_offer_free);
            return FALSE;
        }
        clipboard->offer->deadline_us = new_deadline();
        return TRUE;
    case MUX_OSC5522_EVENT_READ_DONE: {
        PasteOffer *offer = clipboard->offer;

        if (offer == NULL) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_DATA,
                                "Kitty paste offer completed before it began");
            return FALSE;
        }
        clipboard->offer = NULL;
        if (clipboard->read != NULL) {
            g_clear_pointer(&clipboard->pending_offer, paste_offer_free);
            clipboard->pending_offer = offer;
            return TRUE;
        }
        if (!start_offer_read(clipboard, offer, error)) {
            paste_offer_free(offer);
            return FALSE;
        }
        paste_offer_free(offer);
        return TRUE;
    }
    case MUX_OSC5522_EVENT_ERROR:
        handle_remote_error(clipboard, event);
        return TRUE;
    case MUX_OSC5522_EVENT_WRITE_DONE:
    default:
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "invalid unsolicited Kitty clipboard event");
        return FALSE;
    }
}

static gboolean
parse_terminal_event(const guint8 *sequence,
                     gsize length,
                     MuxOsc5522Event **event,
                     GError **error)
{
    static const guint8 prefix[] = "\033]5522;";
    const gsize prefix_length = sizeof(prefix) - 1U;
    g_autofree guint8 *normalized = NULL;
    gsize body_end;
    gsize terminator_length;

    if (sequence == NULL || length <= prefix_length ||
        memcmp(sequence, prefix, prefix_length) != 0)
        return mux_osc5522_parse(sequence, length, event, error);

    if (sequence[length - 1U] == '\a') {
        terminator_length = 1U;
    } else if (length >= 2U && sequence[length - 2U] == '\033' &&
               sequence[length - 1U] == '\\') {
        terminator_length = 2U;
    } else {
        return mux_osc5522_parse(sequence, length, event, error);
    }

    body_end = length - terminator_length;
    if (memchr(sequence + prefix_length,
               ';',
               body_end - prefix_length) != NULL ||
        length >= MUX_OSC5522_MAX_SEQUENCE) {
        return mux_osc5522_parse(sequence, length, event, error);
    }

    /* Kitty follows the OSC 5522 examples and omits the payload separator
     * on responses with no payload. The shared codec uses an explicit empty
     * payload internally, so normalize only that official wire form here. */
    normalized = g_malloc(length + 1U);
    memcpy(normalized, sequence, body_end);
    normalized[body_end] = ';';
    memcpy(normalized + body_end + 1U,
           sequence + body_end,
           terminator_length);
    return mux_osc5522_parse(normalized, length + 1U, event, error);
}

gboolean
mux_kitty_clipboard_osc_matches_pending_read(
    const MuxKittyClipboard *clipboard,
    const guint8 *sequence,
    gsize length,
    gboolean *matches,
    GError **error)
{
    MuxOsc5522Event *event = NULL;

    g_return_val_if_fail(clipboard != NULL, FALSE);
    g_return_val_if_fail(matches != NULL, FALSE);
    *matches = FALSE;
    if (!parse_terminal_event(sequence, length, &event, error))
        return FALSE;
    if (clipboard->read != NULL && event->id != NULL &&
        g_str_equal(event->id, clipboard->read->id))
        *matches = TRUE;
    mux_osc5522_event_free(event);
    return TRUE;
}

gboolean
mux_kitty_clipboard_handle_osc(MuxKittyClipboard *clipboard,
                               const guint8 *sequence,
                               gsize length,
                               GError **error)
{
    MuxKittyClipboard *guard;
    MuxOsc5522Event *event = NULL;
    gboolean result = FALSE;

    g_return_val_if_fail(clipboard != NULL, FALSE);
    guard = mux_kitty_clipboard_ref(clipboard);
    if (!parse_terminal_event(sequence, length, &event, error))
        goto out;

    if (event->id == NULL) {
        result = handle_offer_event(clipboard, event, error);
        goto out;
    }
    if (event->type == MUX_OSC5522_EVENT_ERROR) {
        handle_remote_error(clipboard, event);
        result = TRUE;
        goto out;
    }
    if (event->type == MUX_OSC5522_EVENT_WRITE_DONE) {
        if (clipboard->write != NULL &&
            clipboard->write->stage == WRITE_STAGE_WAIT_DONE &&
            g_str_equal(event->id, clipboard->write->id)) {
            g_clear_pointer(&clipboard->write, write_transaction_free);
            activate_queued_write(clipboard);
        }
        result = TRUE;
        goto out;
    }
    if (clipboard->read == NULL ||
        !g_str_equal(event->id, clipboard->read->id)) {
        result = TRUE;
        goto out;
    }
    if (event->location != clipboard->read->location) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "Kitty clipboard response changed location");
        g_clear_pointer(&clipboard->read, read_transaction_free);
        launch_pending_offer(clipboard);
        goto out;
    }

    switch (event->type) {
    case MUX_OSC5522_EVENT_READ_OK:
        clipboard->read->acknowledged = TRUE;
        clipboard->read->deadline_us = new_deadline();
        result = TRUE;
        break;
    case MUX_OSC5522_EVENT_READ_DATA:
        if (!clipboard->read->acknowledged) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_DATA,
                                "Kitty clipboard data arrived before acknowledgement");
            g_clear_pointer(&clipboard->read, read_transaction_free);
            launch_pending_offer(clipboard);
            break;
        }
        result = read_add_data(clipboard->read,
                               event->mime,
                               event->data,
                               error);
        if (!result) {
            g_clear_pointer(&clipboard->read, read_transaction_free);
            launch_pending_offer(clipboard);
        }
        break;
    case MUX_OSC5522_EVENT_READ_DONE:
        if (!clipboard->read->acknowledged) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_DATA,
                                "Kitty clipboard completed before acknowledgement");
            g_clear_pointer(&clipboard->read, read_transaction_free);
            launch_pending_offer(clipboard);
            break;
        }
        result = complete_read(clipboard, error);
        break;
    case MUX_OSC5522_EVENT_ERROR:
    case MUX_OSC5522_EVENT_WRITE_DONE:
    default:
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "invalid Kitty clipboard read response");
        g_clear_pointer(&clipboard->read, read_transaction_free);
        launch_pending_offer(clipboard);
        break;
    }

out:
    mux_osc5522_event_free(event);
    mux_kitty_clipboard_unref(guard);
    return result;
}

guint
mux_kitty_clipboard_tick(MuxKittyClipboard *clipboard,
                         gint64 monotonic_us)
{
    MuxKittyClipboard *guard;
    guint expired = 0;

    g_return_val_if_fail(clipboard != NULL, 0);
    guard = mux_kitty_clipboard_ref(clipboard);

    if (clipboard->read != NULL &&
        clipboard->read->deadline_us <= monotonic_us) {
        g_clear_pointer(&clipboard->read, read_transaction_free);
        report_literal(clipboard,
                       "clipboard-read",
                       G_IO_ERROR_TIMED_OUT,
                       "Kitty clipboard read timed out");
        expired++;
    }
    if (clipboard->offer != NULL &&
        clipboard->offer->deadline_us <= monotonic_us) {
        g_clear_pointer(&clipboard->offer, paste_offer_free);
        report_literal(clipboard,
                       "paste-offer",
                       G_IO_ERROR_TIMED_OUT,
                       "Kitty paste offer timed out");
        expired++;
    }
    if (clipboard->pending_offer != NULL &&
        clipboard->pending_offer->deadline_us <= monotonic_us) {
        g_clear_pointer(&clipboard->pending_offer, paste_offer_free);
        report_literal(clipboard,
                       "paste-read",
                       G_IO_ERROR_TIMED_OUT,
                       "queued Kitty paste request timed out");
        expired++;
    }
    if (clipboard->write != NULL &&
        clipboard->write->deadline_us <= monotonic_us) {
        g_clear_pointer(&clipboard->write, write_transaction_free);
        activate_queued_write(clipboard);
        report_literal(clipboard,
                       "clipboard-write",
                       G_IO_ERROR_TIMED_OUT,
                       "Kitty clipboard write timed out");
        expired++;
    }

    launch_pending_offer(clipboard);
    activate_queued_write(clipboard);
    if (clipboard->write != NULL &&
        clipboard->write->stage != WRITE_STAGE_WAIT_DONE) {
        g_autoptr(GError) error = NULL;

        if (!flush_write_budget(clipboard, &error))
            report_failure(clipboard, "clipboard-write", error);
    }
    mux_kitty_clipboard_unref(guard);
    return expired;
}

MuxOsc5522Support
mux_kitty_clipboard_get_support(const MuxKittyClipboard *clipboard)
{
    g_return_val_if_fail(clipboard != NULL,
                         MUX_OSC5522_SUPPORT_UNKNOWN);
    return clipboard->support;
}

gboolean
mux_kitty_clipboard_read_pending(const MuxKittyClipboard *clipboard)
{
    g_return_val_if_fail(clipboard != NULL, FALSE);
    return clipboard->read != NULL;
}

gboolean
mux_kitty_clipboard_write_pending(const MuxKittyClipboard *clipboard)
{
    g_return_val_if_fail(clipboard != NULL, FALSE);
    return clipboard->write != NULL || clipboard->queued_write != NULL;
}
