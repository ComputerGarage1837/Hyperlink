# Hyperlink: blueprint milestones

The target is a private, invite-only remote desktop for family use, beginning with Windows computers and an Android viewer. Smoothness takes priority; automatic mode eventually selects genuine 120 fps on eligible end-to-end paths and 60 fps otherwise. Requested encoder/capture rates are not evidence of presented performance.

## First draft: delivered foundations

The 0.1 draft demonstrates a real attended Windows connection, encrypted direct transport, certificate pinning, viewer identity proofs, per-device grants, local approval, view-only enforcement, live screen rendering, control, local stop, revocation, and persistent protected identities. It intentionally provides a truthful, lower-performance prototype while the final native engine is evaluated. Its device grants are not family accounts.

## Gate 1: security and performance engine decision

Audit a pinned RustDesk revision as the blueprint's first candidate. Reject insecure-continuation paths for direct, relayed, input, and auxiliary channels; verify authorization against modified clients. Compare alternative native engines if the audit or measured 120 fps gate fails. The standalone JPEG prototype can validate workflows but cannot satisfy this gate.

Measure requested, captured, encoded, decoded, and distinct presented frames, frame intervals, input latency, and sustained behavior on qualifying hardware, monitors, codecs, and networks. Record explicit pass/fail and 60 fps fallback evidence. Never use duplicated frames or a setting labeled 120 as proof.

## Gate 2: identity and private control service

Implement invite-only user accounts with strong authentication, MFA and recovery. Bind devices to owners through explicit enrollment and persistent device keys. Own and explicitly shared devices are the only devices an account may enumerate or connect to. Provide named, scoped, expiring grants and immediately effective revocation on new and existing sessions. Administrators and routing IDs cannot override owner authorization.

Separate control API, identity, rendezvous, opaque encrypted relay, native media, and permission-checked input/data channels. Use short-lived, key-bound session authorizations. Fail closed during account/service failure and reconnect. Store metadata-only audit events with redacted diagnostics; never log screen contents, keystrokes, or clipboard data.

## Gate 3: trusted Windows core (blueprint R01–R14)

Finish and validate a signed Windows service plus visible per-user helper, unattended restart/pre-login behavior within supported Windows limits, physical-console capture/input, UAC/secure-desktop/lock/sign-out behavior through supported service mechanisms, local session visibility and emergency stop, invalid-package rejection, signed update rollback, and safe held-input recovery. Do not weaken Windows security. Complete hostile-client authorization, reconnect, transport downgrade, update interruption, grant revocation, and fault tests before trusted family use.

## Gate 4: complete everyday release (R15–R23)

Ship Android viewing with hardware decode, touchpad and direct-touch input, zoom, keyboard/modifiers, and the same account/grant rules. Complete multi-monitor/mixed-DPI/rotation/hot-plug coordinates, separately permitted clipboard directions, safe files/folders with progress and cancellation, optional encrypted system audio, bounded reconnect, direct/relay status, automatic 120/60 and visible emergency lower rates, validated headless display support, redacted diagnostics, and maintenance tools.

## Gate 5: explicitly decided options (R24–R30)

Keep remote printing, validated Wake-on-LAN, privacy mode with emergency recovery, explicitly consented visible encrypted recording, transfer resume, portable attended support, iOS, Windows ARM64, HDR, AV1 and advanced simultaneous displays on the decision list. Include or defer each with a stated reason; do not silently drop them. None enters the first trusted pilot accidentally.

## Current compatibility and release evidence

Baseline: Windows 11 x64 with .NET Framework 4.8, ordinary unlocked desktop, one approved viewer per host, direct LAN/private VPN. Windows 10, Windows ARM64, Android and iOS are not validated by this draft. The app is unsigned and requires an open host window. No public signup, billing, marketplace, hidden recording, remote shell, or stealth access is planned.

Local automated tests and a visual launch are draft evidence. Cross-machine Windows control, monitor/DPI combinations, real network faults, accessibility, phone use, signed packages, service behavior, adversarial security review, and hardware performance remain release gates. An EXE compiling successfully is not evidence that those gates passed.
