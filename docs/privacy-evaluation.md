# Local fingerprint observations and live qualification

This is an instrument, not an anti-fingerprinting implementation or an anonymity
test. It does not modify Web APIs, browser settings, identities, Tor circuits, or
site data. A different digest does not prove unlinkability; an identical digest
does not prove membership in a large anonymity set. A small local sample cannot
estimate population uniqueness or the accuracy of a remote identification service.

## Scope and data boundaries

The fixture lives in `scripts/privacy-evaluation/` and has no npm dependencies.
`serve.py` uses the Python standard library, binds only `127.0.0.1`, validates the
exact Host header, serves a fixed asset allowlist, and expires after a bounded
lifetime. By default it has no upload/report endpoint or output files. The separate
explicit automation mode below enables one authenticated local intake. Neither
mode has a request log, cookies, or CORS grants.
It does not expose the repository or arbitrary filesystem paths.

The normal interactive page loads only those local assets and a dedicated module worker. Its CSP
disallows connections, remote resources, forms, and embedded objects. The server
also sends no-store, no-referrer, same-origin resource, and permission restrictions.
That default page has no automatic measurement, analytics, fetch, XHR, beacon, WebSocket,
WebRTC, service worker, storage API, permission-request API, device enumeration,
remote font, or third-party script in the fixture. The worker shares exactly the
same collector implementation as the window; window-only values are not copied
into it. No JS prototypes or native functions are replaced.

Canvas/WebGL readback and OfflineAudioContext are opt-in. The audio recipe renders
only into a short offline buffer, never a realtime audio destination or microphone.
Some hardened browsers may present their own canvas consent UI even though no
permission-request API is used. Cancel that prompt; do not grant permissions to
make this evaluation succeed. Keep denials, blocked context creation, timeouts,
and API absence as separate evidence. There is no portable permission-free
preflight that guarantees such a browser-owned prompt cannot appear.

Interactive reports and imported baselines stay in page memory. Download buttons are explicit
exports, and both reports and detailed comparisons contain sensitive browser
values, rendering digests, and operator labels. They are not anonymized data.
Use synthetic labels, an owner-only temporary directory, restricted artifact
access, and a short retention policy when exporting. Do not publish raw reports
or include them in ordinary CI logs. Closing the page or clearing references is
not a secure-memory-erasure guarantee; browser crash dumps, swap, session restore,
and manually downloaded files are outside this fixture's control.

## Next real-browser invocation

Run this only in the Linux environment where the real Mux/WPE/Kitty stack can run.
No Docker or browser invocation is needed for the helper checks below.

```sh
python3 scripts/privacy-evaluation/serve.py --port 8765 --lifetime 600
```

In another terminal in the same checkout and network namespace:

```sh
./mux http://127.0.0.1:8765/index.html
```

The default `--port 0` instead prints an ephemeral-port URL; use that exact URL in
Mux. The fixed port above is merely a reproducible example, not a production
listener. The server exits rather than moving to another port if binding fails.
Keep one server origin for the comparison; changing a port changes the origin and
confounds per-origin defenses. Do not use `file://`, a remote host, or a generic
server rooted at the whole checkout. Module/worker eligibility and secure-context
exposure must be observed in the target WPE build, not assumed from another browser.

1. Record the exact Mux revision/build, WebKit/WPE version and build flags, OS/image,
   GPU/driver or software renderer, font packages, locale, timezone, display scale,
   window dimensions, security settings, and network mode in a restricted run
   manifest. The free-text build field is operator-supplied, not measured provenance.
2. Enter synthetic identity/visit/build labels. Choose whether to include rendering
   probes, hold the window size steady, and select **Measure**. Inspect the explicit
   per-signal outcomes and window/worker differences. Cancel any permission prompt.
3. Select **Use as baseline**, measure again in the same document, and compare. This
   is a same-document control, not a repeat navigation or fresh identity.
4. For repeat visits, explicitly download report A, navigate away and back or reload
   with the same actual profile, take report B, import A, and choose **Repeat visit**.
   Keep origin, recipe, rendering toggle, viewport, and environment fixed. Repeat
   at least three visits, including a process restart of the same profile.
