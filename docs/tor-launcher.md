# Explicit local Tor SOCKS mode

Mux can route an isolated ephemeral launch through an already-running local Tor
SOCKS5 service:

```sh
mux --tor --tor-socks-proxy socks5://127.0.0.1:9050 https://check.torproject.org/
```

The development wrapper accepts the same arguments. Alternatively, set
`MUX_TOR_SOCKS_PROXY` and pass `--tor`, or set both `MUX_NETWORK_MODE=tor` and
`MUX_TOR_SOCKS_PROXY`. There is no guessed endpoint, automatic download, or Tor
service management. Numeric loopback addresses and an explicit port are required;
hostnames, remote proxies, credentials, bypass lists, and malformed URIs fail.

## Isolation and fail-closed behavior

- Every public Tor launch gets a fresh `tor-private-*` profile. Existing direct
  profile, engine socket, data/cache path, and popup-token environment overrides
  are discarded. Tor engines additionally reject custom sockets and persistent
  profile path overrides. Direct mode cannot use the reserved Tor namespace.
- The launcher checks a bounded SOCKS5 no-auth greeting before starting Kitty.
  The engine repeats that check before startup/reuse. This detects unavailable,
  non-SOCKS5, truncated, and authentication-required endpoints. It does not prove
  that the service is Tor, that Tor has bootstrapped, or that an exit is usable.
- Engine reuse is a HELLO/WELCOME exchange, not merely a successful connection.
  Tor requires an exact profile and versioned network-policy identity, including
  the canonical endpoint. Panes validate the response before CREATE_VIEW, even
  when they connect without running `mux-engine --ensure`. A legacy or direct
  engine cannot silently service a Tor pane. Legacy direct reuse remains valid.
- All Tor views are forced private regardless of `MUX_EPHEMERAL=0`. WebKit
  network sessions are ephemeral, use the configured custom SOCKS proxy without
  a bypass list, and never fall back to direct routing when the proxy fails.
  Related popups inherit the parent's session; private view downloads use that
  view's session. The unused engine-level session is also proxied and ephemeral.
- A real `WebKitSettings` policy disables WebRTC before view construction.
  Normal views and related popups receive the same policy-configured settings.
- Bookmark initialization is memory-only. Permission stores are private and
  memory-only from engine initialization onward. Clipboard history is isolated
  by the fresh profile and private scope, and panes independently force private
  clipboard behavior. No direct cookie, bookmark, or permission store is opened.
- The Tor title and startup message identify this mode. Global `mux ctl` is not
  a Tor-mode selector and cannot be combined with Tor launch flags.

## Limits and validation

This is **not Tor Browser**. A successful SOCKS greeting is not an anonymity test,
and this integration does not establish Tor Browser's fingerprint uniformity,
circuit isolation, hardened browser patches, or resistance to traffic correlation.
The user must operate a trusted local Tor service. Saving downloads or explicitly
copying data out of the private session still creates user-requested external
artifacts. Local processes running as the same user are outside this mode fence's
trust boundary.

Portable regressions cover profile/policy mismatch, missing and malformed
attestation, direct legacy compatibility, local SOCKS handshake refusal, and
memory-only bookmarks. `sh scripts/test-tor-launcher.sh` exercises public option
parsing, fresh namespaces, inherited direct override removal, and preflight
failure before launch using mocks; it does not start Kitty or a browser.

Real WPE proxy/DNS behavior, failed-proxy non-fallback, popup/download routing,
WebRTC suppression, and CreepJS/Fingerprint.com measurements still require the
Linux WPE CI/runtime matrix. A local mocked launcher or GLib test cannot establish
those runtime properties or justify a fingerprint-resistance claim.
