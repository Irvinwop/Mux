// Pure report/comparison helpers. No storage, browser mutation, or networking.
export const SCHEMA = "mux.privacy-evaluation";
export const SCHEMA_VERSION = 1;
export const FIXTURE_VERSION = "1.0.0";
export const RECIPE_VERSION = "2026-09-07.1";
export const MAX_REPORT_BYTES = 2 * 1024 * 1024;
export const CAUTION = "Equal or changed observations do not establish anonymity, uniqueness, or resistance to identification.";
export const RECIPE = Object.freeze({
  dates: ["2020-01-01T12:00:00Z", "2020-07-01T12:00:00Z"],
  canvas: { width: 240, height: 80, text: "Mux local probe 0123456789 \u03a9 \u2603", font: "16px sans-serif" },
  webgl: { width: 64, height: 64, version: "webgl", antialias: false, shader: "triangle-gradient-v1" },
  audio: { channels: 1, frames: 4410, sampleRate: 44100, frequency: 10000, waveform: "triangle", encoding: "float32-little-endian" },
  digest: "SHA-256",
  digestTimeoutMs: 1500,
  audioTimeoutMs: 2500,
  workerTimeoutMs: 8000,
});

export const NAVIGATOR_FIELDS = Object.freeze([
  "language", "languages", "platform", "userAgent", "vendor",
  "hardwareConcurrency", "deviceMemory", "maxTouchPoints",
]);
export const VIEWPORT_FIELDS = Object.freeze([
  "screen.width", "screen.height", "screen.availWidth", "screen.availHeight",
  "screen.colorDepth", "screen.pixelDepth", "window.innerWidth", "window.innerHeight",
  "window.outerWidth", "window.outerHeight", "window.devicePixelRatio",
  "visualViewport.width", "visualViewport.height", "visualViewport.scale",
]);
export const CANVAS_FIELDS = Object.freeze([
  "canvas.api", "canvas.textWidth", "canvas.actualBoundingBoxLeft",
  "canvas.actualBoundingBoxRight", "canvas.actualBoundingBoxAscent",
  "canvas.actualBoundingBoxDescent", "canvas.pixels",
]);
export const WEBGL_FIELDS = Object.freeze([
  "webgl.api", "webgl.version", "webgl.shadingLanguageVersion", "webgl.vendor",
  "webgl.renderer", "webgl.unmaskedVendor", "webgl.unmaskedRenderer", "webgl.extensions",
  "webgl.maxTextureSize", "webgl.maxRenderbufferSize", "webgl.maxViewportDims",
  "webgl.fragmentHighFloatPrecision", "webgl.pixels",
]);
export const AUDIO_FIELDS = Object.freeze([
  "audio.api", "audio.sampleRate", "audio.length", "audio.numberOfChannels",
  "audio.absoluteSampleSum", "audio.samples",
]);
export const COMMON_FIELDS = Object.freeze([
  ...NAVIGATOR_FIELDS.map((key) => `navigator.${key}`),
  "intl.locale", "intl.timeZone", "intl.calendar", "intl.numberingSystem",
  "timezone.offsets", "timezone.dateString", "environment.isSecureContext",
  "environment.crossOriginIsolated",
]);
export const FIELDS = Object.freeze([
  ...COMMON_FIELDS, ...VIEWPORT_FIELDS, ...CANVAS_FIELDS, ...WEBGL_FIELDS, ...AUDIO_FIELDS,
]);
export const CROSS_CONTEXT_FIELDS = Object.freeze([
  ...COMMON_FIELDS,
  ...CANVAS_FIELDS.filter((name) => name !== "canvas.api"),
  ...WEBGL_FIELDS.filter((name) => name !== "webgl.api"),
  ...AUDIO_FIELDS.filter((name) => name !== "audio.api"),
]);
const STATES = new Set(["ok", "unsupported", "error", "timeout", "skipped"]);

