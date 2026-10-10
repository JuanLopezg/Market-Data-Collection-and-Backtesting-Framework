"""Bounded authenticated browser acceptance against an isolated PAPER fixture.

Run on Windows with Edge, requests and websocket-client while paper_trading_test.py
is paused with PAPER_BROWSER_HOLD_SECONDS. No trading actions are submitted.
"""
import argparse
import base64
import json
from pathlib import Path
import subprocess
import time
from urllib.parse import urlparse

import requests
import websocket


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture-directory', required=True)
    parser.add_argument('--browser', default=r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    directory = Path(args.fixture_directory).resolve()
    if directory.parent != (root / 'storage/paper_validation').resolve():
        raise ValueError('Only an isolated local PAPER validation directory is allowed')
    ready = directory / 'browser-ready.json'
    metadata = json.loads(ready.read_text())
    url = metadata['url']
    parsed = urlparse(url)
    if parsed.scheme != 'http' or parsed.hostname != '127.0.0.1' or parsed.username or parsed.password:
        raise ValueError('Only a loopback fixture URL is allowed')
    credentials = dict(line.split('=', 1) for line in (directory / '.env').read_text().splitlines() if '=' in line)
    password = credentials['DASHBOARD_VIEWER_PASSWORD']
    output = directory / 'browser'
    output.mkdir(exist_ok=True)
    profile = output / f'profile_{time.time_ns()}'
    process = subprocess.Popen([args.browser, '--headless=new', '--no-first-run', '--disable-gpu',
        '--disable-extensions', '--disable-background-networking', '--remote-debugging-address=127.0.0.1',
        '--remote-debugging-port=0', '--remote-allow-origins=*', '--user-data-dir=' + str(profile), 'about:blank'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    connection = None
    passed = False
    try:
        deadline = time.monotonic() + 10
        active = profile / 'DevToolsActivePort'
        while not active.exists() and time.monotonic() < deadline:
            time.sleep(.1)
        if not active.exists():
            raise RuntimeError('Browser did not start')
        port = active.read_text().splitlines()[0]
        target = next(t for t in requests.get(f'http://127.0.0.1:{port}/json/list', timeout=5).json() if t['type'] == 'page')
        connection = websocket.create_connection(target['webSocketDebuggerUrl'], timeout=15, suppress_origin=True)
        counter = 0
        def call(method, params=None):
            nonlocal counter
            counter += 1
            connection.send(json.dumps({'id':counter, 'method':method, 'params':params or {}}))
            while True:
                response = json.loads(connection.recv())
                if response.get('id') == counter:
                    if 'error' in response:
                        raise RuntimeError('Browser protocol request failed')
                    return response.get('result', {})
        def evaluate(expression):
            result = call('Runtime.evaluate', {'expression':expression, 'returnByValue':True, 'awaitPromise':True})
            if 'exceptionDetails' in result:
                # Never include the expression: login expressions carry a password.
                raise RuntimeError('Browser evaluation failed')
            return result['result'].get('value')
        def wait(expression, description):
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                if evaluate(expression):
                    return
                time.sleep(.2)
            raise RuntimeError('Timed out waiting for ' + description)
        def require(expression, description):
            if not evaluate(expression):
                raise AssertionError(description)
        def screenshot(name):
            image = call('Page.captureScreenshot', {'format':'png'})
            (output / (name + '.png')).write_bytes(base64.b64decode(image['data']))
        def navigate(path, heading):
            call('Page.navigate', {'url':url + path})
            wait(f'document.querySelector("main h1")?.innerText === {json.dumps(heading)}', heading)
            require('!document.querySelector("main .data-load-error")', heading + ' must load')
        call('Emulation.setDeviceMetricsOverride', {'width':1440, 'height':1100, 'deviceScaleFactor':1, 'mobile':False})
        call('Page.enable')
        call('Page.addScriptToEvaluateOnNewDocument', {'source':
            'window.browserAcceptanceErrors=[]; window.addEventListener("error",e=>window.browserAcceptanceErrors.push(e.message)); '
            'window.addEventListener("unhandledrejection",()=>window.browserAcceptanceErrors.push("unhandled rejection"));'})
        call('Page.navigate', {'url':url + '/risk'})
        wait('location.pathname === "/login" && !!document.querySelector("input[type=password]")', 'protected login')
        # Exercise the rendered React form, including rejection of invalid credentials.
        def login(value):
            evaluate('(()=>{const set=Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,"value").set; '
                'for(const [selector,value] of ' + json.dumps([['input[autocomplete=username]','viewer'], ['input[type=password]',value]]) +
                '){const input=document.querySelector(selector);set.call(input,value);input.dispatchEvent(new Event("input",{bubbles:true}));}})()')
            evaluate('document.querySelector("form").requestSubmit()')
        login('invalid-fixture-password')
        wait('!!document.querySelector(".login-error")', 'invalid login error')
        login(password)
        wait('!!document.querySelector(".sidebar__identity")', 'viewer session')
        require('document.querySelector(".sidebar__identity").innerText.includes("VIEWER")', 'viewer role')
        evaluate("document.querySelector('nav a[href=\"/positions\"]').click()")
        wait('document.querySelector("main h1")?.innerText === "Positions"', 'sidebar navigation')
        pages = [('/', 'Overview'), ('/positions','Positions'), ('/pipeline','Strategy → Portfolio Pipeline'),
            ('/execution','Execution'), ('/risk','Risk & Safety'), ('/market-data','Market Data'),
            ('/infrastructure','Infrastructure'), ('/manual-control','Manual Portfolio Control'),
            ('/alerts-audit','Alerts & Audit'), ('/live-vs-expected','Live vs Expected'),
            ('/reconciliation','Reconciliation')]
        checked = []
        for path, heading in pages:
            navigate(path, heading)
            require('window.browserAcceptanceErrors.length === 0', heading + ' browser runtime errors')
            if path in ('/positions','/pipeline','/risk','/market-data'):
                require('document.querySelector("main").innerText.includes("BTCUSDT") && document.querySelector("main").innerText.includes("ETHUSDT")', heading + ' fixture assets')
            if path == '/':
                require('!!document.querySelector(".overview-unavailable")', 'missing equity history is explicit')
                for index in range(1, 7):
                    evaluate(f'document.querySelector(".range button:nth-child({index})").click()')
                    wait(f'document.querySelector(".range button:nth-child({index})")?.getAttribute("aria-pressed") === "true"', 'selected chart timeframe')
                require('!document.querySelector(".line-chart--interactive")', 'missing real equity history must not become a mock chart')
            if path == '/pipeline':
                require('document.querySelectorAll(".pipeline-table tbody tr.clickable-row").length === 2', 'two pipeline rows')
                evaluate('document.querySelector(".pipeline-table .clickable-row").click()')
                wait('!!document.querySelector(".pipeline-rail")', 'expanded pipeline stages')
                evaluate('document.querySelector(".why-button").click()')
                wait('!!document.querySelector(".inspector-backdrop")', 'pipeline inspector')
                evaluate('document.querySelector(".inspector-backdrop").click()')
                evaluate('(()=>{const input=document.querySelector(".search-box input");Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,"value").set.call(input,"NOT_AN_ASSET");input.dispatchEvent(new Event("input",{bubbles:true}));})()')
                wait('document.querySelector("main").innerText.includes("No assets match your search.")', 'empty search explanation')
            if path == '/risk':
                require('document.querySelectorAll(".risk-evaluation-table tbody tr").length === 2', 'persisted risk evaluations')
                require('document.querySelector("main").innerText.includes("Configuration SHA-256") && document.querySelector("main").innerText.includes("Not inferred")', 'risk provenance and safety limits')
            if path == '/infrastructure':
                require('document.querySelector("main").innerText.includes("Host uptime") && document.querySelector("main").innerText.includes("No fresh observation")', 'stale telemetry is unavailable')
            if path == '/manual-control':
                require('!!document.querySelector(".manual-role-lock") && document.querySelector(".upload-zone button").disabled && document.querySelector("input[type=file]").disabled', 'viewer manual control locked')
                status = evaluate('fetch("/api/manual-control/preview",{method:"POST",headers:{"Content-Type":"application/json"},body:"{}"}).then(r=>r.status)')
                if status != 403:
                    raise AssertionError('Viewer mutation must be rejected with 403')
            screenshot(path.strip('/') or 'overview')
            checked.append(path)
        # Test the UI failure boundary without interrupting any trading service.
        call('Network.enable')
        call('Network.setBlockedURLs', {'urls':['*/api/risk']})
        call('Page.navigate', {'url':url + '/risk'})
        wait('!!document.querySelector(".data-load-error")', 'blocked resource error boundary')
        require('document.querySelector(".data-load-error").innerText.includes("Retry")', 'retry control')
        screenshot('risk-resource-error')
        call('Network.setBlockedURLs', {'urls':[]})
        evaluate('document.querySelector(".data-load-error button").click()')
        wait('!!document.querySelector(".risk-evaluation-table")', 'resource retry recovery')
        evaluate("document.querySelector('button[aria-label=\"Sign out\"]').click()")
        wait('location.pathname === "/login"', 'logout')
        call('Page.navigate', {'url':url + '/positions'})
        wait('location.pathname === "/login" && !!document.querySelector("input[type=password]")', 'logout protects resources')
        (output / 'accepted.json').write_text(json.dumps({'project':metadata['project'], 'pages':checked,
            'authenticated_real_paper':True, 'invalid_login_rejected':True, 'viewer_mutation_forbidden':True,
            'pipeline_interactions':True, 'risk_provenance':True, 'stale_telemetry_unavailable':True,
            'browser_resource_error_retry':True, 'logout_protected':True,
            'limits':'Bounded fixture; no operator routing, mobile/VPS or backend connectivity fault acceptance.'}, indent=2) + '\n')
        print('DASHBOARD-BROWSER: PASS: authenticated PAPER pages, persisted diagnostics, viewer restrictions, resource error/retry and logout')
        passed = True
        call('Browser.close')
    except Exception:
        if connection:
            try:
                screenshot('failure')
                (output / 'failure.json').write_text(json.dumps(evaluate('({path:location.pathname,main:document.querySelector("main")?.innerText,errors:window.browserAcceptanceErrors})'), indent=2))
            except Exception:
                pass
        raise
    finally:
        if connection:
            connection.close()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=5)
        if passed:
            ready.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
