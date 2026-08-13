Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

[StructLayout(LayoutKind.Sequential)]
public struct SIZE { public int cx; public int cy; }

[ComImport, Guid("43826d1e-e718-42ee-bc55-a1e261c37bfe"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IShellItem {
    [PreserveSig] int BindToHandler(IntPtr pbc, [MarshalAs(UnmanagedType.LPStruct)] Guid bhid, [MarshalAs(UnmanagedType.LPStruct)] Guid riid, out IntPtr ppv);
    [PreserveSig] int GetParent(out IShellItem ppsi);
    [PreserveSig] int GetDisplayName(uint sigdnName, [MarshalAs(UnmanagedType.LPWStr)] out string ppszName);
    [PreserveSig] int GetAttributes(uint sfgaoMask, out uint psfgaoAttribs);
    [PreserveSig] int Compare(IShellItem psi, uint hint, out int piOrder);
}

[ComImport, Guid("bcc18b79-ba16-442f-80c4-8a59c30c463b"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IShellItemImageFactory {
    [PreserveSig] int GetImage(SIZE size, uint flags, out IntPtr phbm);
}

public class ShellThumbTester {
    static readonly Guid CLSID_ShellItem = new Guid("43826d1e-e718-42ee-bc55-a1e261c37bfe");
    static readonly Guid BHID_ImageFactory = new Guid("FFE31DBB-2E48-4c25-8340-49381D95ABE9");
    static readonly Guid IID_IShellItemImageFactory = new Guid("bcc18b79-ba16-442f-80c4-8a59c30c463b");

    [DllImport("shell32.dll", CharSet=CharSet.Unicode, PreserveSig=true)]
    static extern int SHCreateItemFromParsingName([MarshalAs(UnmanagedType.LPWStr)] string pszPath, IntPtr pbc, [MarshalAs(UnmanagedType.LPStruct)] Guid riid, out IShellItem ppv);

    [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr hObject);
    [DllImport("gdi32.dll")] static extern int GetObject(IntPtr hgdiobj, int cb, out BITMAP lpvObject);

    [StructLayout(LayoutKind.Sequential)]
    struct BITMAP {
        public int bmType; public int bmWidth; public int bmHeight;
        public int bmWidthBytes; public short bmPlanes; public short bmBitsPixel; public IntPtr bmBits;
    }

    public static int Test(string path) {
        IShellItem item;
        int hr = SHCreateItemFromParsingName(path, IntPtr.Zero, CLSID_ShellItem, out item);
        Console.WriteLine("SHCreateItem hr=0x{0:X8}", hr);
        if (hr != 0) return hr;
        IntPtr pv;
        hr = item.BindToHandler(IntPtr.Zero, BHID_ImageFactory, IID_IShellItemImageFactory, out pv);
        Console.WriteLine("BindToHandler hr=0x{0:X8}", hr);
        if (hr != 0) return hr;
        IShellItemImageFactory factory = (IShellItemImageFactory)Marshal.GetObjectForIUnknown(pv);
        IntPtr hbm;
        SIZE sz = new SIZE { cx = 256, cy = 256 };
        hr = factory.GetImage(sz, 0, out hbm);
        Console.WriteLine("GetImage hr=0x{0:X8} hbm=0x{1:X}", hr, hbm.ToInt64());
        if (hr == 0 && hbm != IntPtr.Zero) {
            BITMAP bm;
            GetObject(hbm, Marshal.SizeOf(typeof(BITMAP)), out bm);
            Console.WriteLine("bitmap {0}x{1} {2}bpp", bm.bmWidth, bm.bmHeight, bm.bmBitsPixel);
            DeleteObject(hbm);
        }
        Marshal.ReleaseComObject(factory);
        Marshal.ReleaseComObject(item);
        return hr;
    }
}
"@

$path = 'C:\Users\Administrator\Desktop\新建文件夹\持枪小人天使2(1)(1).tif'
[ShellThumbTester]::Test($path)
