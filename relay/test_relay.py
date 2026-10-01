import asyncio
import json
import secrets
import tempfile
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from relay import Relay

async def send(socket, data):
    await socket.send(json.dumps(data))

async def receive(socket):
    return json.loads(await asyncio.wait_for(socket.recv(), 3))

async def test():
    with tempfile.TemporaryDirectory() as folder:
        enrollment = secrets.token_hex(32)
        relay = Relay(folder, enrollment)
        async with serve(relay.handle, '127.0.0.1', 0, max_size=65536, max_queue=4, compression=None, close_timeout=1) as server:
            base = 'ws://127.0.0.1:' + str(server.sockets[0].getsockname()[1])
            registration = {'operation': 'register', 'id': secrets.token_hex(32), 'fingerprint': secrets.token_hex(32), 'token': secrets.token_hex(32), 'name': 'Windows computer', 'enrollment': enrollment, 'attestation': 'A' * 512}
            async with connect(base + '/relay/control') as unauthorized:
                await send(unauthorized, {**registration, 'enrollment': secrets.token_hex(32)})
                assert (await receive(unauthorized))['kind'] == 'error'
            async with connect(base + '/relay/control', compression=None) as host:
                await send(host, registration)
                code = (await receive(host))['code']
                assert len(code) == 8
                async with connect(base + '/relay/control') as wrong:
                    await send(wrong, {**registration, 'token': secrets.token_hex(32)})
                    assert (await receive(wrong))['kind'] == 'error'
                await send(host, {'operation': 'heartbeat'})
                assert (await receive(host))['kind'] == 'heartbeat'
                async with connect(base + '/relay/control') as viewer:
                    await send(viewer, {'operation': 'lookup', 'code': code})
                    target = await receive(viewer)
                    request = await receive(host)
                    assert target['fingerprint'] == registration['fingerprint']
                    assert target['token'] != request['token']
                    async with connect(base + '/relay/session', compression=None) as a, connect(base + '/relay/session', compression=None) as b:
                        await send(a, {'session': request['session'], 'token': request['token']})
                        await send(b, {'session': target['session'], 'token': target['token']})
                        assert (await receive(a))['kind'] == 'ready'
                        assert (await receive(b))['kind'] == 'ready'
                        payload = secrets.token_bytes(65536)
                        await a.send(payload)
                        assert await b.recv() == payload
                        await b.send(b'opaque encrypted viewer traffic')
                        assert await a.recv() == b'opaque encrypted viewer traffic'
                        async with connect(base + '/relay/session') as replay:
                            await send(replay, {'session': target['session'], 'token': target['token']})
                            assert (await receive(replay))['kind'] == 'error'
                        await host.close()
                        await asyncio.wait_for(a.wait_closed(), 3)
                        await asyncio.wait_for(b.wait_closed(), 3)
                assert not relay.sessions
            async with connect(base + '/relay/control') as reconnect:
                await send(reconnect, registration)
                assert (await receive(reconnect))['code'] == code
            async with connect(base + '/relay/control') as offline:
                await send(offline, {'operation': 'lookup', 'code': code})
                assert (await receive(offline))['kind'] == 'error'
        assert relay.db.execute('SELECT token FROM hosts').fetchone()[0] != registration['token']
        relay.db.close()
    print('PASS relay registration ownership, stable short code, opaque duplex transport, token replay rejection, offline refusal and disconnect cleanup')

asyncio.run(test())
