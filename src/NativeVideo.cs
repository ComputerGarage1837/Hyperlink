using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

namespace Hyperlink
{
    static class VideoRuntime
    {
        const string ExpectedHash = "ee76bd5b4525289768485bd2db25993046c24127b3a9a332b02d2aca72377f59";
        internal static string Path { get { return System.IO.Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "ffmpeg.exe"); } }
        static readonly object sync = new object(); static bool? available;
        internal static bool Available { get { lock (sync) { if (!available.HasValue) { try { available = File.Exists(Path) && ReleasePackages.Hash(Path) == ExpectedHash; } catch { available = false; } } return available.Value; } } }
        internal static VideoProcess Start(string arguments, bool input)
        {
            if (!Available) throw new IOException("The pinned native video runtime is not installed.");
            return new VideoProcess(Path, arguments);
        }
        internal static void Stop(VideoProcess process)
        {
            if (process == null) return;
            try { if (!process.HasExited) { process.Kill(); process.WaitForExit(2000); } }
            catch (InvalidOperationException) { } finally { process.Dispose(); }
        }
        internal static void PutLong(byte[] bytes, int offset, long value) { for (int i = 7; i >= 0; i--) { bytes[offset + i] = (byte)value; value >>= 8; } }
        internal static long Long(byte[] bytes, int offset) { long value = 0; for (int i = 0; i < 8; i++) value = (value << 8) | bytes[offset + i]; return value; }
    }
    sealed class NativeVideoEncoder : IDisposable
    {
        readonly VideoProcess process; long sequence, first = long.MinValue, previous = -1; long numerator = 1, denominator = 120;
        internal NativeVideoEncoder(int output, int width, int height, int fps, bool synthetic)
        {
            if ((fps != 60 && fps != 120) || width < 2 || height < 2 || width > 1920 || height > 1080 || width % 2 != 0 || height % 2 != 0 || output < 0 || output > 32) throw new InvalidDataException("Unsupported video configuration.");
            string source = synthetic ? "-re -f lavfi -i \"testsrc2=size=" + width + "x" + height + ":rate=" + fps + "\"" :
                "-f lavfi -i \"ddagrab=output_idx=" + output + ":framerate=" + fps + ":dup_frames=0,hwdownload,format=bgra,scale=" + width + ":" + height + ":flags=fast_bilinear,format=nv12\"";
            process = VideoRuntime.Start("-hide_banner -loglevel quiet -nostats -nostdin " + source + " -map 0:v -an -sn -dn -fps_mode passthrough -c:v h264_nvenc -preset p1 -tune ull -profile:v high -level:v 5.1 -bf 0 -rc-lookahead 0 -zerolatency 1 -delay 0 -surfaces 4 -b:v 20M -maxrate 20M -bufsize 2M -g " + fps + " -flags:v -global_header -bsf:v h264_metadata=aud=insert -f tee \"[f=framecrc:flush_packets=1]pipe:2|[f=data:flush_packets=1]pipe:1\"", false);
        }
        internal byte[] Next()
        {
            string line;
            while ((line = process.StandardError.ReadLine()) != null)
            {
                if (line.Length > 512) throw new InvalidDataException("Invalid video frame metadata.");
                if (line.StartsWith("#tb 0:", StringComparison.Ordinal))
                {
                    string[] ratio = line.Substring(6).Trim().Split('/');
                    if (ratio.Length != 2 || !long.TryParse(ratio[0], out numerator) || !long.TryParse(ratio[1], out denominator) || numerator < 1 || denominator < 1 || numerator > 1000000 || denominator > 1000000) throw new InvalidDataException("Invalid video time base.");
                    continue;
                }
                if (line.StartsWith("#", StringComparison.Ordinal) || line.Length == 0) continue;
                string[] columns = line.Split(','); long pts; int size;
                if (columns.Length < 6 || columns[0].Trim() != "0" || !long.TryParse(columns[2].Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out pts) ||
                    !int.TryParse(columns[4].Trim(), out size) || size < 1 || size > 2 * 1024 * 1024 - 16) throw new InvalidDataException("Invalid encoded frame bounds.");
                if (first == long.MinValue) first = pts;
                long time = checked(checked(pts - first) * 1000000 * numerator / denominator);
                if (time < 0 || time < previous) throw new InvalidDataException("Video timestamps moved backwards.");
                byte[] packet = new byte[size + 16]; VideoRuntime.PutLong(packet, 0, ++sequence); VideoRuntime.PutLong(packet, 8, time); int offset = 16;
                while (offset < packet.Length) { int count = process.StandardOutput.BaseStream.Read(packet, offset, packet.Length - offset); if (count == 0) throw new EndOfStreamException("Video encoder ended mid-frame."); offset += count; }
                previous = time; return packet;
            }
            throw new EndOfStreamException("The native video encoder ended.");
        }
        public void Dispose() { VideoRuntime.Stop(process); }
    }
    sealed class NativeVideoDecoder : IDisposable
    {
        readonly VideoProcess process; readonly BlockingCollection<byte[]> packets = new BlockingCollection<byte[]>(3);
        readonly Thread writer, reader; readonly Action<Bitmap> present; readonly Action failed;
        readonly int width, height, rate; int disposed; long sequence, timestamp = -1;
        readonly Stopwatch startup = Stopwatch.StartNew();
        internal NativeVideoDecoder(int w, int h, int fps, Action<Bitmap> frame, Action error)
        {
            width = w; height = h; rate = fps; present = frame; failed = error;
            if (w < 2 || h < 2 || w > 1920 || h > 1080 || w % 2 != 0 || h % 2 != 0 || (fps != 60 && fps != 120)) throw new InvalidDataException("Unsupported decoder configuration.");
            process = VideoRuntime.Start("-hide_banner -loglevel quiet -nostats -nostdin -max_alloc 33554432 -analyzeduration 0 -probesize 32 -flags low_delay -threads 1 -hwaccel d3d11va -max_pixels 2150400 -f matroska -i pipe:0 -an -sn -dn -threads 1 -vf scale=" + w + ":" + h + " -fps_mode passthrough -pix_fmt bgra -flush_packets 1 -f rawvideo pipe:1", true);
            // Drain any diagnostics so stderr can never block decoding.
            process.DrainErrors();
            writer = new Thread(Write) { IsBackground = true, Name = "Hyperlink video input" }; reader = new Thread(Read) { IsBackground = true, Name = "Hyperlink video output" }; writer.Start(); reader.Start();
        }
        internal void Accept(byte[] packet)
        {
            if (Volatile.Read(ref disposed) != 0) throw new IOException("Video decoder stopped.");
            if (packet.Length < 17 || packet.Length > 2 * 1024 * 1024 || VideoRuntime.Long(packet, 0) != sequence + 1 || VideoRuntime.Long(packet, 8) < timestamp) throw new InvalidDataException("Invalid video packet order.");
            if (!packets.TryAdd(packet, startup.ElapsedMilliseconds < 5000 ? 500 : 100)) throw new IOException("Video decoding cannot keep up; choose 60 FPS."); sequence++; timestamp = VideoRuntime.Long(packet, 8);
        }
        void Write()
        {
            try
            {
                var mux = new VideoMux(process.StandardInput.BaseStream, width, height, rate);
                foreach (var packet in packets.GetConsumingEnumerable())
                {
                    if (Volatile.Read(ref disposed) != 0) return;
                    byte[] frame = new byte[packet.Length - 16]; Buffer.BlockCopy(packet, 16, frame, 0, frame.Length); mux.Write(frame, VideoRuntime.Long(packet, 8));
                }
            }
            catch { if (Volatile.Read(ref disposed) == 0) failed(); }
        }
        void Read()
        {
            byte[] bytes = new byte[checked(width * height * 4)];
            try
            {
                while (Volatile.Read(ref disposed) == 0)
                {
                    int offset = 0; while (offset < bytes.Length) { int count = process.StandardOutput.BaseStream.Read(bytes, offset, bytes.Length - offset); if (count == 0) throw new EndOfStreamException(); offset += count; }
                    var bitmap = new Bitmap(width, height, PixelFormat.Format32bppArgb);
                    try
                    {
                        var data = bitmap.LockBits(new Rectangle(0, 0, width, height), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
                        try { for (int row = 0; row < height; row++) Marshal.Copy(bytes, row * width * 4, IntPtr.Add(data.Scan0, row * data.Stride), width * 4); }
                        finally { bitmap.UnlockBits(data); }
                        if (Volatile.Read(ref disposed) != 0) { bitmap.Dispose(); return; }
                        if (present == null) bitmap.Dispose(); else present(bitmap);
                    }
                    catch { bitmap.Dispose(); throw; }
                }
            }
            catch { if (Volatile.Read(ref disposed) == 0) failed(); }
        }
        public void Dispose()
        {
            if (Interlocked.Exchange(ref disposed, 1) != 0) return;
            packets.CompleteAdding();
            new Thread(delegate() { VideoRuntime.Stop(process); if (writer.Join(1000) && reader.Join(1000)) packets.Dispose(); }) { IsBackground = true, Name = "Hyperlink video cleanup" }.Start();
            // Readers/writers exit when their private pipes close. Never join the GUI thread.
        }
    }
}
