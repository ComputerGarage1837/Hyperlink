using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace Hyperlink
{
    /** Small streaming Matroska writer for complete H.264 access units. No desktop data is persisted by this class. */
    sealed class VideoMux
    {
        readonly Stream output; readonly int width, height, fps; bool started; long previous = -1;
        internal VideoMux(Stream destination, int w, int h, int rate) { output = destination; width = w; height = h; fps = rate; }
        internal void Write(byte[] annexB, long microseconds)
        {
            if (annexB.Length < 1 || annexB.Length > 2 * 1024 * 1024 || microseconds < previous) throw new InvalidDataException("Invalid video access unit.");
            var units = Nals(annexB); if (units.Count == 0) throw new InvalidDataException("Empty video access unit.");
            if (!started)
            {
                byte[] sps = units.FirstOrDefault(n => (n[0] & 31) == 7), pps = units.FirstOrDefault(n => (n[0] & 31) == 8);
                if (sps == null || pps == null || sps.Length < 4 || sps.Length > 65535 || pps.Length > 65535) throw new InvalidDataException("Video configuration missing.");
                byte[] configuration;
                using (var avcc = new MemoryStream())
                {
                    avcc.Write(new byte[] { 1, sps[1], sps[2], sps[3], 255, 225 }, 0, 6);
                    Short(avcc, sps.Length); avcc.Write(sps, 0, sps.Length); avcc.WriteByte(1); Short(avcc, pps.Length); avcc.Write(pps, 0, pps.Length); configuration = avcc.ToArray();
                }
                byte[] ebml = Element(0x1A45DFA3, Join(Number(0x4286, 1), Number(0x42F7, 1), Number(0x42F2, 4), Number(0x42F3, 8), Text(0x4282, "matroska"), Number(0x4287, 4), Number(0x4285, 2)));
                output.Write(ebml, 0, ebml.Length); output.Write(new byte[] { 0x18, 0x53, 0x80, 0x67, 1, 255, 255, 255, 255, 255, 255, 255 }, 0, 12);
                byte[] info = Element(0x1549A966, Join(Number(0x2AD7B1, 1000000), Text(0x4D80, "Hyperlink"), Text(0x5741, "Hyperlink")));
                output.Write(info, 0, info.Length);
                byte[] track = Element(0x1654AE6B, Element(0xAE, Join(Number(0xD7, 1), Number(0x73C5, 1), Number(0x83, 1), Number(0x9C, 0),
                    Text(0x86, "V_MPEG4/ISO/AVC"), Element(0x63A2, configuration), Number(0x23E383, 1000000000L / fps), Element(0xE0, Join(Number(0xB0, width), Number(0xBA, height))))));
                output.Write(track, 0, track.Length); started = true;
            }
            byte[] block;
            using (var sample = new MemoryStream())
            {
                sample.Write(new byte[] { 0x81, 0, 0, (byte)(units.Any(n => (n[0] & 31) == 5) ? 0x80 : 0) }, 0, 4);
                foreach (var unit in units) { for (int shift = 24; shift >= 0; shift -= 8) sample.WriteByte((byte)(unit.Length >> shift)); sample.Write(unit, 0, unit.Length); }
                block = sample.ToArray();
            }
            byte[] cluster = Element(0x1F43B675, Join(Number(0xE7, microseconds / 1000), Element(0xA3, block)));
            output.Write(cluster, 0, cluster.Length); output.Flush(); previous = microseconds;
        }
        internal static List<byte[]> Nals(byte[] bytes)
        {
            var units = new List<byte[]>(); int start = -1;
            for (int i = 0; i + 2 < bytes.Length; i++)
            {
                int prefix = bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1 ? 3 :
                    i + 3 < bytes.Length && bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 0 && bytes[i + 3] == 1 ? 4 : 0;
                if (prefix == 0) continue;
                if (start >= 0) { int end = i; while (end > start && bytes[end - 1] == 0) end--; if (end > start) units.Add(bytes.Skip(start).Take(end - start).ToArray()); }
                else if (i != 0) throw new InvalidDataException("Invalid video prefix.");
                start = i + prefix; i += prefix - 1;
            }
            if (start >= 0 && start < bytes.Length) units.Add(bytes.Skip(start).ToArray());
            if (units.Count > 256) throw new InvalidDataException("Too many video units."); return units;
        }
        static void Short(Stream stream, int value) { stream.WriteByte((byte)(value >> 8)); stream.WriteByte((byte)value); }
        internal static byte[] Text(uint id, string text) { return Element(id, Encoding.UTF8.GetBytes(text)); }
        internal static byte[] Number(uint id, long value)
        {
            if (value < 0) throw new InvalidDataException("Invalid video timestamp."); int length = 1; while (length < 8 && value >> (length * 8) != 0) length++;
            byte[] body = new byte[length]; for (int i = 0; i < length; i++) body[length - i - 1] = (byte)(value >> (i * 8)); return Element(id, body);
        }
        internal static byte[] Element(uint id, byte[] body)
        {
            using (var stream = new MemoryStream())
            {
                int count = id > 0xFFFFFF ? 4 : id > 0xFFFF ? 3 : id > 255 ? 2 : 1;
                for (int shift = (count - 1) * 8; shift >= 0; shift -= 8) stream.WriteByte((byte)(id >> shift));
                int size = 1; while (body.LongLength >= (1L << (7 * size)) - 1) size++;
                long encoded = body.LongLength | (1L << (7 * size)); for (int shift = (size - 1) * 8; shift >= 0; shift -= 8) stream.WriteByte((byte)(encoded >> shift));
                stream.Write(body, 0, body.Length); return stream.ToArray();
            }
        }
        internal static byte[] Join(params byte[][] values) { using (var stream = new MemoryStream()) { foreach (byte[] value in values) stream.Write(value, 0, value.Length); return stream.ToArray(); } }
    }
}