5. For a fresh-identity experiment, export the baseline, then use the browser's real,
   supported new-identity/ephemeral-profile operation and record precisely what it
   resets. Take the next report, import the baseline, and choose **Fresh identity**.
   Changing this dropdown or the label performs no reset. Changing a circuit alone
   is not equivalent to resetting a profile; this fixture proves neither operation.
6. Repeat on independently provisioned supported machines and on reference browsers
   under matched conditions. Report the sample size and confounders. A single
   machine's before/after comparison is not population-level evidence.

Do not bypass a production Tor/loopback restriction to force this local fixture to
load. If a privacy mode intentionally blocks loopback, report the local experiment
as unavailable in that mode and qualify it through the separately approved live
or controlled-origin route. Do not infer Tor routing from local JavaScript values.

## Report contract

`schema` is `mux.privacy-evaluation`, `schemaVersion` is `1`, `fixtureVersion` is
`1.0.0`, and `recipeVersion` is `2026-09-07.1`. Rendering inputs and deadlines are
recorded in `recipe`. A measurement/recipe change requires a version change; keep
the exact fixture revision with the run manifest. JSON remains a plain local
artifact, not a request protocol.

Every field in both `measurements.window.signals` and
`measurements.worker.signals` has an explicit state:

| State | Meaning |
| --- | --- |
| `ok` | The API returned a finite JSON-representable value, retained under `value`. |
| `unsupported` | API absent, or a context/extension returned null; the reason distinguishes that ambiguity. Not proof of protection. |
| `error` | Operation threw or the fixed render failed. Only the exception name and a fixed reason are retained, not messages or stacks. |
| `timeout` | A bounded asynchronous probe or worker did not finish. Not a successful privacy result. |
| `skipped` | The operator did not enable optional rendering probes. |

The containing context's `ok` means collection completed, not that every field
succeeded. Worker construction, loading, or module support failures produce a
whole-context error/timeout with every field enumerated; they are not rewritten
as a successful collection. Missing fields or unknown schema versions are rejected.

The shared baseline reads language/languages, platform/user agent/vendor, exposed
hardware concurrency/device memory/touch points, Intl locale/timezone/calendar/
numbering system, and timezone offsets/string at fixed January/July 2020 instants.
It never inserts current time into the sampled timezone values. Screen, viewport,
visual viewport, pixel ratio, secure-context and isolation flags are recorded where
actually exposed. Worker screen/window APIs normally are absent; absence is retained.

Optional rendering uses the same generic-font canvas scene, small WebGL triangle
and parameter reads, and short offline triangle/compressor audio graph in each
context where possible. Canvas and WebGL output are SHA-256 digests of RGBA bytes;
audio is hashed as explicitly little-endian float32 samples and includes an absolute
sample sum. There is no weak-hash fallback if SubtleCrypto is missing or blocked.
The surface path, extension absence, and individual failures remain in the report.
When OffscreenCanvas exists, both contexts use it; only a context without it may
fall back to an unattached HTML canvas. A failing existing API is not replaced by a
synthetic value or retried through a different API. These tiny recipes are not
equivalent to the larger CreepJS or Fingerprint collectors.

`metadata.capturedAt` and operator labels are excluded by `stableProjection()`;
there is deliberately no aggregate "visitor hash" or generated visitor ID. The
derived window/worker comparison is also excluded because the underlying values
are retained in `measurements`. Rendering configuration and the fixture/recipe
versions must match before reports are compared. Array order is preserved except
for the explicitly sorted WebGL extension set. No observations are normalized to
make window and worker agree.

Comparison outcomes are `same`, `different`, `availability-changed`, and
`not-comparable`. Only two successful observations can be `same` or `different`;
two unsupported/error/skipped observations never count as matching measurements.
The shared-context comparison excludes inherently window-only dimensions and
different surface constructor names, but preserves both in the full report.
Legitimate context differences exist; disagreement is evidence to investigate,
not automatically a CreepJS "lie" or proof of spoofing.

## Offline helpers and future CI seam

With Node.js 18+ and Python 3 available:

