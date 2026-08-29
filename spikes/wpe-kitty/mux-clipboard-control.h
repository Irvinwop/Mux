#ifndef MUX_CLIPBOARD_CONTROL_H
#define MUX_CLIPBOARD_CONTROL_H

#include "mux-clipboard.h"

G_BEGIN_DECLS

#define MUX_CLIPBOARD_CONTROL_VERSION 1U
#define MUX_CLIPBOARD_CONTROL_HEADER_SIZE 48U
#define MUX_CLIPBOARD_CONTROL_MAX_PACKET (16U * 1024U)
#define MUX_CLIPBOARD_CONTROL_MAX_PAYLOAD \
    (MUX_CLIPBOARD_CONTROL_MAX_PACKET - MUX_CLIPBOARD_CONTROL_HEADER_SIZE)
#define MUX_CLIPBOARD_CONTROL_MAX_TEXT 4096U

typedef enum {
    MUX_CLIPBOARD_CONTROL_ERROR_INVALID,
    MUX_CLIPBOARD_CONTROL_ERROR_LIMIT
} MuxClipboardControlError;

#define MUX_CLIPBOARD_CONTROL_ERROR \
    (mux_clipboard_control_error_quark())
GQuark mux_clipboard_control_error_quark(void);

typedef enum {
    MUX_CLIPBOARD_CONTROL_HELLO = 1,
    MUX_CLIPBOARD_CONTROL_LIST = 2,
    MUX_CLIPBOARD_CONTROL_SUMMARY = 3,
    MUX_CLIPBOARD_CONTROL_LIST_DONE = 4,
    MUX_CLIPBOARD_CONTROL_SELECT = 5,
    MUX_CLIPBOARD_CONTROL_DELETE = 6,
    MUX_CLIPBOARD_CONTROL_PIN = 7,
    MUX_CLIPBOARD_CONTROL_CLEAR = 8,
    MUX_CLIPBOARD_CONTROL_OK = 9,
    MUX_CLIPBOARD_CONTROL_REMOTE_ERROR = 10,
    MUX_CLIPBOARD_CONTROL_BYE = 11
} MuxClipboardControlType;

typedef enum {
    MUX_CLIPBOARD_CONTROL_FLAG_MODE_DISABLED = 1U << 0,
    MUX_CLIPBOARD_CONTROL_FLAG_MODE_EPHEMERAL = 1U << 1,
    MUX_CLIPBOARD_CONTROL_FLAG_PINNED = 1U << 2,
    MUX_CLIPBOARD_CONTROL_FLAG_INCLUDE_PINNED = 1U << 3,
    MUX_CLIPBOARD_CONTROL_FLAG_PASTE = 1U << 4
} MuxClipboardControlFlags;

#define MUX_CLIPBOARD_CONTROL_FLAGS_ALL \
    (MUX_CLIPBOARD_CONTROL_FLAG_MODE_DISABLED | \
     MUX_CLIPBOARD_CONTROL_FLAG_MODE_EPHEMERAL | \
     MUX_CLIPBOARD_CONTROL_FLAG_PINNED | \
     MUX_CLIPBOARD_CONTROL_FLAG_INCLUDE_PINNED | \
     MUX_CLIPBOARD_CONTROL_FLAG_PASTE)

typedef struct {
    MuxClipboardControlType type;
    guint32 flags;
    guint64 request_id;
    guint64 entry_id;
    gint64 created_us;
    GBytes *payload;
} MuxClipboardControlRecord;

typedef struct {
    guint64 entry_id;
    gint64 created_us;
    gboolean pinned;
    gchar *source_origin;
    gchar *preview;
    guint64 source_view_id;
    guint32 format_count;
    gchar **mime_types;
    guint32 mime_type_count;
    guint64 total_bytes;
} MuxClipboardControlSummary;

GBytes *mux_clipboard_control_record_encode(
    const MuxClipboardControlRecord *record,
    GError **error);
gboolean mux_clipboard_control_record_decode(
    const guint8 *packet,
    gsize packet_length,
    MuxClipboardControlRecord *record,
    GError **error);
void mux_clipboard_control_record_clear(
    MuxClipboardControlRecord *record);

GBytes *mux_clipboard_control_summary_encode(
    guint64 request_id,
    const MuxClipboardControlSummary *summary,
    GError **error);
gboolean mux_clipboard_control_summary_decode(
    const MuxClipboardControlRecord *record,
    MuxClipboardControlSummary *summary,
    GError **error);
void mux_clipboard_control_summary_clear(
    MuxClipboardControlSummary *summary);

gboolean mux_clipboard_control_payload_text(
    const MuxClipboardControlRecord *record,
    gsize max_length,
    gboolean allow_empty,
    gchar **text,
    GError **error);

G_END_DECLS

#endif
