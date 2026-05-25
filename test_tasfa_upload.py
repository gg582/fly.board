#!/usr/bin/env python3
"""Simple TASFA upload test: create a 50 MB file and upload via /file/upload/* endpoints."""

import os
import sys
import time
import urllib.request
import urllib.parse
import urllib.error
import json
import ssl

HOST = "https://127.0.0.1:8888"
FILE_SIZE = 50 * 1024 * 1024  # 50 MB
FILENAME = "test_50mb.bin"
TEST_FILE = "/tmp/test_50mb.bin"

ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE


def make_request(path, data=None, headers=None, method=None):
    url = HOST + path
    req = urllib.request.Request(url, data=data, headers=headers or {}, method=method)
    try:
        with urllib.request.urlopen(req, context=ctx, timeout=60) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:
        return 0, str(e).encode()


def create_test_file():
    if os.path.exists(TEST_FILE) and os.path.getsize(TEST_FILE) == FILE_SIZE:
        print("Using existing test file.")
        return
    print(f"Creating {FILE_SIZE} bytes test file...")
    with open(TEST_FILE, "wb") as f:
        f.write(os.urandom(FILE_SIZE))
    print("Done.")


def upload_plain():
    create_test_file()

    # 1. Init session
    print("Init upload session...")
    body = urllib.parse.urlencode({
        "filename": FILENAME,
        "total_size": str(FILE_SIZE),
        "chunk_count": "50",
        "chunk_size": str(1024 * 1024),
        "post_id": "0",
        "session_id": "test-session-1"
    }).encode()
    status, resp = make_request("/file/upload/init", data=body, headers={
        "Content-Type": "application/x-www-form-urlencoded",
        "Accept": "application/json"
    })
    if status != 200:
        print(f"Init failed: HTTP {status} - {resp.decode()}")
        return False
    payload = json.loads(resp)
    upload_id = payload.get("upload_id")
    upload_token = payload.get("upload_token")
    chunk_size = int(payload.get("chunk_size", 4 * 1024 * 1024))
    chunk_count = int(payload.get("chunk_count", 1))
    if not upload_id or not upload_token:
        print(f"Invalid init response: {payload}")
        return False
    print(f"Session created: upload_id={upload_id} chunk_size={chunk_size} chunk_count={chunk_count}")

    # 2. Upload chunks
    start_time = time.time()
    last_report = start_time
    with open(TEST_FILE, "rb") as f:
        for i in range(chunk_count):
            offset = i * chunk_size
            data = f.read(chunk_size)
            req = urllib.request.Request(
                HOST + "/file/upload",
                data=data,
                headers={
                    "X-TASFA-Upload-ID": upload_id,
                    "X-TASFA-Upload-Token": upload_token,
                    "X-TASFA-Chunk-Index": str(i),
                    "Content-Type": "application/octet-stream",
                },
                method="POST"
            )
            try:
                with urllib.request.urlopen(req, context=ctx, timeout=60) as resp:
                    if resp.status not in (200, 204):
                        body_text = resp.read().decode()
                        print(f"Chunk {i} failed: HTTP {resp.status} - {body_text}")
                        return False
            except urllib.error.HTTPError as e:
                print(f"Chunk {i} error: HTTP {e.code} - {e.read().decode()}")
                return False
            except Exception as e:
                print(f"Chunk {i} error: {e}")
                return False

            now = time.time()
            if now - last_report >= 2 or i == chunk_count - 1:
                pct = (i + 1) / chunk_count * 100
                elapsed = now - start_time
                sent = offset + len(data)
                mbps = sent / 1024 / 1024 / elapsed if elapsed > 0 else 0
                print(f"  [{pct:.1f}%] chunk {i}/{chunk_count}  {mbps:.2f} MB/s")
                last_report = now

    # 3. Complete
    print("Completing upload...")
    body = urllib.parse.urlencode({
        "upload_id": upload_id,
        "upload_token": upload_token
    }).encode()
    status, resp = make_request("/file/upload/complete", data=body, headers={
        "Content-Type": "application/x-www-form-urlencoded",
        "Accept": "application/json"
    })
    if status != 200:
        print(f"Complete failed: HTTP {status} - {resp.decode()}")
        return False
    payload = json.loads(resp)
    if not payload.get("ok"):
        print(f"Complete rejected: {payload}")
        return False
    print(f"Upload complete! file_id={payload.get('file_id')} url={payload.get('url')}")
    total_elapsed = time.time() - start_time
    print(f"Total time: {total_elapsed:.2f}s  Average: {FILE_SIZE/1024/1024/total_elapsed:.2f} MB/s")
    return True


if __name__ == "__main__":
    ok = upload_plain()
    sys.exit(0 if ok else 1)
