using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;

namespace Hyperlink
{
    sealed class SessionExtensions : IDisposable
    {
        readonly Func<string> folder;
        readonly bool synthetic;
        readonly Func<byte[], bool> sendAudio;
        SystemAudioCapture audio;
        FileTransfers files;
        string testClipboard = ""; volatile bool recording;
        PrivacyScreen privacy; bool syntheticPrivacy;
        public Action EndSession;
        public bool PrivacyActive { get { return syntheticPrivacy || (privacy != null && privacy.Active); } }
        public bool Recording { get { return recording; } }
        public SessionExtensions(Func<string> ownerFolder, bool test, Func<byte[], bool> audioSender = null) { folder = ownerFolder; synthetic = test; sendAudio = audioSender; }
        public bool OwnsAudio(byte[] packet) { return audio != null && audio.Owns(packet); }
        public void StartAudio() { if (audio != null) audio.Start(); }
        static long Number(Dictionary<string, object> message, string key)
        {
            object value; if (!message.TryGetValue(key, out value) || !(value is int || value is long)) throw new InvalidDataException("Expected integer.");
            return Convert.ToInt64(value);
        }
        static string Text(Dictionary<string, object> message, string key) { return Wire.Text(message, key); }
        public static bool Allowed(Peer peer, string operation)
        {
            if (peer == null) return false;
            if (operation == "file-cancel") return true;
            if (operation == "privacy-stop") return true;
            if (operation == "privacy-start") return peer.Privacy;
            if (operation == "recording-stop") return true;
            if (operation == "recording-start") return peer.Recording;
            if (operation == "audio-stop") return true;
            if (operation == "audio-start") return peer.Audio;
            if (operation == "file-list" || operation == "file-download-begin" || operation == "file-download-read") return peer.FileRead;
            if (operation == "file-upload-begin" || operation == "file-upload-write" || operation == "file-upload-commit") return peer.FileWrite;
            if (operation == "clipboard-read") return peer.ClipboardFromHost;
            if (operation == "clipboard-write") return peer.ClipboardToHost;
            return false;
        }
        public object Handle(Dictionary<string, object> message)
        {
            string operation = Text(message, "operation");
            if (operation == "privacy-start") { if (PrivacyActive) throw new InvalidOperationException("Privacy is already active."); if (synthetic) syntheticPrivacy = true; else { privacy = new PrivacyScreen(EndSession); privacy.SetRecording(recording); } return new { privacy = true, timeoutSeconds = 1800 }; }
            if (operation == "privacy-stop") { syntheticPrivacy = false; if (privacy != null) privacy.Dispose(); privacy = null; return new { privacy = false }; }
            if (operation == "recording-start") { if (recording) throw new InvalidOperationException("Recording is already active."); recording = true; if (privacy != null) privacy.SetRecording(true); return new { recording = true }; }
            if (operation == "recording-stop") { recording = false; if (privacy != null) privacy.SetRecording(false); return new { recording = false }; }
            if (operation == "audio-start") { if (audio != null || sendAudio == null) throw new InvalidOperationException("Audio is already active or unavailable."); audio = new SystemAudioCapture(synthetic, sendAudio); return audio.Describe(); }
            if (operation == "audio-stop") { if (audio != null) audio.Dispose(); audio = null; return new { stopped = true }; }
            if (operation == "file-cancel") { if (files != null) files.Dispose(); files = null; return new { cancelled = true }; }
            if (operation.StartsWith("clipboard-", StringComparison.Ordinal))
            {
                string text = operation == "clipboard-write" ? Text(message, "text") : null;
                if (text != null) CheckText(text);
                if (synthetic) { if (text != null) testClipboard = text; return new { text = testClipboard }; }
                string result = ""; Exception error = null;
                var thread = new Thread(delegate()
                {
                    try { result = ClipboardText.Exchange(text); }
                    catch (Exception ex) { error = ex; }
                }) { IsBackground = true, Name = "Hyperlink explicit clipboard" };
                thread.SetApartmentState(ApartmentState.STA); thread.Start();
                if (!thread.Join(2000)) throw new IOException("Clipboard is busy.");
                if (error != null) throw new IOException("Clipboard is unavailable."); CheckText(result);
                return new { text = text == null ? result : "" };
            }
            if (files == null) files = new FileTransfers(folder());
            switch (operation)
            {
                case "file-list": return files.List(checked((int)Number(message, "offset")));
                case "file-upload-begin": return files.BeginUpload(Text(message, "name"), Number(message, "size"), Text(message, "sha256"));
                case "file-upload-write": return files.Write(Text(message, "id"), Number(message, "offset"), Text(message, "data"));
                case "file-upload-commit": return files.Commit(Text(message, "id"));
                case "file-download-begin": return files.BeginDownload(Text(message, "name"));
                case "file-download-read": return files.Read(Text(message, "id"), Number(message, "offset"));
                default: throw new InvalidDataException("Unknown extension operation.");
            }
        }
        public static void CheckText(string text)
        {
            if (text == null || text.Length > 1024 || text.IndexOf('\0') >= 0) throw new InvalidDataException("Clipboard text exceeds the 1024-character draft limit.");
            for (int i = 0; i < text.Length; i++)
                if (Char.IsHighSurrogate(text[i])) { if (++i >= text.Length || !Char.IsLowSurrogate(text[i])) throw new InvalidDataException("Invalid Unicode text."); }
                else if (Char.IsLowSurrogate(text[i])) throw new InvalidDataException("Invalid Unicode text.");
        }
        public void Dispose() { syntheticPrivacy = false; if (privacy != null) privacy.Dispose(); privacy = null; recording = false; try { if (files != null) files.Dispose(); files = null; } finally { if (audio != null) audio.Dispose(); audio = null; } }
    }

