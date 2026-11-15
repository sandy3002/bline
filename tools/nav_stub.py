#!/usr/bin/env python3
"""Simulation server for Bline Tri-Screen Dashboard.

Serves both /nav (JSON telemetry) and /map (JPEG raster tiles with nav headers)
for bench testing the ESP32-S3 firmware and testing failover recovery.

Usage:
  python3 tools/nav_stub.py --demo
  python3 tools/nav_stub.py --demo --fail-map
  python3 tools/nav_stub.py --host 0.0.0.0 --port 8080

Runtime Failover Simulation:
  curl http://localhost:8080/fail-map?on=1    # trigger 503 on /map (triggers Screen 1 -> Screen 2 failover)
  curl http://localhost:8080/fail-map?on=0    # restore /map (tests 2-frame automatic recovery to Screen 1)
"""

from __future__ import annotations

import argparse
import io
import json
import math
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

try:
    from PIL import Image, ImageDraw
    HAS_PIL = True
except ImportError:
    HAS_PIL = False

MANEUVERS = [
    "straight",
    "slight-right",
    "turn-right",
    "sharp-right",
    "uturn",
    "sharp-left",
    "turn-left",
    "slight-left",
    "roundabout",
    "arrive",
]

state = {
    "nav": True,
    "maneuver": "turn-left",
    "dist_text": "240 m",
    "street": "MG Road",
    "eta": "12:34",
    "remain": "4.2 km",
    "seq": 1,
    "speed_kmh": 68.5,
    "bearing": 182.4,
    "trip_dist_km": 14.2,
    "elapsed_s": 1540,
    "map_available": True,
}

lock = threading.Lock()
demo = False
fail_map = False
start_time = time.time()


def snapshot() -> dict:
    with lock:
        return dict(state)


def bump(update: dict) -> None:
    with lock:
        state.update(update)
        state["seq"] = int(state["seq"]) + 1


def generate_map_jpeg(bearing: float, speed_kmh: float, street: str) -> bytes:
    if not HAS_PIL:
        # Minimal valid 1x1 JPEG fallback if PIL is missing
        return (
            b"\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01\x01\x01\x00H\x00H\x00\x00\xff\xdb\x00C\x00"
            b"\x08\x06\x06\x07\x06\x05\x08\x07\x07\x07\t\t\x08\n\x0c\x14\r\x0c\x0b\x0b\x0c\x19"
            b"\x12\x13\x0f\x14\x1d\x1a\x1f\x1e\x1d\x1a\x1c\x1c $.' \",#\x1c\x1c(7),01444\x1f"
            b"'9=82<.342\xff\xc0\x00\x0b\x08\x00\xf0\x00\xf0\x01\x01\x11\x00\xff\xc4\x00\x1f"
            b"\x00\x00\x01\x05\x01\x01\x01\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x01\x02"
            b"\x03\x04\x05\x06\x07\x08\t\n\x0b\xff\xda\x00\x08\x01\x01\x00\x00?\x00\xbf\x00\xff\xd9"
        )

    # Generate synthetic 240x240 dark-theme MapTiler-style tile
    img = Image.new("RGB", (240, 240), color=(25, 26, 26))
    draw = ImageDraw.Draw(img)

    # Subtle grid lines
    for x in range(0, 240, 40):
        draw.line([(x, 0), (x, 240)], fill=(36, 40, 40), width=1)
    for y in range(0, 240, 40):
        draw.line([(0, y), (240, y)], fill=(36, 40, 40), width=1)

    # Simulated road passing through vehicle position (120, 160)
    rad = math.radians(bearing)
    dx = math.sin(rad) * 120
    dy = -math.cos(rad) * 120

    # Road casing
    draw.line([(120 - dx, 160 - dy), (120 + dx, 160 + dy)], fill=(65, 70, 75), width=16)
    # Road centerline
    draw.line([(120 - dx, 160 - dy), (120 + dx, 160 + dy)], fill=(160, 165, 170), width=2)

    # Cross street
    cx = math.cos(rad) * 90
    cy = math.sin(rad) * 90
    draw.line([(120 - cx, 160 - cy), (120 + cx, 160 + cy)], fill=(50, 55, 60), width=10)

    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=75)
    return buf.getvalue()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt: str, *args) -> None:
        print("%s - %s" % (self.address_string(), fmt % args))

    def do_GET(self) -> None:
        global fail_map
        parsed = urlparse(self.path)
        qs = parse_qs(parsed.query)

        # Failover test hook
        if parsed.path == "/fail-map":
            if "on" in qs:
                fail_map = qs["on"][0] in ("1", "true", "True")
                bump({"map_available": not fail_map})
            msg = f"fail_map is {'ON (503)' if fail_map else 'OFF (200 OK)'}\n".encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(msg)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(msg)
            return

        if parsed.path in ("/map", "/map.jpg"):
            if fail_map:
                self.send_response(503)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                return

            snap = snapshot()
            body = generate_map_jpeg(snap["bearing"], snap["speed_kmh"], snap["street"])
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("X-Nav-Dist", snap["dist_text"])
            self.send_header("X-Nav-Maneuver", snap["maneuver"])
            self.send_header("X-Nav-Speed", f"{snap['speed_kmh']:.1f}")
            self.send_header("X-Nav-Street", snap["street"])
            self.send_header("X-Nav-Eta", snap["eta"])
            self.end_headers()
            self.wfile.write(body)
            return

        if parsed.path not in ("/nav", "/"):
            self.send_error(404)
            return

        if qs:
            update = {}
            if "nav" in qs:
                update["nav"] = qs["nav"][0] not in ("0", "false", "False")
            if "maneuver" in qs:
                update["maneuver"] = qs["maneuver"][0]
            dist = qs.get("dist", [None])[0]
            unit = qs.get("unit", ["m"])[0]
            if dist is not None:
                update["dist_text"] = f"{dist} {unit}"
            if "dist_text" in qs:
                update["dist_text"] = qs["dist_text"][0]
            if "street" in qs:
                update["street"] = qs["street"][0]
            if "eta" in qs:
                update["eta"] = qs["eta"][0]
            if "remain" in qs:
                update["remain"] = qs["remain"][0]
            if "speed" in qs:
                try:
                    update["speed_kmh"] = float(qs["speed"][0])
                except (ValueError, TypeError):
                    pass
            if "bearing" in qs:
                try:
                    update["bearing"] = float(qs["bearing"][0])
                except (ValueError, TypeError):
                    pass
            if update:
                bump(update)

        body = json.dumps(snapshot()).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)


