# Changelog

## v0.2.0 — 2026-09-29

- **Windows can now connect to other devices too.** One app: it still hosts this PC from the
  tray, and now also opens the **device list** (double-click the tray icon, or start
  Hyperlink from the Start menu): your devices with Online / In use / Offline status, new PCs
  found on your network, and this PC's name and PIN.
- Each connection opens in **its own window**: all screens together or one at a time
  (View menu), choose which screens show, and **open any screen in its own window**.
  Full screen with Ctrl+Alt+Enter; Win key and Alt+Tab go to the remote PC; the remote
  pointer shape is used locally. GPU decoding on NVIDIA, AMD and Intel.
- **Works away from home with Tailscale.** Install Tailscale on your devices and connect once
  at home; after that each device remembers the others' permanent Tailscale addresses and
  connects from anywhere, trying your home network first. Online status works over Tailscale too.
- Android: connects over Tailscale the same way.

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
