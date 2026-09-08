"""Explicit single-report intake; no browser operations or default file writes."""

from datetime import datetime
import hmac
import json
import math
import os
from pathlib import Path
import re
import secrets
import tempfile
import threading
import time


MAX_REPORT_BYTES = 2 * 1024 * 1024
SCHEMA = "mux.privacy-evaluation"
FIXTURE_VERSION = "1.0.0"
RECIPE_VERSION = "2026-09-07.1"
CAUTION = "Equal or changed observations do not establish anonymity, uniqueness, or resistance to identification."
RECIPE = {
    "dates": ["2020-01-01T12:00:00Z", "2020-07-01T12:00:00Z"],
    "canvas": {"width": 240, "height": 80, "text": "Mux local probe 0123456789 \u03a9 \u2603", "font": "16px sans-serif"},
    "webgl": {"width": 64, "height": 64, "version": "webgl", "antialias": False, "shader": "triangle-gradient-v1"},
    "audio": {"channels": 1, "frames": 4410, "sampleRate": 44100, "frequency": 10000, "waveform": "triangle", "encoding": "float32-little-endian"},
    "digest": "SHA-256", "digestTimeoutMs": 1500, "audioTimeoutMs": 2500, "workerTimeoutMs": 8000,
}
COMMON_FIELDS = tuple(f"navigator.{name}" for name in (
    "language", "languages", "platform", "userAgent", "vendor", "hardwareConcurrency", "deviceMemory", "maxTouchPoints",
)) + (
    "intl.locale", "intl.timeZone", "intl.calendar", "intl.numberingSystem", "timezone.offsets",
    "timezone.dateString", "environment.isSecureContext", "environment.crossOriginIsolated",
)
VIEWPORT_FIELDS = (
    "screen.width", "screen.height", "screen.availWidth", "screen.availHeight", "screen.colorDepth", "screen.pixelDepth",
    "window.innerWidth", "window.innerHeight", "window.outerWidth", "window.outerHeight", "window.devicePixelRatio",
    "visualViewport.width", "visualViewport.height", "visualViewport.scale",
)
CANVAS_FIELDS = (
    "canvas.api", "canvas.textWidth", "canvas.actualBoundingBoxLeft", "canvas.actualBoundingBoxRight",
    "canvas.actualBoundingBoxAscent", "canvas.actualBoundingBoxDescent", "canvas.pixels",
)
WEBGL_FIELDS = (
    "webgl.api", "webgl.version", "webgl.shadingLanguageVersion", "webgl.vendor", "webgl.renderer",
    "webgl.unmaskedVendor", "webgl.unmaskedRenderer", "webgl.extensions", "webgl.maxTextureSize",
    "webgl.maxRenderbufferSize", "webgl.maxViewportDims", "webgl.fragmentHighFloatPrecision", "webgl.pixels",
)
AUDIO_FIELDS = (
    "audio.api", "audio.sampleRate", "audio.length", "audio.numberOfChannels", "audio.absoluteSampleSum", "audio.samples",
)
FIELDS = COMMON_FIELDS + VIEWPORT_FIELDS + CANVAS_FIELDS + WEBGL_FIELDS + AUDIO_FIELDS
CROSS_CONTEXT_FIELDS = COMMON_FIELDS + tuple(
    name for name in CANVAS_FIELDS + WEBGL_FIELDS + AUDIO_FIELDS if not name.endswith(".api")
)
STATES = {"ok", "unsupported", "error", "timeout", "skipped"}


class IntakeError(Exception):
    def __init__(self, status, code):
        super().__init__(code)
        self.status = status
        self.code = code


def _invalid():
    raise IntakeError(400, "invalid-report")


def _object_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            _invalid()
        result[key] = value
    return result


def _tree(value, depth=0, budget=None):
    if budget is None:
        budget = [10000]
    budget[0] -= 1
    if depth > 32 or budget[0] < 0:
        _invalid()
    if value is None or type(value) is bool or type(value) is int:
        return
    if type(value) is float:
        if not math.isfinite(value):
            _invalid()
        return
    if type(value) is str:
        if len(value) > 16384:
            _invalid()
        return
    if type(value) is list:
        for item in value:
            _tree(item, depth + 1, budget)
        return
    if type(value) is dict:
        for key, item in value.items():
            if type(key) is not str or len(key) > 128:
                _invalid()
            _tree(item, depth + 1, budget)
        return
    _invalid()


def _canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False)


def _signal(signal):
    if type(signal) is not dict or signal.get("status") not in STATES:
        _invalid()
    if signal["status"] == "ok":
        if set(signal) != {"status", "value"}:
            _invalid()
    else:
        if set(signal) not in ({"status", "reason"}, {"status", "reason", "errorName"}):
            _invalid()
        if type(signal["reason"]) is not str or not 1 <= len(signal["reason"]) <= 128:
            _invalid()
        if "errorName" in signal and (type(signal["errorName"]) is not str or not re.fullmatch(r"[A-Za-z0-9_]{1,64}", signal["errorName"])):
            _invalid()


def _context(context):
    if type(context) is not dict or type(context.get("signals")) is not dict:
        _invalid()
    if context.get("status") == "ok":
        if set(context) != {"status", "signals"}:
            _invalid()
    else:
        _signal({key: value for key, value in context.items() if key != "signals"})
    if set(context["signals"]) != set(FIELDS):
        _invalid()
    for signal in context["signals"].values():
        _signal(signal)


