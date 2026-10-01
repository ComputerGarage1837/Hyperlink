using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Security.Cryptography;
using System.Threading;
using System.Web.Script.Serialization;

namespace Hyperlink
{
    static class ExtensionTests
    {
        static int checks;
        static void Check(bool condition, string reason) { if (!condition) throw new Exception(reason); checks++; }
        static void Denied(Action action, string reason) { bool denied = false; try { action(); } catch { denied = true; } Check(denied, reason); }
        static Dictionary<string, object> Value(object value) { var json = new JavaScriptSerializer(); return (Dictionary<string, object>)json.DeserializeObject(json.Serialize(value)); }
        static string Hash(byte[] bytes) { using (var sha = SHA256.Create()) return BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-", "").ToLowerInvariant(); }
        static void Wait(Func<bool> condition) { DateTime deadline = DateTime.UtcNow.AddSeconds(8); while (!condition()) { if (DateTime.UtcNow > deadline) throw new TimeoutException("Extension session did not start."); Thread.Sleep(20); } }
        public static int Run(string root)
        {
            checks = 0; Directory.CreateDirectory(root); string folder = Path.Combine(root, "shared"); Directory.CreateDirectory(folder);
            byte[] bytes = Enumerable.Range(0, 23017).Select(i => (byte)(i % 251)).ToArray();
            using (var files = new FileTransfers(folder))
            {
                foreach (string name in new[] { "../escape", "a/b", "a\\b", "file.", "file ", "CON", "NUL.txt", "COM1", "COM\u00b9", "x:y", ".hyperlink-private" })
                    Denied(delegate { files.BeginUpload(name, 0, Hash(new byte[0])); }, "Unsafe upload path accepted.");
                Denied(delegate { files.BeginUpload("large.bin", 1024L * 1024 * 1024 + 1, Hash(bytes)); }, "Oversized file accepted.");
                string id = Wire.Text(Value(files.BeginUpload("sample.bin", bytes.Length, Hash(bytes))), "id");
                Check(!File.Exists(Path.Combine(folder, "sample.bin")), "Partial upload was published.");
                Denied(delegate { files.Write(id, 1, Convert.ToBase64String(new byte[] { 1 })); }, "Out-of-order chunk accepted.");
                Denied(delegate { files.Commit(id); }, "Incomplete file published.");
                for (int offset = 0; offset < bytes.Length; offset += 8192) files.Write(id, offset, Convert.ToBase64String(bytes, offset, Math.Min(8192, bytes.Length - offset)));
                files.Commit(id); Check(File.ReadAllBytes(Path.Combine(folder, "sample.bin")).SequenceEqual(bytes), "Uploaded file differs.");
                Denied(delegate { files.BeginUpload("sample.bin", 0, Hash(new byte[0])); }, "Existing file overwritten.");
                id = Wire.Text(Value(files.BeginDownload("sample.bin")), "id");
                var downloaded = new MemoryStream(); long position = 0;
                while (true)
                {
                    var reply = Value(files.Read(id, position)); byte[] chunk = Convert.FromBase64String(Wire.Text(reply, "data")); downloaded.Write(chunk, 0, chunk.Length); position += chunk.Length;
                    if ((bool)reply["complete"]) { Check(Wire.Text(reply, "sha256") == Hash(bytes), "Download hash mismatch."); break; }
                }
                Check(downloaded.ToArray().SequenceEqual(bytes), "Downloaded bytes differ."); downloaded.Dispose();
                id = Wire.Text(Value(files.BeginUpload("empty.bin", 0, Hash(new byte[0]))), "id"); files.Commit(id);
                id = Wire.Text(Value(files.BeginDownload("empty.bin")), "id"); var empty = Value(files.Read(id, 0)); Check((bool)empty["complete"] && Wire.Text(empty, "sha256") == Hash(new byte[0]), "Empty-file transfer failed.");
                id = Wire.Text(Value(files.BeginUpload("corrupt.bin", 1, Hash(new byte[] { 2 }))), "id"); files.Write(id, 0, Convert.ToBase64String(new byte[] { 1 }));
                Denied(delegate { files.Commit(id); }, "Corrupted file published."); Check(!File.Exists(Path.Combine(folder, "corrupt.bin")), "Corrupted destination exists.");
                files.BeginUpload("cancel.bin", 1, Hash(new byte[] { 1 }));
            }
            Check(!Directory.EnumerateFiles(folder, ".hyperlink-*").Any(), "Cancelled partial upload remains.");
            SessionExtensions.CheckText("Family \u00e9 \ud83d\ude00"); checks++;
            foreach (string text in new[] { new string('x', 1025), "a\0b", "\ud800", "\udc00" }) Denied(delegate { SessionExtensions.CheckText(text); }, "Invalid clipboard text accepted.");
            var peer = new Peer { FileRead = true }; Check(SessionExtensions.Allowed(peer, "file-list") && !SessionExtensions.Allowed(peer, "clipboard-read") && !SessionExtensions.Allowed(peer, "file-upload-begin"), "File permission implies another right.");
            string hostNotice = "";
            using (var owner = new Store(Path.Combine(root, "owner")))
            using (var viewer = new Store(Path.Combine(root, "viewer")))
            using (var host = new Host(owner, delegate(string name, bool pair) { return 1; }, delegate(string message) { Volatile.Write(ref hostNotice, message); }, true))
            using (var remote = new Remote(viewer))
            {
                remote.Frame = delegate(System.Drawing.Bitmap image) { image.Dispose(); };
                host.Start(IPAddress.Loopback, 0); var device = remote.Pair(host.Invite("127.0.0.1"));
                lock (owner.Sync) { owner.Data.SharedFolder = folder; owner.Data.Peers[0].FileRead = true; owner.Data.Peers[0].FileWrite = true; owner.Save(); }
                remote.Connect(device); Wait(delegate { return remote.SupportsExtensions; }); Check(!remote.Control, "Extension test unexpectedly has input control.");
                using (var client = new ExtensionClient(remote))
                {
                    var listed = client.Call("file-list", "offset", 0); Check(((object[])listed["files"]).Length == 2, "View-only file list failed.");
                    Denied(delegate { client.Call("clipboard-read"); }, "File permission exposed host clipboard.");
                    string id = Wire.Text(client.Call("file-upload-begin", "name", "network.bin", "size", 1L, "sha256", Hash(new byte[] { 7 })), "id");
                    client.Call("file-upload-write", "id", id, "offset", 0L, "data", Convert.ToBase64String(new byte[] { 7 })); client.Call("file-upload-commit", "id", id);
                    Check(File.ReadAllBytes(Path.Combine(folder, "network.bin"))[0] == 7, "Encrypted file upload failed.");
                    id = Wire.Text(client.Call("file-download-begin", "name", "network.bin"), "id"); var chunk = client.Call("file-download-read", "id", id, "offset", 0L);
                    Check(Convert.FromBase64String(Wire.Text(chunk, "data"))[0] == 7 && Wire.Text(chunk, "sha256") == Hash(new byte[] { 7 }), "Encrypted file download failed.");
                    id = Wire.Text(client.Call("file-upload-begin", "name", "revoked.bin", "size", 1L, "sha256", Hash(new byte[] { 7 })), "id");
                    lock (owner.Sync) owner.Data.Peers[0].FileWrite = false;
                    Denied(delegate { client.Call("file-upload-write", "id", id, "offset", 0L, "data", Convert.ToBase64String(new byte[] { 7 })); }, "Revoked file permission accepted input.");
                    Check(!Directory.EnumerateFiles(folder, ".hyperlink-*").Any() && !File.Exists(Path.Combine(folder, "revoked.bin")), "Revoked upload remains open.");
                    lock (owner.Sync) { owner.Data.Peers[0].ClipboardToHost = true; owner.Data.Peers[0].ClipboardFromHost = true; }
                    client.Call("clipboard-write", "text", "Synthetic clipboard only"); Check(Wire.Text(client.Call("clipboard-read"), "text") == "Synthetic clipboard only", "Directional synthetic clipboard failed.");
                    Denied(delegate { client.Call("audio-start"); }, "Audio captured without a separate grant.");
                    lock (owner.Sync) owner.Data.Peers[0].Audio = true;
                    int audioFrames = 0; bool invalidAudio = false;
                    remote.AudioFrame += delegate(byte[] packet) { if (packet.Length != 3856) invalidAudio = true; Interlocked.Increment(ref audioFrames); };
                    var audio = client.Call("audio-start"); Check(Wire.Number(audio, "sampleRate") == 48000 && Wire.Number(audio, "bits") == 16, "Invalid audio negotiation.");
                    Wait(delegate { return Volatile.Read(ref audioFrames) >= 3; }); Check(!invalidAudio, "Unbounded audio frame.");
                    client.Call("audio-stop"); int stoppedFrames = Volatile.Read(ref audioFrames); Thread.Sleep(120); Check(Volatile.Read(ref audioFrames) == stoppedFrames, "Audio continued after stop acknowledgement.");
                    client.Call("audio-start"); Wait(delegate { return Volatile.Read(ref audioFrames) > stoppedFrames; });
                    lock (owner.Sync) owner.Data.Peers[0].Audio = false;
                    Thread.Sleep(120); stoppedFrames = Volatile.Read(ref audioFrames); Thread.Sleep(120); Check(Volatile.Read(ref audioFrames) == stoppedFrames, "Revoked audio continued streaming.");
                    Denied(delegate { client.Call("recording-start"); }, "Recording started without its separate permission.");
                    lock (owner.Sync) owner.Data.Peers[0].Recording = true;
                    using (var recorder = new JpegRecording(Path.Combine(folder, "network-recording.mkv"), remote.Width, remote.Height))
                    {
                        int recorded = 0;
                        Action<byte[]> capture = delegate(byte[] jpeg) { recorder.Accept(jpeg); Interlocked.Increment(ref recorded); };
                        client.Call("recording-start"); remote.EncodedFrame += capture;
                        Wait(delegate { return Volatile.Read(ref recorded) >= 3 && Volatile.Read(ref hostNotice).StartsWith("RECORDING:", StringComparison.Ordinal); });
                        remote.EncodedFrame -= capture; recorder.Stop(); recorder.Completion.GetAwaiter().GetResult();
                        client.Call("recording-stop");
                        Check(File.Exists(Path.Combine(folder, "network-recording.mkv")), "Encrypted session recording was not saved.");
                        Wait(delegate { return Volatile.Read(ref hostNotice).StartsWith("Recording stopped", StringComparison.Ordinal); });
                    }
                    int ended = 0; remote.Ended += delegate { Interlocked.Exchange(ref ended, 1); };
                    client.Call("recording-start"); lock (owner.Sync) owner.Data.Peers[0].Recording = false;
                    Wait(delegate { return Volatile.Read(ref ended) != 0; });
                    Check(!remote.Connected, "Revoked recording permission left the session active.");
                }
            }
            return checks;
        }
    }
}
