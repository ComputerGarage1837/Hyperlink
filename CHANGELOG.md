# Changelog

## v0.1.1 — 2026-09-29

- **Pinch to zoom** any screen (up to 5×) and drag with two fingers while pinching to move
  around. When zoomed, the PC sends full resolution so text stays sharp, and the view follows the
  pointer.
- **Show/hide screens**: choose which of the PC's monitors appear (remembered per device).
- **Switch screens faster**: two-finger double-tap toggles between all screens and the one under
  your fingers. The menu shows for a few seconds when you connect.
- Turns to landscape while connected (can be switched off in Settings).
- Devices are found by their permanent id instead of their IP address, so router changes and new
  addresses don't matter. The address field is now an optional fallback for connecting from
  elsewhere (e.g. a Tailscale name).

## v0.1.0 — 2026-09-29

First version.

- **Hyperlink Host for Windows**: streams every monitor with hardware encoding (NVIDIA NVENC,
  AMD AMF or Intel Quick Sync; HEVC, H.264 or AV1) at up to 144 fps, low-latency tuned.
  Tray icon, PC name and PIN in Settings, several clients at once, in-app updates from GitHub.
- **Hyperlink for Android**: saved devices with your own names and PINs and live
  online / in-use status, hosts found automatically on your network, all screens together or one at
  a time, each connection or screen in its own window, touchpad and touch-screen mouse modes,
  keyboard with shortcut keys, performance stats, in-app updates from GitHub.
- Custom UDP video transport with forward error correction and instant keyframe recovery.
