"""The canonical endpoint gate must fail on corrupt responses, including -O."""
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


if __name__ == "__main__":
    unittest.main()
