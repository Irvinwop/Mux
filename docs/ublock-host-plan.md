# Executing genuine uBlock Origin in WPE

Status: source-backed implementation proposal, 2026-09-07. No host or runtime
qualification is implemented by this document.

Audience: the engineer implementing Mux's extension host. The next action is to
build the genuine-background boot slice below, then connect an asynchronous
network decision gate. The target remains the pinned full uBO Firefox package,
not uBO Lite, a translated filter list, or an independently extracted filtering
engine. See the [dependency and capability contract](ublock-origin.md).

## Decision

**Stock WPE 2.52.6 cannot provide full Firefox-reference uBO operation through its
public APIs alone.** It can provide the web contexts, native JavaScript bridge,
and isolated content worlds needed to execute the real extension. Complete
operation additionally requires an asynchronous, attributable request/response
interception backend inside WebKit.

Use an application-owned MV2 compatibility host on the stable GLib APIs, with a
small, versioned native backend boundary and a maintained WebKit patch series.
Keep uBO's actual background, filtering code, assets, and UI intact. Mux supplies
browser APIs and applies uBO's returned decisions; Mux does not implement a
second filter parser. This approach can begin on the current runtime without
enabling all experimental features. The full network backend requires a rebuilt
WPE 2.52.6 plus the reviewed patches; it is not an install-time setting.

Do not make synchronous `send-request` RPC the production architecture. Do not
port a GLib controller facade and assume Safari's WebExtensions implementation
already supplies blocking `webRequest`.

## Source baseline and decisive findings

