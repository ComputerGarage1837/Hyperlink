using System;
using System.Runtime.InteropServices;

namespace Hyperlink
{
    /** Match GDI monitor names to the default DXGI adapter's actual output index. */
    static class DesktopOutput
    {
        [StructLayout(LayoutKind.Sequential)] struct Rect { public int Left, Top, Right, Bottom; }
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct Description
        {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Name;
            public Rect Bounds; [MarshalAs(UnmanagedType.Bool)] public bool Attached;
            public int Rotation; public IntPtr Monitor;
        }
        [DllImport("dxgi.dll")] static extern int CreateDXGIFactory1(ref Guid iid, out IntPtr factory);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate int Enumerate(IntPtr self, uint index, out IntPtr item);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate int Describe(IntPtr self, out Description description);
        static T Method<T>(IntPtr instance, int slot) where T : class
        { return (T)(object)Marshal.GetDelegateForFunctionPointer(Marshal.ReadIntPtr(Marshal.ReadIntPtr(instance), slot * IntPtr.Size), typeof(T)); }
        internal static int Index(string monitor)
        {
            IntPtr factory = IntPtr.Zero, adapter = IntPtr.Zero; var iid = new Guid("770aae78-f26f-4dba-a829-253c83d1b387");
            try
            {
                Marshal.ThrowExceptionForHR(CreateDXGIFactory1(ref iid, out factory));
                Marshal.ThrowExceptionForHR(Method<Enumerate>(factory, 12)(factory, 0, out adapter));
                for (uint index = 0; index < 32; index++)
                {
                    IntPtr output; int result = Method<Enumerate>(adapter, 7)(adapter, index, out output);
                    if (result != 0) break;
                    try { Description description; Marshal.ThrowExceptionForHR(Method<Describe>(output, 7)(output, out description)); if (description.Attached && string.Equals(description.Name, monitor, StringComparison.OrdinalIgnoreCase)) return (int)index; }
                    finally { Marshal.Release(output); }
                }
                throw new InvalidOperationException("This monitor is not on the default GPU capture adapter.");
            }
            finally { if (adapter != IntPtr.Zero) Marshal.Release(adapter); if (factory != IntPtr.Zero) Marshal.Release(factory); }
        }
    }
}
