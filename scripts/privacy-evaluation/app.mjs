import { runMeasurement } from "./runner.mjs";
import {
  MAX_REPORT_BYTES, CAUTION, compareReports, comparisonSummary, validateReport,
} from "./report.mjs";

const element = (id) => document.getElementById(id);
let report = null;
let baseline = null;
let comparison = null;
let running = false;
let generation = 0;
const clone = (value) => value === null ? null : JSON.parse(JSON.stringify(value));

function showStatus(message, checkpoint = "ready") {
  element("status").textContent = message;
  // Deliberately constant: fingerprints, labels and timestamps never enter the title.
  document.title = `mux-privacy-evaluation:${checkpoint}`;
}
function controls() {
  element("measure").disabled = running;
  element("pin").disabled = running || report === null;
  element("download").disabled = running || report === null;
  element("compare").disabled = running || report === null || baseline === null;
  element("download-comparison").disabled = running || comparison === null;
  element("import").disabled = running;
}
async function run(options = {}) {
  if (running) throw new Error("A measurement is already in progress");
  const epoch = generation;
  running = true;
  report = null;
  comparison = null;
  element("report").textContent = "No completed report.";
  element("comparison").textContent = "No comparison.";
  controls();
  showStatus("Measuring actual APIs. Keep the window size unchanged.", "running");
  const rendering = options.rendering === true;
  const capturedAt = new Date().toISOString();
  try {
    const result = await runMeasurement({ rendering, capturedAt, labels: options.labels });
    if (epoch !== generation) return null;
    report = result;
    element("report").textContent = JSON.stringify(report, null, 2);
    const outcomes = report.windowWorkerComparison;
    const disagreements = outcomes.filter((item) => item.outcome === "different").length;
    const unavailable = outcomes.filter((item) => item.outcome !== "same" && item.outcome !== "different").length;
    showStatus(`Report held in memory. Window/worker: ${disagreements} observed disagreements; ${unavailable} non-comparable or availability differences. No privacy verdict.`, "report-ready");
    return clone(report);
  } catch (error) {
    if (epoch === generation) showStatus("Collection did not produce a valid report. This is not a privacy result.", "failed");
    throw error;
  } finally {
    running = false;
    controls();
  }
}
function compare(mode = element("mode").value) {
  if (!baseline || !report) throw new Error("A baseline and a current report are required");
  comparison = compareReports(baseline, report, mode);
  element("comparison").textContent = JSON.stringify(comparison, null, 2);
  const summary = comparisonSummary(comparison);
  showStatus(summary.compatible
    ? `Comparison: ${summary.counts.same} same, ${summary.counts.different} different, ${summary.counts["availability-changed"]} availability changes, ${summary.counts["not-comparable"]} non-comparable. ${CAUTION}`
    : `Not comparable: ${summary.reason}.`);
  controls();
  return clone(comparison);
}
function download(value, filename) {
  if (!value) return;
  const url = URL.createObjectURL(new Blob([JSON.stringify(value, null, 2)], { type: "application/json" }));
  const link = document.createElement("a");
  link.href = url;
  link.download = filename;
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
element("measure").addEventListener("click", () => {
  run({
    rendering: element("rendering").checked,
    labels: { identity: element("identity").value, visit: element("visit").value, build: element("build").value },
  }).catch(() => { /* run() already exposes the failure without an exception payload. */ });
});
element("pin").addEventListener("click", () => {
  baseline = clone(report);
  element("baseline").textContent = "Baseline held in this page's memory.";
  comparison = null;
  element("comparison").textContent = "No comparison.";
  controls();
});
element("import").addEventListener("change", async (event) => {
  const file = event.target.files?.[0];
  const epoch = generation;
  try {
    if (!file) return;
    if (file.size > MAX_REPORT_BYTES) throw new Error("Import exceeds size limit");
    const imported = validateReport(JSON.parse(await file.text()));
    if (epoch !== generation) return;
    baseline = imported;
    comparison = null;
    element("comparison").textContent = "No comparison.";
    element("baseline").textContent = "Imported baseline held in this page's memory. File contents were not uploaded.";
    showStatus("Baseline imported locally.");
  } catch {
    if (epoch === generation) showStatus("Import rejected: use a valid fixture JSON report no larger than 2 MiB.");
  } finally { event.target.value = ""; controls(); }
});
element("compare").addEventListener("click", () => { try { compare(); } catch { showStatus("Comparison unavailable: invalid reports or mode."); } });
element("download").addEventListener("click", () => download(report, "mux-privacy-report.json"));
element("download-comparison").addEventListener("click", () => download(comparison, "mux-privacy-comparison.json"));
element("clear").addEventListener("click", () => {
  generation += 1;
  report = null;
  baseline = null;
  comparison = null;
  element("report").textContent = "No completed report.";
  element("comparison").textContent = "No comparison.";
  element("baseline").textContent = "No baseline.";
  element("import").value = "";
  showStatus(running ? "References cleared; an in-flight measurement will be discarded." : "Report references cleared. Downloaded files are unaffected.");
  controls();
});

// Explicit automation hook for a later real-Mux harness, not a Web API override.
globalThis.muxPrivacyEvaluation = Object.freeze({
  run,
  getReport: () => clone(report),
  compareReports,
  comparisonSummary,
});
controls();
showStatus("Ready. No probes run until Measure or the explicit automation hook is invoked.");
