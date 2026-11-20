#!/usr/bin/env python3
"""Run only as root in an isolated environment without a Redis-auth policy."""
import json
import os
from pathlib import Path
import socket
import shutil
import subprocess
import sys
import tempfile
import time


def main():
    policy = Path('/run/redis-auth')
    if os.geteuid() != 0 or policy.exists() or not shutil.which("redis-server"):
        return 77
    binary = str(Path(sys.argv[1]).resolve())
    servers = []
    policy.mkdir(mode=0o755)
    try:
        (policy / 'local-system').mkdir(mode=0o755)
        secret = policy / 'local-system/writer.secret'
        secret.write_text('bridge-fixture-password')
        secret.chmod(0o400)
        with tempfile.TemporaryDirectory(prefix='redfish-redis-') as temporary:
            root = Path(temporary)
            root.chmod(0o755)
            socket_path = root / 'redis.sock'
            acl = root / 'users.acl'
            acl.write_text('user default on nopass ~* &* -@all +@read +select +ping +acl|whoami\n'
                           'user bridge-writer on >bridge-fixture-password ~* &* +@all\n')
            acl.chmod(0o600)
            metadata = root / 'database_config.json'
            metadata.write_text(json.dumps({'INSTANCES': {'redis': {
                'hostname': '127.0.0.1', 'port': 0, 'unix_socket_path': str(socket_path)}},
                'DATABASES': {'STATE_DB': {'id': 6, 'separator': '|', 'instance': 'redis'}}}))
            (policy / 'local.json').write_text(json.dumps({'username': 'bridge-writer',
                'unix_socket': str(socket_path), 'tcp_host': '127.0.0.1', 'tcp_port': 0}))
            server = subprocess.Popen(['redis-server', '--port', '0', '--unixsocket', str(socket_path),
                '--unixsocketperm', '766', '--save', '', '--appendonly', 'no', '--aclfile', str(acl)],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            servers.append(server)
            for _ in range(200):
                if socket_path.exists(): break
                if server.poll() is not None: raise RuntimeError('Redis fixture failed to start')
                time.sleep(0.01)
            else: raise RuntimeError('Redis socket was not created')

            def ordinary():
                os.setgroups([])
                os.setgid(65534)
                os.setuid(65534)

            def check(mode, host='', port=6379, expected=0, user=False):
                result = subprocess.run([binary, str(metadata), mode, host, str(port)],
                    preexec_fn=ordinary if user else None, timeout=15)
                assert result.returncode == expected, (mode, host, result.returncode, expected)

            check('writer')
            check('reader', user=True)
            check('repeat')
            # An explicit failed TCP target must not use the available local socket.
            check('writer', '127.0.0.1', 1, expected=2)
            with socket.socket() as listener:
                listener.bind(('127.0.0.1', 0))
                peer_port = listener.getsockname()[1]
            peer = subprocess.Popen(['redis-server', '--bind', '127.0.0.1', '--port', str(peer_port),
                '--save', '', '--appendonly', 'no'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            servers.append(peer)
            for _ in range(200):
                try:
                    with socket.create_connection(('127.0.0.1', peer_port), timeout=0.1): break
                except OSError: time.sleep(0.01)
            check('remote', '127.0.0.1', peer_port)
            secret.write_text('wrong-fixture-password')
            secret.chmod(0o400)
            check('writer', expected=2)
            check('reader', user=True)
            print('Native local writer, ordinary reader/denied write, TCP isolation, fail-closed auth and descriptor checks passed.')
    finally:
        for server in servers:
            server.terminate()
            server.wait(timeout=5)
        (policy / 'local.json').unlink(missing_ok=True)
        (policy / 'local-system/writer.secret').unlink(missing_ok=True)
        (policy / 'local-system').rmdir()
        policy.rmdir()
    return 0


if __name__ == '__main__':
    sys.exit(main())
