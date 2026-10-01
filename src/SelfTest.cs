using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Net;
using System.Reflection;
using System.Threading;

namespace Hyperlink
{
    static class SelfTest
    {
        static readonly List<string> results = new List<string>();
        static void Check(bool value, string message) { if (!value) throw new Exception(message); }
        static void Pass(string message) { results.Add("PASS  " + message); }
        static void Wait(Func<bool> condition, string message)
        { for (int i = 0; i < 100; i++) { if (condition()) return; Thread.Sleep(50); } throw new Exception("Timed out: " + message); }
        static void Reject(Action action, string message)
        { bool rejected = false; try { action(); } catch (Exception) { rejected = true; } Check(rejected, message); }
        static Device Target(Store host, int port)
        { return new Device { Address = "127.0.0.1", Port = port, Fingerprint = host.Fingerprint, Id = host.Id, Name = "Test host" }; }
        static Wire Authorized(Store viewer, Store host, int port)
        {
            var wire = Wire.Connect("127.0.0.1", port, host.Fingerprint);
            wire.SendJson(new { version = 1, operation = "connect", id = viewer.Id });
            var challenge = wire.ReadJson();
            wire.SendJson(new { signature = viewer.Sign(Util.Transcript(Wire.Text(challenge, "nonce"), host.Fingerprint, viewer.Id)) });
            var accepted = wire.ReadJson(); Check(Wire.Text(accepted, "kind") == "accepted", "Expected authorized session.");
            return wire;
        }
        public static int ScreenCheck(string report, string image)
        {
            string root = Path.Combine(Path.GetTempPath(), "Hyperlink-screen-" + Guid.NewGuid().ToString("N"));
            string result; int code = 0;
            try
            {
                using (var state = new Store(Path.Combine(root, "host")))
                using (var viewer = new Store(Path.Combine(root, "viewer")))
                using (var host = new Host(state, delegate { return 1; }, delegate { }))
                using (var received = new ManualResetEvent(false))
                using (var remote = new Remote(viewer))
                {
                    // Isolated loopback-only fixture. Production hosts always use their UI approval callback.
                    host.Start(IPAddress.Loopback, 0);
                    Device device; using (var pair = new Remote(viewer)) device = pair.Pair(host.Invite("127.0.0.1"));
                    int saved = 0, frames = 0;
                    remote.Frame = delegate(Bitmap b)
                    {
                        try { if (Interlocked.Exchange(ref saved, 1) == 0) b.Save(image, System.Drawing.Imaging.ImageFormat.Png); Interlocked.Increment(ref frames); received.Set(); }
                        finally { b.Dispose(); }
                    };
                    remote.Connect(device); Check(received.WaitOne(7000), "Live desktop frame was not delivered.");
                    Thread.Sleep(1200); Check(frames > 1, "The live stream did not continue.");
                    Check(!remote.Control, "Live screen test should be view-only.");
                    result = "PASS  Actual desktop captured, JPEG-encoded, transported over pinned TLS, decoded and streamed: " + frames + " frames. No input injected.";
                }
            }
            catch (Exception ex) { result = "FAIL  " + ex.ToString(); code = 1; }
            finally { try { if (Directory.Exists(root)) Directory.Delete(root, true); } catch { } }
            File.WriteAllText(report, result); return code;
        }
        public static int ViewerCheck(string report, string image)
        {
            string root = Path.Combine(Path.GetTempPath(), "Hyperlink-viewer-" + Guid.NewGuid().ToString("N"));
            string result = "FAIL  Viewer did not present a frame."; int code = 1;
            try
            {
                using (var state = new Store(Path.Combine(root, "host")))
                using (var viewer = new Store(Path.Combine(root, "viewer")))
                using (var host = new Host(state, delegate { return 2; }, delegate { }, true))
                {
                    host.Start(IPAddress.Loopback, 0);
                    Device device; using (var pair = new Remote(viewer)) device = pair.Pair(host.Invite("127.0.0.1")); device.Name = "Loopback test screen";
                    using (var window = new Viewer(viewer, device))
                    using (var timer = new System.Windows.Forms.Timer { Interval = 3000 })
                    {
                        timer.Tick += delegate
                        {
                            timer.Stop();
                            using (var b = new Bitmap(window.Width, window.Height)) { window.DrawToBitmap(b, new Rectangle(Point.Empty, window.Size)); b.Save(image, System.Drawing.Imaging.ImageFormat.Png); }
                            if (window.HasPresentedFrame)
                            {
                                result = "PASS  Native viewer authenticated, decoded and rendered a live loopback frame. Synthetic screen; no desktop input injected."; code = 0;
                            }
                            else result += " State: " + window.ConnectionState;
                            window.Close();
                        };
                        window.Shown += delegate { timer.Start(); };
                        System.Windows.Forms.Application.Run(window);
                    }
                }
            }
            catch (Exception ex) { result = "FAIL  " + ex.ToString(); }
            finally { try { if (Directory.Exists(root)) Directory.Delete(root, true); } catch { } }
            File.WriteAllText(report, result); return code;
        }
        public static int Run(string report)
        {
            string root = Path.Combine(Path.GetTempPath(), "Hyperlink-test-" + Guid.NewGuid().ToString("N"));
            int exit = 0;
            Host testedHost = null;
            try
            {
                int extensionChecks = ExtensionTests.Run(Path.Combine(root, "extensions"));
                results.Add(UpdateTests.Run(Path.Combine(root, "updates")));
                results.Add(WakeTests.Run());
                results.Add(RecordingTests.Run(Path.Combine(root, "recordings")));
                results.Add(extensionChecks + " session tool checks passed, including encrypted transfer and live revocation.");
                var textInput = new InputController(true);
                textInput.Apply(new Dictionary<string, object> { { "type", "text" }, { "text", "Family \u00e9 \ud83d\ude00" } }, null);
                Check(textInput.Applied == 1 && textInput.Held == 0, "Unicode input did not complete safely.");
                foreach (string invalid in new[] { "", new string('x', 1025), "a\0b", "\ud800", "\udc00" })
                {
                    bool denied = false;
                    try { textInput.Apply(new Dictionary<string, object> { { "type", "text" }, { "text", invalid } }, null); }
                    catch { denied = true; }
                    Check(denied && textInput.Held == 0, "Malformed text input was accepted.");
                }
                using (var hostStore = new Store(Path.Combine(root, "host")))
                using (var viewer = new Store(Path.Combine(root, "viewer")))
                using (var readOnly = new Store(Path.Combine(root, "readonly")))
                using (var stranger = new Store(Path.Combine(root, "stranger")))
                {
                    viewer.Data.Name = "Trusted viewer"; readOnly.Data.Name = "Read-only viewer";
                    int approvals = 0;
                    using (var host = new Host(hostStore, delegate(string name, bool pair) { Interlocked.Increment(ref approvals); return name == "Read-only viewer" ? 1 : 2; }, delegate { }, true))
                    {
                        testedHost = host;
                        host.Start(IPAddress.Loopback, 0);
                        Reject(delegate { using (var c = Wire.Connect("127.0.0.1", host.Port, new string('0', 64))) { } }, "Unpinned server certificate accepted.");
                        Pass("TLS refuses a certificate fingerprint mismatch.");
                        Reject(delegate { using (var remote = new Remote(stranger)) remote.Connect(Target(hostStore, host.Port)); }, "Unknown identity accepted.");
                        Check(approvals == 0 && !host.Active, "Unknown client reached local approval or capture.");
                        Pass("Unknown identities cannot enumerate or start a session.");
                        var invite = host.Invite("127.0.0.1");
                        var decoded = Invitation.Decode(invite.Encode()); Check(decoded.Fingerprint == hostStore.Fingerprint, "Invitation pin changed.");
                        Device device;
                        using (var remote = new Remote(viewer)) device = remote.Pair(decoded);
                        Check(approvals == 1 && hostStore.Data.Peers.Count == 1, "Pairing was not locally approved.");
                        Reject(delegate { using (var remote = new Remote(stranger)) remote.Pair(decoded); }, "Reused invitation accepted.");
                        Pass("Pairing proves the viewer key, requires local approval, and consumes the invite.");
                        using (var hostile = Wire.Connect("127.0.0.1", host.Port, hostStore.Fingerprint))
                        {
                            hostile.SendJson(new { version = 1, operation = "connect", id = viewer.Id }); hostile.ReadJson();
                            hostile.SendJson(new { signature = stranger.Sign(Util.Transcript("wrong challenge", hostStore.Fingerprint, viewer.Id)) });
                            Check(Wire.Text(hostile.ReadJson(), "kind") == "error", "Forged signature accepted.");
                        }
                        Check(approvals == 1, "Forged identity reached local approval.");
                        Pass("A modified client cannot impersonate a paired viewer.");
                        using (var remote = new Remote(viewer))
                        using (var firstFrame = new ManualResetEvent(false))
                        {
                            int width = 0, height = 0;
                            remote.Frame = delegate(Bitmap b) { width = b.Width; height = b.Height; b.Dispose(); firstFrame.Set(); };
                            remote.Connect(device); Check(firstFrame.WaitOne(5000), "No real encoded/decoded frame arrived.");
                            Check(width == 160 && height == 100 && remote.Control, "Invalid frame dimensions or rights.");
                            remote.Send(new { kind = "input", type = "key", key = 0x10, down = 1 }); Wait(delegate { return host.Input.Held == 1; }, "held key");
                            Pass("An encrypted loopback session delivers JPEG frames and authorized input.");
                            var timer = System.Diagnostics.Stopwatch.StartNew(); host.Revoke(viewer.Id);
                            Wait(delegate { return !remote.Connected && host.Input.Held == 0 && !host.Active; }, "revocation cleanup");
                            Check(timer.ElapsedMilliseconds < 2000, "Revocation was too slow.");
                            Pass("Revocation closes the active session within two seconds and releases held input.");
                        }
                        Reject(delegate { using (var remote = new Remote(viewer)) remote.Connect(device); }, "Revoked viewer reconnected.");
                        Pass("Revoked identities cannot reconnect.");
                        var viewInvite = host.Invite("127.0.0.1"); using (var remote = new Remote(readOnly)) remote.Pair(viewInvite);
                        int before = host.Input.Applied;
                        using (var hostile = Authorized(readOnly, hostStore, host.Port))
                        {
                            hostile.SendJson(new { kind = "input", type = "key", key = 65, down = 1 });
                            Wait(delegate { return !host.Active; }, "view-only input rejection");
                        }
                        Check(host.Input.Applied == before && host.Input.Held == 0, "View-only client injected input.");
                        Pass("View-only rights are enforced on the host against a modified client.");
                        var expired = host.Invite("127.0.0.1");
                        typeof(Host).GetField("inviteExpires", BindingFlags.Instance | BindingFlags.NonPublic).SetValue(host, DateTime.UtcNow.AddSeconds(-1).Ticks);
                        Reject(delegate { using (var remote = new Remote(stranger)) remote.Pair(expired); }, "Expired host invitation accepted.");
                        Pass("Host rejects expired invitations even if the client bypasses its expiry UI.");
                        using (var malformed = Wire.Connect("127.0.0.1", host.Port, hostStore.Fingerprint))
                        {
                            malformed.Stream.Write(new byte[] { 1, 0x7F, 0xFF, 0xFF, 0xFF }, 0, 5);
                            Check(Wire.Text(malformed.ReadJson(), "kind") == "error", "Oversized message accepted.");
                        }
                        Pass("Oversized control packets are rejected before allocating their payload.");
                        using (var persisted = new Store(Path.Combine(root, "host")))
                        { Check(persisted.Fingerprint == hostStore.Fingerprint && persisted.Id == hostStore.Id, "Identity did not survive reload."); Check(!persisted.Data.Peers.Exists(p => p.Id == viewer.Id), "Revocation was not persisted."); }
                        Pass("Windows-protected identity and revocation survive an ordinary reload.");
                        host.Stop(); Check(!host.Running && host.Input.Held == 0, "Host stop failed.");
                        Pass("Stopping hosting closes admission and clears held input.");
                    }
                }
                using (var enrolled = new Store(Path.Combine(root, "family-enrolled")))
                using (var guarded = new Host(enrolled, delegate(string name, bool pair) { return 2; }, delegate { }, true))
                {
                    enrolled.Data.FamilyDeviceId = "test-enrolled-device";
                    bool denied = false;
                    try { guarded.Start(IPAddress.Loopback, 0); } catch (InvalidOperationException) { denied = true; }
                    Check(denied, "Family-enrolled host must reject legacy admission.");
                    Pass("Family-enrolled host rejects legacy direct pairing.");
                }
                results.Add("13 checks passed. Network tests use loopback and a synthetic screen; no desktop input is injected.");
            }
            catch (Exception ex) { exit = 1; results.Add("FAIL  " + ex.ToString()); if (testedHost != null && testedHost.LastFailure != null) results.Add("HOST  " + testedHost.LastFailure.ToString()); }
            finally
            {
                try { if (Directory.Exists(root)) Directory.Delete(root, true); } catch (Exception ex) { results.Add("Temporary test cleanup: " + ex.Message); }
                string text = String.Join(Environment.NewLine, results);
                if (report != null) File.WriteAllText(report, text); Console.WriteLine(text);
            }
            return exit;
        }
    }
}
