import { runMeasurement } from "/runner.mjs";
import { MAX_REPORT_BYTES } from "/report.mjs";

let report = null;
let running = false;
const status = document.getElementById("status");
function checkpoint(state, message) {
  document.title = `mux-privacy-evaluation:automation-${state}`;
  status.textContent = message;
}
async function request(url, options = {}) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), 5000);
  try {
    const response = await fetch(url, {
      ...options, signal: controller.signal, mode: "same-origin", credentials: "omit",
      cache: "no-store", redirect: "error", referrerPolicy: "no-referrer",
    });
    if (!response.ok) throw new Error("Local automation request was rejected");
    return await response.json();
  } finally { clearTimeout(timer); }
}
async function run(options) {
  if (running || report !== null) throw new Error("Automation is one-shot");
  running = true;
  try {
    report = await runMeasurement(options);
    return JSON.parse(JSON.stringify(report));
  } finally { running = false; }
}
globalThis.muxPrivacyEvaluation = Object.freeze({
  run, getReport: () => report === null ? null : JSON.parse(JSON.stringify(report)),
});

try {
  const match = /^\/automation\/([a-f0-9]{64})\/index\.html$/.exec(location.pathname);
  if (!match || location.hostname !== "127.0.0.1" || location.protocol !== "http:" || location.search || location.hash) {
    throw new Error("Not a loopback automation URL");
  }
  const base = `/automation/${match[1]}`;
  const configuration = await request(`${base}/configuration`);
  if (configuration.mode !== "automation" || !["direct", "tor"].includes(configuration.label)
      || typeof configuration.rendering !== "boolean" || configuration.labels?.identity !== configuration.label
      || configuration.labels.visit !== "1" || typeof configuration.labels.build !== "string") {
    throw new Error("Invalid automation configuration");
  }
  document.getElementById("configuration").textContent = `Operator label: ${configuration.label}. Optional rendering: ${configuration.rendering ? "enabled" : "disabled"}. Labels do not select or attest to browser routing.`;
  checkpoint("running", "Measuring the real top-level window and a dedicated worker. Keep the viewport fixed.");
  const value = await globalThis.muxPrivacyEvaluation.run({ rendering: configuration.rendering, labels: configuration.labels });
  const body = JSON.stringify(value);
  if (new TextEncoder().encode(body).byteLength > MAX_REPORT_BYTES) throw new Error("Report exceeds the local intake limit");
  const result = await request(`${base}/report`, {
    method: "POST", headers: { "Content-Type": "application/json", "X-Mux-Privacy-Token": match[1] }, body,
  });
  if (result.state !== "stored" || result.reportsStored !== 1) throw new Error("Report storage was not acknowledged");
  checkpoint("stored", "One report stored on this runner. This is capture evidence, not a fingerprint-resistance verdict.");
} catch {
  checkpoint("failed", "Automated capture failed or was rejected. Inspect the token-protected status and runner diagnostics; no privacy success is implied.");
}
