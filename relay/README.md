# Private Hyperlink relay

The relay carries opaque end-to-end TLS traffic and assigns stable eight-digit computer IDs. Windows keeps the unattended PIN hash and independently grants access. Android requires no family account for this flow.

Run on Python 3.11 with `requirements.txt`. Bind only to loopback and reverse proxy `/relay/control` and `/relay/session` as WebSockets behind trusted HTTPS. Keep the SQLite database, enrollment key, owner tokens, process files and logs outside the public document root. `start.py` is an idempotent private launcher suitable for a one-minute account cron watchdog.

Initial enrollment is owner-provisioned. An offline RSA authority signs each computer identity, certificate fingerprint and assigned code. Both clients verify this signature before transmitting a PIN. The authority private key must never be deployed to the relay, committed, or packaged with either app. A new Windows installation requires owner provisioning; the public ZIP alone does not enroll another computer.

The current Windows host runs in the signed-in user session. Start-at-sign-in and tray hosting are supported. Windows login, UAC and restart require the pending Windows service implementation.

`python test_relay.py` checks registration ownership, short-code stability, duplex opaque transport, replay rejection, offline refusal and cleanup. The public-domain draft was separately checked with Windows and the Android Java transport using a synthetic desktop and a private fixture PIN.