    public sealed partial class Remote
    {
        internal readonly object AudioSync = new object();
        internal volatile Dictionary<string, object> AudioDescriptor;
        public bool SupportsExtensions { get; private set; }
        public event Action<Dictionary<string, object>> ExtensionReply;
        public event Action<byte[]> AudioFrame;
        void AcceptAudio(byte[] bytes) { if (bytes.Length < 17 || bytes.Length > 8208) throw new InvalidDataException("Invalid audio frame."); var handler = AudioFrame; if (handler != null) handler(bytes); }
        void AcceptExtension(byte[] bytes)
        {
            if (bytes.Length > 16384) throw new InvalidDataException("Control frame too large.");
            var json = new JavaScriptSerializer { MaxJsonLength = 16384, RecursionLimit = 8 };
            var reply = json.DeserializeObject(new UTF8Encoding(false, true).GetString(bytes)) as Dictionary<string, object>;
            if (reply == null || Wire.Text(reply, "kind") != "extension") throw new IOException("The host stopped this session.");
            var handler = ExtensionReply; if (handler != null) handler(reply);
        }
        public void SendExtension(Dictionary<string, object> message)
        {
            if (!SupportsExtensions || !Connected) throw new IOException("This host does not support file or clipboard extensions.");
            message["kind"] = "extension";
            var connection = wire; if (connection == null) throw new IOException("Disconnected."); connection.SendJson(message);
        }
    }

    sealed class ExtensionClient : IDisposable
    {
        sealed class Pending { public readonly object Sync = new object(); public readonly ManualResetEventSlim Done = new ManualResetEventSlim(); public Dictionary<string, object> Reply; public bool Finished; }
        readonly Remote remote;
        readonly ConcurrentDictionary<string, Pending> pending = new ConcurrentDictionary<string, Pending>();
        volatile bool disposed;
        public ExtensionClient(Remote connection) { remote = connection; remote.ExtensionReply += Reply; }
        public bool Matches(Remote connection) { return remote == connection; }
        void Reply(Dictionary<string, object> message)
        {
            string id = Wire.Text(message, "request"); Pending request;
            if (pending.TryGetValue(id, out request)) lock (request.Sync) { if (!request.Finished) { request.Reply = message; request.Done.Set(); } }
        }
        public Dictionary<string, object> Call(string operation, params object[] fields)
        { return Call(operation, CancellationToken.None, fields); }
        public Dictionary<string, object> Call(string operation, CancellationToken cancellation, params object[] fields)
        {
            if (operation != "audio-start" && operation != "audio-stop") return CallCore(operation, cancellation, fields);
            lock (remote.AudioSync)
            {
                if (operation == "audio-start" && remote.AudioDescriptor != null) throw new IOException("Audio is already active for playback or recording.");
                var result = CallCore(operation, cancellation, fields);
                remote.AudioDescriptor = operation == "audio-start" ? new Dictionary<string, object>(result) : null;
                return result;
            }
        }
        Dictionary<string, object> CallCore(string operation, CancellationToken cancellation, params object[] fields)
        {
            if (disposed || pending.Count >= 4) throw new IOException("The session is closed or busy.");
            string id = Guid.NewGuid().ToString("N"); var wait = new Pending();
            if (!pending.TryAdd(id, wait)) throw new IOException();
            try
            {
                if (disposed) throw new IOException("The extension window is closed.");
                var message = new Dictionary<string, object> { { "request", id }, { "operation", operation } };
                for (int i = 0; i < fields.Length; i += 2) message.Add((string)fields[i], fields[i + 1]);
                remote.SendExtension(message);
                if (!wait.Done.Wait(10000, cancellation) || wait.Reply == null) throw new IOException("The host did not finish this request.");
                object ok; if (!wait.Reply.TryGetValue("ok", out ok) || !(ok is bool) || !(bool)ok) throw new IOException("The owner has not allowed this operation, or the file operation failed.");
                object result; var value = wait.Reply.TryGetValue("result", out result) ? result as Dictionary<string, object> : null;
                if (value == null) throw new InvalidDataException("Invalid extension response."); return value;
            }
            finally { Pending removed; pending.TryRemove(id, out removed); lock (wait.Sync) { wait.Finished = true; wait.Done.Dispose(); } }
        }
        public void Dispose()
        {
            disposed = true; remote.ExtensionReply -= Reply;
            foreach (var request in pending.Values) lock (request.Sync) { if (!request.Finished) request.Done.Set(); }
        }
    }
}
