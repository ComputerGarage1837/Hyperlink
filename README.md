# Hyperlink

A portable Windows remote desktop draft for your own computers and a small, trusted family group. This is a replacement foundation, version **0.1.0**, not the security-audited finished product.

## Run the draft

Use Windows 11 x64 with .NET Framework 4.8. Extract the whole ZIP into a writable folder and open **Hyperlink.exe**. There is no installer, automatic startup, router configuration, or cloud signup.

1. Open Hyperlink on the computer you want to view. Click **Start hosting**.
2. Click **Create invitation**, choose its reachable LAN or private-VPN address, then **Generate and copy invitation**. Share it privately. It expires after five minutes and works once.
3. Open Hyperlink on your viewing computer. Click **Add a computer**, paste the invitation, and request pairing.
4. Approve the named computer **locally on the host**. Choose view-only or view-and-control rights.
5. On the viewer, click **Connect**. The host must approve this session locally. Click the picture to focus keyboard input. The viewer can disconnect; the host can immediately **STOP SESSION**.
6. Under **Access**, the host can revoke a paired computer. Revocation closes its current session and blocks future connections.

Hosting is off on every app launch. The app must remain open on the host, and its ordinary Windows desktop must be unlocked. If Windows prompts for a firewall rule, allow only the private network you intend to use. The default TCP port is **45831**; it can be changed while hosting is off. Do not expose this draft directly to the public internet. No firewall rule is created automatically.

The app displays the first available local network address, which may be a VPN or virtual adapter. Choose the address the viewer can reach. Paired computers retain their certificate identity when you edit a saved address. An offline computer produces a connection error; there is no fabricated online status.

## Implemented

- Native Windows host and viewer, live JPEG screen capture, fitted rendering, selected display, full-screen viewer, pointer, buttons, scroll, and keyboard input.
- Direct **TLS 1.2** with a pinned SHA-256 host certificate fingerprint. A changed, expired, or unexpected certificate is rejected; no insecure fallback is offered.
- A random 192-bit, single-use, expiring pairing invitation and explicit host approval.
- Persistent viewer RSA identities, signed random challenges bound to the host fingerprint and viewer identity, and host-enforced per-device grants. A device ID alone gives no access.
- View-only enforcement, one active viewer per host, local session stop, revocation, and held-key/button cleanup on disconnect or focus loss.
- Local identity, private key, and device metadata encrypted with Windows DPAPI for the current Windows account. Atomic settings saves.
- Received frames are decoded and presented in a bounded UI queue. The viewer shows observed, distinct **presented frames per second**, not just the requested capture rate.

## Draft boundaries

This engine uses Windows screen capture and JPEG with a **30 fps requested cap**, scaled to a maximum **1600 × 1000**. It does not establish sustained 30 fps on every machine and does not implement or prove 120 fps. Hardware encode/decode and the eventual native engine remain a measured engineering decision. The current engine is a functional first-draft prototype, not the final high-performance foundation.

The draft is attended, direct, and Windows-only. Invite-only user accounts, MFA, synchronized private family lists, relay/rendezvous, Android, a signed unattended service, secure-desktop/UAC/lock-screen control, audio, clipboard, file transfers, headless capture, signed updates, and the blueprint's later features are still planned. UAC and lock screens require local action; capture stops on protected desktops. App shutdown or a capture/transport error ends the session. Reconnection requires fresh local approval.

The EXE is **unsigned**. This build has not passed an independent security review or cross-machine field testing. Do not treat it as the trusted family release. See [the roadmap](docs/ROADMAP.md) for the remaining blueprint milestones.

The adjacent **Data/identity.dat** belongs to this Windows user and contains the app's protected identity and remembered computers. Keep the Data folder across upgrades. Copying it to a different Windows account will not transfer the identity; a second user must pair separately. Removing it resets the identity and requires new pairing. Invitations are private credentials; share them only with the intended recipient. Screens and keystrokes are never written to a diagnostic log by the app.

## Build and check

```powershell
.\build.ps1 -Test
.\build\Hyperlink.exe
```

The build uses Windows' .NET Framework C# compiler and framework libraries. It installs no packages. `-Test` runs 12 focused integration checks for TLS pinning, unauthorized and forged identities, pairing, encrypted frames, input permissions, active revocation, expiry, malformed packets, persistence, and stopping. Tests bind to loopback, generate a synthetic screen, and do not inject input into the user's desktop.

GitHub Actions builds the portable app and runs the same checks on Windows. Release artifacts must exclude the runtime Data folder and any private invitations. The repository's earlier app remains recoverable in Git history.
