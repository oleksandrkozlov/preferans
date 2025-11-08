# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2025 Oleksandr Kozlov

from pathlib import Path
from websockets import connect
import asyncio
import contextlib
import os
import pref_pb2
import pytest
import socket
import subprocess
import time


def pytest_addoption(parser):
    parser.addoption('--project-dir', action='store',
                     help='Path to the project')


@pytest.fixture(scope='class')
def service_fixture(request):
    request.cls.project_dir = request.config.getoption('--project-dir')


@pytest.fixture(scope='session')
def project_dir(request):
    return request.config.getoption('--project-dir')


@pytest.fixture
def server_path(project_dir):
    yield Path(os.path.join(project_dir, 'build-server', 'bin', 'server'))


def wait_for_port(host, port, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as sock:
            sock.settimeout(0.2)
            try:
                if sock.connect_ex((host, port)) == 0:
                    return
            except OSError:
                pass
        time.sleep(0.05)
    raise TimeoutError(f'Port {host}:{port} did not open within {timeout}s')


async def send_join(websocket, name):
    jr = pref_pb2.LoginRequest()
    jr.player_name = name
    msg = pref_pb2.Message()
    msg.method = 'LoginRequest'
    msg.payload = jr.SerializeToString()
    await websocket.send(msg.SerializeToString())


async def recv_message(websocket, timeout=5.0):
    data = await asyncio.wait_for(websocket.recv(), timeout=timeout)
    msg = pref_pb2.Message()
    msg.ParseFromString(data)
    return msg


async def recv_until(websocket, predicate, timeout=5.0):
    deadline = asyncio.get_event_loop().time() + timeout
    while True:
        remaining = deadline - asyncio.get_event_loop().time()
        if remaining <= 0:
            raise asyncio.TimeoutError('recv_until timed out')
        msg = await recv_message(websocket, remaining)
        if predicate(msg):
            return msg


async def open_client(name, delay):
    ws = await connect('ws://0.0.0.0:8080')
    await asyncio.sleep(delay)
    await send_join(ws, name)
    return ws


@pytest.fixture
def server_proc(server_path):
    host = '0.0.0.0'
    port = 8080
    proc = subprocess.Popen(
        [str(server_path), host, str(port)],
        stdout=None,
        stderr=None,
        text=True,
        env={**os.environ},
    )
    try:
        wait_for_port(host, port, timeout=10.0)
    except Exception:
        proc.kill()
        out, err = proc.communicate(timeout=2)
        raise RuntimeError(
            f'Server failed to start.\nOUT:\n{out}\nERR:\n{err}')
    yield proc
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
