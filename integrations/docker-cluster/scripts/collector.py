#!/usr/bin/env python3
# Stand-in ingest endpoint for the http sink (src/plugins/sinks/http). Appends
# every POST body verbatim to a file the host can read, so test.sh can check
# that what both workers shipped over the network matches what they wrote
# locally via stdout_json. Every http sink batch already ends each record
# with '\n' (see plugins/sinks/http/sink.cpp), so batches from either worker
# concatenate into valid NDJSON with no extra framing needed here.
import http.server
import threading

OUT = "/var/log/collector/received.jsonl"
LOCK = threading.Lock()


class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        with LOCK, open(OUT, "ab") as f:
            f.write(body)
        self.send_response(204)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, fmt, *args):
        pass


if __name__ == "__main__":
    open(OUT, "a").close()
    http.server.ThreadingHTTPServer(("0.0.0.0", 9000), Handler).serve_forever()
