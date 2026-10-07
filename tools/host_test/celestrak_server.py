#!/usr/bin/env python3
# TEST-1: serves the CelesTrak files cached by fetch_celestrak.py on the paths the firmware asks
# for, so a debug build (celestrak_base = "http://<this Mac>:8765") can download as often as a
# test needs without CelesTrak's 403 rule. Logs each request.
#   ./celestrak_server.py [port]
import http.server, os, sys, time, urllib.parse
HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "cache", "celestrak")
MAP = {  # (path, the query key that names the file): cache file
    ("/NORAD/elements/gp.php", "CATNR=25544"): "iss.csv",
    ("/NORAD/elements/gp.php", "CATNR=48274"): "css.csv",
    ("/NORAD/elements/gp.php", "GROUP=visual"): "visual.csv",
    ("/NORAD/elements/gp.php", "GROUP=starlink"): "starlink.csv",
    ("/NORAD/elements/gp.php", "GROUP=gps-ops"): "gps-ops.csv",
    ("/NORAD/elements/gp.php", "GROUP=galileo"): "galileo.csv",
    ("/NORAD/elements/gp.php", "GROUP=glo-ops"): "glo-ops.csv",
    ("/NORAD/elements/gp.php", "GROUP=beidou"): "beidou.csv",
    ("/NORAD/elements/gp.php", "GROUP=geo"): "geo.csv",
    ("/satcat/records.php", "GROUP=visual"): "satcat_visual.json",
}

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def do_GET(self):
        u = urllib.parse.urlsplit(self.path)
        name = next((f for (p, q), f in MAP.items() if u.path == p and q in u.query), None)
        if name is None and u.path == "/satcat/records.php":  # one satellite's owner: not cached
            body, ctype = b"[]", "application/json"
        elif name is None:
            self.send_error(404)
            return
        else:
            body = open(os.path.join(CACHE, name), "rb").read()
            ctype = "application/json" if name.endswith(".json") else "text/csv"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, fmt, *a):
        sys.stdout.write(f"{time.strftime('%H:%M:%S')} {self.client_address[0]} {fmt % a}\n")
        sys.stdout.flush()

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    print(f"serving {CACHE} on :{port}", flush=True)
    http.server.ThreadingHTTPServer(("0.0.0.0", port), H).serve_forever()
