"""Local HTTP fixture for checking the embedded page without hardware."""
import json
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

root = Path(__file__).resolve().parents[1]
state = dict(mode=7, name="BPSK500", baud=500, tone_hz=1500, signal=True,
             quality=93, audio_ready=True, rms=2450, samples=80000, blocks=313,
             dsp_us=2400, max_dsp_us=3100, deadline_misses=0, dma_overflows=0,
             read_errors=0, clipped_samples=0, output_drops=0, lcd_drops=0,
             invalid_codes=0, control_revision=0, free_heap=94000,
             min_free_heap=89000, uptime_ms=10000, clients=1,
             ssid="ESP32C3-PSK-DEMO (页面测试)", session=123)
data = ("[本地页面测试数据，非板端实测]\nCQ CQ DE ESP32-C3\n"
        "The quick brown fox jumps over the lazy dog. 0123456789\n"
        "中文 UTF-8 文本跨请求测试。\n<img src=x onerror=alert(1)>\n").encode()

class Handler(BaseHTTPRequestHandler):
    def send(self, body, content_type="application/json", status=200, headers=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (headers or {}).items(): self.send_header(k, str(v))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path)
        if path.path == "/":
            return self.send((root / "main/web/index.html").read_bytes(), "text/html; charset=utf-8")
        if path.path == "/api/status":
            return self.send(json.dumps(state).encode())
        if path.path == "/api/rx":
            since = int(parse_qs(path.query).get("since", [0])[0])
            end = min(len(data), since + 67)  # Split UTF-8 and text over polls.
            return self.send(data[since:end], "application/octet-stream", headers={
                "X-RX-Next": end, "X-RX-Dropped": 0, "X-RX-Session": state["session"]})
        return self.send(b"{}", status=404)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
        if self.path != "/api/config": return self.send(b"{}", status=404)
        if self.headers.get("X-PSK-Client") != "receiver": return self.send(b"{}", status=403)
        params = parse_qs(body)
        previous_revision = state["control_revision"]
        state["mode"] = int(params["mode"][0])
        state["tone_hz"] = int(params["tone"][0])
        state["name"] = {0:"BPSK31",4:"BPSK63",5:"BPSK125",6:"BPSK250",7:"BPSK500",8:"BPSK1000"}[state["mode"]]
        state["control_revision"] += 1
        return self.send(json.dumps(dict(queued=True, revision=previous_revision)).encode(), status=202)

print("Page fixture: http://127.0.0.1:8765 (simulated receiver)", flush=True)
HTTPServer(("127.0.0.1", 8765), Handler).serve_forever()
