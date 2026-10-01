using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Hyperlink
{
    sealed class ExtensionPermissions : Form
    {
        public ExtensionPermissions(Store store, Host host, Peer peer)
        {
            Text = "Permissions · " + peer.Name; Size = new Size(600, 590); StartPosition = FormStartPosition.CenterParent;
            BackColor = Theme.Background; ForeColor = Theme.Text;
            Font = Theme.Font(10);
            var body = new FlowLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(20), FlowDirection = FlowDirection.TopDown, WrapContents = false };
            Controls.Add(body);
            body.Controls.Add(new Label { Text = "Each permission is independent. Changes end the current session.", Width = 540, Height = 40 });
            var control = Check(body, "Keyboard and mouse control", peer.Control);
            var read = Check(body, "Download files from the shared folder", peer.FileRead);
            var write = Check(body, "Upload new files to the shared folder", peer.FileWrite);
            var toHost = Check(body, "Send clipboard text to this computer", peer.ClipboardToHost);
            var fromHost = Check(body, "Read clipboard text from this computer", peer.ClipboardFromHost);
            var audio = Check(body, "Listen to this computer's system playback audio", peer.Audio);
            var recording = Check(body, "Record this session on the viewer computer", peer.Recording);
            var privacy = Check(body, "Hide the local displays during this session", peer.Privacy);
            var folder = new TextBox { Width = 530, ReadOnly = true, Text = store.Data.SharedFolder ?? "" }; body.Controls.Add(folder);
            var choose = Theme.Button("Choose shared folder"); choose.Width = 250; body.Controls.Add(choose);
            choose.Click += delegate { using (var dialog = new FolderBrowserDialog { Description = "This folder applies to all peers with file access. Subfolders are not shared." }) if (dialog.ShowDialog(this) == DialogResult.OK) folder.Text = dialog.SelectedPath; };
            var save = Theme.Button("Save permissions"); save.Width = 250; body.Controls.Add(save);
            save.Click += delegate
            {
                try
                {
                    if (read.Checked || write.Checked) using (var validate = new FileTransfers(folder.Text)) { }
                    host.StopSession();
                    lock (store.Sync)
                    {
                        if (!store.Data.Peers.Contains(peer)) throw new InvalidOperationException("This peer was revoked.");
                        bool oldControl = peer.Control, oldRead = peer.FileRead, oldWrite = peer.FileWrite,
                            oldToHost = peer.ClipboardToHost, oldFromHost = peer.ClipboardFromHost, oldAudio = peer.Audio, oldRecording = peer.Recording, oldPrivacy = peer.Privacy;
                        string oldFolder = store.Data.SharedFolder;
                        try
                        {
                        peer.Control = control.Checked; peer.FileRead = read.Checked; peer.FileWrite = write.Checked;
                        peer.ClipboardToHost = toHost.Checked; peer.ClipboardFromHost = fromHost.Checked; peer.Audio = audio.Checked; peer.Recording = recording.Checked; peer.Privacy = privacy.Checked; store.Data.SharedFolder = folder.Text; store.Save();
                        }
                        catch
                        {
                            peer.Control = oldControl; peer.FileRead = oldRead; peer.FileWrite = oldWrite;
                            peer.ClipboardToHost = oldToHost; peer.ClipboardFromHost = oldFromHost; peer.Audio = oldAudio; peer.Recording = oldRecording; peer.Privacy = oldPrivacy;
                            store.Data.SharedFolder = oldFolder;
                            throw;
                        }
                    }
                    DialogResult = DialogResult.OK; Close();
                }
                catch (Exception ex) { MessageBox.Show(this, ex.Message, "Permissions could not be saved"); }
            };
        }
        static CheckBox Check(Control parent, string label, bool value) { var box = new CheckBox { Text = label, Checked = value, Width = 540, Height = 30 }; parent.Controls.Add(box); return box; }
    }

    sealed class TransferPanel : Form
    {
        readonly ExtensionClient client;
        readonly Label status;
        readonly ListBox list;
        readonly TextBox clipboard;
        CancellationTokenSource cancellation;
        bool busy;
        public TransferPanel(Remote remote)
        {
            client = new ExtensionClient(remote); Text = "Session tools · Hyperlink"; Size = new Size(700, 700); StartPosition = FormStartPosition.CenterParent;
            BackColor = Theme.Background; ForeColor = Theme.Text;
            Font = Theme.Font(10);
            var body = new FlowLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(20), FlowDirection = FlowDirection.TopDown, WrapContents = false }; Controls.Add(body);
            status = new Label { Text = "The owner must allow each operation. Files are limited to 1 GiB.", Width = 640, Height = 40 }; body.Controls.Add(status);
            var bar = new FlowLayoutPanel { Width = 640, Height = 48 }; body.Controls.Add(bar);
            Button(bar, "List files", delegate { Run(ListFiles); });
            Button(bar, "Upload", Upload);
            Button(bar, "Download", Download);
            Button(bar, "Cancel", delegate { if (cancellation != null) cancellation.Cancel(); });
            list = new ListBox { Width = 640, Height = 200, BackColor = Theme.Card, ForeColor = Theme.Text }; body.Controls.Add(list);
            body.Controls.Add(new Label { Text = "Clipboard text · manual transfer only · maximum 1024 characters", Width = 640, Height = 30 });
            clipboard = new TextBox { Width = 640, Height = 100, Multiline = true, MaxLength = 1024, BackColor = Theme.Card, ForeColor = Theme.Text, ScrollBars = ScrollBars.Vertical }; body.Controls.Add(clipboard);
            var clipBar = new FlowLayoutPanel { Width = 640, Height = 48 }; body.Controls.Add(clipBar);
            Button(clipBar, "Send text", delegate { string text = clipboard.Text; Run(delegate { SessionExtensions.CheckText(text); client.Call("clipboard-write", "text", text); return "Text sent to the host clipboard."; }); });
            Button(clipBar, "Read host text", delegate { Run(delegate { string text = Wire.Text(client.Call("clipboard-read"), "text"); SessionExtensions.CheckText(text); Post(delegate { clipboard.Text = text; }); return "Host text received. It has not changed your local clipboard."; }); });
            Button(clipBar, "Copy received text", delegate { try { if (clipboard.Text.Length == 0) Clipboard.Clear(); else Clipboard.SetText(clipboard.Text); } catch { status.Text = "Your local clipboard is busy."; } });
            var audioBar = new FlowLayoutPanel { Width = 640, Height = 48 }; body.Controls.Add(audioBar);
            Button(audioBar, "System audio", delegate { AudioPanel.ShowFor(remote, this); });
            Button(audioBar, "Record video", delegate { RecordingWindow.ShowFor(remote, this); });
            Button(audioBar, "Display privacy", delegate { PrivacyWindow.ShowFor(remote, this); });
            FormClosed += delegate { if (cancellation != null) cancellation.Cancel(); client.Dispose(); };
        }
        static void Button(Control parent, string text, Action action) { var button = Theme.Button(text); button.Width = 150; button.Height = 40; button.Click += delegate { action(); }; parent.Controls.Add(button); }
        void Post(Action action) { if (!IsDisposed && IsHandleCreated) try { BeginInvoke(action); } catch (InvalidOperationException) { } }
        void Run(Func<string> operation)
        {
            if (busy) return; busy = true; cancellation = new CancellationTokenSource(); status.Text = "Working…";
            Task.Run(delegate
            {
                string result;
                try { result = operation(); }
                catch (OperationCanceledException) { result = "Cancelled. Files committed before cancellation remain saved."; }
                catch (Exception ex) { result = ex.Message; }
                finally { try { client.Call("file-cancel"); } catch { } }
                Post(delegate { status.Text = result; busy = false; cancellation.Dispose(); cancellation = null; });
            });
        }
        public static void ShowFor(Remote remote, Form owner)
        {
            foreach (Form form in Application.OpenForms) { var tools = form as TransferPanel; if (tools != null && tools.client.Matches(remote)) { tools.Activate(); return; } }
            new TransferPanel(remote).Show(owner);
        }
        string ListFiles()
        {
            var names = new List<string>(); int offset = 0;
            do
            {
                cancellation.Token.ThrowIfCancellationRequested(); var reply = client.Call("file-list", cancellation.Token, "offset", offset);
                object values; if (!reply.TryGetValue("files", out values)) throw new InvalidDataException();
                foreach (object item in (object[])values) { var file = item as Dictionary<string, object>; if (file == null) throw new InvalidDataException(); names.Add(Wire.Text(file, "name")); }
                offset = Wire.Number(reply, "next"); if (names.Count > 10000) throw new IOException("Too many shared files.");
            } while (offset >= 0);
            Post(delegate { list.Items.Clear(); foreach (string name in names) list.Items.Add(name); }); return names.Count + " shared files.";
        }
        static string Hex(byte[] bytes) { return BitConverter.ToString(bytes).Replace("-", "").ToLowerInvariant(); }
        void Upload()
        {
            if (busy) return;
            using (var dialog = new OpenFileDialog { Title = "Choose a file to upload as a new shared file" })
            {
                if (dialog.ShowDialog(this) != DialogResult.OK) return; string path = dialog.FileName;
                Run(delegate
                {
                    using (var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
                    using (var hash = SHA256.Create())
                    {
                        if (input.Length > 1024L * 1024 * 1024) throw new IOException("File exceeds the 1 GiB draft limit.");
                        var bytes = new byte[8192]; int count;
                        while ((count = input.Read(bytes, 0, bytes.Length)) > 0) { cancellation.Token.ThrowIfCancellationRequested(); hash.TransformBlock(bytes, 0, count, null, 0); }
                        hash.TransformFinalBlock(new byte[0], 0, 0); input.Position = 0;
                        var reply = client.Call("file-upload-begin", cancellation.Token, "name", Path.GetFileName(path), "size", input.Length, "sha256", Hex(hash.Hash)); string id = Wire.Text(reply, "id");
                        long offset = 0;
                        while ((count = input.Read(bytes, 0, bytes.Length)) > 0)
                        {
                            cancellation.Token.ThrowIfCancellationRequested(); reply = client.Call("file-upload-write", cancellation.Token, "id", id, "offset", offset, "data", Convert.ToBase64String(bytes, 0, count));
                            offset += count; if (Convert.ToInt64(reply["offset"]) != offset) throw new InvalidDataException("Upload position mismatch.");
                        }
                        cancellation.Token.ThrowIfCancellationRequested(); client.Call("file-upload-commit", cancellation.Token, "id", id); return "Upload verified and published.";
                    }
                });
            }
        }
        void Download()
        {
            if (busy || list.SelectedItem == null) return; string name = (string)list.SelectedItem;
            using (var dialog = new SaveFileDialog { Title = "Save download using a new filename", FileName = name })
            {
                if (dialog.ShowDialog(this) != DialogResult.OK) return; string target = dialog.FileName;
                if (File.Exists(target)) { status.Text = "Choose a new filename. Existing files are never overwritten."; return; }
                Run(delegate
                {
                    string partial = Path.Combine(Path.GetDirectoryName(target), ".hyperlink-" + Guid.NewGuid().ToString("N") + ".part");
                    try
                    {
                        var reply = client.Call("file-download-begin", cancellation.Token, "name", name); string id = Wire.Text(reply, "id"); long size = Convert.ToInt64(reply["size"]), offset = 0;
                        if (size < 0 || size > 1024L * 1024 * 1024) throw new InvalidDataException("Invalid file size.");
                        using (var output = new FileStream(partial, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                        using (var hash = SHA256.Create())
                        {
                            while (true)
                            {
                                cancellation.Token.ThrowIfCancellationRequested(); reply = client.Call("file-download-read", cancellation.Token, "id", id, "offset", offset);
                                byte[] bytes = Convert.FromBase64String(Wire.Text(reply, "data"));
                                if (bytes.Length > 8192 || offset + bytes.Length > size || Convert.ToInt64(reply["offset"]) != offset + bytes.Length) throw new InvalidDataException("Invalid file chunk.");
                                output.Write(bytes, 0, bytes.Length); hash.TransformBlock(bytes, 0, bytes.Length, null, 0); offset += bytes.Length;
                                object complete; if (reply.TryGetValue("complete", out complete) && complete is bool && (bool)complete)
                                {
                                    hash.TransformFinalBlock(new byte[0], 0, 0);
                                    if (offset != size || Wire.Text(reply, "sha256") != Hex(hash.Hash)) throw new InvalidDataException("Download integrity check failed.");
                                    output.Flush(true); break;
                                }
                                if (bytes.Length == 0) throw new InvalidDataException("Empty intermediate file chunk.");
                            }
                        }
                        cancellation.Token.ThrowIfCancellationRequested(); File.Move(partial, target); return "Download verified and saved.";
                    }
                    finally { if (File.Exists(partial)) File.Delete(partial); }
                });
            }
        }
    }
}
