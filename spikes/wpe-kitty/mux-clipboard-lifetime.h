#ifndef MUX_CLIPBOARD_LIFETIME_H
#define MUX_CLIPBOARD_LIFETIME_H

#include <glib.h>

/* Main-context confined ownership used by callback-driven clipboard objects. */
typedef struct {
    guint references;
    gboolean owner_released;
} MuxClipboardLifetime;

static inline void
mux_clipboard_lifetime_init(MuxClipboardLifetime *lifetime)
{
    lifetime->references = 1;
    lifetime->owner_released = FALSE;
}

static inline void
mux_clipboard_lifetime_acquire(MuxClipboardLifetime *lifetime)
{
    g_assert(lifetime->references > 0);
    lifetime->references++;
}

static inline gboolean
mux_clipboard_lifetime_release(MuxClipboardLifetime *lifetime)
{
    g_assert(lifetime->references > 0);
    lifetime->references--;
    return lifetime->references == 0;
}

static inline gboolean
mux_clipboard_lifetime_release_owner(MuxClipboardLifetime *lifetime)
{
    if (lifetime->owner_released)
        return FALSE;
    lifetime->owner_released = TRUE;
    return mux_clipboard_lifetime_release(lifetime);
}

static inline gboolean
mux_clipboard_lifetime_owner_released(
    const MuxClipboardLifetime *lifetime)
{
    return lifetime->owner_released;
}

#endif
