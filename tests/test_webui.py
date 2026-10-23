"""Web UI tests against a mock device that serves the same API as the firmware."""
import copy
import json
import os
import re
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import pytest
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parent.parent
BINS = {p.stem: p for p in (ROOT / "Faces" / "bins").glob("*.bin")}
BASE_CONFIG = json.loads((ROOT / "mochi_config.json").read_text())
TURKISH = re.compile(r"[çğıöşüÇĞİÖŞÜ]|(?i:\b(ve|bir|icin|yuz|yuzler|kaydet|ayar|dokun|pil|uyku)\b)")


def load_ui(sketch):
    text = (ROOT / sketch / "Mochi_html.h").read_text()
    body = text.split("PROGMEM", 1)[1]
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", body))
    assert len(data) == int(re.search(r"INDEX_HTML_GZ_LEN = (\d+)", text).group(1))
    return data


class MockDevice(ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False

    def __init__(self, ui_gz):
        super().__init__(("127.0.0.1", 0), Handler)
        self.ui_gz = ui_gz
        self.config = copy.deepcopy(BASE_CONFIG)
        self.config["accel"] = {"ready": True, "softShakeReactions": [], "hardShakeReactions": []}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send(self, code, body, ctype="text/plain", extra=None):
        if isinstance(body, (dict, list)):
            body, ctype = json.dumps(body), "application/json"
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        dev, url = self.server, urlparse(self.path)
        q = parse_qs(url.query)
        if url.path == "/":
            return self.send(200, dev.ui_gz, "text/html", {"Content-Encoding": "gzip"})
        if url.path == "/api/config":
            return self.send(200, dev.config)
        if url.path == "/api/faces":
            faces = [{"name": n, "frames": p.read_bytes()[4], "size": p.stat().st_size, "format": "MOC3"}
                     for n, p in sorted(BINS.items())]
            return self.send(200, {"faces": faces, "active": dev.config.get("currentFace", "")})
        if url.path == "/api/preview":
            p = BINS.get(q.get("name", [""])[0])
            return self.send(200, p.read_bytes(), "application/octet-stream") if p else self.send(404, "not found")
        if url.path == "/api/sys":
            return self.send(200, {"deviceName": "Mochi", "uptime": 125, "freeRam": 150000, "fsReady": True,
                                   "fsTotal": 2555904, "fsUsed": 900000, "percent": 80, "voltage": 4.0})
        if url.path == "/api/battery":
            return self.send(200, {"percent": 80, "voltage": 4.0, "charging": False})
        if url.path == "/api/wifi/status":
            return self.send(200, {"apSSID": "R2D2", "apClients": 1, "staConnected": False, "ip": "192.168.4.1"})
        if url.path == "/api/ping":
            return self.send(200, "ok")
        return self.send(404, "Not found")

    def do_POST(self):
        dev, url = self.server, urlparse(self.path)
        body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
        if url.path == "/api/config":
            dev.config.update(json.loads(body))  # firmware merges posted keys into its config
            return self.send(200, "ok")
        if url.path in ("/api/test", "/api/setactive"):
            return self.send(200, "ok")
        return self.send(404, "Not found")


@pytest.fixture(scope="module")
def browser():
    with sync_playwright() as p:
        b = p.chromium.launch(executable_path=os.environ.get("CHROMIUM_PATH") or None)
        yield b
        b.close()


@pytest.fixture(params=["Mochi_EN", "Mochi_TR"])
def ui(request, browser):
    dev = MockDevice(load_ui(request.param))
    threading.Thread(target=dev.serve_forever, daemon=True).start()
    page = browser.new_page()
    errors = []
    page.on("pageerror", lambda e: errors.append(str(e)))
    page.goto(f"http://127.0.0.1:{dev.server_address[1]}/")
    page.wait_for_function("document.querySelectorAll('#face-grid .face-card').length > 0")
    yield page, dev, errors, request.param
    page.close()
    dev.shutdown()
    dev.server_close()


def open_tab(page, name):
    page.click(f".tab[data-tab='{name}']")


def config_post(page, action):
    """Run action, wait until the device has answered the config POST, return the posted JSON."""
    with page.expect_response(lambda r: r.url.endswith("/api/config") and r.request.method == "POST") as resp:
        action()
    return json.loads(resp.value.request.post_data)


def test_loads_settings_and_faces(ui):
    page, dev, errors, _ = ui
    assert page.locator("#face-grid .face-card").count() == len(BINS)
    assert page.input_value("#wifi-timeout-min") == str(BASE_CONFIG["wifiTimeoutMinutes"])
    assert page.input_value("#long-press-ms") == str(BASE_CONFIG["longPressMs"])
    assert page.locator("#poke-grid .check-item.checked").count() == len(BASE_CONFIG["pokeReactions"])
    assert errors == []


def test_save_clamps_and_omits_accel(ui):
    page, dev, errors, _ = ui
    open_tab(page, "wifi")
    page.fill("#wifi-timeout-min", "999")
    body = config_post(page, lambda: page.click("#btn-save-wifi-timeout"))
    assert body["wifiTimeoutMinutes"] == 120
    assert "accel" not in body  # a stale accel copy must not overwrite the device's
    assert page.input_value("#wifi-timeout-min") == "120"
    assert dev.config["wifiTimeoutMinutes"] == 120
    assert errors == []


def test_reaction_lists_save_only_with_their_button(ui):
    page, dev, errors, _ = ui
    open_tab(page, "anim")
    posts = []
    page.on("request", lambda r: posts.append(r) if r.method == "POST" and "/api/config" in r.url else None)
    item = page.locator("#poke-grid .check-item:not(.checked)").first
    name = item.get_attribute("data-name")
    item.click()
    page.wait_for_timeout(300)
    assert posts == []
    save = page.locator("#btn-save-anim-events")
    assert save.is_enabled()

    body = config_post(page, lambda: save.click())
    assert name in body["pokeReactions"]
    assert "accel" in body
    page.wait_for_function("document.querySelector('#btn-save-anim-events').disabled")
    assert errors == []


def test_config_import_posts_file_and_reloads(ui, tmp_path):
    page, dev, errors, _ = ui
    open_tab(page, "system")
    imported = dict(BASE_CONFIG, wifiTimeoutMinutes=42, longPressMs=300)
    f = tmp_path / "config.json"
    f.write_text(json.dumps(imported))

    body = config_post(page, lambda: page.set_input_files("#config-file-input", str(f)))
    assert body == imported
    page.wait_for_function("document.querySelector('#wifi-timeout-min').value === '42'")
    assert page.input_value("#long-press-ms") == "300"
    assert errors == []


def test_broken_config_import_changes_nothing(ui, tmp_path):
    page, dev, errors, _ = ui
    open_tab(page, "system")
    posts = []
    page.on("request", lambda r: posts.append(r) if r.method == "POST" else None)
    f = tmp_path / "broken.json"
    f.write_text("{not json")
    page.set_input_files("#config-file-input", str(f))
    page.wait_for_selector("#toast.show.error")
    assert posts == []
    assert dev.config["wifiTimeoutMinutes"] == BASE_CONFIG["wifiTimeoutMinutes"]


def test_english_ui_has_no_turkish(ui):
    page, dev, errors, sketch = ui
    if sketch != "Mochi_EN":
        pytest.skip("English UI only")
    texts = []
    for tab in ("faces", "anim", "wifi", "system"):
        open_tab(page, tab)
        texts.append(page.inner_text("body"))
    texts += page.eval_on_selector_all("[placeholder],[title]",
                                       "els => els.flatMap(e => [e.placeholder || '', e.title || ''])")
    found = sorted({m.group(0) for t in texts for m in TURKISH.finditer(t)})
    assert found == [], found
