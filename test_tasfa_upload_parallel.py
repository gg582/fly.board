#!/usr/bin/env python3
"""Parallel TASFA upload test to reproduce stall at specific percentage."""

import os
import sys
import time
import urllib.request
import urllib.parse
import urllib.error
import json
import ssl
import concurrent.futures
import threading

HOST = "https://127.0.0.1:8888"
FILE_SIZE = 50 * 1024 * 1024  # 50 MB
FILENAME = "test_50mb.bin"
TEST_FILE = "/tmp/test_50mb.bin"
MAX_PARALLEL = 8

ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
lock = threading.Lock()


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


def upload_chunk(upload_id, upload_token, chunk_index, data):
    req = urllib.request.Request(
        HOST + "/file/upload",
        data=data,
        headers={
            "X-TASFA-Upload-ID": upload_id,
            "X-TASFA-Upload-Token": upload_token,
            "X-TASFA-Chunk-Index": str(chunk_index),
            "Content-Type": "application/octet-stream",
        },
        method="POST"
    )
    with urllib.request.urlopen(req, context=ctx, timeout=60) as resp:
        if resp.status not in (200, 204):
            return False, f"HTTP {resp.status}"
    return True, None


def upload_parallel():
    if not os.path.exists(TEST_FILE) or os.path.getsize(TEST_FILE) != FILE_SIZE:
        print(f"Creating {FILE_SIZE} bytes test file...")
        with open(TEST_FILE, "wb") as f:
            f.write(os.urandom(FILE_SIZE))
        print("Done.")
    else:
        print("Using existing test file.")

    # 1. Init session
    print("Init upload session...")
    body = urllib.parse.urlencode({
        "filename": FILENAME,
        "total_size": str(FILE_SIZE),
        "chunk_count": "50",
        "chunk_size": str(1024 * 1024),
        "post_id": "0",
        "session_id": "test-session-parallel"
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
    print(f"Session created: upload_id={upload_id} chunk_size={chunk_size} chunk_count={chunk_count} max_parallel={MAX_PARALLEL}")

    # Read file into memory chunks
    chunks = []
    with open(TEST_FILE, "rb") as f:
        for i in range(chunk_count):
            chunks.append(f.read(chunk_size))

    start_time = time.time()
    last_report = start_time
    completed = [0]
    failed = [False]
    failed_reason = [None]

    def report():
        now = time.time()
        if now - last_report >= 2:
            with lock:
                c = completed[0]
            pct = c / chunk_count * 100
            elapsed = now - start_time
            sent = sum(len(ch) for ch in chunks[:c])
            mbps = sent / 1024 / 1024 / elapsed if elapsed > 0 else 0
            print(f"  [{pct:.1f}%] {c}/{chunk_count} chunks  {mbps:.2f} MB/s")
            return now
        return last_report

    def worker(i):
        if failed[0]:
            return
        try:
            ok, reason = upload_chunk(upload_id, upload_token, i, chunks[i])
            if not ok:
                with lock:
                    if not failed[0]:
                        failed[0] = True
                        failed_reason[0] = f"chunk {i}: {reason}"
                return
            with lock:
                completed[0] += 1
        except Exception as e:
            with lock:
                if not failed[0]:
                    failed[0] = True
                    failed_reason[0] = f"chunk {i}: {e}"

    with concurrent.futures.ThreadPoolExecutor(max_workers=MAX_PARALLEL) as executor:
        futures = [executor.submit(worker, i) for i in range(chunk_count)]
        while completed[0] < chunk_count and not failed[0]:
            time.sleep(0.5)
            last_report = report()
        if failed[0]:
            print(f"Upload failed: {failed_reason[0]}")
            return False

    last_report = report()
    print(f"  [100.0%] {chunk_count}/{chunk_count} chunks")

    # 3. Complete
    print("Completing upload...")
    body = urllib.parse.urlencode({
        "upload_id": upload_id,
        "upload_token": upload_token
    }).encode()
    while True:
        status, resp = make_request("/file/upload/complete", data=body, headers={
            "Content-Type": "application/x-www-form-urlencoded",
            "Accept": "application/json"
        })
        if status == 202:
            payload = json.loads(resp)
            retry_after = payload.get("retry_after", 1)
            print(f"Finalizing upload, retrying in {retry_after}s...")
            time.sleep(retry_after)
            continue
        if status != 200:
            print(f"Complete failed: HTTP {status} - {resp.decode()}")
            return False
        payload = json.loads(resp)
        if not payload.get("ok"):
            print(f"Complete rejected: {payload}")
            return False
        break
    print(f"Upload complete! file_id={payload.get('file_id')} url={payload.get('url')}")
    total_elapsed = time.time() - start_time
    print(f"Total time: {total_elapsed:.2f}s  Average: {FILE_SIZE/1024/1024/total_elapsed:.2f} MB/s")
    return True


if __name__ == "__main__":
    ok = upload_parallel()
    sys.exit(0 if ok else 1)
