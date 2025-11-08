# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2025 Oleksandr Kozlov

import asyncio
import pytest
from conftest import open_client, recv_until


@pytest.mark.skip(reason="FIXME")
@pytest.mark.asyncio
@pytest.mark.timeout(20)
async def test_server(server_proc):
    ws0, ws1, ws2 = await asyncio.gather(
        # TODO: remove the delays after fixing a segfault of the server
        open_client('Player0', 0.0),
        open_client('Player1', 0.1),
        open_client('Player2', 0.2),
    )

    try:
        # Each player should receive its LoginResponse
        jr0 = await recv_until(ws0, lambda m: m.method == 'LoginResponse', timeout=5)
        jr1 = await recv_until(ws1, lambda m: m.method == 'LoginResponse', timeout=5)
        jr2 = await recv_until(ws2, lambda m: m.method == 'LoginResponse', timeout=5)

        assert jr0.method == 'LoginResponse'
        assert jr1.method == 'LoginResponse'
        assert jr2.method == 'LoginResponse'

        # Earlier players should see PlayerJoined for later joiners
        pj01 = await recv_until(ws0, lambda m: m.method == 'PlayerJoined', timeout=5)
        pj02 = await recv_until(ws0, lambda m: m.method == 'PlayerJoined', timeout=5)
        pj03 = await recv_until(ws1, lambda m: m.method == 'PlayerJoined', timeout=5)
        assert pj01.method == 'PlayerJoined'
        assert pj02.method == 'PlayerJoined'
        assert pj03.method == 'PlayerJoined'

        # Everyone should eventually get DealCards
        dc0 = await recv_until(ws0, lambda m: m.method == 'DealCards', timeout=5)
        dc1 = await recv_until(ws1, lambda m: m.method == 'DealCards', timeout=5)
        dc2 = await recv_until(ws2, lambda m: m.method == 'DealCards', timeout=5)
        assert dc0.method == dc1.method == dc2.method == 'DealCards'

        # And PlayerTurn announcing the first bidder
        pt0 = await recv_until(ws0, lambda m: m.method == 'PlayerTurn', timeout=5)
        pt1 = await recv_until(ws1, lambda m: m.method == 'PlayerTurn', timeout=5)
        pt2 = await recv_until(ws2, lambda m: m.method == 'PlayerTurn', timeout=5)
        assert pt0.method == pt1.method == pt2.method == 'PlayerTurn'

    finally:
        await asyncio.gather(
            ws0.close(code=1000),
            ws1.close(code=1000),
            ws2.close(code=1000),
            return_exceptions=True)
