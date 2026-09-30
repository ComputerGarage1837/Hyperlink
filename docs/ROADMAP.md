# Hyperlink implementation gates

Requirements come from the owner's Private Family Remote Desktop Blueprint. The owner chose existing hosting and `hyperlink.myfamilyapps.ca`, and waived Windows code signing for personal use. Other security requirements remain applicable.

## Implemented development slices

- Attended Windows screen/input prototype with TLS pinning, durable key proof, expiring invitation, local consent, view-only admission, revocation, input release and stop.
- PHP 8.4 account/device API with MFA, recovery, explicit viewer trust, DPoP, rotating refresh, owner-only granular grants, signed one-use tickets, thirty-second leases and independent revocation.
- Native account UI and Windows/PHP RSA proof interoperability.
- Legacy admission disabled after family host enrollment.
- Tested SQLite persistence, private keys/configuration outside web root, restore invalidation and thirty-day audit cleanup.

## Foundation gate — open

Pinned RustDesk evaluation: 1.4.9, commit `6c578292e8ebbbec708b76986ba8c4bc7c509747`. An initial audit found a client path returning success without an authenticated peer key. Patch and audit all alternate routes before adoption. Preserve licenses and publish required source.

Enforce family tickets and independent local owner policy inside engine admission. Bind both endpoint keys, prevent replay, and reject invalid keys or expired/revoked leases. Raw IDs, direct IP, LAN discovery, saved passwords, public rendezvous fallback and compatibility clients cannot provide alternative authorization. UI wrapping is insufficient.

## Genuine 120 fps gate — open

Replace JPEG with hardware accelerated capture, encoding, decoding and presentation. Begin with one SDR 1080p display and H.264. Count distinct moving frame identifiers through presentation.

Blueprint reference goals: thirty minutes averaging >=118 distinct presented fps, 1% low >=110 fps, changing-frame drops/repeats <1%. Wired LAN input-to-visible goals: median <=50 ms, p95 <=80 ms. Android also needs sixty-minute endurance. These remain unproved targets.

Adapt readable quality/resolution/bitrate first, then genuine 60 fps if necessary. Label emergency 30 fps or lower. Report actual stage limits and frame age. Qualify forced relay and real internet independently.

## Secure hosted connection gate — open

Deploy after the owner reports the domain ready. Verify live HTTPS, account isolation, proofs, private database permissions and restore behavior. MariaDB is optional and needs a live test before use.

Integrate the engine with the host's independently maintained owner-approved allowlist. Redeem tickets once, renew every five seconds, measure online revoke within ten seconds, and stop by thirty-second lease expiry during policy outage. Host enforcement remains unfinished.

Verify support for native rendezvous and opaque relay on hosting. PHP capability does not establish permission or reachability for TCP/UDP daemons. Preserve end-to-end encryption against a hostile relay.

## Installed Windows lifecycle gate — open

Implement a narrow privileged service and ordinary helper with authenticated local IPC. Test Windows 11 x64 login, lock, UAC, sign-out, user switching, reboot, sleep and emergency stop. Never disable UAC, save a Windows password or hide access. Unattended enrollment needs explicit owner permission and trusted viewers.

## Everyday family use gate — open

Build native Android viewing with OS-protected keys, gestures, modifiers, rotation/cutouts and honest sustained frame reporting. Android needs a stable APK signing identity, separate from purchased Windows code signing.

Add monitor switching/mixed DPI, directional text clipboard rights, safe bounded file transfer, authorized system audio, bounded reconnect and redacted diagnostics. Deny unauthorized use independently on every channel.

## Delivery and operations gate — open

Implement authenticated versioned update manifests, an offline release key separate from the account server, integrity checks, interruption recovery and security rollback policy. Windows executables remain unsigned by the owner's choice.

Run hostile-client/key/relay tests, fault injection, network/display matrix, twenty-four-hour host endurance, restore drill and family pilot. Attach evidence to a build/configuration. CI does not replace hardware, lifecycle or security review.

P2 choices remain explicit: printing, Wake-on-LAN, privacy mode, recording, advanced resume, portable host, iOS, ARM64, HDR and AV1. Headless mode needs a tested display source and a separate driver/hardware maintenance decision.
