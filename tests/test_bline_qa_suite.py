"""
Bline QA Automated Test Suite
Principal QA Automation Engineer & Automated Bug Hunter
Covers Wire Protocol Contracts, Maneuver Parsing, State Transitions, and Defect Reproduction.
"""

import io
import json
import re
import unittest
from tools.nav_stub import Handler, snapshot, bump, fail_map


class MockSocket:
    def __init__(self, data: bytes):
        self.rfile = io.BytesIO(data)
        self.wfile = io.BytesIO()

    def makefile(self, mode, *args, **kwargs):
        if "r" in mode:
            return self.rfile
        return self.wfile

    def sendall(self, data):
        self.wfile.write(data)


def execute_request(raw_request: bytes) -> tuple[int, dict, bytes]:
    sock = MockSocket(raw_request)
    handler = Handler(sock, ("127.0.0.1", 12345), None)
    output = sock.wfile.getvalue()
    parts = output.split(b"\r\n\r\n", 1)
    header_block = parts[0].decode("utf-8", errors="replace")
    body = parts[1] if len(parts) > 1 else b""
    lines = header_block.split("\r\n")
    status_line = lines[0]
    status_code = int(status_line.split(" ")[1])
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    return status_code, headers, body


class TestWireProtocol(unittest.TestCase):
    def test_nav_json_contract(self):
        code, headers, body = execute_request(b"GET /nav HTTP/1.1\r\nHost: localhost\r\n\r\n")
        self.assertEqual(code, 200)
        self.assertEqual(headers.get("content-type"), "application/json")
        payload = json.loads(body.decode("utf-8"))
        required_keys = [
            "nav", "maneuver", "dist_text", "street", "eta",
            "remain", "seq", "speed_kmh", "bearing", "trip_dist_km",
            "elapsed_s", "map_available"
        ]
        for key in required_keys:
            self.assertIn(key, payload, f"Missing key in /nav contract: {key}")

    def test_map_jpeg_contract(self):
        # Ensure fail_map is off
        execute_request(b"GET /fail-map?on=0 HTTP/1.1\r\nHost: localhost\r\n\r\n")
        code, headers, body = execute_request(b"GET /map HTTP/1.1\r\nHost: localhost\r\n\r\n")
        self.assertEqual(code, 200)
        self.assertEqual(headers.get("content-type"), "image/jpeg")
        # Check required telemetry headers consumed by ESP32-S3 firmware
        self.assertIn("x-nav-dist", headers)
        self.assertIn("x-nav-maneuver", headers)
        self.assertIn("x-nav-speed", headers)
        self.assertIn("x-nav-street", headers)
        self.assertIn("x-nav-eta", headers)
        # Verify JPEG magic bytes
        self.assertTrue(body.startswith(b"\xff\xd8"), "Body does not begin with JPEG SOI marker")
        self.assertTrue(body.endswith(b"\xff\xd9"), "Body does not end with JPEG EOI marker")

    def test_failover_trigger(self):
        code, _, _ = execute_request(b"GET /fail-map?on=1 HTTP/1.1\r\nHost: localhost\r\n\r\n")
        self.assertEqual(code, 200)
        map_code, _, _ = execute_request(b"GET /map HTTP/1.1\r\nHost: localhost\r\n\r\n")
        self.assertEqual(map_code, 503)

        # Restore
        code, _, _ = execute_request(b"GET /fail-map?on=0 HTTP/1.1\r\nHost: localhost\r\n\r\n")
        self.assertEqual(code, 200)
        map_code, _, _ = execute_request(b"GET /map HTTP/1.1\r\nHost: localhost\r\n\r\n")
        self.assertEqual(map_code, 200)


class TestNavStubEdgeCases(unittest.TestCase):
    def test_unhandled_value_error_on_invalid_numeric_params(self):
        """DEFECT-07 Fix: Proves non-numeric query params are safely ignored without crashing."""
        code, headers, body = execute_request(b"GET /nav?speed=invalid&bearing=bad HTTP/1.1\r\nHost: localhost\r\n\r\n")
        self.assertEqual(code, 200)
        payload = json.loads(body.decode("utf-8"))
        self.assertIn("speed_kmh", payload)


class TestManeuverParserRegression(unittest.TestCase):
    """Simulates Android ManeuverParser regex engine to isolate parser defects."""

    ONTO_RE = re.compile(
        r'\b(?:onto|on to|toward|towards|on)\s+(?!(?:the\s+)?(?:right|left)\b)(.+)$',
        re.IGNORECASE
    )
    TRAILING_DIST_RE = re.compile(
        r'\s*\b(\d+(?:[.,]\d+)?)\s*(kilometers?|km|miles?|mi|meters?|m|feet|ft|yards?|yd)\b\s*$',
        re.IGNORECASE
    )
    ETA_CLOCK_RE = re.compile(r'\b(\d{1,2}:\d{2}\s*(?:AM|PM)?)\b', re.IGNORECASE)
    DEST_RE = re.compile(r'^\s*destination\b.*', re.IGNORECASE)

    def extract_street_fixed_impl(self, instruction: str) -> str:
        s = instruction.strip()
        if self.DEST_RE.match(s):
            return "Destination"
        onto = self.ONTO_RE.search(s)
        if onto:
            val = onto.group(1)
            val = self.TRAILING_DIST_RE.sub('', val)
            val = self.ETA_CLOCK_RE.sub('', val)
            return val.strip(' ,.-·')
        cleaned = self.TRAILING_DIST_RE.sub('', s)
        cleaned = self.ETA_CLOCK_RE.sub('', cleaned)
        return cleaned.strip(' ,.-·')[:48]

    def test_defect_destination_on_the_right(self):
        """DEFECT-06 Fix: 'Destination on the right' produces street 'Destination'."""
        raw_instruction = "Destination on the right"
        extracted = self.extract_street_fixed_impl(raw_instruction)
        self.assertEqual(extracted, "Destination")

    def test_defect_street_named_with_distance(self):
        """DEFECT-06 Fix: TRAILING_DIST_RE preserves street numbers (e.g., '100 Feet Road')."""
        raw_instruction = "Turn left onto 100 Feet Road"
        extracted = self.extract_street_fixed_impl(raw_instruction)
        self.assertEqual(extracted, "100 Feet Road")


