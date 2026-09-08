# Network and privacy policy

`MuxNetworkPolicy` is immutable configuration owned by an engine profile. Its
portable GLib/GIO core validates input and supplies a canonical policy identity.
The WPE adapter applies that policy to actual network sessions and view settings
before browsing. Neither module starts Tor, probes a SOCKS listener, implements
an extension host, or establishes Tor Browser-equivalent protection.

## Policy contract

- `direct`, also selected by an absent mode, preserves existing WPE behavior.
  Persistent sessions retain their data/cache directories and default proxy
  settings, including any system proxy configuration. An explicit SOCKS proxy
  argument is rejected in this mode, even if it is empty.
- Private direct-mode sessions remain ephemeral, with ITP enabled and persistent
  credential storage disabled. Direct mode leaves WebRTC settings untouched.
- `tor` requires an explicit `socks5://LOOPBACK_IP:PORT` endpoint. IPv4 addresses
  in `127.0.0.0/8` and IPv6 `::1` are accepted, with an explicit port from 1 to
  65535. IPv6 must be bracketed, for example `socks5://[::1]:9050`.
- Proxy hostnames, remote addresses, credentials, paths including `/`, queries,
  fragments, whitespace, escapes, and alternate schemes are rejected. This
  prevents hostname resolution for the proxy address and ambiguous fallback
  configuration. Rejected input is not copied into error messages.
- Every Tor session is ephemeral, regardless of a caller's private flag. ITP is
  enabled and persistent credential storage is disabled. View settings disable
  WebRTC through the real WPE API before any view is constructed.
- Tor sessions use `WEBKIT_NETWORK_PROXY_MODE_CUSTOM`, one default `socks5://`
  proxy, and an explicitly empty host exclusion list. The adapter adds neither
  a direct fallback nor a scheme-specific bypass. Proxy failure must remain a
  browsing failure; callers must never retry through a direct session.

The explicit SOCKS5 scheme matters: WPE documents `socks://` as permitting other
SOCKS versions, and host exclusions permit direct connections. The adapter uses
neither of those configurations. This is a configuration guarantee; packet-level
leak resistance still requires qualification of the complete runtime.
[WPE proxy API](https://github.com/WebKit/WebKit/blob/wpewebkit-2.52.6/Source/WebKit/UIProcess/API/glib/WebKitNetworkProxySettings.cpp#L49)

## Engine and launcher integration

The core does not read environment variables or command-line arguments. The
launcher and engine must agree on configuration and enforce the following:

1. Validate the mode and endpoint before constructing sessions or accepting a
   browsing request. Invalid configuration must terminate startup explicitly.
2. Own one policy for the profile's engine lifetime. Route every persistent and
   private session constructor through `mux_network_policy_wpe_new_session`.
   Related popups and downloads must use sessions from the same policy.
3. Apply `mux_network_policy_wpe_apply_settings` before constructing each view,
   or construct shared settings with the policy before creating any view.
   A failure must abort view creation. Later settings changes must not re-enable
   WebRTC for a Tor profile.
4. Make Tor's effective privacy state apply to browser metadata, permissions,
   clipboard history, recently closed views, and downloads as well as WebKit
   storage. Ephemeral WebKit sessions alone do not make application stores
   ephemeral. User-approved download files still persist on disk.
5. Give Tor browsing a separate profile identity and storage lifetime. Never
   attach a Tor view to a pre-existing direct profile or persistent session.
6. Compare the canonical policy identity alongside profile identity before
   reusing an engine. Include the expected identity in the request/handshake and
   compare it against the running engine's actual, immutable configuration.
   A mismatch is an error, not permission to use the existing direct engine.
7. Treat explicit socket/data-directory overrides and inherited configuration
   as part of the same trust boundary. They must not reconnect a Tor launch to
   another profile or cause direct-mode state to be reused.

`mux_network_policy_get_identity` returns an opaque, versioned ASCII string.
Equivalent endpoint spellings normalize to the same identity; different
addresses, ports, and modes have different identities. Changing the fixed policy
rules requires an identity version bump. This identity is not a Tor circuit ID,
proof of SOCKS server implementation, or proof that a connection used Tor.

The caller must supply a trusted local Tor listener and manage its lifetime or
explicitly document an externally managed listener. Endpoint validation cannot
distinguish Tor from a different SOCKS5 service listening at the same address.
First-party circuit isolation and SOCKS authentication credentials need a
separate design; they are not implemented by this module.

## Tests and qualification

The portable `tests/test-network-policy.c` target checks strict endpoint parsing,
default/direct behavior, effective ephemerality, stable canonical identities,
owned input, and error redaction. It needs only GLib/GIO and the core source.

Compiling the same test with `MUX_NETWORK_POLICY_TEST_WPE` also checks actual WPE
persistent/private/Tor session properties, direct-mode settings preservation,
WebRTC disablement, and required arguments. That target links the WPE adapter
and runs in isolated GLib test directories. It does not browse or contact Tor.

Before exposing leak-resistance claims, the full Linux runtime needs an
egress-restricted qualification environment with a controlled SOCKS service:

- Confirm destination hostnames reach SOCKS without a local DNS lookup.
- Cover redirects, subresources, workers, service workers, WebSockets, popups,
  downloads, and any future extension asset fetching.
- Confirm WebRTC cannot create an alternate network path in every view type.
- Stop or reject the proxy while browsing and prove requests fail without direct
  network traffic, including after engine restarts and profile reuse attempts.
- Exercise system proxy and `NO_PROXY` contamination, localhost destinations,
  IPv4/IPv6 endpoints, and concurrent direct/Tor profiles.
- Confirm Tor profile state does not survive its declared lifetime or become
  visible to a direct profile, including application-owned history and storage.

These checks are distinct from fingerprint-resistance work. The module does not
inject JavaScript fingerprint spoofing, normalize device attributes, or claim
that changing a fingerprint demo's identifier demonstrates anonymity.
