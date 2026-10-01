# Hyperlink Android 0.6.0

Enter the eight-digit computer ID and the PIN chosen in Windows **Unattended access**. No family account is required. Saved computers reconnect using the phone's device key. Long invitations and family-account controls are under Advanced.

Build with JDK 21 and Android SDK/build tools 36 using `./android/build.ps1 -Sdk <sdk-directory> -Jdk <jdk-directory>`. Run `./android/test.ps1 -Jdk <jdk-directory>`. Sign using the same private personal key as the installed APK. Minimum Android is API 26.

Java-WebSocket 1.6.0 and SLF4J 2.0.17 are digest-pinned; licenses are in APK assets. Outer HTTPS validates the relay domain. Native JSSE TLS 1.2 over that relay pins the Windows certificate. An offline-authority attestation is verified before PIN transmission. The PIN is cleared from its input and is not persisted.

Windows must remain signed in. The Java relay implementation passed public-domain PIN pairing, saved reconnect and synthetic JPEG delivery. Physical-phone UI, gestures, rotation, audio and recordings still need device testing. See the root README for current limitations.