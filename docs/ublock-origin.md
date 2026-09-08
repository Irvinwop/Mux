# Genuine uBlock Origin dependency and host contract

## Current boundary

Mux pins genuine **uBlock Origin 1.74.0** from `gorhill/uBlock`, with both the
signed Firefox XPI and the Chromium Manifest V2 ZIP available for acquisition.
Firefox is the default and the full-capability reference. Neither package is
uBlock Origin Lite. Filter-list reuse, a WebKit content-filter conversion, or
opening the extension's popup is not execution of uBlock Origin.

This slice acquires and stages the complete extension only. It does not install
it into a browser profile, modify Mux's installer, start a background page,
register request interception, or activate blocking. `STAGED.json` explicitly
records `staged-not-loaded` and `runtime_qualification: not-performed`.

WPE 2.52 introduced initial WebExtensions API. Its public GLib
`WebKitWebExtension` reads a manifest and supporting resources; that object is
not an executing extension context. The published WPE 2.52.4 API has no public
GLib extension controller/context host. Therefore a successful constructor or
`supports_manifest_version(2)` result cannot qualify uBO operation. The separate
`WPEWebProcessExtension` native library API is not a browser WebExtensions host.
This is an inference from the published API surface, not a runtime test of a
future WebKit release. See the [2.52 release notes](https://wpewebkit.org/release/wpewebkit-2.52.0.html),
[WebExtension class](https://wpewebkit.org/reference/stable/wpe-webkit-2.0/class.WebExtension.html),
[public API index](https://wpewebkit.org/reference/stable/wpe-webkit-2.0/), and
[native web-process extension API](https://wpewebkit.org/reference/stable/wpe-web-extension-2.0/).

## Exact dependency

The machine-readable pin is `dependencies/ublock-origin.lock.json`.
Both downloaded files were checked locally against the SHA-256 digests published
by the [official release metadata](https://api.github.com/repos/gorhill/uBlock/releases/tags/1.74.0).

| Variant | Upstream artifact | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| Firefox, default | `uBlock0_1.74.0.firefox.signed.xpi` | 4617614 | `175756d74468c9ba45863f7fc333d3be670f82d5b066314e915814dd547d1652` |
| Chromium MV2 | `uBlock0_1.74.0.chromium.zip` | 4611592 | `29a475e82688b304f2a9b2c0577c2655a8aaf75ce363fe02d720d389bbb9f451` |

The [release](https://github.com/gorhill/uBlock/releases/tag/1.74.0) points to
[source commit `6dd2d95e50d134a477a4e183343c0b26e9147123`](https://github.com/gorhill/uBlock/tree/6dd2d95e50d134a477a4e183343c0b26e9147123).
The archive hash pins **all bundled bytes**, including JavaScript, HTML, locales,
filter lists, asset catalog, public suffix data, redirect resources, WASM and its
included source, UI assets, and third-party notices. This is reproducible
acquisition of an upstream artifact, not a claim of a reproducible source build.
No mutable `latest` URL or separately fetched filter list is used by the helper.

The uBO source headers grant `GPL-3.0-or-later`. A verbatim GPL copy from the
Chromium package is kept at `dependencies/ublock-origin-LICENSE.txt`. Each variant
has its own pinned `LICENSE.txt` digest, and each staged package retains its own
license bytes and every bundled third-party notice. Do not relabel separately
licensed assets as GPL-only or strip their headers. Downstream redistribution
must also preserve applicable source-availability and notice obligations; a lock
file alone does not fulfill them. The pinned [license](https://github.com/gorhill/uBlock/blob/6dd2d95e50d134a477a4e183343c0b26e9147123/LICENSE.txt)
and [source copyright header](https://github.com/gorhill/uBlock/blob/6dd2d95e50d134a477a4e183343c0b26e9147123/src/js/background.js)
are the upstream references.

## Explicit acquisition

The helper requires Python 3.9 or newer and only the standard library. Run from
the repository root, without `sudo`:

```sh
python3 scripts/fetch-ublock-origin.py --download \
  --destination "$PWD/build/dependencies/ublock-origin-1.74.0-firefox"

python3 scripts/fetch-ublock-origin.py --variant chromium --download \
  --destination "$PWD/build/dependencies/ublock-origin-1.74.0-chromium"
```

An already downloaded artifact can instead be staged without network access:

```sh
python3 scripts/fetch-ublock-origin.py \
  --archive /absolute/path/uBlock0_1.74.0.firefox.signed.xpi \
  --destination "$PWD/build/dependencies/ublock-origin-1.74.0-firefox"
```

`--download` and `--archive` are mutually exclusive and one is required. The
destination must be a new absolute path. Existing files, directories, and final
path symlinks are rejected, not overwritten. The helper reserves that directory
exclusively, removes its own incomplete staging on failure, and writes the
completion receipt last. Readers must not consume an in-progress directory.
An interrupted process killed without cleanup may leave an incomplete directory;
it has no valid completion receipt and must not be treated as installed.

Each successful staging directory contains:

- The original, checksum-verified upstream XPI or ZIP, byte for byte.
- `extension/`, the entire payload, with only the Chromium archive's outer
  `uBlock0.chromium/` directory removed from the destination layout.
- `ublock-origin.lock.json`, the complete dependency pin.
- `STAGED.json`, acquisition provenance that explicitly does not assert execution.

Extraction does not run scripts, load WASM, launch a browser, patch the manifest,
install a package manager dependency, or fetch asset-catalog URLs. Files are
non-executable and the staging root is private to the current user. The helper
checks size and SHA-256 before opening the archive, then checks extension
identity, MV2, background page, exact declared permissions, Firefox extension ID,
required payload files, and the variant-specific license digest. It rejects
traversal, symlinks, special files, duplicate/case-alias paths, encrypted entries,
unsupported compression, and excessive file/member/expanded sizes. Network
requests use certificate-verified HTTPS, bounded reads/time, and an explicit
GitHub release-host redirect allowlist.

The Firefox filename denotes upstream's signed release. The helper verifies the
pinned bytes, **not** Mozilla's signing chain. A future host's signature policy,
extension identity, and grant policy remain separate responsibilities. This
helper is not a Tor-aware downloader: `--download` uses Python's normal network
configuration. Use `--archive` when acquisition must be performed separately
through a particular proxy or transport.

## Required host capability contract

These are requirements for a future host, not capabilities supplied by this
helper or inferred from an installed file. The package manifests and the pinned
`platform/common`, `platform/firefox`, and `platform/chromium` source adapters
define the executable contract. Do not substitute no-op API objects to make a
startup probe appear successful.

| Area | Required behavior |
| --- | --- |
| MV2 lifecycle | Execute `background.html`, its JavaScript modules, and the complete uBO initialization in a privileged, persistent extension origin. Enforce the package CSP and web-accessible-resource boundaries. Own load, unload, permission grants, errors, updates, and shutdown. Keep the extension origin distinct from page origins. |
| Startup request barrier | Register real blocking listeners and finish uBO initialization before protected navigations or restored tabs can issue requests. Support Firefox's pending-request promise/suspension semantics. A late content script or post-response notification is insufficient. |
| Blocking webRequest | Implement listener registration/removal and filters for `onBeforeRequest`, `onBeforeSendHeaders`, and `onHeadersReceived`, including `blocking`, request/response headers, cancellation, redirect, header modification, and promise results where Firefox permits them. Supply `ResourceType`, `handlerBehaviorChanged`, redirect/completion/error events, and the request events used by the packaged adapter. Enforce returned decisions in the network path rather than only reporting them. |
| Request attribution | Provide stable `requestId`, `tabId`, `frameId`, `parentFrameId`, URL, method, resource type, document/origin information, headers, status, and applicable IP/proxy metadata. Attribute subframes, navigations, redirects, WebSockets, and background/behind-the-scenes traffic according to the selected upstream API. Never trust page-provided tab/frame identity. |
| Full Firefox response filtering | Implement `browser.webRequest.filterResponseData(requestId)` with ordered response-body streaming, byte writes, close/disconnect, error handling, and backpressure before the document parser sees replaced/removed content. This is required for HTML filtering and response-body replacement, not merely cosmetic hiding. |
| Full Firefox DNS | Implement `browser.dns.resolve(host, ['canonical_name'])` with canonical names and addresses, and async interception while resolution completes. Honor proxy metadata and remote-DNS isolation; do not perform a local DNS lookup to emulate CNAME support in a proxy/Tor profile. The packaged Firefox adapter itself skips such lookups for applicable proxies. |
| Content execution | Honor the manifest's `document_start`, `all_frames`, `match_about_blank`, and URL match rules; inject the packaged `vapi`, client, and content scripts in the correct isolated world. Implement `tabs.executeScript`, `insertCSS`/`removeCSS` with user-origin CSS and frame targeting, plus Firefox `contentScripts.register`/unregister. Preserve early scriptlet execution and the separate main-world/isolated-world boundary. The Firefox injector uses `wrappedJSObject`; WPE requires compatible semantics or an explicitly reviewed platform port, not a manifest rename. |
| Messaging/runtime | Implement runtime manifest/URL access, port connection/disconnection, background/content/UI messaging, sender attribution, last-error or promise behavior, reload, and lifecycle events. Ports must disconnect on frame/tab teardown and must not survive into unrelated documents. Provide localization through `i18n`. |
| Tabs and navigation | Map Mux webviews to stable tabs and windows with query/get/create/update/move/remove/reload, active-tab state, opener relationships, frame queries, committed/DOMContentLoaded/created-target navigation events, and tab activation/update/removal events. This mapping is needed for per-site rules, blocked-document interstitials, element picker, logger, and popup state. |
| Persistent data and scheduling | Supply extension-scoped `storage.local`, `unlimitedStorage`, IndexedDB/Blob caching, regular web-platform primitives, and `alarms` create/get/clear/onAlarm. Preserve filter settings, dynamic rules, trusted sites, custom filters, and update/cache state across restart. Provide the Firefox package's supported WASM execution rather than arbitrarily replacing the core. Isolate data between profiles and obey private-session lifetimes. |
| User-visible controls | Serve the actual `popup-fenix.html`, `dashboard.html`, logger, picker, and related extension pages. Implement `browserAction` icons/title/badge/click behavior, declared commands, and `menus` or Chromium `contextMenus`. Map extension-opened tabs/windows to usable Mux surfaces with correct user-gesture and permission handling. |
| Privacy and optional facilities | Implement the applicable `privacy` settings instead of pretending to change them. Upstream feature-detects facilities such as sync/managed/session storage and some window/action APIs; unsupported optional facilities must remain visibly unavailable rather than silently claiming success. Account sync is not implicitly authorized by staging. |
| Asset traffic and update policy | Let genuine uBO manage its selected lists and compiled caches through the appropriate profile's network path. Apply the same proxy/private-mode policy to extension traffic as to the protected profile. The initial bundled assets are pinned here; later upstream list updates are separate mutable data. Extension-code updates require an explicit new reviewed pin and requalification, not an unpinned automatic replacement. |

The exact declared Firefox permissions are `alarms`, `dns`, `menus`, `privacy`,
`storage`, `tabs`, `unlimitedStorage`, `webNavigation`, `webRequest`,
`webRequestBlocking`, and `<all_urls>`. Chromium replaces `menus` with
`contextMenus` and does not declare `dns`; its manifest requests split incognito
execution. Permission strings do not prove that the corresponding API semantics
exist. See [upstream's permission explanation](https://github.com/gorhill/uBlock/wiki/About-the-required-permissions).

### Variant differences are not interchangeable fallbacks

The Firefox package provides CNAME uncloaking/IP-aware filtering, HTML/response
body filtering, and stronger startup request suspension. The upstream Firefox
path also uses WASM and IndexedDB-based compression where supported. The
Chromium MV2 package is genuine uBO, but does not provide those Firefox-only
network capabilities merely because it is staged successfully. It is retained as
an explicit compatibility input, not as a way to weaken the full-capability
target. Upstream documents these distinctions in
[uBlock Origin works best on Firefox](https://github.com/gorhill/uBlock/wiki/uBlock-Origin-works-best-on-Firefox).

A Chromium-only host, a list converter, or uBO Lite must not be reported as
fulfilling full Firefox-reference operation. A proxy profile may intentionally
make direct DNS functionality unavailable to avoid leakage; report that precise
limitation without routing DNS outside the proxy.

## Qualification before saying blocking is active

The future integration must demonstrate the real pinned extension's decisions
in a running WPE host, not just manifest parsing or successful helper tests:

1. Load the genuine background entry point, verify initialization and listener
   registration, and expose a failure state if any required capability is absent.
2. Prove blocked requests never reach a controlled HTTP server, allowed requests
   do, and exceptions, redirects, headers, frames, and startup navigation are
   handled by the extension's actual filtering engine.
3. Exercise cosmetic and procedural filtering plus scriptlets on controlled
   pages, including all-frame/about:blank and strict-CSP cases.
4. Exercise Firefox response-body/HTML filtering and CNAME/IP filtering, with
   separate tests that ensure a remote-DNS profile never leaks a local lookup.
5. Operate the actual popup, dashboard, logger, custom rules, per-site toggle,
   element picker, and filter-update controls; check persistence after restart.
6. Check profile/private isolation, unload/crash behavior, stale message-port
   rejection, and the absence of requests escaping the startup protection gate.

Until then, expose `not loaded` or the concrete unsupported/error state, not
`protected`, a synthetic blocked count, or a successful-looking silent fallback.

## Focused helper tests and pin updates

The offline standard-library tests do not start a browser or execute upstream
JavaScript/WASM:

```sh
python3 scripts/test-fetch-ublock-origin.py -v

# Additionally qualify both already-downloaded upstream artifacts byte for byte:
python3 scripts/test-fetch-ublock-origin.py -v \
  --upstream-artifacts /absolute/path/to/directory/containing/both/archives
```

For a deliberate version update, obtain both artifacts from the genuine upstream
release, compare locally computed SHA-256 and byte sizes with release metadata,
review both manifests/platform adapters, update source/asset/license provenance
and the lock together, and run both offline and real-artifact tests. Keep every
notice. Review the host contract and repeat runtime qualification separately.
These helper tests establish acquisition integrity only; they are never evidence
that uBO is running in Mux.
