# Hyperlink 0.6.0

Private family remote desktop for Windows and Android. This is a development draft.

On the provisioned Windows computer, open **Unattended access**, choose a 6–12 digit PIN, and enable hosting. Enter its eight-digit computer ID and PIN in Android. No family account or invitation link is required. Saved devices reconnect using their device keys; Android does not save the PIN.

The private HTTPS/WebSocket relay is `hyperlink.myfamilyapps.ca`. It forwards opaque, pinned end-to-end TLS traffic. An offline authority signs each Windows identity, certificate fingerprint and computer code. Both viewers verify this before sending the PIN. Windows stores its PIN hash and relay credentials in user-protected local data. The relay has no PIN or authority private key.

**Windows must remain signed in.** Optional start-at-sign-in and tray hosting support that user session. Login, UAC and restart require the pending Windows service. Initial enrollment is owner-provisioned; the ZIP alone does not enroll another computer. See [relay deployment notes](relay/README.md).

Available features include desktop viewing/input, independent owner grants and revocation, local-folder file transfers with digest checks and no overwrite, text clipboard, system audio, MJPEG/optional PCM recordings, Android system-picker export, Windows privacy cover/emergency restore, local Wake-on-LAN, signed Windows updates and personally signed APKs. Long invitations and family accounts are advanced functionality; their separate account-ticket transport remains closed.

The shipped JPEG capture is capped at 30 fps. Native DXGI/NVENC remains an experimental engine. Windows service lifecycle, printer redirection, native-video integration, 120 fps qualification and physical-device performance/audio tests remain pending.

## Build and checks

Windows: .NET Framework 4.8; `./build.ps1 -Test`.

Android: JDK 21, Android SDK/build tools 36; `./android/build.ps1 -Sdk <sdk> -Jdk <jdk>` and `./android/test.ps1 -Jdk <jdk>`. Sign with the same private personal key as the installed APK. Minimum Android is API 26.

Relay: Python 3.11; install `relay/requirements.txt`, then `python relay/test_relay.py`.

Dependencies are version/digest pinned. Keep all enrollment, provisioning and signing secrets outside public artifacts and source control. Java-WebSocket and SLF4J licenses are included in Android APK assets.

0.6.0 passed Windows focused checks, existing Android/Windows encrypted protocol checks, relay ownership/replay/cleanup checks, and a public-domain test of Windows and Android's Java relay/TLS code pairing by PIN, reconnecting and receiving a synthetic JPEG. Physical-phone UI, gestures, rotation and audio still need device testing. These checks do not qualify Windows login/UAC or native-video performance.