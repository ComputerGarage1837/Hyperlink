"""Private rendezvous and bounded opaque transport. Desktop PINs stay on hosts."""
import asyncio
import hashlib
import json
import os
import re
import secrets
import sqlite3
import time
from collections import deque
from pathlib import Path
from websockets.asyncio.server import serve
from websockets.http11 import Response
from websockets.datastructures import Headers

HEX = re.compile(r'^[a-f0-9]{64}$')
CODE = re.compile(r'^[1-9][0-9]{7}$')

def digest(value):
    return hashlib.sha256(value.encode('ascii')).hexdigest()

class Relay:
    def __init__(self, folder, enrollment_key=None):
        os.umask(0o077)
        folder = Path(folder)
        folder.mkdir(mode=0o700, parents=True, exist_ok=True)
        key_file = folder / 'enrollment.key'
        if enrollment_key is None:
            if not key_file.exists():
                with key_file.open('x') as output:
                    output.write(secrets.token_hex(32))
            enrollment_key = key_file.read_text().strip()
        if not HEX.fullmatch(enrollment_key):
            raise ValueError('Invalid private enrollment configuration')
        self.enrollment = digest(enrollment_key)
        self.db = sqlite3.connect(folder / 'rendezvous.sqlite')
        self.db.execute('PRAGMA journal_mode=WAL')
        self.db.execute('CREATE TABLE IF NOT EXISTS hosts(code TEXT PRIMARY KEY, identity TEXT UNIQUE NOT NULL, fingerprint TEXT NOT NULL, name TEXT NOT NULL, token TEXT NOT NULL)')
        if 'attestation' not in [row[1] for row in self.db.execute('PRAGMA table_info(hosts)')]:
            self.db.execute("ALTER TABLE hosts ADD COLUMN attestation TEXT NOT NULL DEFAULT ''")
            self.db.commit()
        self.online = {}
        self.sessions = {}
        self.rates = {}

    def limit(self, key, count, seconds):
        now = time.monotonic()
        if len(self.rates) >= 4096:
            self.rates = {k: v for k, v in self.rates.items() if v and v[-1] > now - 3600}
            if key not in self.rates and len(self.rates) >= 4096:
                raise ValueError('Please try again later')
        attempts = self.rates.setdefault(key, deque())
        while attempts and attempts[0] <= now - seconds:
            attempts.popleft()
        if len(attempts) >= count:
            raise ValueError('Please try again later')
        attempts.append(now)

    async def message(self, socket, timeout=10):
        raw = await asyncio.wait_for(socket.recv(), timeout)
        if not isinstance(raw, str) or len(raw.encode('utf-8')) > 16384:
            raise ValueError('Invalid request')
        data = json.loads(raw)
        if not isinstance(data, dict):
            raise ValueError('Invalid request')
        return data

    def client_address(self, socket):
        # Only a loopback-bound reverse proxy may supply this header.
        value = socket.request.headers.get('X-Forwarded-For', '')
        import ipaddress
        try:
            return str(ipaddress.ip_address(value.split(',')[0].strip()))
        except ValueError:
            return socket.remote_address[0]

    async def handle(self, socket):
        try:
            if socket.request.headers.get('Origin') is not None:
                raise ValueError('Native clients only')
            path = socket.request.path
            if path not in ('/relay/control', '/relay/session'):
                raise ValueError('Unknown endpoint')
            address = self.client_address(socket)
            self.limit(('connect', address), 120, 60)
            request = await self.message(socket)
            if path == '/relay/session':
                await self.tunnel(socket, request)
            elif request.get('operation') == 'register':
                await self.host(socket, request, address)
            elif request.get('operation') == 'lookup':
                await self.viewer(socket, request, address)
            else:
                raise ValueError('Unknown operation')
        except (ValueError, KeyError, TypeError, json.JSONDecodeError, asyncio.TimeoutError):
            try:
                await socket.send(json.dumps({'kind': 'error', 'message': 'Connection unavailable or request refused'}))
                await socket.close(1008)
            except Exception:
                pass
        except Exception:
            try:
                await socket.close(1011)
            except Exception:
                pass

    async def host(self, socket, request, address):
        identity, fingerprint, token = (request.get(k, '') for k in ('id', 'fingerprint', 'token'))
        if not all(isinstance(v, str) and HEX.fullmatch(v) for v in (identity, fingerprint, token)):
            raise ValueError('Invalid identity')
        name = request.get('name', '')
        attestation = request.get('attestation', '')
        if not isinstance(attestation, str) or (attestation and not re.fullmatch(r'[A-Za-z0-9+/]{512}', attestation)):
            raise ValueError('Invalid host attestation')
        if not isinstance(name, str) or not 1 <= len(name) <= 48 or any(ord(c) < 32 for c in name):
            raise ValueError('Invalid name')
        row = self.db.execute('SELECT code,token,fingerprint FROM hosts WHERE identity=?', (identity,)).fetchone()
        if row:
            code, saved_token, saved_fingerprint = row
            if not secrets.compare_digest(saved_token, digest(token)) or saved_fingerprint != fingerprint:
                raise ValueError('Registration refused')
        else:
            enrollment = request.get('enrollment', '')
            if not isinstance(enrollment, str) or not HEX.fullmatch(enrollment) or not secrets.compare_digest(digest(enrollment), self.enrollment):
                raise ValueError('Owner enrollment required')
            self.limit(('register', address), 5, 3600)
            if self.db.execute('SELECT COUNT(*) FROM hosts').fetchone()[0] >= 32:
                raise ValueError('Computer limit reached')
            while True:
                code = str(10000000 + secrets.randbelow(90000000))
                if not self.db.execute('SELECT 1 FROM hosts WHERE code=?', (code,)).fetchone():
                    break
            self.db.execute('INSERT INTO hosts(code,identity,fingerprint,name,token,attestation) VALUES(?,?,?,?,?,?)', (code, identity, fingerprint, name, digest(token), attestation))
            self.db.commit()
        if attestation:
            self.db.execute('UPDATE hosts SET attestation=?,name=? WHERE identity=?', (attestation, name, identity))
            self.db.commit()
        if code in self.online:
            raise ValueError('Computer already online')
        self.online[code] = socket
        try:
            await socket.send(json.dumps({'kind': 'registered', 'code': code}))
            while True:
                message = await self.message(socket, 45)
                if message != {'operation': 'heartbeat'}:
                    raise ValueError('Invalid heartbeat')
                await socket.send('{"kind":"heartbeat"}')
        finally:
            if self.online.get(code) is socket:
                self.online.pop(code, None)
            for identifier, session in list(self.sessions.items()):
                if session['code'] == code:
                    await self.end_session(identifier, session)

    async def viewer(self, socket, request, address):
        code = request.get('code', '')
        if not isinstance(code, str) or not CODE.fullmatch(code):
            raise ValueError('Invalid computer code')
        self.limit(('lookup', address), 20, 60)
        self.limit(('computer', code), 20, 60)
        host = self.online.get(code)
        if host is None or sum(s['code'] == code for s in self.sessions.values()) >= 4 or len(self.sessions) >= 64:
            raise ValueError('Computer unavailable')
        row = self.db.execute('SELECT identity,fingerprint,name,attestation FROM hosts WHERE code=?', (code,)).fetchone()
        if not row[3]:
            raise ValueError('Host enrollment incomplete')
        identifier = secrets.token_hex(16)
        host_token, viewer_token = secrets.token_hex(32), secrets.token_hex(32)
        session = {'code': code, 'tokens': {digest(host_token): 'host', digest(viewer_token): 'viewer'}, 'sockets': {}, 'ready': asyncio.Event(), 'done': asyncio.Event(), 'expires': time.monotonic() + 30}
        self.sessions[identifier] = session
        session['expiry_task'] = asyncio.create_task(self.expire(identifier, session))
        try:
            await host.send(json.dumps({'kind': 'session', 'session': identifier, 'token': host_token}))
            await socket.send(json.dumps({'kind': 'computer', 'code': code, 'id': row[0], 'fingerprint': row[1], 'name': row[2], 'attestation': row[3], 'session': identifier, 'token': viewer_token}))
        except Exception:
            await self.end_session(identifier, session)
            raise

    async def expire(self, identifier, session):
        await asyncio.sleep(30)
        if len(session['sockets']) != 2:
            await self.end_session(identifier, session)

    async def end_session(self, identifier, session):
        if self.sessions.get(identifier) is session:
            self.sessions.pop(identifier, None)
        session['done'].set()
        session['ready'].set()
        task = session.get('expiry_task')
        if task and task is not asyncio.current_task():
            task.cancel()
        for socket in list(session['sockets'].values()):
            try:
                await socket.close()
            except Exception:
                pass

    async def tunnel(self, socket, request):
        identifier, token = request.get('session', ''), request.get('token', '')
        if not isinstance(identifier, str) or not isinstance(token, str) or not HEX.fullmatch(token):
            raise ValueError('Invalid session')
        session = self.sessions.get(identifier)
        if session is None or session['expires'] < time.monotonic():
            raise ValueError('Expired session')
        role = session['tokens'].pop(digest(token), None)
        if role is None or role in session['sockets']:
            raise ValueError('Used session token')
        session['sockets'][role] = socket
        try:
            if len(session['sockets']) == 2:
                await asyncio.gather(*(peer.send('{"kind":"ready"}') for peer in session['sockets'].values()))
                session['ready'].set()
            await asyncio.wait_for(session['ready'].wait(), 30)
            if session['done'].is_set():
                return
            peer = session['sockets']['viewer' if role == 'host' else 'host']
            total = 0
            async with asyncio.timeout(4 * 3600):
                async for message in socket:
                    if not isinstance(message, bytes) or not 0 < len(message) <= 65536:
                        raise ValueError('Invalid transport packet')
                    total += len(message)
                    if total > 16 * 1024 * 1024 * 1024:
                        raise ValueError('Session limit reached')
                    await peer.send(message)
        finally:
            await self.end_session(identifier, session)

def health(connection, request):
    if request.path == '/relay/health':
        body = b'Hyperlink relay ready\n'
        return Response(200, 'OK', Headers({'Content-Type': 'text/plain', 'Content-Length': str(len(body)), 'Cache-Control': 'no-store'}), body)

async def main():
    relay = Relay(os.environ.get('HYPERLINK_RELAY_DATA', str(Path.home() / 'hyperlink-relay-data')))
    async with serve(relay.handle, '127.0.0.1', int(os.environ.get('HYPERLINK_RELAY_PORT', '45841')), process_request=health, max_size=65536, max_queue=4, write_limit=65536, compression=None, open_timeout=5, close_timeout=2, ping_interval=15, ping_timeout=30):
        await asyncio.Future()

if __name__ == '__main__':
    asyncio.run(main())
