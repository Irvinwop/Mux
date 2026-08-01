#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define MUX_ENGINE_MAGIC 0x4d555831u
#define MUX_ENGINE_VERSION 2u
#define MUX_ENGINE_HEADER_SIZE 40u
#define MUX_ENGINE_MAX_PAYLOAD (256u * 1024u)
#define MUX_ENGINE_MAX_DAMAGE_RECTS 64u
#define MUX_ENGINE_MAX_TEXT_BYTES (64u * 1024u)

typedef enum {
    MUX_ENGINE_MESSAGE_HELLO = 1,
    MUX_ENGINE_MESSAGE_WELCOME,
    MUX_ENGINE_MESSAGE_CREATE_VIEW,
    MUX_ENGINE_MESSAGE_VIEW_CREATED,
    MUX_ENGINE_MESSAGE_DESTROY_VIEW,
    MUX_ENGINE_MESSAGE_RESIZE,
    MUX_ENGINE_MESSAGE_NAVIGATE,
    MUX_ENGINE_MESSAGE_INPUT_KEY,
    MUX_ENGINE_MESSAGE_INPUT_POINTER,
    MUX_ENGINE_MESSAGE_SET_FOCUS,
    MUX_ENGINE_MESSAGE_FRAME,
    MUX_ENGINE_MESSAGE_FRAME_ACK,
    MUX_ENGINE_MESSAGE_METADATA,
    MUX_ENGINE_MESSAGE_ACK,
    MUX_ENGINE_MESSAGE_ERROR,
    MUX_ENGINE_MESSAGE_PING,
    MUX_ENGINE_MESSAGE_PONG,
    MUX_ENGINE_MESSAGE_TEXT_COMMIT,
    MUX_ENGINE_MESSAGE_REQUEST_CLOSE,
    MUX_ENGINE_MESSAGE_CLOSE_READY,
    MUX_ENGINE_MESSAGE_SET_VISIBILITY,
    MUX_ENGINE_MESSAGE_CANCEL_CLOSE,
    MUX_ENGINE_MESSAGE_EXTENSION = 64,
} MuxEngineMessageType;

typedef enum {
    MUX_ENGINE_FLAG_NONE = 0,
    MUX_ENGINE_FLAG_FULL_DAMAGE = 1u << 0,
    MUX_ENGINE_FLAG_REPLACES_PENDING = 1u << 1,
    MUX_ENGINE_FLAG_EPHEMERAL = 1u << 2,
} MuxEngineMessageFlags;

typedef enum {
    MUX_ENGINE_METADATA_AUDIO_PLAYING = 1u << 0,
    MUX_ENGINE_METADATA_AUDIO_MUTED = 1u << 1,
    MUX_ENGINE_METADATA_CAMERA_ACTIVE = 1u << 2,
    MUX_ENGINE_METADATA_CAMERA_MUTED = 1u << 3,
    MUX_ENGINE_METADATA_MICROPHONE_ACTIVE = 1u << 4,
    MUX_ENGINE_METADATA_MICROPHONE_MUTED = 1u << 5,
    MUX_ENGINE_METADATA_DISPLAY_ACTIVE = 1u << 6,
    MUX_ENGINE_METADATA_DISPLAY_MUTED = 1u << 7,
    MUX_ENGINE_METADATA_FULLSCREEN = 1u << 8,
} MuxEngineMetadataFlags;

typedef enum {
    MUX_ENGINE_PIXEL_RGBA8888 = 1,
} MuxEnginePixelFormat;

typedef enum {
    MUX_ENGINE_NAVIGATE_LOAD = 1,
    MUX_ENGINE_NAVIGATE_BACK,
    MUX_ENGINE_NAVIGATE_FORWARD,
    MUX_ENGINE_NAVIGATE_RELOAD,
    MUX_ENGINE_NAVIGATE_STOP,
} MuxEngineNavigationAction;