def demo_loop() -> None:
    meters = 500
    man_i = 0
    bearing = 0.0
    trip_km = 12.0
    while True:
        man = MANEUVERS[man_i]
        elapsed = int(time.time() - start_time)
        bearing = (bearing + 1.5) % 360.0
        trip_km += 0.02
        speed = 64.0 + 8.0 * math.sin(elapsed / 5.0)

        if man == "arrive":
            bump(
                {
                    "nav": True,
                    "maneuver": "arrive",
                    "dist_text": "0 m",
                    "street": "Destination",
                    "eta": "now",
                    "remain": "0 m",
                    "speed_kmh": 0.0,
                    "bearing": round(bearing, 1),
                    "trip_dist_km": round(trip_km, 1),
                    "elapsed_s": elapsed,
                    "map_available": not fail_map,
                }
            )
            time.sleep(4)
            man_i = (man_i + 1) % len(MANEUVERS)
            meters = 500
            continue

        bump(
            {
                "nav": True,
                "maneuver": man,
                "dist_text": f"{meters} m",
                "street": f"{man.replace('-', ' ').title()} onto Demo Ave",
                "eta": "12:34",
                "remain": f"{max(meters / 1000.0, 0.1):.1f} km",
                "speed_kmh": round(speed, 1),
                "bearing": round(bearing, 1),
                "trip_dist_km": round(trip_km, 1),
                "elapsed_s": elapsed,
                "map_available": not fail_map,
            }
        )
        time.sleep(0.8)
        meters -= 40
        if meters <= 20:
            man_i = (man_i + 1) % len(MANEUVERS)
            meters = 500


def main() -> None:
    global demo, fail_map
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--demo", action="store_true", help="Auto-countdown and simulate telemetry")
    p.add_argument("--fail-map", action="store_true", help="Simulate 503 on /map to test failover")
    args = p.parse_args()
    demo = args.demo
    fail_map = args.fail_map

    if demo:
        threading.Thread(target=demo_loop, daemon=True).start()
        print("Demo mode running: cycling maneuvers, bearing, speed, and rotating map")

    if fail_map:
        print("FAIL-MAP mode: /map will return 503 Service Unavailable")

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"Bline Nav Stub serving on http://{args.host}:{args.port}")
    print(f"  - GET /nav  (JSON)")
    print(f"  - GET /map  (JPEG)")
    print(f"  - GET /fail-map?on=1 (Trigger failover)")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
