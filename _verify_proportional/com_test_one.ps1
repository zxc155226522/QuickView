Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

[ComImport, Guid("B824B49D-22AC-4161-AC8A-9916E8FA3F7F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IInitializeWithFile {
    [PreserveSig] int Initialize([MarshalAs(UnmanagedType.LPWStr)] string pszFilePath, uint grfMode);
}

public enum WTS_ALPHATYPE { WTSAT_UNKNOWN = 0, WTSAT_RGB = 1, WTSAT_ARGB = 2 }

[ComImport, Guid("E357FCCD-A995-4576-B01F-234630154E96"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IThumbnailProvider {
    [PreserveSig] int GetThumbnail(uint cx, out IntPtr phbmp, out WTS_ALPHATYPE pdwAlpha);
}

public class ThumbTester {
    public static int Test(string path) {
        Type t = Type.GetTypeFromCLSID(new Guid("4F8C2A6E-3B5D-4E7F-9A1C-2D3E4F5A6B7C"));
        object obj = Activator.CreateInstance(t);
        IInitializeWithFile init = (IInitializeWithFile)obj;
        int hr = init.Initialize(path, 0);
        Console.WriteLine("Initialize hr=0x{0:X8}", hr);
        if (hr != 0) return hr;
        IThumbnailProvider thumb = (IThumbnailProvider)obj;
        IntPtr hbmp;
        WTS_ALPHATYPE alpha;
        hr = thumb.GetThumbnail(256, out hbmp, out alpha);
        Console.WriteLine("GetThumbnail hr=0x{0:X8} hbmp=0x{1:X} alpha={2}", hr, hbmp.ToInt64(), alpha);
        if (hr == 0 && hbmp != IntPtr.Zero) {
            DeleteObject(hbmp);
        }
        Marshal.ReleaseComObject(obj);
        return hr;
    }
    [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr hObject);
}
"@

$path = 'C:\Users\Administrator\Desktop\新建文件夹\持枪小人天使2(1)(1).tif'
[ThumbTester]::Test($path)
