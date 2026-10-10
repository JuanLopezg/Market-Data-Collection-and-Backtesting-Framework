#!/usr/bin/env python3
"""Bounded TLS transport checks; handset/browser acceptance is separate."""
import http.client
import importlib.util
import json
import os
from pathlib import Path
import socket
import ssl
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


spec = importlib.util.spec_from_file_location('mobile_access',
    Path(__file__).resolve().parents[1] / 'deploy/paper_trading/mobile_access.py')
mobile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mobile)


class DashboardFixture(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == '/api/stream':
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.end_headers()
            self.wfile.write(b'data: ready\n\n')
            self.wfile.flush()
            self.server.release_stream.wait(3)
            self.close_connection = True
            return
        authenticated = self.headers.get('Cookie') == 'session=fixture'
        body = b'private page' if authenticated else b'login required'
        self.send_response(200 if authenticated else 401)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        body = self.rfile.read(int(self.headers['Content-Length']))
        self.send_response(200)
        self.send_header('Set-Cookie', 'session=fixture; HttpOnly; SameSite=Strict')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class MobileAccessTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        context, cls.fingerprint = mobile.certificate(Path(cls.directory.name), '127.0.0.1', 'openssl')
        cls.client_context = ssl.create_default_context(cafile=str(Path(cls.directory.name) / 'certificate.pem'))
        cls.source = ThreadingHTTPServer(('127.0.0.1', 0), DashboardFixture)
        cls.source.daemon_threads = True
        cls.source.release_stream = threading.Event()
        cls.bridge = mobile.DashboardBridge(('127.0.0.1', 0), '127.0.0.1', cls.source.server_port, context)
        cls.threads = []
        for server in (cls.source, cls.bridge):
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            cls.threads.append(thread)

    @classmethod
    def tearDownClass(cls):
        cls.source.release_stream.set()
        for server in (cls.bridge, cls.source):
            server.shutdown()
            server.server_close()
        for thread in cls.threads:
            thread.join(2)
        cls.directory.cleanup()

    def connection(self):
        return http.client.HTTPSConnection('127.0.0.1', self.bridge.server_address[1],
            context=self.client_context, timeout=2)

    def test_auth_status_cookie_and_request_body_survive_tls(self):
        connection = self.connection()
        try:
            connection.request('GET', '/api/risk')
            response = connection.getresponse()
            self.assertEqual(response.status, 401)
            self.assertEqual(response.read(), b'login required')
            body = b'{"password":"isolated-fixture-only"}'
            connection.request('POST', '/api/auth/login', body=body)
            response = connection.getresponse()
            self.assertEqual(response.getheader('Set-Cookie'), 'session=fixture; HttpOnly; SameSite=Strict')
            self.assertEqual(response.read(), body)
            connection.request('GET', '/api/risk', headers={'Cookie': 'session=fixture'})
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertEqual(response.read(), b'private page')
        finally:
            connection.close()

    def test_stream_arrives_before_upstream_closes(self):
        connection = self.connection()
        try:
            connection.request('GET', '/api/stream')
            response = connection.getresponse()
            self.assertEqual(response.getheader('Content-Type'), 'text/event-stream')
            self.assertEqual(response.readline(), b'data: ready\n')
            self.assertFalse(self.source.release_stream.is_set())
        finally:
            connection.close()

    def test_other_source_address_is_rejected_before_tls(self):
        connection = socket.socket()
        connection.settimeout(2)
        connection.bind(('127.0.0.2', 0))
        try:
            connection.connect(self.bridge.server_address)
            with self.assertRaises((OSError, ssl.SSLError)):
                self.client_context.wrap_socket(connection, server_hostname='127.0.0.1')
        finally:
            connection.close()

    def test_certificate_reuse_and_wrong_address(self):
        _, fingerprint = mobile.certificate(Path(self.directory.name), '127.0.0.1', 'openssl')
        self.assertEqual(fingerprint, self.fingerprint)
        with self.assertRaises(ValueError):
            mobile.certificate(Path(self.directory.name), '127.0.0.2', 'openssl')

    def test_upstream_loss_closes_connection_without_success_response(self):
        # Reserve a port without listening: failure is deterministic and cannot
        # accidentally connect to another process between port selection/use.
        with socket.socket() as unavailable:
            unavailable.bind(('127.0.0.1', 0))
            context, _ = mobile.certificate(Path(self.directory.name), '127.0.0.1', 'openssl')
            with mobile.DashboardBridge(('127.0.0.1', 0), '127.0.0.1',
                    unavailable.getsockname()[1], context) as bridge:
                thread = threading.Thread(target=bridge.serve_forever, daemon=True)
                thread.start()
                connection = http.client.HTTPSConnection('127.0.0.1', bridge.server_address[1],
                    context=self.client_context, timeout=2)
                try:
                    with self.assertRaises((OSError, http.client.RemoteDisconnected)):
                        connection.request('GET', '/api/risk')
                        connection.getresponse()
                finally:
                    connection.close()
                    bridge.shutdown()
                    thread.join(2)

    def test_public_and_wildcard_listener_are_rejected(self):
        for address in ('0.0.0.0', '15.237.145.179', '8.8.8.8'):
            with self.assertRaises(ValueError):
                mobile.local_address(address)


@unittest.skipUnless(os.environ.get('MOBILE_TEST_API_PORT'), 'No isolated Go API fixture supplied')
class GoAuthenticationTest(unittest.TestCase):
    def test_real_session_role_csrf_and_logout_through_bridge(self):
        with tempfile.TemporaryDirectory() as directory:
            context, _ = mobile.certificate(Path(directory), '127.0.0.1', 'openssl')
            client_context = ssl.create_default_context(cafile=str(Path(directory) / 'certificate.pem'))
            with mobile.DashboardBridge(('127.0.0.1', 0), '127.0.0.1',
                    int(os.environ['MOBILE_TEST_API_PORT']), context) as bridge:
                thread = threading.Thread(target=bridge.serve_forever, daemon=True)
                thread.start()
                connection = http.client.HTTPSConnection('127.0.0.1', bridge.server_address[1],
                    context=client_context, timeout=5)
                try:
                    def request(method, path, body=None, headers=None):
                        connection.request(method, path, body=body, headers=headers or {})
                        response = connection.getresponse()
                        result = response.status, dict(response.getheaders()), response.read()
                        return result

                    self.assertEqual(request('GET', '/api/risk')[0], 401)
                    status, headers, body = request('POST', '/api/auth/login',
                        json.dumps({'username': 'viewer', 'password': os.environ['MOBILE_TEST_API_PASSWORD']}),
                        {'Content-Type': 'application/json'})
                    self.assertEqual(status, 200)
                    session = json.loads(body)
                    self.assertEqual(session['user']['role'], 'VIEWER')
                    cookie = headers['Set-Cookie'].split(';', 1)[0]
                    self.assertIn('HttpOnly', headers['Set-Cookie'])
                    auth = {'Cookie': cookie}
                    for page in ('overview', 'pipeline', 'risk', 'infrastructure'):
                        self.assertEqual(request('GET', '/api/' + page, headers=auth)[0], 200)
                    self.assertEqual(request('POST', '/api/auth/logout', headers=auth)[0], 403)
                    csrf = {**auth, 'X-CSRF-Token': session['csrfToken']}
                    self.assertEqual(request('POST', '/api/manual-control/preview', body='{}', headers=csrf)[0], 403)
                    self.assertEqual(request('POST', '/api/auth/logout', headers=csrf)[0], 200)
                    self.assertEqual(request('GET', '/api/risk', headers=auth)[0], 401)
                finally:
                    connection.close()
                    bridge.shutdown()
                    thread.join(2)


if __name__ == '__main__':
    unittest.main()
