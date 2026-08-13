Add-Type -ReferencedAssemblies "System.Drawing" -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Drawing;
using System.Drawing.Imaging;

[ComImport, Guid("e357fccd-a995-4576-b01f-234630154e96"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IThumbnailProvider {
    void GetThumbnail(uint cx, out IntPtr hBitmap, out uint pdwAlpha);
}

[ComImport, Guid("b824b49d-22ac-4161-ac8a-9916e8fa3f7f"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IInitializeWithFile {
    void Initialize([MarshalAs(UnmanagedType.LPWStr)] string pszFilePath, uint grfMode);
}

[ComImport, Guid("4f8c2a6e-3b5d-4e7f-9a1c-2d3e4f5a6b7c")]
class ThumbProvider {}

public class Tester {
    [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr h);
    public static void Run(string path, uint cx) {
        try {
            var prov = (IThumbnailProvider)new ThumbProvider();
            ((IInitializeWithFile)prov).Initialize(path, 0);
            IntPtr hbmp; uint alpha;
            prov.GetThumbnail(cx, out hbmp, out alpha);
            if (hbmp != IntPtr.Zero) {
                using (var bmp = Bitmap.FromHbitmap(hbmp)) {
                    string outp = path + ".com_test.png";
                    bmp.Save(outp, ImageFormat.Png);
                    Console.WriteLine("PROVIDER_OK alpha=" + alpha + " saved=" + outp + " size=" + bmp.Width + "x" + bmp.Height);
                }
                DeleteObject(hbmp);
            } else {
                Console.WriteLine("PROVIDER_NULL_HBITMAP");
            }
        } catch (Exception ex) {
            Console.WriteLine("PROVIDER_EXCEPTION: " + ex.Message);
        }
    }
}
'@
[Tester]::Run("C:\Users\Administrator\Desktop\新建文件夹 (3)\4个酒杯研彩.tif", 256)
