#pragma once

#include "mux-network-policy.h"
#include "src/mux-engine-protocol.h"

G_BEGIN_DECLS

#define MUX_TOR_PROFILE_PREFIX "tor-private-"

/* The reserved namespace also fails closed when a child loses its Tor env. */
gboolean mux_network_handshake_check_profile(const MuxNetworkPolicy *policy,
                                              const gchar *profile,
                                              GError **error);

/* Optional v4 trailer: profile, immutable policy identity. Legacy trailers are
 * accepted only by direct mode. No network request or view is created here. */
void mux_network_handshake_append(MuxEngineBuilder *builder,
                                  const MuxNetworkPolicy *policy,
                                  const gchar *profile);
gboolean mux_network_handshake_check_trailer(MuxEngineCursor *cursor,
                                              const MuxNetworkPolicy *policy,
                                              const gchar *profile,
                                              GError **error);
gboolean mux_network_handshake_check_welcome(GBytes *payload,
                                              const MuxNetworkPolicy *policy,
                                              const gchar *profile,
                                              const gchar *socket_path,
                                              GError **error);

/* A bounded, numeric-loopback SOCKS5 no-auth greeting. This checks availability,
 * not that the service is Tor or that Tor has bootstrapped. Direct is a no-op. */
gboolean mux_network_handshake_probe_socks(const MuxNetworkPolicy *policy,
                                           GError **error);

G_END_DECLS
