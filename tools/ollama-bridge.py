#!/usr/bin/env python3
"""ollama-bridge - lets a penguinOS board talk to Ollama on your computer.

WHY THIS EXISTS

penguinOS asks its model server a deliberately tiny question:

    GET /ask?stream=1&max=256&model=<name>&system=<text>&q=<prompt>

and expects the answer as chunked PLAIN TEXT, one fragment at a time. That is
about as little as an HTTP client can be, which matters when the client has
30 KB of free heap and no JSON encoder.

Ollama speaks something else: POST /api/generate with a JSON body, answering
with newline-delimited JSON objects. Neither side is wrong; they just do not
meet. This script is the forty lines in between.

Run it on the computer that runs Ollama. Point the board at THIS script's
address and port - not at Ollama's - in the board's Settings tab.

    python3 tools/ollama-bridge.py
    python3 tools/ollama-bridge.py --model qwen3.5:4b --port 8080

It also works with anything else that speaks Ollama's API - LM Studio and
llama.cpp both do - by pointing --ollama somewhere else.
"""

import argparse
import json
import sys
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ARGS = None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):                      # one tidy line per request
        sys.stderr.write("  %s\n" % (fmt % a))

    # ---------------------------------------------------------------- routes

    def do_GET(self):
        path, _, query = self.path.partition("?")
        q = urllib.parse.parse_qs(query)
        if path == "/health":
            return self._health()
        if path == "/ask":
            return self._ask(q, (q.get("q") or [""])[0])
        self.send_error(404, "penguinOS asks for /ask or /health")

    def do_POST(self):
        path, _, query = self.path.partition("?")
        q = urllib.parse.parse_qs(query)
        if path != "/ask":
            return self.send_error(404, "penguinOS asks for /ask or /health")
        # The board sends the prompt as a text/plain body on POST, and only the
        # other parameters in the query string.
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n).decode("utf-8", "replace") if n else ""
        return self._ask(q, body)

    # ---------------------------------------------------------------- health

    def _health(self):
        """The board polls this to decide whether the model is reachable.

        It answers with a Content-Length rather than chunked, deliberately: the
        board's parser handles both framings and this exercises the simpler one.
        """
        try:
            with urllib.request.urlopen(ARGS.ollama + "/api/tags", timeout=5) as r:
                tags = json.load(r)
            names = [m.get("name", "") for m in tags.get("models", [])]
            payload = {"ok": True, "models": names, "default": ARGS.model}
        except Exception as exc:                          # noqa: BLE001
            payload = {"ok": False, "error": str(exc)}
        raw = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(raw)

    # ------------------------------------------------------------------- ask

    def _ask(self, q, prompt):
        if not prompt.strip():
            return self.send_error(400, "no prompt")

        model = (q.get("model") or [ARGS.model])[0] or ARGS.model
        system = (q.get("system") or [""])[0]
        try:
            num_predict = int((q.get("max") or ["256"])[0])
        except ValueError:
            num_predict = 256

        req = {
            "model": model,
            "prompt": prompt,
            "stream": True,
            # Reasoning models - qwen3, deepseek-r1 and friends - stream their
            # chain of thought in a "thinking" field and leave "response" EMPTY
            # until it is finished. The board asks for 256 tokens by default, and
            # such a model can spend every one of them thinking and return not a
            # single character. The chat then looks broken when it is working
            # exactly as designed.
            #
            # think:false makes them answer directly. Verified harmless on models
            # that have no thinking mode - llama3.2 answers identically with and
            # without it - so it is on by default and --think turns it back on
            # for anyone who wants to watch the reasoning.
            "think": ARGS.think,
            "options": {"num_predict": num_predict},
        }
        if system:
            req["system"] = system

        # Chunked, with no Content-Length. The board streams the reply onto the
        # screen as it arrives, so buffering the whole answer here would undo
        # the only reason it streams at all.
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("X-Accel-Buffering", "no")
        self.send_header("Connection", "close")
        self.end_headers()

        try:
            post = urllib.request.Request(
                ARGS.ollama + "/api/generate",
                data=json.dumps(req).encode(),
                headers={"Content-Type": "application/json"},
            )
            sent = 0
            thought = 0
            with urllib.request.urlopen(post, timeout=ARGS.timeout) as r:
                for line in r:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        obj = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if obj.get("error"):
                        self._chunk(("[ollama: %s]" % obj["error"]).encode())
                        break
                    if obj.get("thinking"):
                        thought += 1
                    piece = obj.get("response", "")
                    if piece:
                        sent += len(piece)
                        self._chunk(piece.encode("utf-8"))
                    if obj.get("done"):
                        break
            # Say WHY nothing came back, rather than leaving a blank reply that
            # looks like a bug in the board.
            if sent == 0:
                if thought:
                    self._chunk(("[%s spent its whole %d-token budget thinking and "
                                 "produced no answer. Raise the token limit in the "
                                 "board's Settings, or run the bridge without "
                                 "--think.]" % (model, num_predict)).encode())
                else:
                    self._chunk(("[%s returned nothing. Check the model name.]"
                                 % model).encode())
        except urllib.error.URLError as exc:
            self._chunk(("\n[bridge: cannot reach Ollama at %s - %s]\n"
                         % (ARGS.ollama, exc.reason)).encode())
        except Exception as exc:                          # noqa: BLE001
            self._chunk(("\n[bridge: %s]\n" % exc).encode())

        # The zero chunk. Without it the board waits out its whole read timeout
        # and the reply never finishes on screen.
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def _chunk(self, raw):
        self.wfile.write(b"%x\r\n" % len(raw) + raw + b"\r\n")
        self.wfile.flush()


