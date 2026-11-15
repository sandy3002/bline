#!/usr/bin/env python3
"""Throwaway /nav server so the ESP32 display can be tested before the Android app exists.

Usage:
  python3 tools/nav_stub.py
  python3 tools/nav_stub.py --demo          # auto-countdown through maneuvers
  python3 tools/nav_stub.py --host 0.0.0.0 --port 8080

Then in secrets.h set:
  #define NAV_SERVER_HOST "YOUR_LAPTOP_IP"
  #define NAV_SERVER_PORT 8080

Query overrides (demo off):
  GET /nav
  GET /nav?maneuver=turn-left&dist=240&unit=m&street=MG+Road&eta=12:34&remain=4.2+km
  GET /nav?nav=0
"""

from __future__ import annotations

import argparse
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

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
}
lock = threading.Lock()
demo = False


def snapshot() -> dict:
    with lock:
        return dict(state)


def bump(update: dict) -> None:
    with lock:
        state.update(update)
        state["seq"] = int(state["seq"]) + 1


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt: str, *args) -> None:
        print("%s - %s" % (self.address_string(), fmt % args))

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path not in ("/nav", "/"):
            self.send_error(404)
            return

        qs = parse_qs(parsed.query)
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
            if update:
                bump(update)

        body = json.dumps(snapshot()).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)


def demo_loop() -> None:
    meters = 500
    man_i = 0
    while True:
        man = MANEUVERS[man_i]
        if man == "arrive":
            bump(
                {
                    "nav": True,
                    "maneuver": "arrive",
                    "dist_text": "0 m",
                    "street": "Destination",
                    "eta": "now",
                    "remain": "0 m",
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
                "dist_text": f"{meters} m" if meters >= 1000 else f"{meters} m",
                "street": f"{man.replace('-', ' ').title()} onto Demo St",
                "eta": "12:34",
                "remain": f"{max(meters / 1000.0, 0.1):.1f} km",
            }
        )
        time.sleep(0.8)
        meters -= 40
        if meters <= 20:
            man_i = (man_i + 1) % len(MANEUVERS)
            meters = 500


def main() -> None:
    global demo
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--demo", action="store_true")
    args = p.parse_args()
    demo = args.demo

    if demo:
        threading.Thread(target=demo_loop, daemon=True).start()
        print("demo mode: cycling maneuvers")

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"nav stub on http://{args.host}:{args.port}/nav")
    httpd.serve_forever()


if __name__ == "__main__":
    main()
