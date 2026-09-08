import { collect } from "./collector.mjs";
import { failure, unavailableContext } from "./report.mjs";

globalThis.onmessage = async (event) => {
  if (event.data?.type !== "collect") return;
  globalThis.onmessage = null;
  try {
    const context = await collect({ rendering: event.data.rendering === true });
    globalThis.postMessage({ type: "result", context });
  } catch (error) {
    globalThis.postMessage({ type: "result", context: unavailableContext(failure(error, "worker-collection-failed")) });
  } finally {
    globalThis.close();
  }
};
