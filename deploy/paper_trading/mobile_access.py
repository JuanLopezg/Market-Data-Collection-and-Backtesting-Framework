#!/usr/bin/env python3
"""Temporary HTTPS access from one phone to an existing localhost dashboard.

Run on the PC, with a narrowly scoped firewall rule. The dashboard or VPS SSH
tunnel must already be running. This program never starts trading or changes VPS.
"""
import argparse
import hashlib
import ipaddress
from pathlib import Path
import select
import shutil
import socket
import socketserver
import ssl
import subprocess


ROOT = Path(__file__).resolve().parents[2]
PRIVATE_NETWORKS = tuple(ipaddress.ip_network(value) for value in
                         ('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16'))


def local_address(value):
    address = ipaddress.IPv4Address(value)
    if not address.is_loopback and not any(address in network for network in PRIVATE_NETWORKS):
        raise ValueError('Use a specific private IPv4 address, never a public or wildcard address')
    return str(address)


def certificate(directory, address, openssl):
    directory.mkdir(parents=True, exist_ok=True)
    certificate_file, key = directory / 'certificate.pem', directory / 'private-key.pem'
    if not certificate_file.exists() or not key.exists():
        result = subprocess.run([openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-sha256', '-nodes',
            '-days', '30', '-subj', '/CN=AlgoTrading Local Dashboard', '-addext', 'subjectAltName=IP:' + address,
            '-keyout', str(key), '-out', str(certificate_file)], capture_output=True, timeout=30)
        if result.returncode:
            raise RuntimeError('Local certificate generation failed')
        key.chmod(0o600)
    # Check existing certificates too: an expired or wrong-IP certificate must
    # not silently become the next mobile endpoint.
    for arguments in (['-checkend', '0'], ['-checkip', address]):
        result = subprocess.run([openssl, 'x509', '-in', str(certificate_file), '-noout', *arguments],
                                capture_output=True, timeout=10)
        if result.returncode or (arguments[0] == '-checkip' and b'does match certificate' not in result.stdout):
            raise ValueError('Local certificate expired or does not match this address; use a fresh certificate directory')
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(certificate_file, key)
    der = ssl.PEM_cert_to_DER_cert(certificate_file.read_text())
    fingerprint = hashlib.sha256(der).hexdigest().upper()
    return context, ':'.join(fingerprint[index:index+2] for index in range(0, len(fingerprint), 2))


class DashboardBridge(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address, phone, target_port, context):
        self.phone, self.target_port, self.context = phone, target_port, context
        super().__init__(address, DashboardConnection)

    def verify_request(self, request, client_address):
        return client_address[0] == self.phone

    def handle_error(self, request, client_address):
        # Requests contain cookies/passwords; never log their contents or traces.
        print('Mobile connection ended unexpectedly.')


class DashboardConnection(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            self.request.settimeout(10)
            with self.server.context.wrap_socket(self.request, server_side=True) as client:
                with socket.create_connection(('127.0.0.1', self.server.target_port), timeout=5) as upstream:
                    client.settimeout(15)
                    upstream.settimeout(15)
                    # A bounded buffer relays HTTP/SSE unchanged, including session
                    # cookies and CSRF. No buffering of an entire stream or body.
                    while True:
                        readable, _, _ = select.select([client, upstream], [], [], 0 if client.pending() else 2)
                        if client.pending() and client not in readable:
                            readable.append(client)
                        for source in readable:
                            data = source.recv(65536)
                            if not data:
                                return
                            (upstream if source is client else client).sendall(data)
        except (OSError, ssl.SSLError):
            # Disconnects, invalid handshakes and unavailable upstreams carry no
            # application evidence. Authentication remains owned by the Go API.
            return


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bind-ip', required=True, type=local_address)
    parser.add_argument('--phone-ip', required=True, type=local_address)
    parser.add_argument('--target-port', type=int, default=8092, help='8092 local PAPER; 8093 existing VPS SSH tunnel')
    parser.add_argument('--port', type=int, default=8094)
    parser.add_argument('--openssl', default=shutil.which('openssl') or r'C:\Program Files\Git\usr\bin\openssl.exe')
    args = parser.parse_args()
    if not all(1024 <= value <= 65535 for value in (args.port, args.target_port)) or args.port == args.target_port:
        parser.error('Choose distinct ports between 1024 and 65535')
    try:
        with socket.create_connection(('127.0.0.1', args.target_port), timeout=5):
            pass
    except OSError:
        parser.error(f'No dashboard reachable at localhost:{args.target_port}; start the dashboard or SSH tunnel first')
    directory = ROOT / 'storage/mobile_access' / args.bind_ip
    context, fingerprint = certificate(directory, args.bind_ip, args.openssl)
    with DashboardBridge((args.bind_ip, args.port), args.phone_ip, args.target_port, context) as server:
        print(f'Phone URL: https://{args.bind_ip}:{args.port}', flush=True)
        print(f'Allowed phone: {args.phone_ip}; upstream: localhost:{args.target_port}', flush=True)
        print('Certificate SHA-256: ' + fingerprint, flush=True)
        print('Keep this terminal open. Ctrl+C stops mobile access. No requests or credentials are logged.', flush=True)
        try:
            server.serve_forever(poll_interval=.5)
        except KeyboardInterrupt:
            pass


if __name__ == '__main__':
    main()
