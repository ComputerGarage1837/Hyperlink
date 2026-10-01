using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Security.Cryptography;

namespace Hyperlink
{
    static class RecordingTests
    {
        internal static string Run(string folder)
        {
            Directory.CreateDirectory(folder);
            if (SessionExtensions.Allowed(new Peer { Control = true, Audio = true }, "recording-start")) throw new Exception("Recording must have its own permission.");
            if (!SessionExtensions.Allowed(new Peer { Recording = true }, "recording-start")) throw new Exception("Recording grant denied.");
            byte[] jpeg;
            using (var bitmap = new Bitmap(64, 32))
            using (var drawing = Graphics.FromImage(bitmap))
            using (var buffer = new MemoryStream())
            { drawing.Clear(Color.Teal); bitmap.Save(buffer, ImageFormat.Jpeg); jpeg = buffer.ToArray(); }
            string file = Path.Combine(folder, "recording.mkv");
            using (var recorder = new JpegRecording(file, 64, 32))
            { recorder.Accept(jpeg); recorder.Accept(jpeg); recorder.Stop(); recorder.Completion.GetAwaiter().GetResult(); }
            if (!File.Exists(file) || Directory.GetFiles(folder, "*.part").Length != 0) throw new Exception("Recording was not finalized safely.");
            byte[] original = File.ReadAllBytes(file); bool denied = false;
            try { new JpegRecording(file, 64, 32); } catch (IOException) { denied = true; }
            if (!denied || !original.SequenceEqual(File.ReadAllBytes(file))) throw new Exception("Existing recording was replaced.");
            string empty = Path.Combine(folder, "empty.mkv");
            using (var recorder = new JpegRecording(empty, 64, 32))
            {
                recorder.Stop(); denied = false;
                try { recorder.Completion.GetAwaiter().GetResult(); } catch (IOException) { denied = true; }
                if (!denied || File.Exists(empty)) throw new Exception("Empty recording was published.");
            }
            if (Directory.GetFiles(folder, "*.part").Length != 0) throw new Exception("Failed recording left a temporary file.");
            using (var first = new JpegRecording(Path.Combine(folder, "exit-first.mkv"), 64, 32))
            using (var second = new JpegRecording(Path.Combine(folder, "exit-second.mkv"), 64, 32))
            {
                first.Accept(jpeg); second.Accept(jpeg); JpegRecording.FinishAll();
                if (JpegRecording.HasActive || !File.Exists(Path.Combine(folder, "exit-first.mkv")) || !File.Exists(Path.Combine(folder, "exit-second.mkv"))) throw new Exception("Application exit did not finalize its active recordings.");
            }
            using (var output = File.Create(Path.Combine(folder, "timing.mkv")))
            {
                JpegRecording.Header(output, 64, 32);
                foreach (long ms in new long[] { 0, 125, 500 }) JpegRecording.Sample(output, jpeg, ms);
            }
            var audio = new System.Collections.Generic.Dictionary<string, object> { { "id", Guid.NewGuid().ToString("N") }, { "sampleRate", 48000 }, { "channels", 2 }, { "bits", 16 }, { "encoding", 1 } };
            byte[] packet = new byte[16 + 1920]; Buffer.BlockCopy(Guid.Parse((string)audio["id"]).ToByteArray(), 0, packet, 0, 16);
            for (int n = 0; n < 480; n++) { short sample = (short)(Math.Sin(n * 2 * Math.PI * 1000 / 48000) * 12000); for (int channel = 0; channel < 2; channel++) { packet[16 + n * 4 + channel * 2] = (byte)sample; packet[17 + n * 4 + channel * 2] = (byte)(sample >> 8); } }
            using (var recorder = new JpegRecording(Path.Combine(folder, "audio.mkv"), 64, 32, audio))
            {
                recorder.Accept(jpeg); recorder.AcceptAudio(packet);
                byte[] wrong = (byte[])packet.Clone(); wrong[0] ^= 1; bool refused = false;
                try { recorder.AcceptAudio(wrong); } catch (InvalidDataException) { refused = true; }
                if (!refused) throw new Exception("Recording accepted a different audio stream.");
                recorder.Stop(); recorder.Completion.GetAwaiter().GetResult();
            }
            audio["encoding"] = 3; bool formatRefused = false;
            try { using (var recorder = new JpegRecording(Path.Combine(folder, "bad-audio.mkv"), 64, 32, audio)) { } } catch (InvalidDataException) { formatRefused = true; }
            if (!formatRefused) throw new Exception("Recording accepted an unsupported audio format.");
            return "Recording permission, file finalization, no-overwrite, audio stream validation and failed-recording cleanup checks passed.";
        }
    }
}
