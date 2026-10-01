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
            return "Recording permission, file finalization, no-overwrite and failed-recording cleanup checks passed.";
        }
    }
}