typedef enum {
    MUX_ENGINE_KEY_PRESS = 1,
    MUX_ENGINE_KEY_REPEAT,
    MUX_ENGINE_KEY_RELEASE,
} MuxEngineKeyEvent;

typedef enum {
    MUX_ENGINE_POINTER_MOVE = 1,
    MUX_ENGINE_POINTER_DOWN,
    MUX_ENGINE_POINTER_UP,
    MUX_ENGINE_POINTER_ENTER,
    MUX_ENGINE_POINTER_LEAVE,
    MUX_ENGINE_POINTER_SCROLL,
} MuxEnginePointerEvent;

typedef enum {
    MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE = 1,
    MUX_ENGINE_REMOTE_ERROR_BAD_STATE,
    MUX_ENGINE_REMOTE_ERROR_NOT_FOUND,
    MUX_ENGINE_REMOTE_ERROR_NOT_OWNER,
    MUX_ENGINE_REMOTE_ERROR_LIMIT,
    MUX_ENGINE_REMOTE_ERROR_NOT_IMPLEMENTED,
    MUX_ENGINE_REMOTE_ERROR_INTERNAL,
} MuxEngineRemoteError;

typedef enum {
    MUX_ENGINE_ERROR_CLOSED,
    MUX_ENGINE_ERROR_IO,
    MUX_ENGINE_ERROR_PROTOCOL,
    MUX_ENGINE_ERROR_TOO_LARGE,
} MuxEngineError;

#define MUX_ENGINE_ERROR (mux_engine_error_quark())
GQuark mux_engine_error_quark(void);

typedef struct {
    guint16 type;
    guint32 flags;
    guint64 view_id;
    guint64 serial;
    GBytes *payload;
} MuxEngineMessage;

typedef struct {
    GByteArray *bytes;
} MuxEngineBuilder;

typedef struct {
    const guint8 *data;
    gsize length;
    gsize offset;
} MuxEngineCursor;

void mux_engine_message_init(MuxEngineMessage *message,
                             guint16 type,
                             guint32 flags,
                             guint64 view_id,
                             guint64 serial,
                             GBytes *payload);
void mux_engine_message_clear(MuxEngineMessage *message);

gboolean mux_engine_send_message(int fd,
                                 const MuxEngineMessage *message,
                                 GError **error);
gboolean mux_engine_receive_message(int fd,
                                    MuxEngineMessage *message,
                                    GError **error);

void mux_engine_builder_init(MuxEngineBuilder *builder);
void mux_engine_builder_clear(MuxEngineBuilder *builder);
void mux_engine_builder_put_u16(MuxEngineBuilder *builder, guint16 value);
void mux_engine_builder_put_u32(MuxEngineBuilder *builder, guint32 value);
void mux_engine_builder_put_u64(MuxEngineBuilder *builder, guint64 value);
void mux_engine_builder_put_bytes(MuxEngineBuilder *builder,
                                  const guint8 *data,
                                  gsize length);
void mux_engine_builder_put_string(MuxEngineBuilder *builder,
                                   const gchar *value);
GBytes *mux_engine_builder_finish(MuxEngineBuilder *builder);

void mux_engine_cursor_init(MuxEngineCursor *cursor, GBytes *payload);
gboolean mux_engine_cursor_get_u16(MuxEngineCursor *cursor, guint16 *value);
gboolean mux_engine_cursor_get_u32(MuxEngineCursor *cursor, guint32 *value);
gboolean mux_engine_cursor_get_u64(MuxEngineCursor *cursor, guint64 *value);
gboolean mux_engine_cursor_get_bytes(MuxEngineCursor *cursor,
                                     gsize length,
                                     const guint8 **data);
gboolean mux_engine_cursor_get_string(MuxEngineCursor *cursor,
                                      gchar **value);
gboolean mux_engine_cursor_done(const MuxEngineCursor *cursor);

G_END_DECLS
