using System;
using System.Drawing;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Hyperlink
{
    sealed class RecordingWindow : Form
    {
        readonly Remote remote; readonly ExtensionClient client;
        readonly Label status; readonly Button start, stop;
        JpegRecording recording; bool busy, closed;
        RecordingWindow(Remote connection)
        {
            remote = connection; client = new ExtensionClient(remote);
            Text = "Session recording"; Size = new Size(590, 290); StartPosition = FormStartPosition.CenterParent;
            BackColor = Theme.Background; ForeColor = Theme.Text; Font = Theme.Font(10);
            var body = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(20), ColumnCount = 1 };
            var help = Theme.Label("Recording needs the computer owner's separate permission. The host displays a recording notice. Video is saved locally as an MKV file with its actual arrival times; audio is not included.", 11, Theme.Muted);
            help.AutoSize = false; help.Height = 85; help.Dock = DockStyle.Top; body.Controls.Add(help);
            var buttons = new FlowLayoutPanel { Dock = DockStyle.Top, Height = 50 };
            start = Theme.Button("Start recording", true); start.Width = 230; start.Click += async delegate { await StartRecording(); }; buttons.Controls.Add(start);
            stop = Theme.Button("Stop and save"); stop.Width = 230; stop.Enabled = false; stop.Click += delegate { StopRecording(); }; buttons.Controls.Add(stop); body.Controls.Add(buttons);
            status = Theme.Label("Choose a new local file. Existing files are never replaced.", 10, Theme.Muted); status.AutoSize = false; status.Height = 65; status.Dock = DockStyle.Top; body.Controls.Add(status); Controls.Add(body);
            remote.Ended += Ended;
            FormClosed += delegate { closed = true; remote.Ended -= Ended; StopRecording(); if (!busy && recording == null) client.Dispose(); };
        }
        public static void ShowFor(Remote remote, Form owner)
        {
            foreach (Form form in Application.OpenForms) { var window = form as RecordingWindow; if (window != null && window.remote == remote) { window.Activate(); return; } }
            new RecordingWindow(remote).Show(owner);
        }
        async Task StartRecording()
        {
            if (busy || recording != null) return;
            string file;
            using (var dialog = new SaveFileDialog { Filter = "Matroska video (*.mkv)|*.mkv", DefaultExt = "mkv", AddExtension = true, OverwritePrompt = true, FileName = "Hyperlink-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + ".mkv" })
            { if (dialog.ShowDialog(this) != DialogResult.OK) return; file = dialog.FileName; }
            busy = true; start.Enabled = false; status.Text = "Checking the owner's recording permission…";
            bool granted = false;
            try
            {
                await Task.Run(() => client.Call("recording-start")); granted = true;
                if (!closed && remote.Connected)
                {
                recording = new JpegRecording(file, remote.Width, remote.Height);
                remote.EncodedFrame += recording.Accept; stop.Enabled = true;
                status.Text = "● RECORDING · Video only · Stop to save the file."; status.ForeColor = Theme.Red;
                Observe(recording, file);
                }
            }
            catch (Exception ex) { if (!closed) status.Text = ex.Message; }
            if (granted && recording == null) await StopHost();
            busy = false; if (!closed) start.Enabled = recording == null; else if (recording == null) client.Dispose();
        }
        async void Observe(JpegRecording current, string file)
        {
            string message;
            try { await current.Completion; message = "Saved: " + file; }
            catch (Exception ex) { message = "Recording failed: " + ex.Message; }
            remote.EncodedFrame -= current.Accept; current.Dispose();
            await StopHost(); if (recording == current) recording = null;
            if (closed) client.Dispose();
            else { status.Text = message; status.ForeColor = Theme.Muted; stop.Enabled = false; start.Enabled = !busy && remote.Connected; }
        }
        Task StopHost()
        {
            return Task.Run(delegate { try { if (remote.Connected) client.Call("recording-stop"); } catch { remote.Dispose(); } });
        }
        void StopRecording()
        {
            var current = recording; if (current == null) return;
            remote.EncodedFrame -= current.Accept; current.Stop();
            if (!closed) { stop.Enabled = false; status.Text = "Saving recording…"; }
        }
        void Ended(string reason)
        {
            if (!closed && IsHandleCreated) try { BeginInvoke((Action)StopRecording); } catch (InvalidOperationException) { }
        }
    }
}