def validate_payload(payload, configuration):
    if not payload or len(payload) > MAX_REPORT_BYTES:
        raise IntakeError(413, "report-size-limit")
    try:
        report = json.loads(payload.decode("utf-8"), object_pairs_hook=_object_pairs, parse_constant=lambda _value: _invalid())
        _tree(report)
        expected_keys = {
            "schema", "schemaVersion", "fixtureVersion", "recipeVersion", "recipe", "configuration",
            "metadata", "measurements", "windowWorkerComparison", "caution",
        }
        if type(report) is not dict or set(report) != expected_keys:
            _invalid()
        if report["schema"] != SCHEMA or type(report["schemaVersion"]) is not int or report["schemaVersion"] != 1:
            _invalid()
        if report["fixtureVersion"] != FIXTURE_VERSION or report["recipeVersion"] != RECIPE_VERSION:
            _invalid()
        if _canonical(report["recipe"]) != _canonical(RECIPE) or report["caution"] != CAUTION:
            _invalid()
        if _canonical(report["configuration"]) != _canonical({"rendering": configuration["rendering"]}):
            _invalid()
        metadata = report["metadata"]
        if type(metadata) is not dict or set(metadata) != {"capturedAt", "labels"}:
            _invalid()
        timestamp = metadata["capturedAt"]
        if type(timestamp) is not str or not re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,6})?Z", timestamp):
            _invalid()
        datetime.fromisoformat(timestamp.replace("Z", "+00:00"))
        if metadata["labels"] != configuration["labels"]:
            _invalid()
        measurements = report["measurements"]
        if type(measurements) is not dict or set(measurements) != {"window", "worker"}:
            _invalid()
        _context(measurements["window"])
        _context(measurements["worker"])
        entries = report["windowWorkerComparison"]
        if type(entries) is not list or len(entries) != len(CROSS_CONTEXT_FIELDS):
            _invalid()
        for field, entry in zip(CROSS_CONTEXT_FIELDS, entries):
            if type(entry) is not dict or set(entry) != {"signal", "window", "worker", "outcome"} or entry["signal"] != field:
                _invalid()
            left = measurements["window"]["signals"][field]
            right = measurements["worker"]["signals"][field]
            if _canonical(entry["window"]) != _canonical(left) or _canonical(entry["worker"]) != _canonical(right):
                _invalid()
            if left["status"] == right["status"] == "ok":
                outcome = "same" if _canonical(left["value"]) == _canonical(right["value"]) else "different"
            else:
                outcome = "not-comparable" if left["status"] == right["status"] else "availability-changed"
            if entry["outcome"] != outcome:
                _invalid()
        return report
    except IntakeError:
        raise
    except (ValueError, TypeError, KeyError, OverflowError, RecursionError):
        _invalid()


class AutomationSession:
    def __init__(self, label, rendering, build, lifetime, output_root=None):
        if label not in ("direct", "tor") or type(rendering) is not bool:
            raise ValueError("Explicit automation label/configuration required")
        if not isinstance(build, str) or not 1 <= len(build) <= 128 or not build.isascii():
            raise ValueError("Automation build label must be 1-128 ASCII characters")
        self.configuration = {
            "mode": "automation", "label": label, "rendering": rendering,
            "labels": {"identity": label, "visit": "1", "build": build},
        }
        self.token = secrets.token_hex(32)
        self.directory = Path(tempfile.mkdtemp(prefix="mux-privacy-evaluation-", dir=output_root)).resolve()
        os.chmod(self.directory, 0o700)
        self.report_path = self.directory / "report.json"
        self.expires_at = time.monotonic() + lifetime
        self._lock = threading.Lock()
        self._stored = False

    def authorize(self, token):
        if type(token) is not str or not re.fullmatch(r"[a-f0-9]{64}", token) or not hmac.compare_digest(token, self.token):
            raise IntakeError(403, "invalid-capability")
        if time.monotonic() >= self.expires_at:
            raise IntakeError(410, "session-expired")

    def status(self):
        with self._lock:
            return {"state": "stored" if self._stored else "waiting", "reportsStored": int(self._stored)}

    def startup(self, origin):
        base = f"{origin}/automation/{self.token}"
        return {
            "schema": "mux.privacy-automation-session", "schemaVersion": 1,
            "mode": "automation", "label": self.configuration["label"], "rendering": self.configuration["rendering"],
            "pageUrl": f"{base}/index.html", "statusUrl": f"{base}/status",
            "reportDirectory": str(self.directory), "reportPath": str(self.report_path),
        }

    def accept(self, payload):
        with self._lock:
            self.authorize(self.token)
            if self._stored or self.report_path.exists():
                raise IntakeError(409, "report-already-stored")
            report = validate_payload(payload, self.configuration)
            encoded = (_canonical(report) + "\n").encode("utf-8")
            temporary_path = None
            try:
                with tempfile.NamedTemporaryFile(mode="wb", prefix=".report-", dir=self.directory, delete=False) as file:
                    temporary_path = Path(file.name)
                    os.fchmod(file.fileno(), 0o600)
                    file.write(encoded)
                    file.flush()
                    os.fsync(file.fileno())
                self.authorize(self.token)
                # The output name is fixed by this session, never supplied by HTTP.
                os.replace(temporary_path, self.report_path)
                temporary_path = None
                self._stored = True
            finally:
                if temporary_path is not None:
                    temporary_path.unlink(missing_ok=True)
            return {"state": "stored", "reportsStored": 1}
