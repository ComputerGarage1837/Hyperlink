using System;
using System.Drawing;
using System.Threading;
using System.Collections.Generic;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Hyperlink
{
    sealed class RecordingWindow : Form
    {
        readonly Remote remote; readonly ExtensionClient client;
        readonly Label status; readonly Button start, stop; readonly CheckBox audio; int ownsAudio, ownsRecording;
        JpegRecording recording; bool busy, closed;
        RecordingWindow(Remote connection)
        {
            remote = connection; client = new ExtensionClient(remote);
            Text = "Session recording"; Size = new Size(590, 330); StartPosition = FormStartPosition.CenterParent;
            BackColor = Theme.Background; ForeColor = Theme.Text; Font = Theme.Font(10);
            var body = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(20), ColumnCount = 1 };
            var help = Theme.Label("Recording needs the owner's permission. Optional system audio also needs audio permission. Stop listening before starting an audio recording.", 11, Theme.Muted);
            help.AutoSize = false; help.Height = 85; help.Dock = DockStyle.Top; body.Controls.Add(help);
            audio = new CheckBox { Text = "Include system audio", AutoSize = true }; body.Controls.Add(audio);
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
            Dictionary<string, object> descriptor = null;
            try
            {
                bool includeAudio = audio.Checked; audio.Enabled = false;
                await Task.Run(delegate {
                    if (includeAudio) { descriptor = client.Call("audio-start"); Interlocked.Exchange(ref ownsAudio, 1); }
                    client.Call("recording-start"); Interlocked.Exchange(ref ownsRecording, 1);
                });
                if (!closed && remote.Connected)
                {
                recording = new JpegRecording(file, remote.Width, remote.Height, descriptor);
                remote.EncodedFrame += recording.Accept; if (descriptor != null) remote.AudioFrame += recording.AcceptAudio; stop.Enabled = true;
                status.Text = (descriptor == null ? "● RECORDING · Video only · Stop to save the file." : "● RECORDING · Video and system audio · Stop to save the file."); status.ForeColor = Theme.Red;
                Observe(recording, file);
                }
            }
            catch (Exception ex) { if (!closed) status.Text = ex.Message; }
            if (recording == null) await StopHost(); if (!closed && recording == null) audio.Enabled = true;
            busy = false; if (!closed) start.Enabled = recording == null; else if (recording == null) client.Dispose();
        }
        async void Observe(JpegRecording current, string file)
        {
            string message;
            try { await current.Completion; message = "Saved: " + file; }
            catch (Exception ex) { message = "Recording failed: " + ex.Message; }
            remote.EncodedFrame -= current.Accept; remote.AudioFrame -= current.AcceptAudio; current.Dispose();
            await StopHost(); if (recording == current) recording = null;
            if (closed) client.Dispose();
            else { status.Text = message; status.ForeColor = Theme.Muted; stop.Enabled = false; start.Enabled = !busy && remote.Connected; audio.Enabled = true; }
        }
        Task StopHost()
        {
            return Task.Run(delegate { try { if (Interlocked.Exchange(ref ownsRecording, 0) != 0 && remote.Connected) client.Call("recording-stop"); if (Interlocked.Exchange(ref ownsAudio, 0) != 0 && remote.Connected) client.Call("audio-stop"); } catch { remote.Dispose(); } });
        }
        void StopRecording()
        {
            var current = recording; if (current == null) return;
            remote.EncodedFrame -= current.Accept; remote.AudioFrame -= current.AcceptAudio; current.Stop();
            if (!closed) { stop.Enabled = false; status.Text = "Saving recording…"; }
        }
        void Ended(string reason)
        {
            if (!closed && IsHandleCreated) try { BeginInvoke((Action)StopRecording); } catch (InvalidOperationException) { }
        }
    }
}
