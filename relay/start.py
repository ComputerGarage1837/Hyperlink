"""Start only this account's relay, if its verified process is absent."""
import fcntl
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent
os.umask(0o077)
with (root / 'start.lock').open('a') as lock:
    fcntl.flock(lock, fcntl.LOCK_EX)
    pid_file = root / 'relay.pid'
    if pid_file.exists():
        try:
            pid = int(pid_file.read_text())
            command = Path('/proc') / str(pid) / 'cmdline'
            if str(root / 'relay.py').encode() in command.read_bytes().split(b'\0'):
                sys.exit(0)
        except (ValueError, FileNotFoundError, PermissionError):
            pass
    environment = {**os.environ, 'HYPERLINK_RELAY_DATA': str(root / 'data')}
    with (root / 'relay.log').open('ab') as log:
        process = subprocess.Popen([str(root / 'venv/bin/python'), str(root / 'relay.py')], stdin=subprocess.DEVNULL, stdout=log, stderr=log, env=environment, start_new_session=True)
    pid_file.write_text(str(process.pid))
    print('Hyperlink relay started')