export function canonicalJSON(value, seen = new Set(), depth = 0) {
  if (depth > 32) throw new TypeError("Report exceeds the nesting limit");
  if (value === null || typeof value === "string" || typeof value === "boolean") return JSON.stringify(value);
  if (typeof value === "number" && Number.isFinite(value)) return JSON.stringify(value);
  if (typeof value !== "object" || seen.has(value)) throw new TypeError("Expected finite, acyclic JSON data");
  seen.add(value);
  let result;
  if (Array.isArray(value)) {
    result = `[${Array.from(value, (item) => canonicalJSON(item, seen, depth + 1)).join(",")}]`;
  } else {
    const prototype = Object.getPrototypeOf(value);
    if (prototype !== null && prototype !== Object.prototype) throw new TypeError("Expected a JSON object");
    result = `{${Object.keys(value).sort().map((key) => `${JSON.stringify(key)}:${canonicalJSON(value[key], seen, depth + 1)}`).join(",")}}`;
  }
  seen.delete(value);
  return result;
}

export function ok(value) {
  return { status: "ok", value: JSON.parse(canonicalJSON(value)) };
}
export function unsupported(reason = "api-not-exposed") { return { status: "unsupported", reason }; }
export function skipped(reason = "rendering-not-enabled") { return { status: "skipped", reason }; }
export function failure(error, reason = "api-read-failed") {
  const name = typeof error?.name === "string" && /^[A-Za-z0-9_]{1,64}$/.test(error.name) ? error.name : "Error";
  return { status: name === "TimeoutError" ? "timeout" : "error", reason, errorName: name };
}
export function observe(read) {
  try {
    const value = read();
    return value === undefined ? unsupported() : ok(value);
  } catch (error) {
    return failure(error);
  }
}
export function bounded(promise, milliseconds) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      const error = new Error("Probe deadline exceeded");
      error.name = "TimeoutError";
      reject(error);
    }, milliseconds);
    Promise.resolve(promise).then(
      (value) => { clearTimeout(timer); resolve(value); },
      (error) => { clearTimeout(timer); reject(error); },
    );
  });
}
export async function digestBytes(bytes) {
  try {
    if (typeof globalThis.crypto?.subtle?.digest !== "function") return unsupported("sha256-not-exposed");
    const buffer = await bounded(globalThis.crypto.subtle.digest("SHA-256", bytes), RECIPE.digestTimeoutMs);
    const hex = Array.from(new Uint8Array(buffer), (byte) => byte.toString(16).padStart(2, "0")).join("");
    return ok({ algorithm: "SHA-256", byteLength: bytes.byteLength, hex });
  } catch (error) {
    return failure(error, "digest-failed-or-timed-out");
  }
}
export function unavailableContext(state = unsupported("context-not-exposed")) {
  return { ...state, signals: Object.fromEntries(FIELDS.map((field) => [field, { ...state }])) };
}

