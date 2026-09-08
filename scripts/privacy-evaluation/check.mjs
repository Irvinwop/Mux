// Synthetic data only. These checks do not execute a browser or qualify Mux.
import test from "node:test";
import assert from "node:assert/strict";
import {
  FIELDS, CAUTION, RECIPE, canonicalJSON, ok, unsupported, failure, observe, bounded,
  unavailableContext, validateReport, validateContext, createReport, stableProjection,
  compareReports, compareContexts, comparisonSummary,
} from "./report.mjs";

function context(values = {}) {
  const result = unavailableContext(unsupported("synthetic-unavailable"));
  result.status = "ok";
  delete result.reason;
  for (const [key, value] of Object.entries(values)) result.signals[key] = ok(value);
  return result;
}
function report(options = {}) {
  return createReport(
    context(options.window ?? { "navigator.language": "en-US", "navigator.hardwareConcurrency": 4 }),
    context(options.worker ?? { "navigator.language": "en-US", "navigator.hardwareConcurrency": 4 }),
    { rendering: options.rendering ?? false, capturedAt: options.capturedAt ?? "2026-01-01T00:00:00Z", labels: options.labels },
  );
}

test("canonical JSON sorts objects but preserves array order", () => {
  assert.equal(canonicalJSON({ z: 1, a: { y: 2, x: 3 } }), '{"a":{"x":3,"y":2},"z":1}');
  assert.notEqual(canonicalJSON([1, 2]), canonicalJSON([2, 1]));
});
test("canonical JSON rejects silently lossy or cyclic data", () => {
  for (const value of [undefined, NaN, Infinity, { value: undefined }, new Date()]) assert.throws(() => canonicalJSON(value));
  const cyclic = {}; cyclic.self = cyclic;
  assert.throws(() => canonicalJSON(cyclic));
});
test("canonical JSON bounds nesting", () => {
  let value = null;
  for (let index = 0; index < 34; index += 1) value = { value };
  assert.throws(() => canonicalJSON(value));
});
test("observation preserves zero, false, empty string and null", () => {
  for (const value of [0, false, "", null]) assert.deepEqual(observe(() => value), { status: "ok", value });
  assert.equal(observe(() => undefined).status, "unsupported");
});
test("exceptions have explicit states without messages or stacks", () => {
  const error = new Error("SENSITIVE PAYLOAD"); error.name = "SecurityError";
  assert.deepEqual(observe(() => { throw error; }), { status: "error", reason: "api-read-failed", errorName: "SecurityError" });
  assert.ok(!JSON.stringify(failure(error)).includes("SENSITIVE"));
});
test("deadline settles success and rejection without hiding either", async () => {
  assert.equal(await bounded(Promise.resolve(7), 100), 7);
  await assert.rejects(bounded(Promise.reject(new TypeError("synthetic")), 100), TypeError);
});
test("deadline identifies a stalled asynchronous operation", async () => {
  await assert.rejects(bounded(new Promise(() => {}), 5), { name: "TimeoutError" });
  const timeout = new Error(); timeout.name = "TimeoutError";
  assert.equal(failure(timeout).status, "timeout");
});
test("unavailable contexts enumerate every signal explicitly", () => {
  const value = unavailableContext(unsupported("worker-not-exposed"));
  assert.equal(Object.keys(value.signals).length, FIELDS.length);
  assert.ok(Object.values(value.signals).every((signal) => signal.status === "unsupported"));
  validateContext(value);
});
test("report versions and missing fields are rejected", () => {
  const wrongVersion = report(); wrongVersion.schemaVersion = 99;
  assert.throws(() => validateReport(wrongVersion));
  const missing = report(); delete missing.measurements.window.signals["navigator.language"];
  assert.throws(() => validateReport(missing));
});
test("unavailable cannot carry a pretend observed value", () => {
  const value = context();
  value.signals["navigator.language"] = { status: "unsupported", reason: "synthetic", value: null };
  assert.throws(() => validateContext(value));
});
test("timestamps and operator labels do not affect stable comparison data", () => {
  const before = report({ labels: { identity: "A", visit: "1", build: "one" } });
  const after = report({ capturedAt: "2026-05-02T03:04:05Z", labels: { identity: "B", visit: "2", build: "two" } });
  assert.equal(canonicalJSON(stableProjection(before)), canonicalJSON(stableProjection(after)));
  assert.equal(compareReports(before, after).counts.different, 0);
  assert.equal(compareReports(before, after).counts.same, 4);
});
test("identical unsupported signals never count as matching measured values", () => {
  const value = report({ window: {}, worker: {} });
  const result = compareReports(value, value);
  assert.equal(result.counts.same, 0);
  assert.equal(result.counts["not-comparable"], FIELDS.length * 2);
});
test("repeat visits retain exactly which measured values changed", () => {
  const before = report();
  const after = report({ window: { "navigator.language": "fr-FR", "navigator.hardwareConcurrency": 4 } });
  const result = compareReports(before, after, "repeat-visit");
  const changed = result.entries.filter((entry) => entry.outcome === "different");
  assert.equal(changed.length, 1);
  assert.equal(changed[0].signal, "navigator.language");
  assert.equal(changed[0].before.value, "en-US");
  assert.equal(changed[0].after.value, "fr-FR");
});
test("fresh identity is experiment metadata, not an anonymity assertion", () => {
  const value = report();
  const result = compareReports(value, value, "fresh-identity");
  assert.equal(result.mode, "fresh-identity");
  assert.equal(result.caution, CAUTION);
  assert.equal(result.counts.same, 4);
  assert.ok(!Object.hasOwn(result, "passed"));
  assert.ok(!Object.hasOwn(result, "anonymityScore"));
});
test("availability changes are distinct from observed value changes", () => {
  const before = report(); const after = report();
  after.measurements.worker.signals["navigator.language"] = failure(new Error(), "synthetic-denial");
  const result = compareReports(before, after);
  assert.equal(result.counts.different, 0);
  assert.equal(result.counts["availability-changed"], 1);
});
test("window/worker disagreement is preserved and viewport absence is not spoofed", () => {
  const left = context({ "navigator.language": "en-US", "screen.width": 1000 });
  const right = context({ "navigator.language": "fr-FR" });
  const entries = compareContexts(left, right);
  const language = entries.find((entry) => entry.signal === "navigator.language");
  assert.equal(language.outcome, "different");
  assert.equal(language.window.value, "en-US");
  assert.equal(language.worker.value, "fr-FR");
  assert.equal(right.signals["screen.width"].status, "unsupported");
  assert.ok(!entries.some((entry) => entry.signal === "screen.width"));
});
test("probe configuration changes make a comparison inconclusive", () => {
  assert.deepEqual(compareReports(report(), report({ rendering: true })).reason, "probe-configuration-changed");
});
test("recipe and fixture changes are not silently compared", () => {
  for (const field of ["recipeVersion", "fixtureVersion"]) {
    const after = report(); after[field] = "different";
    assert.equal(compareReports(report(), after).compatible, false);
  }
  const after = report(); after.recipe = { ...RECIPE, digest: "different" };
  assert.equal(compareReports(report(), after).reason, "fixture-or-recipe-changed");
});
test("unknown comparison modes are rejected", () => {
  assert.throws(() => compareReports(report(), report(), "declare-anonymous"));
});
test("default summary omits values, labels, timestamps and digests", () => {
  const before = report({ window: { "navigator.language": "SECRET_BEFORE" }, labels: { identity: "SECRET_LABEL" } });
  const after = report({ window: { "navigator.language": "SECRET_AFTER" } });
  const summary = comparisonSummary(compareReports(before, after));
  const output = JSON.stringify(summary);
  assert.ok(!output.includes("SECRET"));
  assert.ok(!output.includes("capturedAt"));
  assert.equal(summary.changes[0].signal, "navigator.language");
  assert.equal(summary.changes[0].beforeStatus, "ok");
});
