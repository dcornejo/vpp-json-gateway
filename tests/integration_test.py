#!/usr/bin/env python3
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

"""Real Redis integration checks; uses only Python's standard library."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import tempfile
import time
import uuid


class Redis:
    def __init__(self, port):
        self.socket = socket.create_connection(('127.0.0.1', port), timeout=3)
        self.file = self.socket.makefile('rb')

    def command(self, *args):
        values = [str(a).encode() if not isinstance(a, bytes) else a for a in args]
        self.socket.sendall(b'*%d\r\n' % len(values) + b''.join(
            b'$%d\r\n' % len(a) + a + b'\r\n' for a in values))
        return self.read()

    def read(self):
        line = self.file.readline()
        if not line:
            raise RuntimeError('Redis disconnected')
        kind, value = line[:1], line[1:-2]
        if kind == b'+':
            return value.decode()
        if kind == b'-':
            raise RuntimeError(value.decode())
        if kind == b':':
            return int(value)
        if kind == b'$':
            size = int(value)
            if size == -1:
                return None
            data = self.file.read(size)
            assert self.file.read(2) == b'\r\n'
            return data.decode()
        if kind == b'*':
            return [self.read() for _ in range(int(value))]
        raise RuntimeError(line)

    def close(self):
        self.file.close()
        self.socket.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', required=True)
    parser.add_argument('--redis-server', required=True)
    parser.add_argument('--client')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='vpp-json-integration-') as temporary:
        root = Path(temporary)
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        log = (root / 'process.log').open('w')
        redis_process = subprocess.Popen([args.redis_server, '--port', str(port),
            '--bind', '127.0.0.1', '--save', '', '--appendonly', 'no',
            '--dir', temporary], stdout=log, stderr=log)
        server = None
        redis = None
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                try:
                    redis = Redis(port)
                    break
                except OSError:
                    time.sleep(.05)
            assert redis is not None, 'Redis startup failed'
            token = 'integration-only-' + 'a' * 32
            config = root / 'clients.json'
            config.write_text(json.dumps({'alice': token, 'bob': token}))
            server_args = [args.server, '--backend', 'mock', '--mock-count', '3000',
                '--redis-port', str(port), '--clients', str(config), '--state', str(root / 'state')]

            def start():
                process = subprocess.Popen(server_args, stdout=log, stderr=log)
                time.sleep(.1)
                assert process.poll() is None
                return process

            def send(key, method, params=None, request_id=None, **extra):
                request_id = request_id or uuid.uuid4().hex
                body = dict(id=request_id, method=method, **extra)
                if params is not None:
                    body['params'] = params
                redis.command('XADD', key, '*', 'json', json.dumps(body, separators=(',', ':')))
                return request_id

            def receive(key, request_id, timeout=40):
                frames = {}
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    assert server.poll() is None, 'Gateway exited'
                    for stream_id, fields in redis.command('XRANGE', key, '-', '+', 'COUNT', 32):
                        frame = json.loads(dict(zip(fields[::2], fields[1::2]))['json'])
                        redis.command('XDEL', key, stream_id)
                        if frame['id'] != request_id:
                            continue
                        frames[frame['seq']] = frame
                        if frame['type'] in ('error', 'result', 'complete'):
                            assert sorted(frames) == list(range(frame['seq'] + 1)), 'Response sequence gap'
                            return [frames[i] for i in sorted(frames)]
                    time.sleep(.003)
                raise AssertionError(f'Timeout: {request_id}, frames={len(frames)}')

            def register(client):
                identifier = send('vpp:register', 'session.register',
                    {'client': client, 'token': token, 'protocol': 1})
                return receive('vpp:bootstrap:' + client, identifier)[-1]['result']

            def call(session, method, params=None, **extra):
                identifier = send(session['requests'], method, params, **extra)
                return receive(session['responses'], identifier)

            server = start()
            # A separate journal cannot claim an already owned Redis namespace.
            other_args = server_args.copy()
            other_args[-1] = str(root / 'other-state')
            other = subprocess.run(other_args, stdout=log, stderr=log, timeout=5)
            assert other.returncode != 0, 'Second owner was admitted'
            print('PASS exclusive Redis namespace ownership')
            alice = register('alice')
            bob = register('bob')
            assert alice['session'] != bob['session']
            assert call(alice, 'api.describe')[-1]['result']['backend'] == 'mock'
            print('PASS registration and capability discovery')

            changed = send(alice['requests'], 'interface.set_state', {'interface': 'loop1', 'state': 'up'})
            assert receive(alice['responses'], changed)[-1]['type'] == 'result'
            assert call(alice, 'interface.set_state', {'interface': 'missing', 'state': 'up'})[-1]['error']['code'] == 'resource_not_found'
            assert call(alice, 'interface.set_state', {'interface': 'loop1', 'state': 1})[-1]['error']['code'] == 'invalid_params'
            assert call(alice, 'interface.set_state', {'interface': 'loop1', 'state': 'down'})[-1]['type'] == 'result'
            send(alice['requests'], 'api.replay', {'request': changed})
            assert receive(alice['responses'], changed)[-1]['type'] == 'result'
            print('PASS symbolic mutation, validation and retained replay')

            # Hold Alice's dump without acknowledging; Bob must keep working.
            dump = send(alice['requests'], 'interface.list')
            time.sleep(.7)
            assert redis.command('XLEN', alice['responses']) == 32
            assert call(bob, 'api.describe')[-1]['type'] == 'result'
            assert redis.command('XLEN', alice['responses']) == 32
            frames = receive(alice['responses'], dump)
            assert frames[-1]['type'] == 'complete' and frames[-1]['items'] == 3000
            items = [frame['items'][0] for frame in frames[:-1]]
            assert next(i for i in items if i['interface'] == 'loop1')['state'] == 'down', 'Replay executed mutation twice'
            assert sum(len(json.dumps(f)) for f in frames) > 256 * 1024
            print('PASS multi-frame dump, sequence integrity and slow-client isolation')

            transfer = 'upload'
            body = ' ' * (1024 * 1024) + '{"interface":"loop2","state":"up"}'
            assert call(bob, 'transfer.begin', {'transfer': transfer, 'bytes': len(body), 'sha256': hashlib.sha256(body.encode()).hexdigest()})[-1]['type'] == 'result'
            assert call(alice, 'transfer.status', {'transfer': transfer})[-1]['error']['code'] == 'resource_not_found'
            for seq, offset in enumerate(range(0, len(body), 100000)):
                assert call(bob, 'transfer.chunk', {'transfer': transfer, 'seq': seq, 'data': body[offset:offset+100000]})[-1]['type'] == 'result'
            assert call(bob, 'transfer.commit', {'transfer': transfer})[-1]['result']['committed']
            assert call(bob, 'interface.set_state', params_ref=transfer)[-1]['type'] == 'result'
            small = '{"interface":"loop2","state":"up"}'
            assert call(bob, 'transfer.begin', {'transfer': 'small', 'bytes': len(small), 'sha256': hashlib.sha256(small.encode()).hexdigest()})[-1]['type'] == 'result'
            assert call(bob, 'transfer.chunk', {'transfer': 'small', 'seq': 0, 'data': small})[-1]['type'] == 'result'
            assert call(bob, 'transfer.commit', {'transfer': 'small'})[-1]['type'] == 'result'
            assert call(bob, 'interface.set_state', params_ref='small')[-1]['type'] == 'result'
            print('PASS large upload, checksum commit, session isolation and params_ref')

            conflict = send(alice['requests'], 'api.describe', request_id=changed)
            assert receive(alice['responses'], conflict)[-1]['error']['code'] == 'id_conflict'
            malformed = redis.command('XADD', alice['requests'], '*', 'json', '{"id":7}')
            assert receive(alice['responses'], 'invalid')[-1]['error']['code'] == 'invalid_request'
            assert malformed
            print('PASS conflicting IDs and malformed envelopes')

            # Model a durable running marker left at a process crash boundary.
            server.terminate()
            server.wait(timeout=10)
            with sqlite3.connect(root / 'state/journal.sqlite') as db:
                db.execute("UPDATE jobs SET state='running' WHERE sid=? AND id=?", (alice['session'], changed))
            server = start()
            assert receive(alice['responses'], changed)[-1]['error']['code'] == 'outcome_unknown'
            assert call(bob, 'transfer.status', {'transfer': 'small'})[-1]['result']['committed']
            print('PASS durable session/transfer recovery and uncertain-operation recovery')

            if args.client:
                env = dict(os.environ, GATEWAY_TOKEN=token)
                session_file = root / 'client-session.json'
                command = [args.client, '--client', 'alice', '--session', str(session_file),
                    '--redis-port', str(port), '--method', 'interface.set_state',
                    '--params', '{"interface":"loop0","state":"up"}', '--request-id', 'cli-request']
                first = subprocess.run(command, env=env, capture_output=True, text=True, timeout=10)
                assert first.returncode == 0, first.stderr + first.stdout
                second = subprocess.run(command, env=env, capture_output=True, text=True, timeout=10)
                assert second.returncode == 0, second.stderr + second.stdout
                print('PASS C++ client registration and retry')
                # Isolate a protocol fixture from the gateway: exercise the real
                # C++ client's reassembly across more than one Redis window.
                server.terminate()
                server.wait(timeout=10)
                saved = json.loads(session_file.read_text())
                payload = json.dumps({'id': 'fragment-test', 'type': 'result', 'result': {'data': '☃' * 700000}}, ensure_ascii=False).encode()
                digest = hashlib.sha256(payload).hexdigest()
                with (root / 'fragment-output.json').open('w+') as output:
                    command[-1] = 'fragment-test'
                    process = subprocess.Popen(command, env=env, stdout=output, stderr=subprocess.PIPE, text=True)
                    try:
                        for seq, offset in enumerate(range(0, len(payload), 48 * 1024)):
                            deadline = time.monotonic() + 10
                            while redis.command('XLEN', saved['responses']) >= 32:
                                assert time.monotonic() < deadline, 'Fragment ACK timeout'
                                time.sleep(.005)
                            frame = {'id': 'fragment-test', 'type': 'fragment', 'seq': seq, 'record': 0, 'offset': offset, 'total': len(payload), 'sha256': digest, 'encoding': 'base64-json', 'data': base64.b64encode(payload[offset:offset + 48 * 1024]).decode()}
                            redis.command('XADD', saved['responses'], '*', 'json', json.dumps(frame))
                        _, errors = process.communicate(timeout=10)
                        assert process.returncode == 0, errors
                        output.seek(0)
                        assert json.load(output)['result']['data'] == '☃' * 700000
                    finally:
                        if process.poll() is None:
                            process.kill()
                            process.wait(timeout=5)
                print('PASS C++ client reassembles a large Unicode result through Redis')
            print('All Redis integration checks passed')
        finally:
            if server is not None and server.poll() is None:
                server.terminate()
                server.wait(timeout=35)
            if redis is not None:
                redis.close()
            redis_process.terminate()
            redis_process.wait(timeout=10)
            log.close()


if __name__ == '__main__':
    main()
