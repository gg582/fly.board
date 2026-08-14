#!/usr/bin/env python3
"""Local-only batched translation service for fly.board."""

import json
import os
import threading
from collections import OrderedDict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import ctranslate2
import sentencepiece as spm
from huggingface_hub import snapshot_download

MODEL_ID = os.environ.get("FLYBOARD_TRANSLATION_MODEL", "mijuanlo/nllb-200-distilled-600M-ct2-int8")
MODEL_REVISION = os.environ.get("FLYBOARD_TRANSLATION_MODEL_REVISION", "16bc5ff0482f9f1c0d35bdef950721ce58640789")
MODEL_ROOT = os.environ.get("FLYBOARD_TRANSLATION_MODEL_DIR", "/var/lib/flyboard-translation")
HOST = os.environ.get("FLYBOARD_TRANSLATION_HOST", "127.0.0.1")
PORT = int(os.environ.get("FLYBOARD_TRANSLATION_PORT", "8765"))
LANGUAGES = {
    "eng_Latn", "kor_Hang", "jpn_Jpan", "zho_Hans", "spa_Latn", "fra_Latn",
    "deu_Latn", "por_Latn", "rus_Cyrl", "arb_Arab", "hin_Deva", "ind_Latn", "vie_Latn",
}


class Engine:
    def __init__(self):
        model_path = snapshot_download(repo_id=MODEL_ID, revision=MODEL_REVISION, cache_dir=MODEL_ROOT)
        self.tokenizer = spm.SentencePieceProcessor(model_file=os.path.join(model_path, "sentencepiece.bpe.model"))
        self.translator = ctranslate2.Translator(
            model_path, device="cpu", compute_type="int8", inter_threads=1, intra_threads=4
        )
        self.inference_lock = threading.Lock()
        self.cache_lock = threading.Lock()
        self.cache = OrderedDict()

    def translate(self, chunks, source, target):
        cache_key = (source, target, tuple(chunks))
        with self.cache_lock:
            cached = self.cache.get(cache_key)
            if cached is not None:
                self.cache.move_to_end(cache_key)
                return list(cached)
        encoded = self.tokenizer.encode(chunks, out_type=str)
        source_tokens = [[source] + pieces + ["</s>"] for pieces in encoded]
        if not self.inference_lock.acquire(timeout=1):
            raise BusyError("translation engine is busy")
        try:
            results = self.translator.translate_batch(
                source_tokens,
                target_prefix=[[target]] * len(source_tokens),
                batch_type="tokens",
                max_batch_size=1024,
                beam_size=2,
                max_decoding_length=512,
            )
        finally:
            self.inference_lock.release()
        translated = [self.tokenizer.decode(result.hypotheses[0][1:]) for result in results]
        with self.cache_lock:
            self.cache[cache_key] = tuple(translated)
            self.cache.move_to_end(cache_key)
            while len(self.cache) > 256:
                self.cache.popitem(last=False)
        return translated


class BusyError(Exception):
    pass


ENGINE = Engine()


class Handler(BaseHTTPRequestHandler):
    server_version = "flyboard-translation/1"

    def log_message(self, fmt, *args):
        print("translation:", fmt % args, flush=True)

    def send_json(self, status, payload):
        body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            self.send_json(200, {"ok": True, "model": MODEL_ID})
        else:
            self.send_json(404, {"ok": False, "error": "not found"})

    def do_POST(self):
        if self.path != "/translate":
            self.send_json(404, {"ok": False, "error": "not found"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length < 2 or length > 100_000:
                raise ValueError("invalid request size")
            request = json.loads(self.rfile.read(length))
            source = request.get("source")
            target = request.get("target")
            chunks = request.get("chunks")
            if source not in LANGUAGES or target not in LANGUAGES or source == target:
                raise ValueError("unsupported language pair")
            if not isinstance(chunks, list) or not 1 <= len(chunks) <= 64:
                raise ValueError("chunks must contain 1 to 64 strings")
            if any(not isinstance(chunk, str) or not chunk or len(chunk) > 2000 for chunk in chunks):
                raise ValueError("invalid translation chunk")
            self.send_json(200, {"ok": True, "parts": ENGINE.translate(chunks, source, target)})
        except ValueError as error:
            self.send_json(400, {"ok": False, "error": str(error)})
        except BusyError as error:
            self.send_json(503, {"ok": False, "error": str(error)})
        except Exception as error:
            print("translation error:", repr(error), flush=True)
            self.send_json(500, {"ok": False, "error": "translation engine failed"})


if __name__ == "__main__":
    ThreadingHTTPServer((HOST, PORT), Handler).serve_forever()
