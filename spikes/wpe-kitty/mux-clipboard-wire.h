#ifndef MUX_CLIPBOARD_WIRE_H
#define MUX_CLIPBOARD_WIRE_H

#include "mux-clipboard.h"

G_BEGIN_DECLS

#define MUX_CLIPBOARD_WIRE_VERSION 1U
#define MUX_CLIPBOARD_WIRE_HEADER_SIZE 64U
#define MUX_CLIPBOARD_WIRE_MAX_PACKET (256U * 1024U)
#define MUX_CLIPBOARD_WIRE_MAX_CHUNK (192U * 1024U)
#define MUX_CLIPBOARD_WIRE_MAX_PROFILE 128U
#define MUX_CLIPBOARD_WIRE_MAX_ORIGIN 2048U
#define MUX_CLIPBOARD_WIRE_TIMEOUT_MS 10000U

typedef enum {
    MUX_CLIPBOARD_WIRE_ERROR_INVALID,
    MUX_CLIPBOARD_WIRE_ERROR_LIMIT,
    MUX_CLIPBOARD_WIRE_ERROR_STATE
} MuxClipboardWireError;

#define MUX_CLIPBOARD_WIRE_ERROR (mux_clipboard_wire_error_quark())
GQuark mux_clipboard_wire_error_quark(void);

typedef enum {
    MUX_CLIPBOARD_WIRE_SNAPSHOT_BEGIN = 1,
    MUX_CLIPBOARD_WIRE_ITEM_BEGIN = 2,
    MUX_CLIPBOARD_WIRE_ITEM_DATA = 3,
    MUX_CLIPBOARD_WIRE_SNAPSHOT_COMMIT = 4,
    MUX_CLIPBOARD_WIRE_CANCEL = 5,
    MUX_CLIPBOARD_WIRE_ACK = 6,
    MUX_CLIPBOARD_WIRE_REMOTE_ERROR = 7
} MuxClipboardWireType;

typedef enum {
    MUX_CLIPBOARD_WIRE_FLAG_CURRENT = 1U << 0,
    MUX_CLIPBOARD_WIRE_FLAG_PASTE = 1U << 1,
    MUX_CLIPBOARD_WIRE_FLAG_HISTORY = 1U << 2,
    MUX_CLIPBOARD_WIRE_FLAG_EPHEMERAL = 1U << 3,
    MUX_CLIPBOARD_WIRE_FLAG_PRIMARY = 1U << 4
} MuxClipboardWireFlags;

#define MUX_CLIPBOARD_WIRE_FLAGS_ALL \
    (MUX_CLIPBOARD_WIRE_FLAG_CURRENT | \
     MUX_CLIPBOARD_WIRE_FLAG_PASTE | \
     MUX_CLIPBOARD_WIRE_FLAG_HISTORY | \
     MUX_CLIPBOARD_WIRE_FLAG_EPHEMERAL | \
     MUX_CLIPBOARD_WIRE_FLAG_PRIMARY)

typedef struct {
    MuxClipboardWireType type;
    guint32 flags;
    guint64 transaction_id;
    guint64 serial;
    guint64 source_view_id;
    gint64 created_us;
    guint32 item_index;
    guint32 item_count;
    GBytes *payload;
} MuxClipboardWireRecord;

GBytes *mux_clipboard_wire_record_encode(
    const MuxClipboardWireRecord *record,
    GError **error);
gboolean mux_clipboard_wire_record_decode(const guint8 *packet,
                                          gsize packet_length,
                                          MuxClipboardWireRecord *record,
                                          GError **error);
void mux_clipboard_wire_record_clear(MuxClipboardWireRecord *record);

typedef gboolean (*MuxClipboardWireSendFunc)(GBytes *packet,
                                             gpointer user_data,
                                             GError **error);

gboolean mux_clipboard_wire_send_snapshot(
    guint64 transaction_id,
    guint32 flags,
    const gchar *profile,
    const gchar *source_origin,
    guint64 source_view_id,
    gint64 created_us,
    const MuxClipboardSnapshot *snapshot,
    MuxClipboardWireSendFunc send_func,
    gpointer user_data,
    GError **error);

typedef struct _MuxClipboardWireTransfer MuxClipboardWireTransfer;
typedef struct _MuxClipboardWireAssembler MuxClipboardWireAssembler;

typedef enum {
    MUX_CLIPBOARD_WIRE_FEED_ACCEPTED,
    MUX_CLIPBOARD_WIRE_FEED_COMPLETED,
    MUX_CLIPBOARD_WIRE_FEED_REJECTED,
    MUX_CLIPBOARD_WIRE_FEED_CANCELLED
} MuxClipboardWireFeedResult;

MuxClipboardWireAssembler *mux_clipboard_wire_assembler_new(
    gsize max_snapshot_bytes);
void mux_clipboard_wire_assembler_free(
    MuxClipboardWireAssembler *assembler);
void mux_clipboard_wire_assembler_reset(
    MuxClipboardWireAssembler *assembler);

MuxClipboardWireFeedResult mux_clipboard_wire_assembler_feed(
    MuxClipboardWireAssembler *assembler,
    const guint8 *packet,
    gsize packet_length,
    gint64 monotonic_us,
    MuxClipboardWireTransfer **completed,
    GError **error);

gboolean mux_clipboard_wire_assembler_tick(
    MuxClipboardWireAssembler *assembler,
    gint64 monotonic_us);

void mux_clipboard_wire_transfer_free(MuxClipboardWireTransfer *transfer);
guint64 mux_clipboard_wire_transfer_get_transaction_id(
    const MuxClipboardWireTransfer *transfer);
guint32 mux_clipboard_wire_transfer_get_flags(
    const MuxClipboardWireTransfer *transfer);
const gchar *mux_clipboard_wire_transfer_get_profile(
    const MuxClipboardWireTransfer *transfer);
const gchar *mux_clipboard_wire_transfer_get_source_origin(
    const MuxClipboardWireTransfer *transfer);
guint64 mux_clipboard_wire_transfer_get_source_view_id(
    const MuxClipboardWireTransfer *transfer);
gint64 mux_clipboard_wire_transfer_get_created_us(
    const MuxClipboardWireTransfer *transfer);
const MuxClipboardSnapshot *mux_clipboard_wire_transfer_get_snapshot(
    const MuxClipboardWireTransfer *transfer);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxClipboardWireAssembler,
                              mux_clipboard_wire_assembler_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxClipboardWireTransfer,
                              mux_clipboard_wire_transfer_free)

G_END_DECLS

#endif
