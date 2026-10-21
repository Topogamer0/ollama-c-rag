#!/usr/bin/env python3
"""
A fake Ollama server for the integration tests.

Usage: mock_server.py <command> [args...]

Starts the server on a free local port, runs the command with OLLAMA_HOST
pointing to it and exits with the command's exit code.

/api/embed returns deterministic embeddings: a bag of words hashed into a
vector, so that texts sharing words are similar. /api/chat streams an answer
that repeats the question and the number of sources found in the prompt.

Special models:
  missing         404 "model not found" (embed and chat)
  wrong-count     /api/embed returns one embedding too few
  midstream-error /api/chat fails in the middle of the stream

Control endpoints for the tests:
  GET  /_last    the last request: {method, path, headers, body}
  GET  /_stats   counters: embed_requests, embed_inputs, max_batch, chat_requests
  POST /_config  {"dimensions": n} changes the default size of the embeddings,
                 {"fail_after": n} makes /api/embed fail after n more requests
  POST /_reset   resets the counters and the configuration
"""

import hashlib
import json
import os
import re
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_DIMENSIONS = 256

LOCK = threading.Lock()
STATE = {}
LAST_REQUEST = {}


def reset():
    STATE.clear()
    STATE.update({
        'embed_requests': 0,
        'embed_inputs': 0,
        'max_batch': 0,
        'chat_requests': 0,
        'dimensions': DEFAULT_DIMENSIONS,
        'fail_after': None,
    })


reset()


def embedding(text, dimensions):
    """Hashed bag of words, deliberately not normalized."""
    vector = [0.0] * dimensions
    for word in re.findall(r'\w+', text.lower()):
        h = int.from_bytes(hashlib.sha256(word.encode()).digest()[:8], 'little')
        vector[h % dimensions] += 2.5 if (h >> 32) & 1 else -2.5
    return vector


def timings(**extra):
    return {
        'model': 'mock',
        'created_at': '2024-01-01T00:00:00Z',
        'done': True,
        'done_reason': 'stop',
        'total_duration': 1500000000,
        'load_duration': 1000000,
        'prompt_eval_count': 10,
        'prompt_eval_duration': 2000000,
        'eval_count': 3,
        'eval_duration': 3000000,
        **extra,
    }


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def read_body(self):
        length = int(self.headers.get('Content-Length') or 0)
        return self.rfile.read(length) if length else b''

    def send_json(self, obj, status=200):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def send_error_json(self, message, status):
        self.send_json({'error': message}, status)

    def send_stream(self, parts, delay=0.002):
        self.send_response(200)
        self.send_header('Content-Type', 'application/x-ndjson')
        self.send_header('Transfer-Encoding', 'chunked')
        self.end_headers()
        for part in parts:
            line = (json.dumps(part) + '\n').encode()
            self.wfile.write(b'%x\r\n%s\r\n' % (len(line), line))
            self.wfile.flush()
            time.sleep(delay)
        self.wfile.write(b'0\r\n\r\n')
        self.wfile.flush()

    def record(self, body):
        with LOCK:
            LAST_REQUEST.clear()
            LAST_REQUEST.update({
                'method': self.command,
                'path': self.path,
                'headers': {k.lower(): v for k, v in self.headers.items()},
                'body': body.decode('utf-8', 'replace'),
            })

    def do_GET(self):
        if self.path == '/_last':
            with LOCK:
                return self.send_json(LAST_REQUEST)
        if self.path == '/_stats':
            with LOCK:
                return self.send_json(STATE)
        self.send_error_json('not found', 404)

    def do_POST(self):
        body = self.read_body()
        if self.path == '/_reset':
            with LOCK:
                reset()
            return self.send_json({})
        if self.path == '/_config':
            with LOCK:
                STATE.update(json.loads(body))
            return self.send_json({})

        self.record(body)
        try:
            # like Ollama, refuse invalid UTF-8
            req = json.loads(body.decode('utf-8')) if body else {}
        except ValueError:
            return self.send_error_json('invalid JSON', 400)

        model = req.get('model')
        if model == 'missing':
            return self.send_error_json(f'model "{model}" not found, try pulling it first', 404)
        route = {'/api/embed': self.embed, '/api/chat': self.chat}.get(self.path)
        if not route:
            return self.send_error_json('not found', 404)
        route(req)

    def embed(self, req):
        inputs = req.get('input')
        if isinstance(inputs, str):
            inputs = [inputs]
        if not isinstance(inputs, list) or not all(isinstance(i, str) for i in inputs):
            return self.send_error_json('invalid input', 400)
        with LOCK:
            STATE['embed_requests'] += 1
            if STATE['fail_after'] is not None:
                if STATE['fail_after'] <= 0:
                    return self.send_error_json('simulated failure', 500)
                STATE['fail_after'] -= 1
            STATE['embed_inputs'] += len(inputs)
            STATE['max_batch'] = max(STATE['max_batch'], len(inputs))
            dimensions = req.get('dimensions') or STATE['dimensions']
        embeddings = [embedding(text, dimensions) for text in inputs]
        if req.get('model') == 'wrong-count':
            embeddings = embeddings[:-1]
        self.send_json(timings(model=req.get('model'), embeddings=embeddings))

    def chat(self, req):
        with LOCK:
            STATE['chat_requests'] += 1
        messages = req.get('messages', [])
        prompt = messages[-1]['content'] if messages else ''
        question = prompt.rsplit('Question:', 1)[-1].strip()
        sources = len(re.findall(r'^\[\d+\]', prompt, re.M))
        text = f'Found {sources} sources [1]. You asked: {question}'
        if req.get('stream') is False:
            return self.send_json(timings(message={'role': 'assistant', 'content': text}))
        parts = [{'model': req.get('model'), 'message': {'role': 'assistant', 'content': text[i:i + 5]}, 'done': False}
                 for i in range(0, len(text), 5)]
        if req.get('model') == 'midstream-error':
            parts = parts[:2] + [{'error': 'something went wrong'}]
        else:
            parts.append(timings(model=req.get('model'), message={'role': 'assistant', 'content': ''}))
        self.send_stream(parts)


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        # the tests close some streams before their end
        if not isinstance(sys.exc_info()[1], (BrokenPipeError, ConnectionResetError)):
            super().handle_error(request, client_address)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    server = Server(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    env = dict(os.environ, OLLAMA_HOST=f'127.0.0.1:{server.server_address[1]}')
    env.pop('OLLAMA_API_KEY', None)
    try:
        return subprocess.run(sys.argv[1:], env=env).returncode
    finally:
        server.shutdown()


if __name__ == '__main__':
    sys.exit(main())
