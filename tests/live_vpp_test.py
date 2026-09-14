#!/usr/bin/env python3
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

"""Exercise a disposable, unprivileged VPP instance; no host interfaces touched."""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import sqlite3
import subprocess
import time
import uuid

from integration_test import Redis


def main():
    parser = argparse.ArgumentParser()
    for name in ('vpp', 'vppctl', 'server', 'redis-server', 'workdir'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    root = Path(args.workdir).resolve()
    root.mkdir(mode=0o700, parents=True, exist_ok=False)
    log = (root / 'process.log').open('w')
    processes = []
    redis = None
    vpp = None
    server = None
    paused = False
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    shared_prefix = 'json-test-' + uuid.uuid4().hex
    config = root / 'vpp.conf'
    config.write_text(f'''unix {{ nodaemon runtime-dir {root} cli-listen {root}/cli.sock log {root}/vpp.log }}
api-segment {{ prefix {shared_prefix} }}
memory {{ main-heap-size 512M main-heap-page-size 4K }}
buffers {{ buffers-per-numa 1024 page-size 4K }}
socksvr {{ socket-name {root}/api.sock }}
statseg {{ socket-name {root}/stats.sock size 128M page-size 4K }}
plugins {{ plugin default {{ disable }} }}
''')
    token = uuid.uuid4().hex + uuid.uuid4().hex
    (root / 'clients.json').write_text(json.dumps({'live': token, 'observer': token}))

    def launch(command):
        process = subprocess.Popen(command, stdout=log, stderr=log)
        processes.append(process)
        return process

    def cli(command):
        completed = subprocess.run([args.vppctl, '-s', str(root / 'cli.sock'), command],
                                   text=True, capture_output=True, timeout=20)
        assert completed.returncode == 0, completed.stderr + completed.stdout
        assert 'unknown input' not in completed.stdout.lower(), completed.stdout
        return completed.stdout

    def start_vpp():
        for name in ('api.sock', 'cli.sock', 'stats.sock'):
            (root / name).unlink(missing_ok=True)
        process = launch([args.vpp, '-c', str(config)])
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            assert process.poll() is None, 'VPP startup failed'
            if (root / 'api.sock').exists() and (root / 'cli.sock').exists():
                return process
            time.sleep(.05)
        raise AssertionError('VPP socket startup timeout')

    def start_server():
        return launch([args.server, '--backend', 'vapi', '--vpp-socket', str(root / 'api.sock'),
                       '--redis-port', str(port), '--state', str(root / 'state'),
                       '--clients', str(root / 'clients.json')])

    def send(stream, method, params=None):
        identifier = uuid.uuid4().hex
        request = {'id': identifier, 'method': method}
        if params is not None:
            request['params'] = params
        redis.command('XADD', stream, '*', 'json', json.dumps(request))
        return identifier

    def receive(stream, identifier, timeout=60):
        deadline = time.monotonic() + timeout
        frames = {}
        while time.monotonic() < deadline:
            assert server.poll() is None, 'Gateway exited'
            for entry_id, fields in redis.command('XRANGE', stream, '-', '+', 'COUNT', 32):
                frame = json.loads(dict(zip(fields[::2], fields[1::2]))['json'])
                redis.command('XDEL', stream, entry_id)
                if frame['id'] != identifier:
                    continue
                frames[frame['seq']] = frame
                if frame['type'] in ('result', 'complete', 'error'):
                    assert sorted(frames) == list(range(frame['seq'] + 1))
                    return [frames[i] for i in sorted(frames)]
            time.sleep(.003)
        raise AssertionError(f'Timeout waiting for {identifier}')

    def register(client):
        identifier = send('vpp:register', 'session.register',
                          {'client': client, 'token': token, 'protocol': 1})
        return receive('vpp:bootstrap:' + client, identifier)[-1]['result']

    def call(session, method, params=None):
        identifier = send(session['requests'], method, params)
        return receive(session['responses'], identifier)

    def assert_state(name, state):
        output = cli('show interface ' + name)
        rows = [line.split() for line in output.splitlines() if line.split() and line.split()[0] == name]
        assert len(rows) == 1 and rows[0][2].lower() == state, output

    try:
        launch([args.redis_server, '--bind', '127.0.0.1', '--port', str(port),
                '--save', '', '--appendonly', 'no', '--dir', str(root)])
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                redis = Redis(port)
                break
            except OSError:
                time.sleep(.05)
        assert redis is not None
        vpp = start_vpp()
        version = cli('show version').strip()
        assert '26.06' in version, version
        print('VPP:', version, flush=True)
        server = start_server()
        live = register('live')
        observer = register('observer')
        initial = call(live, 'interface.list')
        assert initial[-1]['type'] == 'complete', initial
        assert any(frame['items'][0]['interface'] == 'local0' for frame in initial[:-1])
        cli('create loopback interface instance 0')
        first = send(live['requests'], 'interface.set_state', {'interface': 'loop0', 'state': 'up'})
        assert receive(live['responses'], first)[-1]['type'] == 'result'
        assert_state('loop0', 'up')
        assert call(live, 'interface.set_state', {'interface': 'loop0', 'state': 'down'})[-1]['type'] == 'result'
        assert_state('loop0', 'down')
        send(live['requests'], 'api.replay', {'request': first})
        assert receive(live['responses'], first)[-1]['type'] == 'result'
        assert_state('loop0', 'down')
        assert call(live, 'interface.set_state', {'interface': 'missing', 'state': 'up'})[-1]['error']['code'] == 'resource_not_found'
        print('PASS live symbolic list/up/down, independent CLI verification and non-mutating replay', flush=True)

        description = call(live, 'api.describe', {'method': 'vpp.sw_interface_set_flags'})[-1]
        assert description['type'] == 'result', description
        created = call(live, 'vpp.create_loopback', {'mac_address': '02:00:00:00:00:01'})[-1]
        assert created['type'] == 'result', created
        generated_name = created['result']['sw_if_index']
        assert isinstance(generated_name, str) and generated_name.startswith('loop'), created
        changed = call(live, 'vpp.sw_interface_set_flags', {'sw_if_index': generated_name, 'flags': ['IF_STATUS_API_FLAG_ADMIN_UP']})[-1]
        assert changed['type'] == 'result', changed
        assert_state(generated_name, 'up')
        rejected = call(live, 'vpp.sw_interface_set_flags', {'sw_if_index': 1, 'flags': 1})[-1]
        assert rejected['type'] == 'error' and rejected['error']['code'] == 'invalid_params', rejected
        address = {'sw_if_index': generated_name, 'is_add': True, 'del_all': False, 'prefix': '192.0.2.1/24'}
        added = call(live, 'vpp.sw_interface_add_del_address', address)[-1]
        assert added['type'] == 'result', added
        assert '192.0.2.1/24' in cli('show interface address ' + generated_name)
        dumped = call(live, 'vpp.sw_interface_dump', {'sw_if_index': generated_name, 'name_filter_valid': False, 'name_filter': ''})
        assert dumped[-1]['type'] == 'complete' and dumped[-1]['items'] == 1, dumped
        assert dumped[0]['items'][0]['sw_if_index'] == generated_name, dumped
        large_params = json.dumps({'sw_if_index': generated_name, 'name_filter_valid': False, 'name_filter': 'x' * 300000})
        import hashlib
        transfer = 'large-vapi-params'
        assert call(live, 'transfer.begin', {'transfer': transfer, 'bytes': len(large_params), 'sha256': hashlib.sha256(large_params.encode()).hexdigest()})[-1]['type'] == 'result'
        for seq, offset in enumerate(range(0, len(large_params), 100000)):
            assert call(live, 'transfer.chunk', {'transfer': transfer, 'seq': seq, 'data': large_params[offset:offset + 100000]})[-1]['type'] == 'result'
        assert call(live, 'transfer.commit', {'transfer': transfer})[-1]['type'] == 'result'
        large_id = uuid.uuid4().hex
        redis.command('XADD', live['requests'], '*', 'json', json.dumps({'id': large_id, 'method': 'vpp.sw_interface_dump', 'params_ref': transfer}))
        large_dump = receive(live['responses'], large_id)
        assert large_dump[-1]['type'] == 'complete' and large_dump[-1]['items'] == 1, large_dump
        print('PASS uploaded 300 KB parameters encoded and dispatched through live VAPI', flush=True)
        deleted = call(live, 'vpp.delete_loopback', {'sw_if_index': generated_name})[-1]
        assert deleted['type'] == 'result', deleted
        print('PASS generated discovery, create/delete, symbolic flags, CIDR and dump; numeric symbols rejected', flush=True)

        threads = call(live, 'vpp.show_threads')[-1]
        assert threads['type'] == 'result', threads
        assert threads['result']['thread_data'] and 'count' not in threads['result'], threads
        assert isinstance(threads['result']['thread_data'][0]['name'], str), threads
        table = {'table_id': 8123, 'is_ip6': False, 'name': 'gateway-test'}
        assert call(live, 'vpp.ip_table_add_del', {'table': table})[-1]['type'] == 'result'
        tables = call(live, 'vpp.ip_table_dump')
        assert tables[-1]['type'] == 'complete', tables
        assert any(f['items'][0]['table']['table_id'] == 8123 for f in tables[:-1]), tables
        cli('ip route add table 8123 198.51.100.0/24 via 192.0.2.2 loop0')
        routes = call(live, 'vpp.ip_route_dump', {'table': table})
        assert routes[-1]['type'] == 'complete', routes
        route = next(f['items'][0]['route'] for f in routes[:-1] if f['items'][0]['route']['prefix'] == '198.51.100.0/24')
        assert route['paths'] and 'n_paths' not in route, route
        assert route['paths'][0]['nh']['address']['ip4'] == '192.0.2.2', route
        lookup = call(live, 'vpp.ip_route_lookup', {'table_id': 8123, 'exact': 1, 'prefix': '198.51.100.0/24'})[-1]
        assert lookup['type'] == 'result' and lookup['result']['route']['paths'], lookup
        cli('ip route del table 8123 198.51.100.0/24 via 192.0.2.2 loop0')
        table6 = {'table_id': 8124, 'is_ip6': True, 'name': 'gateway-test-v6'}
        assert call(live, 'vpp.ip_table_add_del', {'table': table6})[-1]['type'] == 'result'
        cli('ip route add table 8124 2001:db8:1::/64 via 2001:db8::2 loop0')
        lookup6 = call(live, 'vpp.ip_route_lookup', {'table_id': 8124, 'exact': 1, 'prefix': '2001:db8:1::/64'})[-1]
        assert lookup6['type'] == 'result', lookup6
        assert lookup6['result']['route']['paths'][0]['nh']['address']['ip6'] == '2001:db8::2', lookup6
        cli('ip route del table 8124 2001:db8:1::/64 via 2001:db8::2 loop0')
        assert call(live, 'vpp.ip_table_add_del', {'is_add': False, 'table': table6})[-1]['type'] == 'result'
        print('PASS nested route dump and lookup with variable paths and IPv4/IPv6 union decoding', flush=True)
        assert call(live, 'vpp.ip_table_add_del', {'is_add': False, 'table': table})[-1]['type'] == 'result'
        # A loopback has no hardware TX queue. The explicit completion error
        # must end the response instead of hanging or claiming an empty success.
        placement = call(live, 'vpp.sw_interface_tx_placement_get', {'sw_if_index': '@all'})
        assert placement[-1]['type'] in ('complete', 'error'), placement
        if placement[-1]['type'] == 'error':
            assert placement[-1]['error']['code'] == 'vpp_rejected', placement
        assert call(live, 'vpp.sw_interface_tx_placement_get', {'sw_if_index': '@all', 'cursor': 0})[-1]['error']['code'] == 'invalid_params'
        print('PASS variable thread reply, IP table lifecycle, explicit stream completion and hidden cursor', flush=True)

        commands = root / 'loopbacks.cli'
        commands.write_text(''.join(f'create loopback interface instance {i}\n' for i in range(1, 3000)))
        cli('exec ' + str(commands))
        assert vpp.poll() is None, 'VPP exited while creating fixture interfaces'
        dump_id = send(live['requests'], 'interface.list')
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline and redis.command('XLEN', live['responses']) < 32:
            assert server.poll() is None
            assert vpp.poll() is None, 'VPP exited during dump'
            time.sleep(.05)
        assert redis.command('XLEN', live['responses']) == 32
        assert call(observer, 'api.describe')[-1]['type'] == 'result'
        frames = receive(live['responses'], dump_id)
        assert frames[-1]['type'] == 'complete' and frames[-1]['items'] == 3001, frames[-1]
        assert sum(len(json.dumps(frame)) for frame in frames) > 256 * 1024
        print('PASS 3,001 live interfaces, bounded delivery, another client and complete sequence', flush=True)

        # Pause only our disposable VPP. Kill the gateway after its durable
        # running marker is visible, then recover against the same VPP state.
        vpp.send_signal(signal.SIGSTOP)
        paused = True
        uncertain = send(live['requests'], 'interface.set_state', {'interface': 'loop0', 'state': 'up'})
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            with sqlite3.connect(root / 'state/journal.sqlite') as journal:
                row = journal.execute('SELECT state FROM jobs WHERE sid=? AND id=?', (live['session'], uncertain)).fetchone()
            if row and row[0] == 'running':
                break
            time.sleep(.01)
        assert row and row[0] == 'running'
        server.kill()
        server.wait(timeout=5)
        vpp.send_signal(signal.SIGCONT)
        paused = False
        server = start_server()
        recovered = receive(live['responses'], uncertain)
        assert recovered[-1]['error']['code'] == 'outcome_unknown', recovered
        assert_state('loop0', 'down')
        print('PASS actual gateway crash with an in-flight operation; no automatic mutation retry', flush=True)

        vpp.terminate()
        vpp.wait(timeout=10)
        offline = call(live, 'interface.list')
        assert offline[-1]['type'] == 'error', offline
        vpp = start_vpp()
        cli('create loopback interface instance 9000')
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            refreshed = call(live, 'interface.list')
            if refreshed[-1]['type'] == 'complete':
                break
            time.sleep(.2)
        assert refreshed[-1]['type'] == 'complete', refreshed
        names = {frame['items'][0]['interface'] for frame in refreshed[:-1]}
        assert names == {'local0', 'loop9000'}, names
        assert call(live, 'interface.set_state', {'interface': 'loop0', 'state': 'up'})[-1]['error']['code'] == 'resource_not_found'
        assert call(live, 'interface.set_state', {'interface': 'loop9000', 'state': 'up'})[-1]['type'] == 'result'
        assert_state('loop9000', 'up')
        print('PASS VPP disconnect/restart, fresh interface resolution and recovered mutations', flush=True)
        print('All live VPP checks passed', flush=True)
    finally:
        if paused and vpp is not None and vpp.poll() is None:
            vpp.send_signal(signal.SIGCONT)
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=35)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        if redis is not None:
            redis.close()
        for suffix in ('-global_vm', '-vpe-api'):
            Path('/dev/shm', shared_prefix + suffix).unlink(missing_ok=True)
        log.close()
        print('Logs retained in', root, flush=True)
        if os.path.exists(root / 'vpp.log'):
            print((root / 'vpp.log').read_text(errors='replace')[-3000:], flush=True)


if __name__ == '__main__':
    main()
