#ifndef MUX_UI_PROTOCOL_H
#define MUX_UI_PROTOCOL_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define MUX_UI_WIRE_VERSION 1U
#define MUX_UI_MAX_PAYLOAD (256U * 1024U)
#define MUX_UI_MAX_MESSAGE (8U * 1024U)
#define MUX_UI_MAX_VALUE (8U * 1024U)
#define MUX_UI_MAX_PATH (16U * 1024U)
#define MUX_UI_MAX_CHOICES 256U
#define MUX_UI_MAX_PATHS 128U

typedef enum {
    MUX_UI_ERROR_INVALID,
    MUX_UI_ERROR_TRUNCATED,
    MUX_UI_ERROR_TOO_LARGE,
    MUX_UI_ERROR_UNSUPPORTED,
} MuxUiError;

#define MUX_UI_ERROR (mux_ui_error_quark())
GQuark mux_ui_error_quark(void);

typedef enum {
    MUX_UI_RECORD_REQUEST = 1,
    MUX_UI_RECORD_RESPONSE = 2,
    MUX_UI_RECORD_CANCEL = 3,
} MuxUiRecordType;

typedef enum {
    MUX_UI_REQUEST_DIALOG_ALERT = 1,
    MUX_UI_REQUEST_DIALOG_CONFIRM = 2,
    MUX_UI_REQUEST_DIALOG_PROMPT = 3,
    MUX_UI_REQUEST_DIALOG_BEFORE_UNLOAD = 4,
    MUX_UI_REQUEST_PERMISSION = 5,
    MUX_UI_REQUEST_AUTHENTICATION = 6,
    MUX_UI_REQUEST_FILE_CHOOSER = 7,
    MUX_UI_REQUEST_DOWNLOAD_DESTINATION = 8,
    MUX_UI_REQUEST_CONTEXT_MENU = 9,
    MUX_UI_REQUEST_OPTION_MENU = 10,
    MUX_UI_REQUEST_CRASH = 11,
    MUX_UI_REQUEST_NOTIFICATION = 12,
} MuxUiRequestKind;

typedef enum {
    MUX_UI_REQUEST_FLAG_MULTIPLE = 1U << 0,
    MUX_UI_REQUEST_FLAG_USER_GESTURE = 1U << 1,
    MUX_UI_REQUEST_FLAG_PRIVATE_PROFILE = 1U << 2,
    MUX_UI_REQUEST_FLAG_PASSWORD = 1U << 3,
    MUX_UI_REQUEST_FLAG_DANGER = 1U << 4,
    MUX_UI_REQUEST_FLAG_PERSISTENCE_AVAILABLE = 1U << 5,
} MuxUiRequestFlags;

typedef enum {
    MUX_UI_CHOICE_FLAG_DISABLED = 1U << 0,
    MUX_UI_CHOICE_FLAG_SEPARATOR = 1U << 1,
    MUX_UI_CHOICE_FLAG_SELECTED = 1U << 2,
    MUX_UI_CHOICE_FLAG_DANGER = 1U << 3,
} MuxUiChoiceFlags;

typedef enum {
    MUX_UI_ACTION_ACKNOWLEDGE = 1,
    MUX_UI_ACTION_ACCEPT = 2,
    MUX_UI_ACTION_CANCEL = 3,
    MUX_UI_ACTION_SUBMIT = 4,
    MUX_UI_ACTION_LEAVE = 5,
    MUX_UI_ACTION_STAY = 6,
    MUX_UI_ACTION_ALLOW_ONCE = 7,
    MUX_UI_ACTION_DENY_ONCE = 8,
    MUX_UI_ACTION_ALLOW_ALWAYS = 9,
    MUX_UI_ACTION_DENY_ALWAYS = 10,
    MUX_UI_ACTION_SELECT = 11,
    MUX_UI_ACTION_RELOAD = 12,
    MUX_UI_ACTION_CLOSE = 13,
    MUX_UI_ACTION_UNSUPPORTED = 14,
} MuxUiAction;

typedef enum {
    MUX_UI_CANCEL_UNDERLYING_GONE = 1,
    MUX_UI_CANCEL_VIEW_DESTROYED = 2,
    MUX_UI_CANCEL_NAVIGATION = 3,
    MUX_UI_CANCEL_PANE_DISCONNECTED = 4,
    MUX_UI_CANCEL_SUPERSEDED = 5,
} MuxUiCancelReason;

typedef struct {
    guint32 id;
    guint32 flags;
    gchar *label;
} MuxUiChoice;

typedef struct {
    guint64 request_id;
    MuxUiRequestKind kind;
    guint32 flags;
    guint32 deadline_ms;
    gchar *origin;
    gchar *heading;
    gchar *message;
    gchar *default_value;
    GPtrArray *choices;
} MuxUiRequest;

typedef struct {
    guint64 request_id;
    MuxUiAction action;
    gchar *value;
    GPtrArray *paths;
} MuxUiResponse;

MuxUiChoice *mux_ui_choice_new(guint32 id, guint32 flags, const gchar *label);
MuxUiChoice *mux_ui_choice_copy(const MuxUiChoice *choice);
void mux_ui_choice_free(MuxUiChoice *choice);

MuxUiRequest *mux_ui_request_new(MuxUiRequestKind kind);
MuxUiRequest *mux_ui_request_copy(const MuxUiRequest *request);
void mux_ui_request_free(MuxUiRequest *request);

MuxUiResponse *mux_ui_response_new(guint64 request_id, MuxUiAction action);
MuxUiResponse *mux_ui_response_copy(const MuxUiResponse *response);
void mux_ui_response_free(MuxUiResponse *response);

gboolean mux_ui_action_is_valid(MuxUiRequestKind kind, MuxUiAction action);

GBytes *mux_ui_request_encode(const MuxUiRequest *request, GError **error);
gboolean mux_ui_request_decode(const guint8 *data,
                               gsize length,
                               MuxUiRequest **request,
                               GError **error);

GBytes *mux_ui_response_encode(const MuxUiResponse *response, GError **error);
gboolean mux_ui_response_decode(const guint8 *data,
                                gsize length,
                                MuxUiResponse **response,
                                GError **error);

GBytes *mux_ui_cancel_encode(guint64 request_id,
                             MuxUiCancelReason reason,
                             GError **error);
gboolean mux_ui_cancel_decode(const guint8 *data,
                              gsize length,
                              guint64 *request_id,
                              MuxUiCancelReason *reason,
                              GError **error);

gboolean mux_ui_record_type(const guint8 *data,
                            gsize length,
                            MuxUiRecordType *type,
                            GError **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxUiChoice, mux_ui_choice_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxUiRequest, mux_ui_request_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxUiResponse, mux_ui_response_free)

G_END_DECLS

#endif
