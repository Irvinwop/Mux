#!/usr/bin/env python3
"""Bounded loopback fixture; report intake exists only with explicit --automation."""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import socket
import threading
import time

from intake import AutomationSession, IntakeError, MAX_REPORT_BYTES

ASSETS = {
    "/index.html": "text/html; charset=utf-8",
    "/style.css": "text/css; charset=utf-8",
    "/app.mjs": "text/javascript; charset=utf-8",
    "/runner.mjs": "text/javascript; charset=utf-8",
    "/collector.mjs": "text/javascript; charset=utf-8",
    "/report.mjs": "text/javascript; charset=utf-8",
    "/worker.mjs": "text/javascript; charset=utf-8",
}
CSP = (
    "default-src 'none'; script-src 'self'; style-src 'self'; "
    "worker-src 'self'; child-src 'self'; connect-src 'none'; "
    "img-src 'none'; font-src 'none'; media-src 'none'; object-src 'none'; "
    "base-uri 'none'; form-action 'none'; frame-ancestors 'none'"
)
AUTOMATION_CSP = CSP.replace("connect-src 'none'", "connect-src 'self'")
CONNECTION_DEADLINE_SECONDS = 10


class FixtureHandler(BaseHTTPRequestHandler):
    server_version = "MuxLocalPrivacyFixture/1"
    sys_version = ""

    def setup(self):
        super().setup()
        # Absolute wall-clock cap, not merely a resettable per-read idle timeout.
        self.connection_timer = threading.Timer(CONNECTION_DEADLINE_SECONDS, self.expire_connection)
        self.connection_timer.daemon = True
        self.connection_timer.start()

    def expire_connection(self):
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.connection.close()

    def finish(self):
        self.connection_timer.cancel()
        super().finish()

    def log_message(self, _format, *_args):
        # Never retain request paths, headers, addresses, or browser observations.
        pass

    def respond(self, status, body=b"", content_type="text/plain; charset=utf-8", csp=CSP):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Content-Security-Policy", csp)
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Permissions-Policy", "camera=(), microphone=(), geolocation=(), midi=(), display-capture=(), usb=()")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def json_response(self, status, value):
        self.respond(status, json.dumps(value, separators=(",", ":")).encode("utf-8"), "application/json; charset=utf-8", AUTOMATION_CSP)

    def valid_host(self):
        return self.headers.get_all("Host", []) == [f"127.0.0.1:{self.server.server_port}"]

    def automation_resource(self):
        session = self.server.automation
        if session is None:
            raise IntakeError(404, "automation-disabled")
        parts = self.path.split("/")
        if len(parts) != 4 or parts[:2] != ["", "automation"]:
            raise IntakeError(404, "unknown-resource")
        session.authorize(parts[2])
        return parts[3]

    def do_GET(self):
        # Exact Host validation prevents use through a DNS-rebinding hostname.
        if not self.valid_host():
            self.respond(403)
            return
        if self.path.startswith("/automation/"):
            try:
                resource = self.automation_resource()
                if resource == "configuration":
                    self.json_response(200, self.server.automation.configuration)
                elif resource == "status":
                    self.json_response(200, self.server.automation.status())
                elif resource in self.server.automation_assets:
                    body, content_type = self.server.automation_assets[resource]
                    self.respond(200, body, content_type, AUTOMATION_CSP)
                else:
                    raise IntakeError(404, "unknown-resource")
            except IntakeError as error:
                self.json_response(error.status, {"error": error.code})
            return
        path = "/index.html" if self.path == "/" else self.path
        if path not in self.server.assets:
            self.respond(404)
            return
        body, content_type = self.server.assets[path]
        self.respond(200, body, content_type)

    do_HEAD = do_GET

    def do_POST(self):
        if not self.valid_host():
            self.respond(403)
            return
        if self.server.automation is None:
            self.respond(405)
            return
        try:
            if self.automation_resource() != "report":
                raise IntakeError(404, "unknown-resource")
            session = self.server.automation
            origin = f"http://127.0.0.1:{self.server.server_port}"
            if self.headers.get_all("Origin", []) != [origin]:
                raise IntakeError(403, "invalid-origin")
            tokens = self.headers.get_all("X-Mux-Privacy-Token", [])
            if len(tokens) != 1:
                raise IntakeError(403, "invalid-capability")
            session.authorize(tokens[0])
            if self.headers.get("Transfer-Encoding") is not None or self.headers.get("Expect") is not None:
                raise IntakeError(400, "unsupported-transfer")
            if self.headers.get("Content-Encoding") is not None or self.headers.get("Content-Type", "").lower() != "application/json":
                raise IntakeError(415, "unsupported-content-type")
            lengths = self.headers.get_all("Content-Length", [])
            if not lengths:
                raise IntakeError(411, "content-length-required")
            if len(lengths) != 1 or not lengths[0].isascii() or not lengths[0].isdigit() or len(lengths[0]) > 10:
                raise IntakeError(400, "invalid-content-length")
            length = int(lengths[0])
            if not 0 < length <= MAX_REPORT_BYTES:
                raise IntakeError(413, "report-size-limit")
            payload = self.rfile.read(length)
            if len(payload) != length:
                raise IntakeError(400, "incomplete-body")
            result = session.accept(payload)
            self.json_response(201, result)
        except IntakeError as error:
            self.json_response(error.status, {"error": error.code})
        except (TimeoutError, socket.timeout):
            self.json_response(408, {"error": "connection-deadline"})
        except OSError:
            self.json_response(500, {"error": "local-intake-failed"})

    def reject_method(self):
        self.respond(405)

    do_PUT = reject_method
    do_PATCH = reject_method
    do_DELETE = reject_method
    do_OPTIONS = reject_method


class FixtureServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, *args, **kwargs):
        self.connection_slots = threading.BoundedSemaphore(8)
        self.automation = None
        self.automation_assets = {}
        super().__init__(*args, **kwargs)

    def process_request(self, request, client_address):
        if not self.connection_slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self.connection_slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.connection_slots.release()

    def get_request(self):
        connection, address = super().get_request()
        connection.settimeout(5)
        return connection, address

    def handle_error(self, _request, _client_address):
        # Broken client connections must not dump headers or paths into CI logs.
        pass


def create_server(port=0, *, automation=False, label=None, rendering=False,
                  build="runtime-smoke", output_root=None, lifetime=600):
    if not automation and (label is not None or rendering or output_root is not None or build != "runtime-smoke"):
        raise ValueError("Automation options require --automation")
    if automation and label not in ("direct", "tor"):
        raise ValueError("An explicit direct or tor label is required")
    root = Path(__file__).resolve().parent
    assets = {path: ((root / path[1:]).read_bytes(), mime) for path, mime in ASSETS.items()}
    server = FixtureServer(("127.0.0.1", port), FixtureHandler)
    try:
        server.assets = assets
        if automation:
            server.automation_assets = {
                "index.html": ((root / "automation.html").read_bytes(), "text/html; charset=utf-8"),
                "automation.mjs": ((root / "automation.mjs").read_bytes(), "text/javascript; charset=utf-8"),
            }
            server.automation = AutomationSession(label, rendering, build, lifetime, output_root)
        return server
    except BaseException:
        server.server_close()
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=0, help="Loopback port; 0 selects an ephemeral port (default)")
    parser.add_argument("--lifetime", type=int, default=600, help="Exit after 1-3600 seconds (default: 600)")
    parser.add_argument("--automation", action="store_true", help="Explicitly enable one token-gated automated report and a private output directory")
    parser.add_argument("--automation-label", choices=("direct", "tor"), help="Required experiment label with --automation; does not select browser routing")
    parser.add_argument("--automation-rendering", action="store_true", help="Explicitly include optional rendering probes in automation")
    parser.add_argument("--automation-build", default="runtime-smoke", help="Synthetic build label, 1-128 ASCII characters")
    parser.add_argument("--automation-output-root", help="Existing trusted directory under which a unique owner-only output directory is created")
    args = parser.parse_args()
    if not 0 <= args.port <= 65535:
        parser.error("--port must be between 0 and 65535")
    if not 1 <= args.lifetime <= 3600:
        parser.error("--lifetime must be between 1 and 3600 seconds")
    if not args.automation and (args.automation_label is not None or args.automation_rendering or args.automation_output_root is not None or args.automation_build != "runtime-smoke"):
        parser.error("automation options require --automation")
    if args.automation and args.automation_label is None:
        parser.error("--automation requires --automation-label direct|tor")
    if not 1 <= len(args.automation_build) <= 128 or not args.automation_build.isascii():
        parser.error("--automation-build must be 1-128 ASCII characters")
    with create_server(args.port, automation=args.automation, label=args.automation_label,
                       rendering=args.automation_rendering, build=args.automation_build,
                       output_root=args.automation_output_root, lifetime=args.lifetime) as server:
        server.timeout = 0.5
        deadline = time.monotonic() + args.lifetime
        origin = f"http://127.0.0.1:{server.server_port}"
        print(json.dumps(server.automation.startup(origin)) if server.automation else f"{origin}/index.html", flush=True)
        try:
            while time.monotonic() < deadline:
                server.handle_request()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
