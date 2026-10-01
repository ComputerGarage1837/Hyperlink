# Hyperlink

Private family remote desktop for Windows and Android. Current version: **0.5.1 development draft**. The owner authorized replacing the original prototype.

0.3 adds a native Android attended viewer and Unicode keyboard input on Windows. The APK builds and its personal signature verifies. The shipped Java TLS/RSA implementation passes synthetic Windows host interoperability checks, including rejection of a wrong certificate pin and malicious read-only text input. Android-generated DPoP proofs pass the PHP controller's checks. Android launch, Keystore persistence, gestures and rotation remain unverified: the local software emulator has not booted. See [Android build and test instructions](android/README.md).

## Windows draft

Build with Windows PowerShell and .NET Framework 4.8: `./build.ps1 -Test`, then run `./build/Hyperlink.exe`. No extra runtime packages are needed. Identity and remembered-login data use Windows DPAPI. Keep `Data/` private; never publish it or copy host identities.

The attended local draft provides expiring single-use invitations, exact TLS certificate pinning, durable key proof, local session approval, view-only enforcement, selected-monitor capture, pointer/keyboard input, revocation, held-input release and a visible stop button. Its temporary JPEG path is capped at 30 fps.

The Family account page adds native HTTPS/DPoP login, explicit trusted-viewer approval, authenticator enrollment, recovery, optional protected remembered login, host enrollment, seven-day named view/control shares and login/viewer revocation. The owner is preparing `hyperlink.myfamilyapps.ca`; deployment is pending. Revoked viewer keys cannot re-enroll through ordinary login.

**Family-enrolled hosts disable the temporary direct-pairing route.** Hosted transport stays closed until host-side ticket, lease and independent owner-policy enforcement is integrated. Enrollment does not enable unattended control.

Windows executables are unsigned by the owner's personal-use choice. No purchased Windows signing certificate is required. API TLS certificates, authenticated transport keys and authenticated update manifests are separate requirements.

## Hosted API

See [server/README.md](server/README.md) for PHP 8.4 safeguards, tests, private-data placement and restore handling. SQLite is the tested initial database. The SQL layout permits PDO MariaDB configuration, but live MariaDB compatibility is untested. GitHub holds source, CI and downloads; it is not the backend.

## Verification and remaining work

The Windows self-test covers real loopback authentication, pairing replay, certificate mismatch, encrypted synthetic frames, hostile view-only input, revocation, persistence, shutdown and legacy-admission rejection on enrolled hosts. `--screen-test` captures the real desktop without input injection. `--viewer-smoke` exercises the real viewer form with an isolated synthetic host.

The PHP suite tests account isolation, MFA/request replay, key-bound login/refresh, explicit viewer trust, independent revocation, scoped tickets, expired leases, recovery, rate limits, host-key binding and administrator boundaries. The HTTP suite exercises the real API entry point. A native Windows RSA proof has also been verified by the PHP JWT library.

The high-performance engine, private relay, Windows service/UAC/reboot lifecycle, Android viewer, clipboard/files/audio, display compatibility and update chain remain unfinished. These checks do not qualify 120 fps or unattended operation. See [docs/ROADMAP.md](docs/ROADMAP.md).

### Windows session tools (0.4 development checkpoint)

Paired viewers can use file transfer, directional plain-text clipboard exchange, and system playback audio when the computer owner explicitly grants each permission. Open Access > Permissions and shared folder on the host, then Session tools in the viewer. File transfer uses an owner-selected local folder, validates SHA-256, publishes complete files atomically, and never overwrites existing files. The draft limits individual files to 1 GiB and text to 1,024 characters. Clipboard exchange is manual; there is no initial synchronization. Audio uses the default playback device, never a microphone or camera.

The Windows package now includes NAudio.Core.dll, NAudio.Wasapi.dll, and their MIT license notice. Keep these beside Hyperlink.exe. Automated checks cover encrypted transfers, cancellation, independent permissions, directional clipboard exchange, and synthetic audio with live revocation. Real audio streaming and playback still require end-to-end verification. Android session tools are included in the signed 0.4 APK, with encrypted Java/Windows file, clipboard, and synthetic-audio protocol checks. Android document-picker, lifecycle, and actual playback testing remain unverified. Unattended access, hardware encoding, live family account session authorization, internet relay, remain unfinished; verified update support is described below. The default draft stream remains capped at 30 FPS.

### Verified updates (0.5.1 development checkpoint)

About this draft opens the verified update screen. Local .hup packages use a separate offline RSA-3072 release key. The installer verifies the signed inventory and every file hash, rejects unsafe entries and older release counters, backs up public runtime files, waits for the running app to close, and checks that the new app starts. A failed or interrupted replacement restores the prior files; identity Data is outside the update inventory. The highest applied counter is stored separately from Data in the current user's Hyperlink registry key. Do not delete the offline private release key or an update recovery folder while a transaction is pending.

Optional daily update checks and installation when no session is active are available, both off by default. HTTPS delivery uses the operating system's certificate checks and rejects redirects; release authenticity uses the independent offline key. Public release-feed deployment and live delivery testing remain pending. Local installation/restart, failed-startup rollback, tampering, downgrade refusal, and identity preservation have been tested. The latest controller package remains unchanged.

Hardware H.264 encoding and GPU capture were probed successfully on generated data and a discarded frame. Live H.264 transport, true 120 FPS qualification, unattended service/UAC handling, printing, privacy mode, recording, Wake-on-LAN, and real Android device validation remain unfinished. The shipped live stream is still the 30 FPS JPEG draft.
