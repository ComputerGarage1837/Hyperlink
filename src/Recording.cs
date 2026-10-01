using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

namespace Hyperlink
{
    sealed class JpegRecording : IDisposable
    {
        sealed class Frame { public byte[] Bytes; public long Milliseconds; }
        readonly BlockingCollection<Frame> frames = new BlockingCollection<Frame>(8);
        readonly Stopwatch clock = Stopwatch.StartNew();
        readonly string destination, temporary;
        readonly int width, height;
        readonly Task worker;
        int stopped;
        public Task Completion { get { return worker; } }
        internal JpegRecording(string file, int w, int h)
        {
            destination = Path.GetFullPath(file);
            if (!String.Equals(Path.GetExtension(destination), ".mkv", StringComparison.OrdinalIgnoreCase)) throw new ArgumentException("Choose a .mkv recording file.");
            using (var folder = new FileTransfers(Path.GetDirectoryName(destination))) { }
            if (File.Exists(destination) || Directory.Exists(destination)) throw new IOException("Choose a new recording filename; existing files are never replaced.");
            if (w < 1 || h < 1 || w > 1920 || h > 1080) throw new ArgumentException("Unsupported recording dimensions.");
            width = w; height = h;
            temporary = Path.Combine(Path.GetDirectoryName(destination), ".hyperlink-recording-" + Guid.NewGuid().ToString("N") + ".part");
            worker = Task.Run((Action)Write);
        }
        internal void Accept(byte[] jpeg)
        {
            if (Volatile.Read(ref stopped) != 0) return;
            try
            {
                if (!frames.TryAdd(new Frame { Bytes = jpeg, Milliseconds = clock.ElapsedMilliseconds }))
                { Stop(); throw new IOException("Recording stopped because storage could not keep up."); }
            }
            catch (InvalidOperationException) { }
        }
        internal void Stop()
        {
            if (Interlocked.Exchange(ref stopped, 1) == 0) frames.CompleteAdding();
        }
        void Write()
        {
            bool complete = false; int count = 0;
            try
            {
                using (var output = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None, 65536, FileOptions.SequentialScan))
                {
                    Header(output, width, height);
                    long first = -1;
                    foreach (Frame frame in frames.GetConsumingEnumerable())
                    {
                        if (first < 0) first = frame.Milliseconds;
                        if (output.Length + frame.Bytes.Length + 64 > 2L * 1024 * 1024 * 1024) { Stop(); break; }
                        Sample(output, frame.Bytes, frame.Milliseconds - first); count++;
                    }
                    output.Flush(true);
                }
                if (count == 0) throw new IOException("No video frames arrived during recording.");
                using (var folder = new FileTransfers(Path.GetDirectoryName(destination))) { }
                File.Move(temporary, destination); complete = true;
            }
            finally
            {
                Stop(); if (!complete && File.Exists(temporary)) File.Delete(temporary);
            }
        }
        internal static void Header(Stream output, int width, int height)
        {
            byte[] header = VideoMux.Element(0x1A45DFA3, VideoMux.Join(VideoMux.Number(0x4286, 1), VideoMux.Number(0x42F7, 1), VideoMux.Number(0x42F2, 4), VideoMux.Number(0x42F3, 8), VideoMux.Text(0x4282, "matroska"), VideoMux.Number(0x4287, 4), VideoMux.Number(0x4285, 2)));
            output.Write(header, 0, header.Length);
            output.Write(new byte[] { 0x18, 0x53, 0x80, 0x67, 1, 255, 255, 255, 255, 255, 255, 255 }, 0, 12);
            byte[] info = VideoMux.Element(0x1549A966, VideoMux.Join(VideoMux.Number(0x2AD7B1, 1000000), VideoMux.Text(0x4D80, "Hyperlink"), VideoMux.Text(0x5741, "Hyperlink")));
            output.Write(info, 0, info.Length);
            byte[] track = VideoMux.Element(0x1654AE6B, VideoMux.Element(0xAE, VideoMux.Join(VideoMux.Number(0xD7, 1), VideoMux.Number(0x73C5, 1), VideoMux.Number(0x83, 1), VideoMux.Text(0x86, "V_MJPEG"), VideoMux.Element(0xE0, VideoMux.Join(VideoMux.Number(0xB0, width), VideoMux.Number(0xBA, height))))));
            output.Write(track, 0, track.Length);
        }
        internal static void Sample(Stream output, byte[] jpeg, long milliseconds)
        {
            if (jpeg == null || jpeg.Length < 4 || jpeg.Length > 4 * 1024 * 1024 || jpeg[0] != 255 || jpeg[1] != 216 || jpeg[jpeg.Length - 2] != 255 || jpeg[jpeg.Length - 1] != 217 || milliseconds < 0) throw new InvalidDataException("Invalid recording frame.");
            byte[] cluster = VideoMux.Element(0x1F43B675, VideoMux.Join(VideoMux.Number(0xE7, milliseconds), VideoMux.Element(0xA3, VideoMux.Join(new byte[] { 0x81, 0, 0, 0x80 }, jpeg))));
            output.Write(cluster, 0, cluster.Length);
        }
        public void Dispose() { Stop(); }
    }
}