function isObject(value) { return value !== null && typeof value === "object" && !Array.isArray(value); }
export function validateContext(context) {
  if (!isObject(context) || !STATES.has(context.status) || !isObject(context.signals)) throw new TypeError("Invalid context");
  if (Object.keys(context.signals).length !== FIELDS.length) throw new TypeError("Incomplete signal set");
  for (const field of FIELDS) {
    const signal = context.signals[field];
    if (!isObject(signal) || !STATES.has(signal.status)) throw new TypeError("Invalid signal state");
    if (signal.status === "ok") {
      if (!Object.hasOwn(signal, "value")) throw new TypeError("Missing observed value");
      canonicalJSON(signal.value);
    } else if (typeof signal.reason !== "string" || Object.hasOwn(signal, "value")) {
      throw new TypeError("Unavailable signals require a reason, not a value");
    }
    canonicalJSON(signal);
  }
  return context;
}
export function validateReport(report) {
  if (!isObject(report) || report.schema !== SCHEMA || report.schemaVersion !== SCHEMA_VERSION) throw new TypeError("Unsupported report schema");
  if (typeof report.fixtureVersion !== "string" || typeof report.recipeVersion !== "string") throw new TypeError("Missing fixture version");
  if (!isObject(report.recipe) || !isObject(report.configuration) || typeof report.configuration.rendering !== "boolean") throw new TypeError("Missing recipe/configuration");
  if (!isObject(report.metadata) || typeof report.metadata.capturedAt !== "string" || !isObject(report.metadata.labels)) throw new TypeError("Missing report metadata");
  if (!isObject(report.measurements)) throw new TypeError("Missing measurements");
  validateContext(report.measurements.window);
  validateContext(report.measurements.worker);
  canonicalJSON(report);
  return report;
}
function compareSignal(before, after) {
  if (before.status === "ok" && after.status === "ok") return canonicalJSON(before.value) === canonicalJSON(after.value) ? "same" : "different";
  return before.status === after.status ? "not-comparable" : "availability-changed";
}
export function compareContexts(windowContext, workerContext) {
  validateContext(windowContext);
  validateContext(workerContext);
  return CROSS_CONTEXT_FIELDS.map((signal) => ({
    signal, window: windowContext.signals[signal], worker: workerContext.signals[signal],
    outcome: compareSignal(windowContext.signals[signal], workerContext.signals[signal]),
  }));
}
export function createReport(windowContext, workerContext, options = {}) {
  const labels = {};
  for (const name of ["identity", "visit", "build"]) labels[name] = String(options.labels?.[name] ?? "unrecorded").slice(0, 128);
  const report = {
    schema: SCHEMA, schemaVersion: SCHEMA_VERSION, fixtureVersion: FIXTURE_VERSION,
    recipeVersion: RECIPE_VERSION, recipe: RECIPE,
    configuration: { rendering: options.rendering === true },
    metadata: { capturedAt: options.capturedAt ?? new Date().toISOString(), labels },
    measurements: { window: windowContext, worker: workerContext },
    windowWorkerComparison: compareContexts(windowContext, workerContext),
    caution: CAUTION,
  };
  return validateReport(report);
}
export function stableProjection(report) {
  validateReport(report);
  return {
    schema: report.schema, schemaVersion: report.schemaVersion,
    fixtureVersion: report.fixtureVersion, recipeVersion: report.recipeVersion,
    recipe: report.recipe, configuration: report.configuration, measurements: report.measurements,
  };
}
export function compareReports(before, after, mode = "repeat-visit") {
  if (!["repeat-visit", "fresh-identity"].includes(mode)) throw new TypeError("Unknown comparison mode");
  const left = stableProjection(before);
  const right = stableProjection(after);
  const base = { schema: "mux.privacy-comparison", schemaVersion: 1, mode, caution: CAUTION };
  if (left.fixtureVersion !== right.fixtureVersion || left.recipeVersion !== right.recipeVersion || canonicalJSON(left.recipe) !== canonicalJSON(right.recipe)) {
    return { ...base, compatible: false, reason: "fixture-or-recipe-changed" };
  }
  if (canonicalJSON(left.configuration) !== canonicalJSON(right.configuration)) return { ...base, compatible: false, reason: "probe-configuration-changed" };
  const counts = { same: 0, different: 0, "availability-changed": 0, "not-comparable": 0 };
  const entries = [];
  for (const context of ["window", "worker"]) {
    for (const signal of FIELDS) {
      const previous = left.measurements[context].signals[signal];
      const current = right.measurements[context].signals[signal];
      const outcome = compareSignal(previous, current);
      counts[outcome] += 1;
      entries.push({ context, signal, outcome, before: previous, after: current });
    }
  }
  return { ...base, compatible: true, counts, inputs: { before: before.metadata, after: after.metadata }, entries };
}
export function comparisonSummary(comparison) {
  const { schema, schemaVersion, mode, compatible, reason, counts, caution } = comparison;
  if (!compatible) return { schema, schemaVersion, mode, compatible, reason, caution };
  return {
    schema, schemaVersion, mode, compatible, counts, caution,
    changes: comparison.entries.filter((entry) => ["different", "availability-changed"].includes(entry.outcome))
      .map(({ context, signal, outcome, before, after }) => ({ context, signal, outcome, beforeStatus: before.status, afterStatus: after.status })),
  };
}