The inspected release is [WPE 2.52.6](https://wpewebkit.org/release/wpewebkit-2.52.6.html),
whose annotated tag resolves to WebKit commit
[`3bcefb149bd7e5645d18c3f0b9abd515b274649f`](https://github.com/WebKit/WebKit/tree/3bcefb149bd7e5645d18c3f0b9abd515b274649f).
The source archive's published SHA-256 is
`b2bafef2751625b7fdf530f230ff0f542ff0eeba3590c3a989d931b2a55c858e`.
The uBO source baseline is release 1.74.0, commit
[`6dd2d95e50d134a477a4e183343c0b26e9147123`](https://github.com/gorhill/uBlock/tree/6dd2d95e50d134a477a4e183343c0b26e9147123).

| Finding in the inspected source | Consequence |
| --- | --- |
| WPE's `ENABLE_WK_WEB_EXTENSIONS` defaults to `ENABLE_EXPERIMENTAL_FEATURES`. Mux's current Nix runtime has experimental features disabled. | A newer package parser or flag is not an executing host. The stable native web-process extension APIs are a separate facility. [WPE options](https://github.com/WebKit/WebKit/blob/3bcefb149bd7e5645d18c3f0b9abd515b274649f/Source/cmake/OptionsWPE.cmake) |
| Native `WebExtensionAPIWebRequestEvent::addListener` removes every extra-info option except request body/request headers/response headers. Its listener dispatch does not consume callback results. | The `blocking` option is discarded. Returning `{cancel: true}` or a promise cannot affect a load through this implementation. [Event implementation](https://github.com/WebKit/WebKit/blob/3bcefb149bd7e5645d18c3f0b9abd515b274649f/Source/WebKit/WebProcess/Extensions/API/Cocoa/WebExtensionAPIWebRequestEventCocoa.mm) |
| The native WebRequest interface consists of event attributes; the implementation emits them from resource-load notifications. There is no `filterResponseData` method in its IDL. | A GLib port would still need new blocking request and response-stream behavior, not just bindings. [Interface](https://github.com/WebKit/WebKit/blob/3bcefb149bd7e5645d18c3f0b9abd515b274649f/Source/WebKit/WebProcess/Extensions/Interfaces/WebExtensionAPIWebRequest.idl), [notification dispatch](https://github.com/WebKit/WebKit/blob/3bcefb149bd7e5645d18c3f0b9abd515b274649f/Source/WebKit/WebProcess/Extensions/API/Cocoa/WebExtensionAPIWebRequestCocoa.mm) |
| GLib's `WebKitWebPage::send-request` may mutate the request/headers or cancel it. The implementation receives a native frame and loader ID, then discards both when emitting the two-argument GLib signal. The signal returns synchronously. | Useful for bounded diagnostics, but insufficient for full request attribution, async Firefox DNS decisions, or safe cross-process background dispatch. No promise/continuation exists to retain and finish later. [WebPage implementation](https://github.com/WebKit/WebKit/blob/3bcefb149bd7e5645d18c3f0b9abd515b274649f/Source/WebKit/WebProcess/InjectedBundle/API/glib/WebKitWebPage.cpp) |
| `WebKitWebView:web-extension-mode` sets extension CSP mode; it does not install a `browser` API namespace or a controller. | It is an appropriate part of the privileged background view, not the host itself. [WebView implementation](https://github.com/WebKit/WebKit/blob/3bcefb149bd7e5645d18c3f0b9abd515b274649f/Source/WebKit/UIProcess/API/glib/WebKitWebView.cpp) |
| Named `WebKitScriptWorld`s isolate page variables while retaining DOM access. `window-object-cleared` supplies the page and frame before custom window properties should be installed. | These stable APIs can bootstrap real extension/content contexts. [Script worlds](https://github.com/WebKit/WebKit/blob/3bcefb149bd7e5645d18c3f0b9abd515b274649f/Source/WebKit/WebProcess/InjectedBundle/API/glib/WebKitScriptWorld.cpp), [frame JSC access](https://github.com/WebKit/WebKit/blob/3bcefb149bd7e5645d18c3f0b9abd515b274649f/Source/WebKit/WebProcess/InjectedBundle/API/glib/WebKitFrame.cpp) |

Current upstream main was also checked at
[`19cfe1ed983a6e886e86ff307e9fc9ee3053e525`](https://github.com/WebKit/WebKit/tree/19cfe1ed983a6e886e86ff307e9fc9ee3053e525).
It has begun a [GLib context implementation](https://github.com/WebKit/WebKit/blob/19cfe1ed983a6e886e86ff307e9fc9ee3053e525/Source/WebKit/UIProcess/Extensions/glib/WebExtensionContextGLib.cpp)
and a [public context header](https://github.com/WebKit/WebKit/blob/19cfe1ed983a6e886e86ff307e9fc9ee3053e525/Source/WebKit/UIProcess/API/glib/WebKitWebExtensionContext.h.in),
but that header exposes construction, URIs, and injected-content queries, not a
load/controller API. The inspected main WebContext/WebView headers do not add
such a loader, and its [WebRequest event code](https://github.com/WebKit/WebKit/blob/19cfe1ed983a6e886e86ff307e9fc9ee3053e525/Source/WebKit/WebProcess/Extensions/API/Cocoa/WebExtensionAPIWebRequestEventCocoa.mm)
still discards blocking options. This is useful porting progress, not a ready-made
full-uBO backport.

## Why not just finish the native GLib controller port?

That remains a possible upstream-oriented project, but is a larger first step.
In the pinned release, controller initialization, load/unload, page/process-pool
attachment, and user-content-controller integration live in the
[Cocoa controller implementation](https://github.com/WebKit/WebKit/blob/3bcefb149bd7e5645d18c3f0b9abd515b274649f/Source/WebKit/UIProcess/Extensions/Cocoa/WebExtensionControllerCocoa.mm).
The port also needs background-view lifecycle, extension URL schemes, permission
delegates, tabs/windows/actions/menus, native namespace/callback/filter bindings,
and their IPC plumbing. Much of that implementation uses Objective-C/Foundation
and Cocoa view delegates. Shared controller/context/storage code is reusable,
but a public GObject wrapper does not replace the missing implementations.

After that work, blocking request returns, Firefox response streaming, and DNS
semantics would still need implementation. Keep Mux's host/backend boundary
independent of its initial binding implementation so an upstream controller can
replace it later **only when the same capability tests pass**. Do not maintain
two filtering engines or two independent stores of uBO settings.

## Chosen architecture

The following component names describe proposed responsibilities, not existing
programs or implemented interfaces:

```text
Mux profile owner / extension broker
  |-- owns package identity, grants, lifecycle, storage, tab/frame mapping
  |-- native authenticated API messages --> privileged background WebProcess
  |                                         real background.html + js/start.js
  |                                         real uBO filtering and UI modules
  |-- native authenticated API messages --> ordinary page WebProcesses
  |                                         named isolated content world
  |                                         actual uBO content scripts
  `-- async decision/stream protocol <----> patched WebKit network backend
                                            request pause / decision / resume
                                            response headers / filtered bytes
```

### Package and execution contexts

Serve the verified, read-only package through an extension-only URI scheme with
a stable, profile-scoped extension origin. Do not grant file-scheme access or
serve it as ordinary localhost website content. The scheme handler must validate
canonical paths and serve only package members; web pages may request only the
manifest's web-accessible resources, not arbitrary extension pages or private
state. Privilege is determined by an application-owned view role plus current
origin and document generation, never by a URL string supplied in a message.

Create a non-presented background `WebKitWebView` in a **separate WebKit context
from ordinary page views**, with MV2 CSP mode, its own extension data namespace,
and the selected profile's transport policy. Load the actual `background.html`.
Keep this view alive while the profile's host is loaded. Do not rely on related
views remaining in the same WebProcess, and do not put the background behind an
ordinary page's main-thread execution dependency.

Register the native web-process extension directory and initialization data
before any view loads. Install the host's JavaScript browser-API bindings at
`window-object-cleared` in the privileged background/extension-page context.
For ordinary documents, install only the required content-side APIs in a named
isolated world, then load the actual manifest content scripts in order. Keep the
renderer sandbox enabled. The [WebContext implementation](https://github.com/WebKit/WebKit/blob/3bcefb149bd7e5645d18c3f0b9abd515b274649f/Source/WebKit/UIProcess/API/glib/WebKitWebContext.cpp)
provides the native-extension initialization and URI-scheme entry points.

The host adapter is versioned separately from the upstream package. It may
implement Firefox-specific world-bridging semantics using native JSC access;
it must not silently rewrite generated uBO scripts or substitute simplified
scriptlets. If an upstream platform-adapter change proves necessary, record it
as a reviewed, separately pinned uBO port patch with source and notices, not as
execution of an unchanged signed Firefox artifact.

### Browser APIs, state, and trust

Implement the API behavior the pinned package actually uses. This includes
promise/callback/error behavior, listener registration/removal, runtime ports,
manifest/URL/localization access, alarms, persistent local storage, real
tab/navigation operations, and the actual extension UI. Unsupported optional
facilities must be absent or return their documented error, not success-shaped
no-ops. Missing required behavior prevents normal protected-page admission.

Use one extension store per Mux profile, shared by that profile's background and
extension UI. Preserve actual uBO settings and IndexedDB/Blob caches across
restart; use an ephemeral namespace for a private session. Do not substitute an
in-memory object for persistent storage in a qualification test. Apply profile
transport rules to filter updates and other extension-origin fetches too.

The broker owns tab IDs and maps native process/page/frame/document identities
to WebExtensions identities. Native frame IDs are process-unique, not globally
unique; do not cast arbitrary 64-bit native identifiers into JavaScript numbers.
Maintain explicit mapping and generation counters. Bind every port, request,
and reply to its profile, host generation, tab, and document generation. Drop
stale replies and disconnect ports on navigation, process exit, or unload.

Expose the real popup, dashboard, logger, picker, and other package pages in Mux
surfaces. `browserAction`, commands, menus, and tab/window requests must operate
those surfaces and the actual target tab. A screenshot of the popup alone does
not qualify the host.

### Content scripts and main-world injection

Implement `document_start`, all frames, about:blank inheritance, match patterns,
frame-targeted script execution, and user-origin CSS insertion/removal. Support
Firefox's dynamic content-script registration and unregister lifecycle. Bind
content ports to their native sender, not a page-provided `tabId` or `frameId`.

The pinned Firefox injector accesses `wrappedJSObject` and performs main-world
scriptlet injection. WebKit isolated worlds are not Gecko Xray wrappers. Treat
that as a concrete compatibility requirement: implement a constrained native
world bridge or an explicitly reviewed platform port, and test strict CSP,
early inline scripts, nested frames, and process swaps. Exposing the entire
privileged `browser` object in the page's default world is not a workaround.

## Required WebKit backend delta

The backend transports browser events and applies returned actions. It must not
parse filter lists or decide whether a URL is an advertisement.

### Request state machine

```text
created -> awaiting-host -> allowed -> existing WebKit loading pipeline
                       `-> cancelled
                       `-> validated redirect -> awaiting-host for next hop
                       `-> timeout / host loss -> cancelled with visible error
```

Add a typed asynchronous decision IPC before a protected request can cause
network/preflight traffic or consume a cached response. Keep the existing WebKit
security, TLS, cookie, cache-partition, and proxy checks around the resulting
request. Revalidate mutations and redirects; a host reply cannot bypass those
checks. Each decision is single-use and bound to a live native request token.
Await a real Firefox-compatible promise result without blocking a WebProcess,
the UI loop, or the network loop on synchronous IPC or a nested main loop.

The source has clear insertion points in
[NetworkResourceLoader](https://github.com/WebKit/WebKit/blob/3bcefb149bd7e5645d18c3f0b9abd515b274649f/Source/WebKit/NetworkProcess/NetworkResourceLoader.cpp):
`startRequest`, `startNetworkLoad`, redirect continuation, `didReceiveResponse`,
and delivery through `didReceiveBuffer`. Existing resource-load notifications
are not sufficient; they have no reply that changes the request. Reuse native
[ResourceLoadInfo](https://github.com/WebKit/WebKit/blob/3bcefb149bd7e5645d18c3f0b9abd515b274649f/Source/WebKit/Shared/ResourceLoadInfo.h)
metadata and extend the contract where necessary instead of guessing types from
file extensions or HTTP headers. Preserve request identity across redirects and
provide real frame/parent/document identity, URL, method, resource type,
initiator, headers, response status, and applicable proxy/IP metadata.

Implement `onBeforeRequest`, `onBeforeSendHeaders`, `onHeadersReceived`, and
the required observation/completion/error events with the expected ordering.
Listener registration must acknowledge actual backend installation. Requests
must not slip through while a filter listener is still being installed.

NetworkResourceLoader is an initial insertion point, **not proof of complete
coverage**. Workers, service-worker-produced responses, memory-cache hits,
WebSocket handshakes, downloads, and speculative traffic need explicit coverage
or additional hooks. A NetworkProcess-only patch may miss a WebCore memory-cache
path. Maintain a per-path qualification matrix and leave full-host readiness
false while a required path bypasses interception.

### Response headers and bytes

Implement Firefox's `filterResponseData(requestId)` as an actual per-request
stream. Header decisions and the filter attachment must complete before response
bytes reach the parser. Support ordered start/data/stop/error events, byte
writes, close/disconnect, cancellation, and backpressure. Preserve the ordinary
browser's security checks and coherent encoding/content-length semantics. A
post-load `get_data()` call or DOM edit cannot implement response filtering.
The expected browser contract is documented by
[Mozilla](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/webRequest/filterResponseData).

Use bounded per-stream credits and an aggregate memory budget, not unbounded
base64/JSON buffering through the UI. The Soup backend has asynchronous reads
and suspended/pending-result states; its
[read path](https://github.com/WebKit/WebKit/blob/3bcefb149bd7e5645d18c3f0b9abd515b274649f/Source/WebKit/NetworkProcess/soup/NetworkDataTaskSoup.cpp)
must stop scheduling/consuming further data when filter credits run out. Keep
unmodified shared HTTP cache data separate from profile-specific transformed
delivery, and apply filters to cached responses as well as fresh responses.
Timeout, malformed output, host death, and overflow require explicit terminal
states, not a fallback that leaks unfiltered bytes.

### Startup and failure barrier

Mux owns a startup barrier independent of uBO's internal suspension settings.
The pinned [startup module](https://github.com/gorhill/uBlock/blob/6dd2d95e50d134a477a4e183343c0b26e9147123/src/js/start.js)
can discard suspension during first installation or when the user's setting
requests it. Its `readyToFilter` assignment also precedes tab initialization and
starting the final network observers. Therefore neither a manifest load nor
that flag alone is a sufficient admission signal.

A host-owned readiness sidecar may await evaluation of the **same cached**
`js/start.js` module in the background's module map, inspect the pinned core's
readiness without modifying it, and combine that result with acknowledged
backend listener registration and the capability gate. Do not run a second
uBO core or import it in an unrelated context. Only then admit ordinary page
navigations. Extension bootstrap resources need a separate, constrained path
that cannot recursively wait for the background it is starting; any permitted
bootstrap network access still obeys the profile's proxy and DNS rules.

Bound pending requests, decision deadlines, response queues, and restart attempts.
On host loss, invalidate its generation and cancel pending protected requests.
Keep normal pages behind a visible failure state until a new host has initialized
and re-established interception. Diagnostic partial execution must never set
the product's full-protection state.

## DNS and Tor: preserve the real privacy boundary

For a direct-network profile, implement asynchronous `browser.dns.resolve` with
the canonical-name and address behavior required by the package, honoring the
profile's resolver policy and isolating caches. An address-only lookup is not
CNAME uncloaking. Reject unsupported flags rather than silently changing the
resolver's meaning. See [Mozilla's DNS API contract](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/dns/resolve).

For a remote-DNS/Tor profile, provide truthful `proxyInfo` so the
[actual Firefox adapter](https://github.com/gorhill/uBlock/blob/6dd2d95e50d134a477a4e183343c0b26e9147123/platform/firefox/vapi-background-ext.js)
preserves its existing skip-DNS behavior. Independently prohibit local resolver
calls in that profile's native DNS backend, even if a caller asks for one.
Never resolve a destination locally before handing its hostname to SOCKS.

Tor's SOCKS `RESOLVE` returns an address; `RESOLVE_PTR` is reverse lookup. Neither
is a forward CNAME-chain API. Do not invent canonical-name answers from PTR or
silently add a public DoH service. Full CNAME functionality in a remote-DNS
profile needs a separately chosen resolver mechanism that respects that profile's
privacy/isolation policy. Until then report CNAME/IP DNS enrichment as unavailable
by policy, matching the genuine adapter's proxy behavior, not as implemented.
The [Tor SOCKS specification](https://spec.torproject.org/socks-extensions.html)
defines these limits. No direct-network fallback is allowed on proxy failure.

## Smallest executing slices

### E0: actual package boot on stock WPE 2.52.6

This is the first implementation slice, not full blocking:

1. Create the isolated background context, read-only package scheme, native
   bootstrap bridge, and profile-scoped data store. Inject real API implementations
   used during startup before loading the unchanged `background.html`.
2. Implement actual runtime/port/localization, storage, alarms, and tab-state
   operations needed by that startup. Use an honestly empty tab set before any
   page is created; do not fake successful unsupported methods to suppress errors.
3. Await the real startup module, record imported package identity and initialization
   results, and display the actual dashboard connected to the same background.
   Change a setting through its real UI and confirm it survives a restart.
4. Report `background-executing, network-unqualified`. Keep ordinary protected
   browsing disabled. If an unimplemented required startup call is reached, fail
   with that exact method/capability instead of declaring the slice passed.

This proves execution of the genuine full package rather than archive presence.
It deliberately proves no network-blocking, HTML-filtering, or Tor protection.

### E1: first end-to-end genuine filtering decision

Add the first asynchronous native request gate and its typed metadata/reply
contract. This slice requires the corresponding WPE rebuild in controlled CI;
there is no public-API promise hook that can replace it safely.

1. Start a deterministic loopback fixture server and the genuine E0 background.
   Add a narrowly scoped rule through uBO's real settings/dashboard path.
2. After the host startup barrier opens for this test harness, navigate a real
   WPE view to a fixture page with one allowed and one blocked request. Dispatch
   the native events to the registered genuine `webRequest` listeners and apply
   their returned results in WebKit. No host-side URL denylist is permitted.
3. Assert that the server receives the allowed request and never receives the
   blocked request, while uBO's own logger reports the matching rule and decision.
4. Remove the rule through the same real extension path and repeat: the formerly
   blocked request must now reach the server. This detects a host that merely
   hard-codes the fixture URL.
5. Hold startup artificially, then kill or disconnect the background with a
   request pending. Assert no early/timeout request reaches the server and the
   exact failure is visible. Exercise redirect and stale-generation rejection.

E1 is the first real background-to-network execution proof. Keep its limited
path coverage explicit. Do not promote it to full operation before response
filtering, frame/world behavior, remaining request paths, persistence, UI, and
proxy-isolation gates pass.

## Full-operation gate and hard limits

The host must expose separate facts for verified package, background execution,
API coverage, network enforcement, response streaming, content-world behavior,
storage/UI operation, and profile transport policy. Full readiness is derived
from those facts; it is not a Boolean set by finding an extension directory.

Required qualification covers fresh/cached requests, redirects and headers,
iframes/about:blank, workers/service workers, WebSockets, startup/restored tabs,
cosmetic/procedural filters, strict-CSP scriptlets, real HTML/response replacement,
CNAME/IP behavior in direct mode, no local DNS in remote-DNS mode, UI controls,
restart persistence, private-profile isolation, and host failure under load.
Use controlled servers and negative assertions, not blocked counters alone.

The hard limits are explicit: stock public WPE APIs cannot provide complete
asynchronous `webRequest` plus response streams; the inspected native controller
port is not complete; Safari-style notification events are not blocking events;
Gecko-specific world behavior needs a real compatibility implementation; and
standard Tor SOCKS does not expose forward CNAME information. If maintaining the
necessary WebKit backend delta is unacceptable, the full-uBO-on-current-WPE
requirement cannot honestly be met by an application-only workaround.

No local browser, Docker runtime, WebKit build, or host prototype was run for
this investigation. The cited code establishes feasibility constraints, not
runtime compatibility. Existing dependency-staging tests do not qualify E0, E1,
or full operation.
