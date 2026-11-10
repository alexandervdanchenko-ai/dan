# Тот же сервер на Python: http.server с потоком на соединение (замеры/вебсервер/замер.sh).
import http.server, sys, urllib.parse
class О(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        т = ("Привет, " + q.get("имя", ["мир"])[0]).encode()
        self.send_response(200); self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(т))); self.send_header("Connection", "close"); self.end_headers()
        self.wfile.write(т)
http.server.ThreadingHTTPServer.request_queue_size = 128
с = http.server.ThreadingHTTPServer(("127.0.0.1", 0), О)
print(с.server_address[1], flush=True)
с.serve_forever()
