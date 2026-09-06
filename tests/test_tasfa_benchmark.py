"""The endpoint gate must fail on corrupt responses, including -O."""
import io
from pathlib import Path
from types import SimpleNamespace
from typing import Any
import unittest

BENCHMARK = Path(__file__).resolve().parents[1] / "tools/benchmark_tasfa_compression.py"


class GateTests(unittest.TestCase):
    def check_failure(self, headers_ok, encoding, verify_result):
        for optimize in (0, 1, 2):
            for function in ("verify_endpoint", "measure"):
                with self.subTest(optimize=optimize, function=function):
                    scope: dict[str, Any] = {"__file__": str(BENCHMARK), "__name__": "benchmark_under_test"}
                    exec(compile(BENCHMARK.read_text(), str(BENCHMARK), "exec", optimize=optimize), scope)
                    server = SimpleNamespace(server_port=1, shutdown=lambda: None, server_close=lambda: None)
                    scope["run_server"] = lambda *args: (server, SimpleNamespace(join=lambda: None))
                    scope["request"] = lambda *args: (b"response", {"headers_ok": headers_ok, "encoding": encoding}, 1.0, 1.0)
                    path = SimpleNamespace(open=lambda *args: io.BytesIO(b"expected"), stat=lambda: SimpleNamespace(st_size=8))
                    fixtures = {name: (path, "application/octet-stream") for name in ("random", "text")}
                    calls = []
                    def verify(*args):
                        calls.append(args)
                        return verify_result
                    lib = SimpleNamespace(fixture_verify=verify)
                    with self.assertRaises(AssertionError):
                        if function == "verify_endpoint":
                            scope[function](lib, fixtures)
                        else:
                            scope[function](lib, fixtures, [1], [1], 1)
                    if headers_ok and not encoding:
                        self.assertEqual(len(calls), 1)

    def test_bad_headers_fail(self):
        self.check_failure(False, "", 1)

    def test_unadvertised_encoding_fails(self):
        self.check_failure(True, "unsupported", 1)

    def test_bad_bytes_fail(self):
        self.check_failure(True, "", 0)


class PacerTests(unittest.TestCase):
    def load(self):
        scope: dict[str, Any] = {"__file__": str(BENCHMARK), "__name__": "benchmark_under_test"}
        exec(compile(BENCHMARK.read_text(), str(BENCHMARK), "exec"), scope)
        self.assertIn("PayloadPacer", scope)
        return scope

    def test_rate_must_be_positive_and_finite(self):
        pacer = self.load()["PayloadPacer"]
        for rate in (0, -1, float("inf"), float("nan")):
            with self.subTest(rate=rate), self.assertRaises(ValueError):
                pacer(rate)

    def test_reservations_share_one_link_and_do_not_bank_idle_time(self):
        import concurrent.futures
        now = [0.0]
        pacer = self.load()["PayloadPacer"](1, clock=lambda: now[0])
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            waits = list(pool.map(pacer.reserve, [125000] * 4))
        self.assertEqual(sorted(waits), [1, 2, 3, 4])
        now[0] = 20
        self.assertEqual(pacer.reserve(125000), 1)
        self.assertEqual(pacer.reserve(0), 0)
        with self.assertRaises(ValueError):
            pacer.reserve(-1)

    def test_paced_writes_keep_exact_bytes_and_charge_the_tail(self):
        scope = self.load()
        now = [0.0]
        def sleep(seconds):
            now[0] += seconds
        pacer = scope["PayloadPacer"](8, clock=lambda: now[0])
        body = b"a" * (65536 + 3)
        output = io.BytesIO()
        scope["write_paced"](output, body, pacer, sleep=sleep)
        self.assertEqual(output.getvalue(), body)
        self.assertAlmostEqual(now[0], len(body) / 1000000)

    def test_accepted_socket_uses_nodelay(self):
        import socket
        import urllib.request
        server, thread = self.load()["run_server"](None, {}, 100)
        seen = []
        def probe(handler):
            seen.append(bool(handler.connection.getsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY)))
            handler.send_response(200)
            handler.send_header("Content-Length", "0")
            handler.end_headers()
        server.RequestHandlerClass.do_GET = probe
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{server.server_port}/", timeout=3) as response:
                self.assertEqual(response.read(), b"")
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)
        self.assertEqual(seen, [True])

    def test_sleep_overshoot_does_not_accumulate_or_bank_idle_credit(self):
        scope = self.load()
        now = [0.0]
        delays = []
        def sleep(seconds):
            delays.append(seconds)
            now[0] += seconds + 0.05
        pacer = scope["PayloadPacer"](1, clock=lambda: now[0])
        scope["write_paced"](io.BytesIO(), b"a" * (65536 * 4), pacer, sleep=sleep)
        self.assertAlmostEqual(now[0], 65536 * 4 / 125000 + 0.05)
        now[0] += 10
        scope["write_paced"](io.BytesIO(), b"b" * 1000, pacer, sleep=sleep)
        self.assertAlmostEqual(delays[-1], 1000 / 125000)

    def test_unpaced_writes_do_not_sleep(self):
        scope = self.load()
        output = io.BytesIO()
        scope["write_paced"](output, b"abc", None, sleep=lambda _: self.fail("slept"))
        self.assertEqual(output.getvalue(), b"abc")


if __name__ == "__main__":
    unittest.main()
