"""Bounded browser acceptance of Overview timeframe, hover, zoom and pan controls.

Run against an isolated frontend-local-mocks preview, never a real trading account.
The browser profile and screenshot are generated under ignored storage/.
"""
import argparse
import base64
import json
from pathlib import Path
import subprocess
import time
import requests
import websocket

parser = argparse.ArgumentParser()
parser.add_argument('--url', default='http://127.0.0.1:8094')
parser.add_argument('--browser', default=r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe')
args = parser.parse_args()
run = Path(__file__).resolve().parents[1] / 'storage/overview_browser'
run.mkdir(parents=True, exist_ok=True)
profile = run / f'profile_{time.time_ns()}'
process = subprocess.Popen([args.browser, '--headless=new', '--no-first-run', '--disable-gpu',
    '--disable-extensions', '--disable-background-networking', '--remote-debugging-address=127.0.0.1',
    '--remote-debugging-port=0', '--remote-allow-origins=*', '--user-data-dir=' + str(profile), 'about:blank'],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
connection = None
try:
    for _ in range(100):
        active = profile / 'DevToolsActivePort'
        if active.exists(): break
        time.sleep(.1)
    else: raise RuntimeError('Browser did not start')
    port = active.read_text().splitlines()[0]
    target = next(t for t in requests.get(f'http://127.0.0.1:{port}/json/list', timeout=5).json() if t['type'] == 'page')
    connection = websocket.create_connection(target['webSocketDebuggerUrl'], timeout=15, suppress_origin=True)
    counter = 0
    def call(method, params=None):
        global counter
        counter += 1
        connection.send(json.dumps({'id': counter, 'method': method, 'params': params or {}}))
        while True:
            response = json.loads(connection.recv())
            if response.get('id') == counter:
                if 'error' in response: raise RuntimeError(response['error'])
                return response.get('result', {})
    def evaluate(expression):
        result = call('Runtime.evaluate', {'expression': expression, 'returnByValue': True})
        if 'exceptionDetails' in result: raise RuntimeError(result['exceptionDetails'])
        return result['result'].get('value')
    def click(selector):
        evaluate(f'document.querySelector({json.dumps(selector)}).click()')
        time.sleep(.15)
    def readout(): return evaluate("document.querySelector('.chart-readout')?.innerText")
    def line(): return evaluate("document.querySelector('.chart-line--primary').getAttribute('points')")
    call('Emulation.setDeviceMetricsOverride', {'width': 1440, 'height': 1100, 'deviceScaleFactor': 1, 'mobile': False})
    call('Page.navigate', {'url': args.url + '/overview'})
    for _ in range(80):
        if readout(): break
        time.sleep(.1)
    else: raise RuntimeError('Mock Overview chart did not render')
    assert '21 observations' in readout(), readout()
    assert evaluate("document.querySelector('.chart-controls').innerText.includes('Equity (USD)')")
    for index, count in enumerate([1, 7, 21, 21, 21, 21], 1):
        click(f'.range button:nth-child({index})')
        assert f'{count} observations' in readout(), readout()
        assert evaluate(f'document.querySelector(".range button:nth-child({index})").getAttribute("aria-pressed")') == 'true'
    original = line()
    click('button[aria-label="Zoom in X axis"]'); assert line() != original
    click('.chart-controls > button'); assert line() == original
    click('button[aria-label="Zoom in Y axis"]'); assert line() != original
    click('.chart-controls > button'); assert line() == original
    rect = evaluate("(()=>{const r=document.querySelector('.line-chart--interactive').getBoundingClientRect();return {x:r.x,y:r.y,w:r.width,h:r.height}})()")
    px, py = rect['x'] + rect['w'] * .55, rect['y'] + rect['h'] * .5
    call('Input.dispatchMouseEvent', {'type': 'mouseMoved', 'x': px, 'y': py})
    time.sleep(.15)
    assert 'Equity:' in readout() and 'Reference:' in readout() and 'USD' in readout(), readout()
    call('Input.dispatchMouseEvent', {'type': 'mousePressed', 'x': px, 'y': py, 'button': 'left', 'clickCount': 1})
    time.sleep(.15)
    call('Input.dispatchMouseEvent', {'type': 'mouseMoved', 'x': px + 50, 'y': py + 25, 'buttons': 1, 'button': 'left'})
    time.sleep(.15)
    call('Input.dispatchMouseEvent', {'type': 'mouseReleased', 'x': px + 50, 'y': py + 25, 'button': 'left', 'clickCount': 1})
    time.sleep(.15)
    assert line() != original
    click('.chart-controls > button'); assert line() == original
    click('button[aria-label="Zoom in X axis"]')
    click('.range button:nth-child(2)'); assert '7 observations' in readout()
    click('.range button:nth-child(6)'); assert line() == original
    assert not evaluate("[...document.querySelectorAll('.line-chart--interactive')].some(svg=>svg.outerHTML.includes('NaN'))")
    image = call('Page.captureScreenshot', {'format': 'png'})
    (run / 'overview.png').write_bytes(base64.b64decode(image['data']))
    print('OVERVIEW-BROWSER: PASS: all six ranges, one-point chart, hover USD values, independent X/Y zoom, two-axis drag, reset and timeframe-reset')
    call('Browser.close')
finally:
    if connection: connection.close()
    try: process.wait(timeout=5)
    except subprocess.TimeoutExpired: process.terminate()
