#ifndef MUX_OSC5522_H
#define MUX_OSC5522_H

#include <glib.h>

G_BEGIN_DECLS

#define MUX_OSC5522_MAX_CHUNK 4096U
#define MUX_OSC5522_MAX_SEQUENCE (64U * 1024U)
#define MUX_OSC5522_MAX_ID 128U
#define MUX_OSC5522_MAX_MIME 512U
#define MUX_OSC5522_MAX_MIME_TYPES 32U
#define MUX_OSC5522_MAX_TEXT 1024U

typedef enum {
    MUX_OSC5522_CODEC_ERROR_INVALID,
    MUX_OSC5522_CODEC_ERROR_LIMIT
} MuxOsc5522CodecError;

#define MUX_OSC5522_ERROR (mux_osc5522_error_quark())
GQuark mux_osc5522_error_quark(void);

typedef enum {
    MUX_OSC5522_LOCATION_CLIPBOARD,
    MUX_OSC5522_LOCATION_PRIMARY
} MuxOsc5522Location;

typedef enum {
    MUX_OSC5522_SUPPORT_UNKNOWN,
    MUX_OSC5522_SUPPORT_ENABLED,
    MUX_OSC5522_SUPPORT_DISABLED,
    MUX_OSC5522_SUPPORT_PERMANENTLY_ENABLED,
    MUX_OSC5522_SUPPORT_PERMANENTLY_DISABLED
} MuxOsc5522Support;

typedef enum {
    MUX_OSC5522_EVENT_READ_OK,
    MUX_OSC5522_EVENT_READ_DATA,
    MUX_OSC5522_EVENT_READ_DONE,
    MUX_OSC5522_EVENT_WRITE_DONE,
    MUX_OSC5522_EVENT_ERROR
} MuxOsc5522EventType;

typedef enum {
    MUX_OSC5522_REMOTE_ERROR_NONE,
    MUX_OSC5522_REMOTE_ERROR_IO,
    MUX_OSC5522_REMOTE_ERROR_INVALID,
    MUX_OSC5522_REMOTE_ERROR_UNSUPPORTED,
    MUX_OSC5522_REMOTE_ERROR_PERMISSION,
    MUX_OSC5522_REMOTE_ERROR_BUSY
} MuxOsc5522RemoteError;

typedef struct {
    MuxOsc5522EventType type;
    MuxOsc5522RemoteError remote_error;
    MuxOsc5522Location location;
    gchar *id;
    gchar *mime;
    gchar *password;
    gchar *human_name;
    GBytes *data;
} MuxOsc5522Event;

GBytes *mux_osc5522_set_paste_events(gboolean enabled);
GBytes *mux_osc5522_query_support(void);

gboolean mux_osc5522_parse_support(const guint8 *sequence,
                                   gsize length,
                                   MuxOsc5522Support *support,
                                   GError **error);

GBytes *mux_osc5522_read_request(const gchar *id,
                                 MuxOsc5522Location location,
                                 const gchar *const *mime_types,
                                 const gchar *password,
                                 const gchar *human_name,
                                 GError **error);

GBytes *mux_osc5522_read_mime_request(const gchar *id,
                                      MuxOsc5522Location location,
                                      const gchar *mime,
                                      const gchar *password,
                                      const gchar *human_name,
                                      GError **error);

GBytes *mux_osc5522_write_begin(const gchar *id,
                                MuxOsc5522Location location,
                                const gchar *password,
                                const gchar *human_name,
                                GError **error);

GBytes *mux_osc5522_write_data(const gchar *mime,
                               const guint8 *data,
                               gsize length,
                               GError **error);

GBytes *mux_osc5522_write_end(GError **error);

gboolean mux_osc5522_parse(const guint8 *sequence,
                           gsize length,
                           MuxOsc5522Event **event,
                           GError **error);

void mux_osc5522_event_free(MuxOsc5522Event *event);

G_END_DECLS

#endif
