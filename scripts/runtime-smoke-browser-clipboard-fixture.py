#!/usr/bin/env python3
"""Loopback-only browser and clipboard fixtures for runtime-smoke."""

from __future__ import annotations

import hashlib
import html
import json
import os
from pathlib import Path
import secrets
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit


MAX_REPORT_BYTES = 4096
TOKEN_MARKER = "MUX_SMOKE_PROFILE_SOURCE_"


def die(message: str) -> "NoReturn":
    raise SystemExit(f"runtime-smoke fixture: {message}")


if len(sys.argv) != 3:
    die("usage: runtime-smoke-browser-clipboard-fixture.py PORT_FILE STATE_DIR")

port_file = Path(sys.argv[1])
state_dir = Path(sys.argv[2])
if not state_dir.is_dir():
    die(f"state directory does not exist: {state_dir}")
if os.stat(state_dir).st_mode & 0o077:
    die(f"state directory is not owner-only: {state_dir}")

capability = secrets.token_urlsafe(32)
tokens = {
    "same": "MUX_SMOKE_SAME_" + secrets.token_urlsafe(36),
    "cross-pane": "MUX_SMOKE_CROSS_PANE_" + secrets.token_urlsafe(36),
    "cross-profile": TOKEN_MARKER + secrets.token_urlsafe(36),
}
state_lock = threading.Lock()
origin = ""