```sh
node --test scripts/privacy-evaluation/check.mjs
node --check scripts/privacy-evaluation/report.mjs
node --check scripts/privacy-evaluation/collector.mjs
node --check scripts/privacy-evaluation/worker.mjs
node --check scripts/privacy-evaluation/app.mjs
node --check scripts/privacy-evaluation/runner.mjs
node --check scripts/privacy-evaluation/automation.mjs
node --check scripts/privacy-evaluation/compare.mjs
PYTHONDONTWRITEBYTECODE=1 python3 scripts/privacy-evaluation/check_server.py
python3 scripts/privacy-evaluation/serve.py --help
```

The unit checks use synthetic values to cover schema rejection, stable comparison,
unavailable/error/timeout handling, window/worker disagreements, redaction, and
version/configuration boundaries. The Python checks additionally use synthetic
reports and loopback HTTP requests to exercise opt-in/token/origin rejection,
schema/body limits, absolute connection deadlines, private atomic publication,
and replay rejection. They cross-check the current JS producer against the Python
intake contract. They do not instantiate a browser, test native
API behavior, or establish visual, fingerprinting, or network qualification.

Two explicitly exported reports can be compared without starting a server:

```sh
node scripts/privacy-evaluation/compare.mjs /private/tmp/A.json /private/tmp/B.json --mode repeat-visit
node scripts/privacy-evaluation/compare.mjs /private/tmp/A.json /private/tmp/C.json --mode fresh-identity
```

Default output contains counts and changed field names/states, not raw values,
digests, labels, timestamps, or file contents. `--details` explicitly emits the
sensitive full comparison. Inputs are capped at 2 MiB each. Exit `0` means compatible
reports were processed, not privacy success; exit `2` means invalid inputs or an
incompatible comparison. No output file is created automatically.

A later real-Mux harness can invoke the page-local hook after loading the fixture:

```js
const report = await window.muxPrivacyEvaluation.run({
  rendering: true,
  labels: { identity: "A", visit: "1", build: "exact-build-label" },
});
// Consume the result in the harness's memory or an explicitly restricted artifact.
// window.muxPrivacyEvaluation.getReport() returns a JSON clone of the last report.
```

This is a test namespace, not an override of any Web API. Page titles expose only
constant `mux-privacy-evaluation:loading`, `:ready`, `:running`, `:report-ready`, or
`:failed` checkpoints, never report values. A title checkpoint alone proves neither
rendered visibility nor that probes succeeded. Integration must capture the actual
structured report through an authorized real-engine evaluation/export route and
correlate it with the runtime harness's real displayed-frame evidence. If no such
route is available, use manual explicit export and mark automated capture pending.
Do not use page title/URL payloads, console dumps, third-party endpoints, or network
beacons to transport fingerprints. The explicitly enabled, authenticated loopback
intake below is the bounded automated alternative to manual export.

Use an independent outer process deadline (for example 30 seconds per observation)
in CI. The worker has an 8-second termination deadline, hashes have 1.5-second
deadlines, and offline audio has a 2.5-second deadline. Browser timers cannot
interrupt a stalled main-thread native graphics call. OfflineAudioContext has no
portable cancellation mechanism; the short graph is disconnected after completion
or timeout, but the browser may still finish internal work. Close test views and
reap the fixture server during teardown. A stalled runtime, blocked module, absent
worker, denied API, or missing displayed-frame evidence must stay visible as a gap.

No existing workflow, runtime-smoke script, Meson target, engine, UI, proxy, Tor, or
uBO configuration is changed by this fixture. Automated real-Mux CI integration
remains a separate task.

## Explicit one-shot Linux automation intake

Normal `serve.py` mode still writes no report and never exposes an intake endpoint.
Only `--automation` with an explicit `--automation-label direct|tor` enables a
separate automated page. That page uses the same `runner.mjs` as the interactive
UI, in its own real top-level window, not a surrogate iframe. The normal
`/index.html` continues to require **Measure** even when automation is enabled.
The automated page calls `window.muxPrivacyEvaluation.run()` once with the
server-declared labels and rendering choice, then performs one same-origin fetch
POST to the local fixture. It never sends a report off the runner.

Example, from a real Linux test runner with an existing owner-only `$run_dir`:

