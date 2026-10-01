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

### Local wake requests (0.5.2)
Windows and Android can send validated Wake-on-LAN packets through a selected local IPv4 broadcast network. The target must already support and enable Wake-on-LAN. Delivery does not prove wake-up; waking over the internet is not implemented.

The optional -Video build and --video-self-test command are experimental codec diagnostics. They are not connected to live remote sessions. The synthetic 1080p decode test achieved about 58 FPS, so 120 FPS remains unqualified. Default builds use the existing JPEG session path.


### Windows session recording (0.5.3)
The owner can grant recording independently of control, files, clipboard and audio. The Windows viewer's Session tools window can then save video to a new local MKV file. The host shows a recording notice. Recordings preserve arrival timestamps, finalize through a temporary file, never overwrite existing files, and stop at 2 GiB. Audio recording and Android recording are still pending. Revoking recording rights ends the active session.

### Local display privacy (0.5.4)
Windows and Android viewers can request local display privacy with a separate owner grant. On supported Windows 10 version 2004 or later, opaque capture-excluded covers hide local displays and allow pointer input through. Ctrl+Alt+Shift+H restores the displays and disconnects the viewer. Disconnect, display changes, session lock, window closure and a 30-minute safety timeout also restore displays. Active recording is shown on the covers. A small owned-window physical check passed for capture exclusion and emergency recovery; full multi-monitor use remains a pilot check. This does not add login-screen or UAC control.

### Optional system-audio recording (0.5.6)

Windows recording has an **Include system audio** checkbox. Android offers separate video-only and video-with-audio buttons. Sound requires the owner's audio grant as well as the recording grant. Stop playback before starting a recording with sound; one tool owns each audio capture. Recordings contain MJPEG video and 48 kHz stereo 16-bit PCM in a local MKV file. Microphone capture is not used.

Windows and Android fixtures pass stream-identity validation and decode independently with FFmpeg. Packet arrival times determine the timeline; physical-device sound, synchronization, storage speed, and prolonged sessions still need pilot testing.

### Android video recording (0.5.5)
The Android viewer can request the same separate recording grant, save MKV video privately on the phone, and export a verified copy to a selected local storage folder. Existing files are not overwritten. Clips stop at 1 GiB or if storage cannot keep up. The Recordings screen lists finalized clips and supports export or deletion. Audio recording is still pending. Java recording finalization, failure cleanup, no-overwrite and independent media playback/timing checks passed; physical Android recording and storage-provider use remain pilot checks.

Windows now finishes queued recordings before ordinary application exit, and the update window requires active recordings to stop before installation. Abrupt process termination is not a completed recording.
