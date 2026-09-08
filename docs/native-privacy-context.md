# Native Tor timezone and language policy

Tor-mode engines construct a fresh WebKit context with native
`time-zone-override="UTC"`, then set the ordered preferred-language list to
`en-US`, `en` before any view receives that context. No JavaScript properties are
replaced and no process-wide `TZ`, `LANG`, or `LC_*` variables are changed.
Direct mode continues using WebKit's existing default context without either
override. Every Tor pane uses the engine context, and related popups inherit it.

The minimum-supported WPE 2.52.5 implementation exposes the timezone property as
a readable, writable, construct-only string and transfers it into the process
pool's initial configuration. Its public language setter operates on the
context's process pool. The API contract describes effects on HTTP language
preferences, navigator language values, and the default Intl locale. These are
upstream contracts, not measured Mux outcomes. See the pinned
[WebKit context implementation](https://github.com/WebKit/WebKit/blob/wpewebkit-2.52.5/Source/WebKit/UIProcess/API/glib/WebKitWebContext.cpp)
and [public API declarations](https://github.com/WebKit/WebKit/blob/wpewebkit-2.52.5/Source/WebKit/UIProcess/API/glib/WebKitWebContext.h.in).

## Failure and reuse boundaries

- The context factory checks the native property's name, type, readability,
  writability, and construct-only flag before construction.
- The native timezone getter must return `UTC` after construction. Missing
  support or failed readback aborts engine initialization rather than falling
  back to the host timezone.
- Tor attestation is now
  `mux-network-v2-tor-tz-utc-lang-en-us-en-v1-<endpoint-sha256>`. A pre-normalization
  Tor engine cannot satisfy a new pane's WELCOME check, even at the same SOCKS
  endpoint and profile. Direct attestation remains `mux-network-v1-direct`.
- The fixed policy has no per-user timezone or language knob. Any future change
  to these fixed values must also revise the privacy identity token.

## Evidence still required

Portable tests exercise fixed values, direct-mode preservation, native-property
contract failures, and rejection of an old Tor engine's real WELCOME payload.
The Linux WPE test target exercises actual context construction, native timezone
readback, distinct Tor contexts, and the unchanged default context. It creates no
WebViews and does not establish language propagation into page or worker code.

The real Linux runtime matrix must still record initial and repeated loads,
cross-origin frames, related popups, new panes, worker contexts, and engine
restart/reuse. Capture Date/Intl timezone behavior and navigator/Intl language
values, plus an owned HTTP fixture's actual `Accept-Language` header, under
different host locales/timezones. Require direct-mode baselines to remain
unchanged and Tor results to agree with the fixed policy across those surfaces.

UTC and a common language list reduce selected host-specific signals. They do
not establish worker/Intl/network alignment until measured, do not reproduce
Tor Browser's anonymity set, and do not guarantee defeat of CreepJS, Fingerprint,
or other fingerprinting systems. Font, rendering, hardware, timing, viewport,
and other signals remain separate qualification work.