```sh
python3 scripts/privacy-evaluation/serve.py \
  --automation --automation-label direct --automation-rendering \
  --automation-build runtime-smoke --automation-output-root "$run_dir" \
  --port 0 --lifetime 120 > "$run_dir/privacy-session.json" &
fixture_pid=$!
```

Wait for the single startup JSON line. It contains `pageUrl`, `statusUrl`,
`reportDirectory`, `reportPath`, `label`, and `rendering`. These URLs contain a
fresh 256-bit capability; keep the startup JSON private and do not echo it into
ordinary CI logs. Without automation, stdout retains its original single plain
interactive URL. Root-owned runtime integration can parse the JSON and launch:

```sh
./mux "$page_url"
```

Use the actual supported runtime/profile selection outside this fixture. The
`direct` and `tor` labels do not select, modify, or attest to routing or browser
configuration. For the Tor-labeled run, start a separate explicitly labeled
session and use the actual Tor-normalized test context. If comparing contexts at
the same origin matters, restart sequential sessions on the same fixed port;
different ephemeral ports are an origin confounder. Do not bypass a production
loopback restriction merely to run the fixture.

Only the token-gated automated page allows `connect-src 'self'`; the interactive
page still uses `connect-src 'none'`. Automated configuration, module, and status
routes are inaccessible without the capability. The report POST additionally
requires the exact loopback Host, exact same-origin Origin header, and a matching
`X-Mux-Privacy-Token` header. There are no CORS grants, redirects, query-controlled
output paths, arbitrary file routes, cookies, or request/report access logs.
Unknown/traversal routes are rejected. Tokens expire with the bounded session.

The intake accepts only a single `application/json` body with one explicit
Content-Length of at most 2 MiB, no transfer/content encoding, and a complete
current report schema. Duplicate JSON keys, nonfinite values, excessive depth/node
counts, unsupported versions, missing signals, mismatched recipe/configuration/
labels, and inconsistent derived window/worker comparisons are rejected. Actual
signal values remain observations; the server does not invent expected native
fingerprints or treat unavailable probes as success. Keep the Python and JS report
contracts synchronized when changing the fixture; the cross-language unit check
guards that boundary.

Every connection has a 10-second absolute deadline plus a shorter idle timeout;
at most eight request handlers run concurrently. The existing finite server
lifetime and the external per-observation process deadline are still required.
The automated page bounds each local fetch to five seconds and makes no retries.
No report is logged to console, page title, URL, or response body.

Enabling automation explicitly creates one unique directory under the trusted
`--automation-output-root` (or the OS temporary root if omitted), mode `0700`.
Its only final report name is the server-chosen `report.json`, mode `0600`. The
body cannot select a filename. A validated report is flushed/fsynced to an
owner-only staging file and atomically renamed into place; failed staging is
removed and subsequent submissions are rejected without replacing the report.
The output is intentionally retained after server exit for the harness to consume.
The harness owns teardown and deletion of the reported private directory, including
empty directories left by a failed/expired run. This opt-in artifact is sensitive
and must not be automatically uploaded as a public CI artifact.

Successful storage returns HTTP `201` with exactly the small success signal:

```json
{"state":"stored","reportsStored":1}
```

The token-protected `statusUrl` then returns the same signal with HTTP `200`;
before capture it reports `{"state":"waiting","reportsStored":0}`. The page
sets the constant title `mux-privacy-evaluation:automation-stored` after receiving
the acknowledgement; other checkpoints are `automation-loading`,
`automation-running`, and `automation-failed`. The report file appears only after
atomic publication. A stored report establishes capture, not successful native
probes, displayed-frame visibility, Tor routing, or fingerprint resistance. The
runtime harness must separately require real displayed-frame evidence and inspect
the actual window/worker signal states and expected normalization differences.
No browser-supplied report is an attestation against a malicious process that
already possesses the local capability.

No CreepJS or Fingerprint.com claim follows from this local intake. Live service
qualification remains the separately approved matrix below.

## Why real CreepJS and Fingerprint.com still matter

Primary sources consulted on 2026-09-07; recheck them and record exact deployed
versions at live qualification time.

