#ifndef MUX_EXTENSION_PROTOCOL_H
#define MUX_EXTENSION_PROTOCOL_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define MUX_EXTENSION_VERSION 1U
#define MUX_EXTENSION_HEADER_SIZE 24U
#define MUX_EXTENSION_MAX_PACKET (256U * 1024U)
#define MUX_EXTENSION_MAX_PAYLOAD \
    (MUX_EXTENSION_MAX_PACKET - MUX_EXTENSION_HEADER_SIZE)

typedef enum {
    MUX_EXTENSION_ERROR_INVALID,
    MUX_EXTENSION_ERROR_LIMIT
} MuxExtensionError;

#define MUX_EXTENSION_ERROR (mux_extension_error_quark())
GQuark mux_extension_error_quark(void);

typedef enum {
    MUX_EXTENSION_CHANNEL_UI = 1,
    MUX_EXTENSION_CHANNEL_CLIPBOARD = 2,
    MUX_EXTENSION_CHANNEL_CLIPBOARD_BROKER = 3,
    MUX_EXTENSION_CHANNEL_DIAGNOSTIC = 4
} MuxExtensionChannel;

#define MUX_EXTENSION_MAX_CHANNEL 16U

typedef struct {
    guint16 channel;
    guint32 flags;
    GBytes *payload;
} MuxExtensionRecord;

GBytes *mux_extension_record_encode(const MuxExtensionRecord *record,
                                    GError **error);
gboolean mux_extension_record_decode(const guint8 *packet,
                                     gsize packet_length,
                                     MuxExtensionRecord *record,
                                     GError **error);
void mux_extension_record_clear(MuxExtensionRecord *record);

G_END_DECLS

#endif
