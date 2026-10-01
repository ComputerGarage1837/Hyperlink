using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace Hyperlink
{
    // Eager Unicode data avoids OLE's delayed clipboard-write callbacks after a revoked request.
    static class ClipboardText
    {
        const uint UnicodeText = 13;
        [DllImport("user32.dll", SetLastError = true)] static extern bool OpenClipboard(IntPtr owner);
        [DllImport("user32.dll")] static extern bool CloseClipboard();
        [DllImport("user32.dll")] static extern bool EmptyClipboard();
        [DllImport("user32.dll")] static extern bool IsClipboardFormatAvailable(uint format);
        [DllImport("user32.dll")] static extern IntPtr GetClipboardData(uint format);
        [DllImport("user32.dll")] static extern IntPtr SetClipboardData(uint format, IntPtr data);
        [DllImport("kernel32.dll")] static extern IntPtr GlobalAlloc(uint flags, UIntPtr bytes);
        [DllImport("kernel32.dll")] static extern IntPtr GlobalLock(IntPtr memory);
        [DllImport("kernel32.dll")] static extern bool GlobalUnlock(IntPtr memory);
        [DllImport("kernel32.dll")] static extern UIntPtr GlobalSize(IntPtr memory);
        [DllImport("kernel32.dll")] static extern IntPtr GlobalFree(IntPtr memory);
        public static string Exchange(string text)
        {
            var owner = new NativeWindow();
            owner.CreateHandle(new CreateParams { Caption = "Hyperlink explicit clipboard transfer", Parent = new IntPtr(-3) });
            bool opened = false;
            try
            {
                for (int retry = 0; retry < 5 && !opened; retry++) { opened = OpenClipboard(owner.Handle); if (!opened) Thread.Sleep(20); }
                if (!opened) throw new IOException("Clipboard is busy.");
                if (text == null)
                {
                    if (!IsClipboardFormatAvailable(UnicodeText)) return "";
                    IntPtr memory = GetClipboardData(UnicodeText); if (memory == IntPtr.Zero) throw new IOException("Clipboard text is unavailable.");
                    int length = (int)Math.Min(2050UL, GlobalSize(memory).ToUInt64()); length -= length % 2;
                    if (length < 2) throw new InvalidDataException("Invalid clipboard text.");
                    IntPtr pointer = GlobalLock(memory); if (pointer == IntPtr.Zero) throw new IOException("Cannot read clipboard text.");
                    byte[] bytes = new byte[length]; try { Marshal.Copy(pointer, bytes, 0, length); } finally { GlobalUnlock(memory); }
                    int end = -1; for (int i = 0; i + 1 < bytes.Length; i += 2) if (bytes[i] == 0 && bytes[i + 1] == 0) { end = i; break; }
                    if (end < 0) throw new InvalidDataException("Clipboard text exceeds the draft limit.");
                    string value = new UnicodeEncoding(false, false, true).GetString(bytes, 0, end); SessionExtensions.CheckText(value); return value;
                }
                SessionExtensions.CheckText(text); byte[] encoded = Encoding.Unicode.GetBytes(text + "\0");
                IntPtr allocated = GlobalAlloc(0x42, new UIntPtr((uint)encoded.Length));
                if (allocated == IntPtr.Zero) throw new IOException("Cannot allocate clipboard text.");
                try
                {
                    IntPtr pointer = GlobalLock(allocated); if (pointer == IntPtr.Zero) throw new IOException("Cannot write clipboard text.");
                    try { Marshal.Copy(encoded, 0, pointer, encoded.Length); } finally { GlobalUnlock(allocated); }
                    if (!EmptyClipboard() || SetClipboardData(UnicodeText, allocated) == IntPtr.Zero) throw new IOException("Cannot publish clipboard text.");
                    allocated = IntPtr.Zero; // Windows owns the fully populated buffer after successful publication.
                }
                finally { if (allocated != IntPtr.Zero) GlobalFree(allocated); }
                return "";
            }
            finally { if (opened) CloseClipboard(); owner.DestroyHandle(); }
        }
    }
}