def digest(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def safe_atom(value: object, fallback: str = "unknown") -> str:
    text = str(value)
    if not text or len(text) > 64:
        return fallback
    if not all(character.isalnum() or character in "._-" for character in text):
        return fallback
    return text


def write_all(descriptor: int, data: bytes) -> None:
    remaining = memoryview(data)
    while remaining:
        written = os.write(descriptor, remaining)
        if written <= 0:
            raise OSError("status write made no progress")
        remaining = remaining[written:]


def write_status(scenario: str, event: str, fields: dict[str, object]) -> None:
    path = state_dir / f"{scenario}.{event}.status"
    with state_lock:
        if path.exists():
            return
        result = "PASS" if fields.pop("passed") else "FAIL"
        atoms = [result, f"scenario={scenario}", f"event={event}"]
        atoms.extend(f"{key}={safe_atom(value)}" for key, value in fields.items())
        temporary = state_dir / f".{path.name}.{threading.get_ident()}.tmp"
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            write_all(descriptor, ("\t".join(atoms) + "\n").encode("ascii"))
        finally:
            os.close(descriptor)
        os.replace(temporary, path)


def common_script() -> str:
    return f"""
const reportCapability = {json.dumps(capability)};

async function sha256(value) {{
    const bytes = new TextEncoder().encode(value);
    const hash = await crypto.subtle.digest("SHA-256", bytes);
    return Array.from(new Uint8Array(hash), byte => byte.toString(16).padStart(2, "0")).join("");
}}

async function report(payload, passTitle, failTitle) {{
    try {{
        const response = await fetch(`/report/${{reportCapability}}`, {{
            method: "POST",
            credentials: "same-origin",
            cache: "no-store",
            headers: {{"Content-Type": "application/json"}},
            body: JSON.stringify(payload),
        }});
        const passed = response.ok;
        let checkpoint = document.getElementById("mux-runtime-checkpoint");
        if (!checkpoint) {{
            checkpoint = document.createElement("output");
            checkpoint.id = "mux-runtime-checkpoint";
            checkpoint.style.display = "block";
            checkpoint.style.margin = "1rem 0";
            checkpoint.style.padding = ".75rem";
            checkpoint.style.fontWeight = "700";
            document.body.prepend(checkpoint);
        }}
        checkpoint.textContent = `${{payload.scenario}}:${{payload.event}}:${{passed ? "PASS" : "FAIL"}}`;
        checkpoint.style.background = passed ? "#b8edca" : "#ffb4a8";
        checkpoint.style.color = "#14251a";
        document.title = passed ? passTitle : failTitle;
    }} catch (_error) {{
        document.title = failTitle;
    }}
}}
"""


def page(title: str, body: str, script: str) -> bytes:
    document = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{html.escape(title)}</title>
<style>
:root {{ color-scheme: light; font-family: sans-serif; background: #e8f0ea; color: #14251a; }}
body {{ margin: 3rem; max-width: 52rem; }}
label {{ display: block; margin: 1rem 0; font-weight: 700; }}
textarea {{ display: block; box-sizing: border-box; margin-top: .5rem; width: 100%; min-height: 7rem; }}
</style>
</head>
<body>{body}
<script>"use strict";{common_script()}{script}</script>
</body>
</html>
"""
    return document.encode("utf-8")


def static_page(title: str, text: str, color: str) -> bytes:
    return page(
        title,
        f'<main id="result">{html.escape(text)}</main>',
        f"document.body.style.background = {json.dumps(color)};",
    )


def copy_listener(scenario: str, source_id: str, pass_title: str) -> str:
    return f"""
const source = document.getElementById({json.dumps(source_id)});
source.addEventListener("copy", async event => {{
    const selected = source.value.slice(source.selectionStart, source.selectionEnd);
    await report({{
        scenario: {json.dumps(scenario)},
        event: "copy",
        kind: "copy",
        trusted: event.isTrusted,
        selected_all: selected === source.value,
        digest: await sha256(selected),
        length: new TextEncoder().encode(selected).length,
    }}, {json.dumps(pass_title)}, "MUX_CLIP_COPY_FAIL");
}});
"""


def paste_listener(scenario: str, target_id: str, pass_title: str) -> str:
    return f"""
const target = document.getElementById({json.dumps(target_id)});
let trustedPaste = false;
target.addEventListener("paste", event => {{ trustedPaste = event.isTrusted; }});
target.addEventListener("input", async event => {{
    if (event.inputType !== "insertFromPaste" && !trustedPaste) return;
    await report({{
        scenario: {json.dumps(scenario)},
        event: "paste",
        kind: "paste",
        paste_trusted: trustedPaste,
        input_trusted: event.isTrusted,
        input_type: event.inputType || "missing",
        digest: await sha256(target.value),
        length: new TextEncoder().encode(target.value).length,
    }}, {json.dumps(pass_title)}, "MUX_CLIP_PASTE_FAIL");
}});
"""


def source_page(scenario: str, ready_title: str, pass_title: str) -> bytes:
    token = tokens[scenario]
    body = f"""
<h1>Clipboard source</h1>
<label>Selected source<textarea id="source">{html.escape(token)}</textarea></label>
"""
    script = copy_listener(scenario, "source", pass_title)
    script += f"""
addEventListener("load", () => {{
    source.focus();
    source.select();
    requestAnimationFrame(() => {{ document.title = {json.dumps(ready_title)}; }});
}});
"""
    return page("MUX_CLIP_SOURCE_LOADING", body, script)


def target_page(scenario: str, ready_title: str, pass_title: str) -> bytes:
    body = """
<h1>Clipboard destination</h1>
<label>Paste destination<textarea id="target"></textarea></label>
"""
    script = paste_listener(scenario, "target", pass_title)
    script += f"""
addEventListener("load", () => {{
    target.focus();
    requestAnimationFrame(() => {{ document.title = {json.dumps(ready_title)}; }});
}});
"""
    return page("MUX_CLIP_TARGET_LOADING", body, script)


def passive_read_page() -> bytes:
    body = """
<h1>Passive clipboard-read boundary</h1>
<p>This page attempts one unprompted Clipboard API read without a user gesture.</p>
"""
    script = """
addEventListener("load", async () => {
    if (!navigator.clipboard || typeof navigator.clipboard.readText !== "function") {
        await report({
            scenario: "passive-read",
            event: "read",
            kind: "passive_read",
            outcome: "unavailable",
            error_name: "APIUnavailable",
        }, "MUX_CLIP_PRIVACY_UNAVAILABLE", "MUX_CLIP_PRIVACY_FAIL");
        return;
    }
    try {
        const value = await navigator.clipboard.readText();
        await report({
            scenario: "passive-read",
            event: "read",
            kind: "passive_read",
            outcome: "resolved",
            digest: await sha256(value),
            length: new TextEncoder().encode(value).length,
        }, "MUX_CLIP_PRIVACY_FAIL", "MUX_CLIP_PRIVACY_FAIL");
    } catch (error) {
        await report({
            scenario: "passive-read",
            event: "read",
            kind: "passive_read",
            outcome: "rejected",
            error_name: error && error.name ? error.name : "Rejected",
        }, "MUX_CLIP_PRIVACY_BLOCKED", "MUX_CLIP_PRIVACY_FAIL");
    }
});
"""
    return page("MUX_CLIP_PRIVACY_LOADING", body, script)


PAGES = {
    "/index.html": static_page(
        "MUX_SMOKE_BOOT_LOADED", "Mux boot fixture rendered", "#d7f4e8"
    ),
    "/bar.html": static_page(
        "MUX_SMOKE_BAR_LOADED", "Mux global URL bar fixture rendered", "#f7dfb2"
    ),
    "/second.html": static_page(
        "MUX_SMOKE_SECOND_LOADED", "Mux targeted navigation fixture rendered", "#c8e3fa"
    ),
    "/clipboard-same.html": source_page(
        "same", "MUX_CLIP_SAME_SOURCE_READY", "MUX_CLIP_SAME_COPY_PASS"
    ),
    "/clipboard-same-target.html": target_page(
        "same", "MUX_CLIP_SAME_TARGET_READY", "MUX_CLIP_SAME_PASS"
    ),
    "/clipboard-cross-pane-source.html": source_page(
        "cross-pane", "MUX_CLIP_CROSS_PANE_SOURCE_READY", "MUX_CLIP_CROSS_PANE_COPY_PASS"
    ),
    "/clipboard-cross-pane-target.html": target_page(
        "cross-pane", "MUX_CLIP_CROSS_PANE_TARGET_READY", "MUX_CLIP_CROSS_PANE_PASS"
    ),
    "/clipboard-cross-profile-source.html": source_page(
        "cross-profile",
        "MUX_CLIP_CROSS_PROFILE_SOURCE_READY",
        "MUX_CLIP_CROSS_PROFILE_COPY_PASS",
    ),
    "/clipboard-cross-profile-target.html": target_page(
        "cross-profile",
        "MUX_CLIP_CROSS_PROFILE_TARGET_READY",
        "MUX_CLIP_CROSS_PROFILE_PASS",
    ),
    "/clipboard-passive-read.html": passive_read_page(),
}


def verify_report(payload: object) -> tuple[int, str]:
    if not isinstance(payload, dict):
        return 400, "invalid-payload"
    scenario = safe_atom(payload.get("scenario"), "")
    event = safe_atom(payload.get("event"), "")
    kind = safe_atom(payload.get("kind"), "")
    if scenario == "passive-read" and event == "read" and kind == "passive_read":
        outcome = safe_atom(payload.get("outcome"), "invalid")
        error_name = safe_atom(payload.get("error_name"))
        passed = outcome in {"rejected", "unavailable"}
        fields: dict[str, object] = {
            "passed": passed,
            "mode": outcome,
            "error": error_name,
        }
        if outcome == "resolved":
            fields["actual_sha256"] = safe_atom(payload.get("digest"))
            fields["actual_length"] = payload.get("length", "invalid")
            fields["reason"] = "ambient-read-resolved"
        write_status(scenario, event, fields)
        return (204, "accepted") if passed else (422, "privacy-boundary-failed")

    if scenario not in tokens or event not in {"copy", "paste"}:
        return 400, "unknown-event"
    expected = tokens[scenario]
    expected_digest = digest(expected)
    expected_length = len(expected.encode("utf-8"))
    actual_digest = safe_atom(payload.get("digest"))
    actual_length = payload.get("length", -1)
    if event == "copy" and kind == "copy":
        trusted = payload.get("trusted") is True
        selected_all = payload.get("selected_all") is True
        passed = (
            trusted
            and selected_all
            and actual_digest == expected_digest
            and actual_length == expected_length
        )
        fields = {
            "passed": passed,
            "expected_sha256": expected_digest,
            "actual_sha256": actual_digest,
            "length": actual_length,
            "trusted": int(trusted),
            "selected_all": int(selected_all),
        }
    elif event == "paste" and kind == "paste":
        paste_trusted = payload.get("paste_trusted") is True
        input_trusted = payload.get("input_trusted") is True
        input_type = safe_atom(payload.get("input_type"))
        passed = (
            paste_trusted
            and input_trusted
            and input_type == "insertFromPaste"
            and actual_digest == expected_digest
            and actual_length == expected_length
        )
        fields = {
            "passed": passed,
            "expected_sha256": expected_digest,
            "actual_sha256": actual_digest,
            "length": actual_length,
            "paste_trusted": int(paste_trusted),
            "input_trusted": int(input_trusted),
            "input_type": input_type,
        }
    else:
        return 400, "kind-mismatch"
    write_status(scenario, event, fields)
    return (204, "accepted") if passed else (422, "clipboard-event-failed")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, _format: str, *_args: object) -> None:
        return

    def send_bytes(self, status: int, content_type: str, body: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        path = urlsplit(self.path).path
        if path == "/health":
            self.send_bytes(200, "text/plain; charset=ascii", b"ok\n")
            return
        body = PAGES.get(path)
        if body is None:
            self.send_bytes(404, "text/plain; charset=ascii", b"not found\n")
            return
        self.send_bytes(200, "text/html; charset=utf-8", body)

    def do_POST(self) -> None:
        if urlsplit(self.path).path != f"/report/{capability}":
            self.send_bytes(404, "application/json", b'{"error":"not-found"}\n')
            return
        if self.headers.get("Origin") != origin:
            self.send_bytes(403, "application/json", b'{"error":"origin"}\n')
            return
        if self.headers.get_content_type() != "application/json":
            self.send_bytes(415, "application/json", b'{"error":"content-type"}\n')
            return
        try:
            length = int(self.headers.get("Content-Length", "-1"))
        except ValueError:
            length = -1
        if length < 0 or length > MAX_REPORT_BYTES:
            self.send_bytes(413, "application/json", b'{"error":"size"}\n')
            return
        try:
            payload = json.loads(self.rfile.read(length))
        except (UnicodeDecodeError, json.JSONDecodeError):
            self.send_bytes(400, "application/json", b'{"error":"json"}\n')
            return
        status, message = verify_report(payload)
        body = json.dumps({"result": message}, separators=(",", ":")).encode("ascii") + b"\n"
        self.send_bytes(status, "application/json", body)


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
server.daemon_threads = True
origin = f"http://127.0.0.1:{server.server_port}"
port_file.write_text(str(server.server_port), encoding="ascii")
os.chmod(port_file, 0o600)
server.serve_forever()
