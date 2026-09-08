#ifndef MUX_PANE_DIAGNOSTIC_H
#define MUX_PANE_DIAGNOSTIC_H

#include <glib.h>

G_BEGIN_DECLS

#define MUX_PANE_DIAGNOSTIC_ROWS 3u

typedef struct {
    guint painted_rows;
    guint64 generation;
    gboolean recovered;
} MuxPaneDiagnostic;

/* Output must admit the complete sequence or leave it unwritten. */
typedef gboolean (*MuxPaneDiagnosticOutput)(const guint8 *data,
                                            gsize length,
                                            gpointer user_data,
                                            GError **error);

gboolean mux_pane_diagnostic_show(MuxPaneDiagnostic *diagnostic,
                                  guint32 code,
                                  const gchar *detail,
                                  gboolean reconnecting,
                                  guint columns,
                                  guint rows,
                                  MuxPaneDiagnosticOutput output,
                                  gpointer user_data,
                                  GError **error);

/* Zero rows defer cleanup until terminal geometry is available again. */
gboolean mux_pane_diagnostic_clear(MuxPaneDiagnostic *diagnostic,
                                   guint rows,
                                   MuxPaneDiagnosticOutput output,
                                   gpointer user_data,
                                   GError **error);

/* Capture generation when submitting a frame, then pass it on successful ACK. */
gboolean mux_pane_diagnostic_recover(MuxPaneDiagnostic *diagnostic,
                                     guint64 generation,
                                     guint rows,
                                     MuxPaneDiagnosticOutput output,
                                     gpointer user_data,
                                     GError **error);

G_END_DECLS

#endif
