#!/usr/bin/env python3
import os
import socket
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HELPER = ROOT / "tests" / "test_realtime_nats"
NAME = f"flyboard-realtime-{os.getpid()}"


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, **kwargs)


def wait_port(port):
    for _ in range(200):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("NATS container did not open its port")


def main():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    url = f"nats://127.0.0.1:{port}"
    try:
        run("docker", "run", "-d", "--name", NAME,
            "-p", f"127.0.0.1:{port}:4222", "nats:2.11-alpine",
            stdout=subprocess.DEVNULL)
        wait_port(port)
        run(str(HELPER), "multi", url)
        with tempfile.TemporaryDirectory(prefix="flyboard-realtime-") as tmp:
            ready = Path(tmp) / "ready"
            process = subprocess.Popen([str(HELPER), "reconnect", url, str(ready)])
            try:
                for _ in range(200):
                    if ready.exists():
                        break
                    if process.poll() is not None:
                        raise RuntimeError("reconnect helper exited before broker outage")
                    time.sleep(0.1)
                else:
                    raise RuntimeError("reconnect helper did not connect")
                run("docker", "stop", "-t", "1", NAME, stdout=subprocess.DEVNULL)
                time.sleep(2)
                run("docker", "start", NAME, stdout=subprocess.DEVNULL)
                process.wait(timeout=60)
                if process.returncode:
                    raise RuntimeError(f"reconnect helper failed: {process.returncode}")
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
    finally:
        subprocess.run(["docker", "rm", "-f", NAME], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=False)


if __name__ == "__main__":
    main()
