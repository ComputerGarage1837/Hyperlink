using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
using System.Threading;
using Microsoft.Win32.SafeHandles;

namespace Hyperlink
{
    /** Owner-only large anonymous pipes; only the three child stdio handles are inherited. */
    sealed class VideoProcess : IDisposable
    {
        [StructLayout(LayoutKind.Sequential)] struct Security { public int Size; public IntPtr Descriptor; [MarshalAs(UnmanagedType.Bool)] public bool Inherit; }
        [StructLayout(LayoutKind.Sequential)] struct Startup
        {
            public int Size; public IntPtr Reserved, Desktop, Title;
            public int X, Y, Width, Height, XChars, YChars, Fill, Flags;
            public short Show, ReservedSize; public IntPtr ReservedBytes, Input, Output, Error;
        }
        [StructLayout(LayoutKind.Sequential)] struct StartupEx { public Startup Startup; public IntPtr Attributes; }
        [StructLayout(LayoutKind.Sequential)] struct Information { public IntPtr Process, Thread; public int Id, ThreadId; }
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool CreatePipe(out SafeFileHandle read, out SafeFileHandle write, ref Security security, int size);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetHandleInformation(SafeFileHandle handle, int mask, int value);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool InitializeProcThreadAttributeList(IntPtr list, int count, int flags, ref IntPtr size);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool UpdateProcThreadAttribute(IntPtr list, int flags, IntPtr attribute, IntPtr value, IntPtr size, IntPtr previous, IntPtr returned);
        [DllImport("kernel32.dll")] static extern void DeleteProcThreadAttributeList(IntPtr list);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool CreateProcess(string file, StringBuilder command, IntPtr processSecurity, IntPtr threadSecurity, bool inherit, int flags, IntPtr environment, string directory, ref StartupEx startup, out Information information);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateProcess(IntPtr process, int code);
        [DllImport("kernel32.dll")] static extern int WaitForSingleObject(IntPtr handle, int milliseconds);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
        [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool ConvertStringSecurityDescriptorToSecurityDescriptor(string text, int revision, out IntPtr descriptor, out int size);
        [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr pointer);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool QueryFullProcessImageName(IntPtr process, int flags, StringBuilder path, ref int length);
        IntPtr handle; readonly string executable;
        internal readonly StreamWriter StandardInput; internal readonly StreamReader StandardOutput, StandardError;
        internal VideoProcess(string file, string arguments)
        {
            executable = Path.GetFullPath(file);
            SafeFileHandle inputRead = null, inputWrite = null, outputRead = null, outputWrite = null, errorRead = null, errorWrite = null;
            IntPtr descriptor = IntPtr.Zero, attributes = IntPtr.Zero, values = IntPtr.Zero; bool initialized = false;
            try
            {
                string sid; using (var identity = WindowsIdentity.GetCurrent()) sid = identity.User.Value;
                int descriptorSize;
                Require(ConvertStringSecurityDescriptorToSecurityDescriptor("D:P(A;;GA;;;SY)(A;;GA;;;" + sid + ")", 1, out descriptor, out descriptorSize));
                var security = new Security { Size = Marshal.SizeOf(typeof(Security)), Descriptor = descriptor, Inherit = true };
                Require(CreatePipe(out inputRead, out inputWrite, ref security, 65536));
                Require(CreatePipe(out outputRead, out outputWrite, ref security, 1048576));
                Require(CreatePipe(out errorRead, out errorWrite, ref security, 65536));
                Require(SetHandleInformation(inputWrite, 1, 0)); Require(SetHandleInformation(outputRead, 1, 0)); Require(SetHandleInformation(errorRead, 1, 0));
                IntPtr size = IntPtr.Zero; InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref size);
                if (size == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
                attributes = Marshal.AllocHGlobal(size); Require(InitializeProcThreadAttributeList(attributes, 1, 0, ref size)); initialized = true;
                values = Marshal.AllocHGlobal(3 * IntPtr.Size);
                Marshal.WriteIntPtr(values, 0, inputRead.DangerousGetHandle()); Marshal.WriteIntPtr(values, IntPtr.Size, outputWrite.DangerousGetHandle()); Marshal.WriteIntPtr(values, 2 * IntPtr.Size, errorWrite.DangerousGetHandle());
                Require(UpdateProcThreadAttribute(attributes, 0, new IntPtr(0x20002), values, new IntPtr(3 * IntPtr.Size), IntPtr.Zero, IntPtr.Zero));
                var startup = new StartupEx { Attributes = attributes, Startup = new Startup { Size = Marshal.SizeOf(typeof(StartupEx)), Flags = 0x101, Show = 0,
                    Input = inputRead.DangerousGetHandle(), Output = outputWrite.DangerousGetHandle(), Error = errorWrite.DangerousGetHandle() } };
                Information information;
                Require(CreateProcess(executable, new StringBuilder("\"" + executable + "\" " + arguments), IntPtr.Zero, IntPtr.Zero, true, 0x08080000, IntPtr.Zero, Path.GetDirectoryName(executable), ref startup, out information));
                handle = information.Process; CloseHandle(information.Thread);
                inputRead.Dispose(); inputRead = null; outputWrite.Dispose(); outputWrite = null; errorWrite.Dispose(); errorWrite = null;
                StandardInput = new StreamWriter(new FileStream(inputWrite, FileAccess.Write, 65536, false), new UTF8Encoding(false)); inputWrite = null;
                StandardOutput = new StreamReader(new FileStream(outputRead, FileAccess.Read, 1048576, false), Encoding.UTF8, false, 4096); outputRead = null;
                StandardError = new StreamReader(new FileStream(errorRead, FileAccess.Read, 65536, false), Encoding.UTF8, false, 4096); errorRead = null;
            }
            catch { if (handle != IntPtr.Zero) { TerminateProcess(handle, 1); CloseHandle(handle); handle = IntPtr.Zero; } throw; }
            finally
            {
                if (initialized) DeleteProcThreadAttributeList(attributes); if (attributes != IntPtr.Zero) Marshal.FreeHGlobal(attributes); if (values != IntPtr.Zero) Marshal.FreeHGlobal(values); if (descriptor != IntPtr.Zero) LocalFree(descriptor);
                foreach (var pipe in new[] { inputRead, inputWrite, outputRead, outputWrite, errorRead, errorWrite }) if (pipe != null) pipe.Dispose();
            }
        }
        static void Require(bool success) { if (!success) throw new Win32Exception(Marshal.GetLastWin32Error()); }
        internal bool HasExited { get { return handle == IntPtr.Zero || WaitForSingleObject(handle, 0) == 0; } }
        internal bool WaitForExit(int milliseconds) { return handle == IntPtr.Zero || WaitForSingleObject(handle, milliseconds) == 0; }
        internal void Kill()
        {
            if (HasExited) return; var path = new StringBuilder(32768); int length = path.Capacity;
            Require(QueryFullProcessImageName(handle, 0, path, ref length));
            if (!string.Equals(Path.GetFullPath(path.ToString()), executable, StringComparison.OrdinalIgnoreCase)) throw new IOException("Video child identity changed.");
            Require(TerminateProcess(handle, 1));
        }
        internal void DrainErrors() { new Thread(delegate() { try { while (StandardError.ReadLine() != null) { } } catch (IOException) { } catch (ObjectDisposedException) { } }) { IsBackground = true, Name = "Hyperlink video diagnostics" }.Start(); }
        public void Dispose()
        {
            if (handle != IntPtr.Zero) { if (!HasExited) Kill(); WaitForExit(2000); CloseHandle(handle); handle = IntPtr.Zero; }
            if (StandardInput != null) StandardInput.Dispose(); if (StandardOutput != null) StandardOutput.Dispose(); if (StandardError != null) StandardError.Dispose();
        }
    }
}
