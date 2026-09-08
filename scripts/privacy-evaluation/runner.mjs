// Shared native measurement runner for interactive and explicitly enabled automation.
import { collect } from "./collector.mjs";
import {
  RECIPE, createReport, validateContext, unavailableContext, unsupported, failure,
} from "./report.mjs";

function collectWorker(rendering) {
  if (typeof Worker !== "function") return Promise.resolve(unavailableContext(unsupported("worker-not-exposed")));
  return new Promise((resolve) => {
    let worker;
    let timer;
    let settled = false;
    const finish = (context) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      worker?.terminate();
      resolve(context);
    };
    try {
      worker = new Worker(new URL("./worker.mjs", import.meta.url), { type: "module", name: "mux-local-privacy-evaluation" });
      timer = setTimeout(() => finish(unavailableContext({ status: "timeout", reason: "worker-deadline-exceeded" })), RECIPE.workerTimeoutMs);
      worker.onmessage = (event) => {
        try {
          if (event.data?.type !== "result") throw new TypeError("Unexpected worker response");
          finish(validateContext(event.data.context));
        } catch (error) { finish(unavailableContext(failure(error, "invalid-worker-response"))); }
      };
      worker.onerror = (event) => {
        event.preventDefault();
        finish(unavailableContext(failure(event.error, "worker-startup-or-runtime-failed")));
      };
      worker.onmessageerror = () => finish(unavailableContext(failure(new Error(), "worker-message-decode-failed")));
      worker.postMessage({ type: "collect", rendering });
    } catch (error) { finish(unavailableContext(failure(error, "worker-construction-failed"))); }
  });
}

export async function runMeasurement(options = {}) {
  const rendering = options.rendering === true;
  const capturedAt = options.capturedAt ?? new Date().toISOString();
  const [windowContext, workerContext] = await Promise.all([
    collect({ rendering }).catch((error) => unavailableContext(failure(error, "window-collection-failed"))),
    collectWorker(rendering),
  ]);
  return createReport(windowContext, workerContext, { rendering, capturedAt, labels: options.labels });
}
