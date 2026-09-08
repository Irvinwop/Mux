import {
  RECIPE, FIELDS, NAVIGATOR_FIELDS, VIEWPORT_FIELDS, CANVAS_FIELDS, WEBGL_FIELDS, AUDIO_FIELDS,
  ok, observe, unsupported, skipped, failure, bounded, digestBytes,
} from "./report.mjs";

function assignState(signals, fields, state) {
  for (const field of fields) signals[field] = { ...state };
}
function surface(width, height) {
  if (typeof globalThis.OffscreenCanvas === "function") return { canvas: new OffscreenCanvas(width, height), api: "OffscreenCanvas" };
  if (globalThis.document?.createElement) {
    const canvas = document.createElement("canvas");
    canvas.width = width;
    canvas.height = height;
    return { canvas, api: "HTMLCanvasElement" };
  }
  return null;
}
function basicSignals(signals) {
  for (const key of NAVIGATOR_FIELDS) {
    signals[`navigator.${key}`] = observe(() => {
      const value = globalThis.navigator?.[key];
      return key === "languages" && value !== undefined ? Array.from(value) : value;
    });
  }
  const options = observe(() => new Intl.DateTimeFormat().resolvedOptions());
  for (const key of ["locale", "timeZone", "calendar", "numberingSystem"]) {
    signals[`intl.${key}`] = options.status === "ok" ? observe(() => options.value[key]) : { ...options };
  }
  signals["timezone.offsets"] = observe(() => RECIPE.dates.map((date) => ({ date, minutes: new Date(date).getTimezoneOffset() })));
  signals["timezone.dateString"] = observe(() => new Date(RECIPE.dates[0]).toString());
  for (const key of ["isSecureContext", "crossOriginIsolated"]) signals[`environment.${key}`] = observe(() => globalThis[key]);
  for (const field of VIEWPORT_FIELDS) {
    const [owner, key] = field.split(".");
    // Do not copy window values into the worker: absence is itself evidence.
    signals[field] = observe(() => owner === "window" ? globalThis[key] : globalThis[owner]?.[key]);
  }
}
async function canvasSignals(signals) {
  let canvas;
  try {
    const result = surface(RECIPE.canvas.width, RECIPE.canvas.height);
    if (!result) { assignState(signals, CANVAS_FIELDS, unsupported("canvas-not-exposed")); return; }
    canvas = result.canvas;
    signals["canvas.api"] = ok(result.api);
    const context = canvas.getContext("2d");
    if (!context) {
      assignState(signals, CANVAS_FIELDS.filter((field) => field !== "canvas.api"), unsupported("context-unavailable-or-policy-blocked"));
      return;
    }
    context.fillStyle = "#f4efe2";
    context.fillRect(0, 0, canvas.width, canvas.height);
    const gradient = context.createLinearGradient(0, 0, canvas.width, canvas.height);
    gradient.addColorStop(0, "#126a60");
    gradient.addColorStop(1, "#b24729");
    context.fillStyle = gradient;
    context.fillRect(7, 9, 173, 38);
    context.font = RECIPE.canvas.font;
    context.textBaseline = "alphabetic";
    context.fillStyle = "#17251f";
    context.fillText(RECIPE.canvas.text, 11, 61);
    const metrics = context.measureText(RECIPE.canvas.text);
    signals["canvas.textWidth"] = observe(() => metrics.width);
    for (const key of ["actualBoundingBoxLeft", "actualBoundingBoxRight", "actualBoundingBoxAscent", "actualBoundingBoxDescent"]) {
      signals[`canvas.${key}`] = observe(() => metrics[key]);
    }
    try {
      const pixels = context.getImageData(0, 0, canvas.width, canvas.height).data;
      signals["canvas.pixels"] = await digestBytes(pixels);
    } catch (error) {
      signals["canvas.pixels"] = failure(error, "canvas-readback-failed");
    }
  } catch (error) {
    for (const field of CANVAS_FIELDS) if (signals[field].status === "unsupported") signals[field] = failure(error, "canvas-probe-failed");
  } finally {
    if (canvas) { canvas.width = 0; canvas.height = 0; }
  }
}
function drawWebGL(gl) {
  const shaders = [];
  let program;
  let buffer;
  try {
    for (const [type, source] of [
      [gl.VERTEX_SHADER, "attribute vec2 p; varying vec2 v; void main(){v=p; gl_Position=vec4(p,0.0,1.0);}"],
      [gl.FRAGMENT_SHADER, "precision mediump float; varying vec2 v; void main(){gl_FragColor=vec4(v*0.5+0.5,0.5,1.0);}"],
    ]) {
      const shader = gl.createShader(type);
      if (!shader) throw new Error("Shader allocation failed");
      shaders.push(shader);
      gl.shaderSource(shader, source);
      gl.compileShader(shader);
      if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) throw new Error("Shader compilation failed");
    }
    program = gl.createProgram();
    if (!program) throw new Error("Program allocation failed");
    for (const shader of shaders) gl.attachShader(program, shader);
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) throw new Error("Program link failed");
    gl.useProgram(program);
    buffer = gl.createBuffer();
    if (!buffer) throw new Error("Buffer allocation failed");
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-0.8, -0.8, 0.8, -0.8, 0, 0.8]), gl.STATIC_DRAW);
    const location = gl.getAttribLocation(program, "p");
    if (location < 0) throw new Error("Attribute unavailable");
    gl.enableVertexAttribArray(location);
    gl.vertexAttribPointer(location, 2, gl.FLOAT, false, 0, 0);
    gl.viewport(0, 0, RECIPE.webgl.width, RECIPE.webgl.height);
    gl.clearColor(0.1, 0.2, 0.3, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    const pixels = new Uint8Array(RECIPE.webgl.width * RECIPE.webgl.height * 4);
    gl.readPixels(0, 0, RECIPE.webgl.width, RECIPE.webgl.height, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    if (gl.isContextLost() || gl.getError() !== gl.NO_ERROR) throw new Error("WebGL readback failed");
    return pixels;
  } finally {
    if (buffer) gl.deleteBuffer(buffer);
    if (program) gl.deleteProgram(program);
    for (const shader of shaders) gl.deleteShader(shader);
  }
}
async function webglSignals(signals) {
  let gl;
  let canvas;
  try {
    const result = surface(RECIPE.webgl.width, RECIPE.webgl.height);
    if (!result) { assignState(signals, WEBGL_FIELDS, unsupported("canvas-not-exposed")); return; }
    canvas = result.canvas;
    gl = canvas.getContext("webgl", { antialias: false, preserveDrawingBuffer: true });
    if (!gl) { assignState(signals, WEBGL_FIELDS, unsupported("context-unavailable-or-policy-blocked")); return; }
    signals["webgl.api"] = ok(`${result.api}:webgl`);
    for (const [key, constant] of [
      ["version", "VERSION"], ["shadingLanguageVersion", "SHADING_LANGUAGE_VERSION"],
      ["vendor", "VENDOR"], ["renderer", "RENDERER"], ["maxTextureSize", "MAX_TEXTURE_SIZE"],
      ["maxRenderbufferSize", "MAX_RENDERBUFFER_SIZE"],
    ]) signals[`webgl.${key}`] = observe(() => gl.getParameter(gl[constant]));
    signals["webgl.maxViewportDims"] = observe(() => Array.from(gl.getParameter(gl.MAX_VIEWPORT_DIMS)));
    signals["webgl.extensions"] = observe(() => {
      const extensions = gl.getSupportedExtensions();
      return extensions === null ? undefined : extensions.slice().sort();
    });
    signals["webgl.fragmentHighFloatPrecision"] = observe(() => {
      const precision = gl.getShaderPrecisionFormat(gl.FRAGMENT_SHADER, gl.HIGH_FLOAT);
      return precision ? { rangeMin: precision.rangeMin, rangeMax: precision.rangeMax, precision: precision.precision } : undefined;
    });
    try {
      const extension = gl.getExtension("WEBGL_debug_renderer_info");
      for (const [key, constant] of [["unmaskedVendor", "UNMASKED_VENDOR_WEBGL"], ["unmaskedRenderer", "UNMASKED_RENDERER_WEBGL"]]) {
        signals[`webgl.${key}`] = extension ? observe(() => gl.getParameter(extension[constant])) : unsupported("debug-renderer-extension-unavailable-or-policy-blocked");
      }
    } catch (error) {
      assignState(signals, ["webgl.unmaskedVendor", "webgl.unmaskedRenderer"], failure(error, "debug-renderer-read-failed"));
    }
    try { signals["webgl.pixels"] = await digestBytes(drawWebGL(gl)); }
    catch (error) { signals["webgl.pixels"] = failure(error, "webgl-render-or-readback-failed"); }
  } catch (error) {
    for (const field of WEBGL_FIELDS) if (signals[field].status === "unsupported") signals[field] = failure(error, "webgl-probe-failed");
  } finally {
    try { gl?.getExtension("WEBGL_lose_context")?.loseContext(); } catch { /* Best-effort release, not an observation. */ }
    if (canvas) { canvas.width = 0; canvas.height = 0; }
  }
}
async function audioSignals(signals) {
  let oscillator;
  let compressor;
  try {
    const Constructor = globalThis.OfflineAudioContext ?? globalThis.webkitOfflineAudioContext;
    if (typeof Constructor !== "function") { assignState(signals, AUDIO_FIELDS, unsupported("offline-audio-not-exposed")); return; }
    signals["audio.api"] = ok(globalThis.OfflineAudioContext ? "OfflineAudioContext" : "webkitOfflineAudioContext");
    const context = new Constructor(RECIPE.audio.channels, RECIPE.audio.frames, RECIPE.audio.sampleRate);
    oscillator = context.createOscillator();
    oscillator.type = RECIPE.audio.waveform;
    oscillator.frequency.value = RECIPE.audio.frequency;
    compressor = context.createDynamicsCompressor();
    compressor.threshold.value = -50;
    compressor.knee.value = 40;
    compressor.ratio.value = 12;
    compressor.attack.value = 0;
    compressor.release.value = 0.25;
    oscillator.connect(compressor);
    compressor.connect(context.destination);
    oscillator.start(0);
    const rendered = new Promise((resolve, reject) => {
      context.oncomplete = (event) => resolve(event.renderedBuffer);
      try {
        const promise = context.startRendering();
        if (promise?.then) promise.then(resolve, reject);
      } catch (error) { reject(error); }
    });
    const buffer = await bounded(rendered, RECIPE.audioTimeoutMs);
    signals["audio.sampleRate"] = observe(() => buffer.sampleRate);
    signals["audio.length"] = observe(() => buffer.length);
    signals["audio.numberOfChannels"] = observe(() => buffer.numberOfChannels);
    const samples = buffer.getChannelData(0);
    let absoluteSum = 0;
    const bytes = new Uint8Array(samples.length * 4);
    const view = new DataView(bytes.buffer);
    for (let index = 0; index < samples.length; index += 1) {
      if (!Number.isFinite(samples[index])) throw new Error("Non-finite audio sample");
      absoluteSum += Math.abs(samples[index]);
      view.setFloat32(index * 4, samples[index], true);
    }
    signals["audio.absoluteSampleSum"] = ok(absoluteSum);
    signals["audio.samples"] = await digestBytes(bytes);
  } catch (error) {
    for (const field of AUDIO_FIELDS) if (signals[field].status === "unsupported") signals[field] = failure(error, "offline-audio-failed-or-timed-out");
  } finally {
    try { oscillator?.disconnect(); compressor?.disconnect(); } catch { /* No realtime audio device was opened. */ }
  }
}

export async function collect({ rendering = false } = {}) {
  const signals = Object.fromEntries(FIELDS.map((field) => [field, unsupported("not-yet-observed")]));
  basicSignals(signals);
  if (rendering) {
    // Small, fixed recipes; serial order avoids overlap between GPU/audio probes.
    await canvasSignals(signals);
    await webglSignals(signals);
    await audioSignals(signals);
  } else {
    assignState(signals, [...CANVAS_FIELDS, ...WEBGL_FIELDS, ...AUDIO_FIELDS], skipped());
  }
  return { status: "ok", signals };
}