def main():
    global ARGS
    ap = argparse.ArgumentParser(
        description="Bridge a penguinOS board to Ollama on this computer.")
    ap.add_argument("--port", type=int, default=8080,
                    help="port THIS bridge listens on; the board points here (default 8080)")
    ap.add_argument("--ollama", default="http://127.0.0.1:11434",
                    help="where Ollama is (default http://127.0.0.1:11434)")
    # Matches EOS_BRAIN_DEFAULT_MODEL in kernel/svc/include/eos_brain.h, so the
    # bridge and the board agree without anyone configuring anything. It is a
    # REASONING model - see the think handling in _ask(); it returns an empty
    # answer if allowed to think within the board's token budget.
    ap.add_argument("--model", default="qwen3.5:2b",
                    help="model to use when the board does not name one "
                         "(default matches the board's own default)")
    ap.add_argument("--timeout", type=int, default=120, help="seconds to wait on Ollama")
    ap.add_argument("--think", action="store_true",
                    help="let reasoning models think out loud. OFF by default: they can "
                         "spend the board's whole token budget thinking and return "
                         "nothing, which looks like a broken chat")
    ap.add_argument("--bind", default="0.0.0.0",
                    help="interface to listen on; 0.0.0.0 so the board can reach it")
    ARGS = ap.parse_args()

    # Fail loudly HERE rather than on the board, where the only symptom is a
    # chat that never answers.
    try:
        with urllib.request.urlopen(ARGS.ollama + "/api/tags", timeout=5) as r:
            names = [m.get("name", "") for m in json.load(r).get("models", [])]
        print("Ollama at %s has %d model(s): %s"
              % (ARGS.ollama, len(names), ", ".join(names) or "none"))
        if names and ARGS.model not in names:
            print("  note: --model %s is not among them; pull it or pass one of the above"
                  % ARGS.model)
    except Exception as exc:                              # noqa: BLE001
        print("Cannot reach Ollama at %s: %s" % (ARGS.ollama, exc))
        print("  Start it with:  ollama serve")
        print("  Then pull a model:  ollama pull qwen3.5:2b")
        return 1

    srv = ThreadingHTTPServer((ARGS.bind, ARGS.port), Handler)
    print("\nBridge listening on %s:%d" % (ARGS.bind, ARGS.port))
    print("Point the board's Settings tab at this computer's LAN address, port %d."
          % ARGS.port)
    print("Ctrl-C to stop.\n")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