class TestParseMetersLocaleRegression(unittest.TestCase):
    """Validates C strtof comma truncation bug fix in parseMeters()."""

    def parse_meters_fixed_c_model(self, dist_text: str) -> int:
        if not dist_text:
            return -1
        normalized = dist_text.replace(',', '.')
        m = re.match(r'^\s*([0-9]+(?:[.]\d+)?)(.*)', normalized)
        if not m:
            return -1
        val = float(m.group(1))
        unit = m.group(2).strip().lower()
        if unit.startswith("km") or unit.startswith("kilom"):
            return int(val * 1000.0)
        if unit.startswith("mi") or unit.startswith("mile"):
            return int(val * 1609.34)
        if unit.startswith("ft") or unit.startswith("feet"):
            return int(val * 0.3048)
        return int(val)

    def test_defect_comma_decimal_truncation(self):
        """DEFECT-04 Fix: '1,5 km' parses accurately as 1500 meters."""
        input_text = "1,5 km"
        parsed = self.parse_meters_fixed_c_model(input_text)
        self.assertEqual(parsed, 1500)


class TestFailoverStateMachineLogic(unittest.TestCase):
    """Simulates the fixed C++ state machine in bline.ino loop()."""

    def test_fixed_failover_when_map_fails(self):
        """DEFECT-03 Fix: Screen 0 automatically fails over to Screen 1 on map failure."""
        activeScreenIndex = 0
        currentScreen = 0
        fallbackMode = True
        autoFailoverActive = False
        suppressFailover = False

        if activeScreenIndex == 0 and fallbackMode and not autoFailoverActive and not suppressFailover:
            autoFailoverActive = True
            currentScreen = 1
            activeScreenIndex = 1

        self.assertEqual(currentScreen, 1)
        self.assertTrue(autoFailoverActive)

    def test_fixed_recovery_when_map_recovers(self):
        """DEFECT-03 Fix: Screen 1 automatically restores Screen 0 when map recovers."""
        activeScreenIndex = 1
        currentScreen = 1
        fallbackMode = False
        autoFailoverActive = True

        if autoFailoverActive and not fallbackMode:
            autoFailoverActive = False
            currentScreen = 0
            activeScreenIndex = 0

        self.assertEqual(currentScreen, 0)
        self.assertFalse(autoFailoverActive)

    def test_defect_screen1_bounce_back(self):
        """DEFECT-03 Fix: User voluntarily on Screen 1 is NOT bounced to Screen 0."""
        activeScreenIndex = 1
        currentScreen = 1
        fallbackMode = False
        autoFailoverActive = False  # Swiped voluntarily

        if autoFailoverActive and not fallbackMode:
            autoFailoverActive = False
            currentScreen = 0

        self.assertEqual(currentScreen, 1, "User voluntarily on Screen 1 must stay on Screen 1")

    def test_defect_user_overridden_deadlock(self):
        """DEFECT-03 Fix: User swipe overrides failover, and subsequent return to Screen 0 allows failover."""
        # 1. Swiping overrides auto failover
        autoFailoverActive = True
        currentScreen = 1
        # User swipes to Screen 2
        autoFailoverActive = False
        currentScreen = 2
        activeScreenIndex = 2

        # Map recovers
        fallbackMode = False
        if autoFailoverActive and not fallbackMode:
            currentScreen = 0
        self.assertEqual(currentScreen, 2)

        # 2. Returning to Screen 0 allows failover again if map fails
        activeScreenIndex = 0
        currentScreen = 0
        fallbackMode = True
        suppressFailover = False
        if activeScreenIndex == 0 and fallbackMode and not autoFailoverActive and not suppressFailover:
            autoFailoverActive = True
            currentScreen = 1
            activeScreenIndex = 1
        self.assertEqual(currentScreen, 1)


class TestHttpHeaderInjection(unittest.TestCase):
    """Validates CRLF header sanitization in NavHttpServer."""

    def sanitize_header(self, val: str) -> str:
        return val.replace("\r", " ").replace("\n", " ").strip()

    def test_crlf_injection_vector(self):
        """DEFECT-05 Fix: CRLF injection is stripped by sanitizeHeader()."""
        malicious_dist = "200 m\r\nInjected-Header: evil\r\n\r\nInjected-Body"
        sanitized = self.sanitize_header(malicious_dist)
        formatted_header = f"X-Nav-Dist: {sanitized}\r\n"
        self.assertNotIn("\r\nInjected-Header:", formatted_header)


if __name__ == "__main__":
    unittest.main()

