"""Bounded mobile/desktop layout acceptance against an isolated mock frontend.

Run with Playwright Chromium and a built mock bundle, never a trading account.
"""
import argparse
import functools
import http.server
import json
from pathlib import Path
import threading

from playwright.sync_api import sync_playwright

parser = argparse.ArgumentParser()
parser.add_argument("--dist", required=True)
parser.add_argument("--output", required=True)
args = parser.parse_args()
output = Path(args.output)
output.mkdir(parents=True, exist_ok=True)


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_args):
        pass


server = http.server.ThreadingHTTPServer(
    ("127.0.0.1", 0), functools.partial(QuietHandler, directory=args.dist)
)
threading.Thread(target=server.serve_forever, daemon=True).start()
results = []
try:
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch()
        for width in (360, 390, 768, 1440):
            page = browser.new_page(viewport={"width": width, "height": 900},
                                    is_mobile=width < 900, has_touch=width < 900)
            errors = []
            page.on("pageerror", lambda error: errors.append(str(error)))
            page.goto(f"http://127.0.0.1:{server.server_port}/")
            page.locator(".page-heading").wait_for()
            paths = page.locator(".sidebar nav a").evaluate_all("items => items.map(a => a.getAttribute('href'))")
            for path in paths:
                if width < 900:
                    page.get_by_role("button", name="Open navigation", exact=True).click()
                    assert page.locator(".sidebar").is_visible()
                page.locator(f'.sidebar nav a[href="{path}"]').click()
                page.locator(".page-heading").wait_for()
                page.wait_for_timeout(200)
                dimensions = page.evaluate("""() => ({
                    viewport: innerWidth, document: document.documentElement.scrollWidth,
                    overflowing: [...document.querySelectorAll('main *')].filter(el => {
                        const r = el.getBoundingClientRect();
                        return r.right > innerWidth + 1 && !el.closest('.table-wrap')
                            && getComputedStyle(el).position !== 'fixed';
                    }).slice(0, 8).map(el => el.className)
                })""")
                assert dimensions["document"] <= width + 1, (width, path, dimensions)
                if width < 900:
                    assert not page.locator(".sidebar").is_visible()
                results.append({"width": width, "path": path, **dimensions})
                if path == "/":
                    header = page.locator(".topbar").bounding_box()
                    heading = page.locator(".page-heading").bounding_box()
                    if width < 900:
                        assert heading["y"] >= header["y"] + header["height"], (header, heading)
                    picker = page.locator(".time-range__button").first
                    if picker.count():
                        picker.click()
                        menu = page.get_by_role("dialog", name="Time range").bounding_box()
                        assert menu["x"] >= -1 and menu["x"] + menu["width"] <= width + 1, menu
                        page.keyboard.press("Escape")
                    page.screenshot(path=str(output / f"overview-{width}.png"), full_page=True)
            if width < 900:
                toggle = page.get_by_role("button", name="Open navigation", exact=True)
                toggle.click()
                page.keyboard.press("Escape")
                assert not page.locator(".sidebar").is_visible()
                toggle.click()
                page.locator(".mobile-menu-backdrop").click(position={"x": width - 8, "y": 100}, force=True)
                assert not page.locator(".sidebar").is_visible()
                toggle.click()
                page.get_by_role("button", name="Sign out", exact=True).click()
                page.locator(".login-form").wait_for()
                assert page.evaluate("document.documentElement.scrollWidth") <= width + 1
                page.screenshot(path=str(output / f"login-{width}.png"))
            assert not errors, errors
            page.close()
        browser.close()
    (output / "results.json").write_text(json.dumps(results, indent=2))
    print(f"PASS: {len(results)} page/viewport checks, navigation and no page errors")
finally:
    server.shutdown()