**CreepJS.** Its official repository describes prototype-tampering detection,
inconsistency collection, and a much broader set of rendering/runtime/API tests
than this fixture. It explicitly identifies the GitHub Pages deployment as its
only official live site. This fixture neither vendors CreepJS nor reproduces its
scores, population observations, or lie detection. Use the official link only,
not lookalike `.com` or `.org` services. [CreepJS repository](https://github.com/abrahamjuliot/creepjs).

**Fingerprint.com.** The service uses browser observations plus server-side
processing and network data; a local browser-only digest is not its visitor ID.
The open-source FingerprintJS project is also not the commercial identification
service. [Fingerprint introduction](https://docs.fingerprint.com/docs/introduction),
[FingerprintJS repository](https://github.com/fingerprintjs/fingerprintjs).

Fingerprint documents both deterministic and probabilistic identification and
possible false positives/negatives. Its confidence score concerns false-positive
identification and must not be interpreted as overall accuracy or an anonymity
score. A new identifier, including one with high confidence, does not by itself
establish that a returning browser was not linkable.
[Identification, accuracy, and confidence](https://docs.fingerprint.com/docs/identification-accuracy-and-confidence).

**Tor Browser.** Tor's stated anti-fingerprinting objective is reducing the number
of distinguishable groups, including through constrained language choices and
letterboxing, rather than making each visit look arbitrarily different. This
motivates measuring consistency across visits and machines; it does not mean a
WebKit-based Mux build shares Tor Browser's protections or anonymity set.
[Tor fingerprinting protections](https://support.torproject.org/tor-browser/features/fingerprinting-protections/).

**WebKit.** WebKit documents anti-fingerprinting measures and storage partitioning,
but explicitly notes platform differences for some behaviors. Its tracking policy
also includes network and transport-level vectors beyond JavaScript. Treat these
as motivation and a checklist, not evidence that a particular WPE configuration
ships every Safari mitigation. The exact Mux build needs direct measurement.
[Tracking Prevention in WebKit](https://webkit.org/tracking-prevention/),
[WebKit Tracking Prevention Policy](https://webkit.org/tracking-prevention-policy/).

## Required live qualification, still pending

1. Obtain explicit approval for external evaluation: visiting the official live
   services discloses browser/network information and may retain it. Keep this
   separate from the local fixture. This work performs no live browser visit,
   account creation, signup, credential entry, subscription, or paid API action.
2. Use a real displayed Mux build, not Safari/Chromium as a surrogate. Record exact
   runtime provenance and repeat the same-document, repeat-navigation, same-profile
   restart, real fresh-identity, and cross-machine matrix. Keep variables fixed
   first; vary viewport, locale/timezone, and hardware in separately labeled trials.
3. Visit [official CreepJS](https://abrahamjuliot.github.io/creepjs) only after approval.
   Record its then-current version/revision, displayed category results, errors,
   worker/window inconsistencies and tampering indicators, and the relevant
   fingerprint components across the matrix. Preserve blocked or missing tests;
   do not modify scripts, spoof APIs, or grant permissions to improve a score.
4. Separately evaluate the official Fingerprint.com demonstration or an already
   authorized, operator-controlled integration, subject to its terms and available
   access. Record the exposed visitor-ID relationships, returning-visitor outcomes,
   confidence semantics/version when actually available, and errors across the
   same matrix. Keep IDs and network metadata in restricted artifacts. If the
   necessary fields require access not already authorized, stop and mark that
   portion unqualified instead of creating accounts or purchasing access.
5. Record whether scripts or endpoints were blocked by uBO/network policy. Blocking
   a collector is a coverage outcome, not proof that the underlying API exposures
   are resistant. Do not disable protections merely to obtain a successful score;
   any controlled comparison with different protection settings needs its own
   explicitly approved arm and clear labels.
6. Qualify proxy/Tor routing, DNS, IP/TLS/HTTP exposure, identity/site-data reset, and
   cross-origin isolation separately using the relevant real runtime/network
   harness. This loopback, permission-free, browser-value fixture measures none of
   those guarantees. Worker consistency does not prove service-worker, shared-worker,
   iframe, cross-site, extension, or network isolation.
7. Publish a bounded conclusion with sample size, dates/versions, comparability,
   missing coverage, confounders, and observed linkage rather than a single hash
   or "anonymous"/"undetectable" claim. No live qualification is claimed here.
