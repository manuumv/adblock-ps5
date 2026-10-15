"""HTTPS server for the list-download tests.

usage: https_server.py CERT KEY DOCROOT PORTFILE

Binds 127.0.0.1 on a free port, writes the port to PORTFILE, and serves:
  /list.txt        DOCROOT/list.txt with an ETag; If-None-Match on it gives 304
  /big             2 MiB of domains, no Content-Length
  /slow            1 KiB every 0.1 s (10 KiB/s) for a long time
  /broken          announces 100000 bytes, sends 1000 and closes
  /html            an HTML error page with status 200
  /redirect-http   302 to the same list over plain http
  /redirect-https  302 to /list.txt on the same https origin
  /redirect-mirror 302 to /list.txt on mirror.test (another host, same server)
  /redirect-nowhere 302 to a host the tests cannot resolve
  /redirect-loop   302 to itself
  /bad-etag        200 with an ETag holding a control character
"""

import hashlib
import http.server
import os
import ssl
import sys
import time

CERT, KEY, DOCROOT, PORTFILE = sys.argv[1:5]


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *args):
        pass

    def send(self, status, body, headers=()):
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        if body is not None:
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self):
        try:
            self.route()
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass

    def route(self):
        if self.path == "/list.txt":
            with open(os.path.join(DOCROOT, "list.txt"), "rb") as f:
                body = f.read()
            etag = '"%s"' % hashlib.sha256(body).hexdigest()[:16]
            if self.headers.get("If-None-Match") == etag:
                self.send(304, None, [("ETag", etag)])
            else:
                self.send(200, body, [("ETag", etag), ("Last-Modified", "Wed, 15 Oct 2026 10:00:00 GMT")])
        elif self.path == "/big":
            self.send_response(200)
            self.end_headers()
            line = b"0123456789.bigdomain.example\n"
            for _ in range(2 * 1024 * 1024 // len(line) + 1):
                self.wfile.write(line)
        elif self.path == "/slow":
            self.send_response(200)
            self.end_headers()
            chunk = b"slow.example\n" * 78  # ~1 KiB
            for _ in range(1200):
                self.wfile.write(chunk)
                self.wfile.flush()
                time.sleep(0.1)
        elif self.path == "/broken":
            self.send_response(200)
            self.send_header("Content-Length", "100000")
            self.end_headers()
            self.wfile.write(b"part.example\n" * 77)
            self.wfile.flush()
            self.connection.shutdown(2)
        elif self.path == "/html":
            self.send(200, b"<html><body>Rate limited</body></html>\n", [("Content-Type", "text/html")])
        elif self.path == "/redirect-http":
            port = self.server.server_address[1]
            self.send(302, b"", [("Location", "http://list.test:%d/list.txt" % port)])
        elif self.path == "/redirect-https":
            self.send(302, b"", [("Location", "/list.txt")])
        elif self.path in ("/redirect-mirror", "/redirect-nowhere"):
            host = "mirror.test" if self.path == "/redirect-mirror" else "nowhere.invalid"
            port = self.server.server_address[1]
            self.send(302, b"", [("Location", "https://%s:%d/list.txt" % (host, port))])
        elif self.path == "/redirect-loop":
            self.send(302, b"", [("Location", "/redirect-loop")])
        elif self.path == "/bad-etag":
            self.send(200, b"a.example\n", [("ETag", '"a\x01b"')])
        else:
            self.send(404, b"not found\n")


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
server.daemon_threads = True
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(CERT, KEY)
server.socket = context.wrap_socket(server.socket, server_side=True)
with open(PORTFILE + ".tmp", "w") as f:
    f.write(str(server.server_address[1]))
os.rename(PORTFILE + ".tmp", PORTFILE)
server.serve_forever()
