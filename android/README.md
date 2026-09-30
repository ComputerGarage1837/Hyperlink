# Hyperlink Android viewer

Native Java / Android framework viewer, Android 8 or later. No third-party runtime dependencies.

Build with JDK 21 and Android SDK platform 36 plus build-tools 36.0.0:

```powershell
./android/build.ps1 -Sdk C:/path/to/android-sdk -Jdk C:/path/to/jdk -Output C:/path/to/build
```

The build creates an unsigned APK. Installation requires APK signing with your private personal key. Keep that key outside source control and release archives; subsequent updates require the same key. A purchased Windows signing certificate is unrelated to APK signing.

`./android/test.ps1 -Jdk C:/path/to/jdk` verifies the shipped signing and TLS implementations against a synthetic loopback Windows host. It checks certificate pin rejection, RSA pairing, JPEG decoding, Unicode input and host rejection of text from a read-only client. No desktop input is injected. CI runs these checks independently of an Android emulator. Full APK launch, Android Keystore persistence, touch controls and rotation still require emulator or physical-device verification.

The attended viewer pairs using a five-minute invitation, pins the Windows host certificate, and proves possession of a nonexportable Android Keystore RSA key. Every session needs host approval. Input rights are checked by the host. The account API uses its fixed HTTPS origin, normal platform CA validation, DPoP and explicit viewer trust. Remembered refresh credentials are encrypted with an Android Keystore AES key; passwords and MFA codes are not saved. Backups are disabled. Desktop frames are not persisted.

This is the Windows draft's capped 30 fps JPEG transport. It does not implement the planned hardware 120 fps engine, hosted desktop tickets, unattended access, files, clipboard, audio or printing. Family account devices are listed without a connect action until host-side family policy enforcement is ready. Unicode text requires the updated Windows host; older hosts retain view/touch support.

Touchpad/direct touch, long-press drag, two-finger scroll, pinch zoom, right-click, keyboard and modifier buttons are available. Backgrounding closes the session; reconnect requires approval again. The viewer blocks screenshots to protect desktop content. Do not disable host approval or certificate pinning to work around a connection failure.
