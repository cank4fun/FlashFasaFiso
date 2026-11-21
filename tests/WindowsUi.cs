using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;

public static class WindowsUi
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)]
    public struct Point { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetLastActivePopup(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr window, int id);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr window, ref Point point);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr window, uint command);
    [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr window, uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hgt, uint flags);
    [DllImport("user32.dll")] public static extern bool UpdateWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr process, uint flags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr window, IntPtr after, string cls, string name);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr SendMessageTimeout(
        IntPtr window, uint message, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageTimeoutW")]
    private static extern IntPtr SendTextTimeout(IntPtr window, uint message, IntPtr w, string text,
        uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] private static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr window, StringBuilder text, int capacity);
    [DllImport("dwmapi.dll")] private static extern int DwmGetWindowAttribute(IntPtr window, int attribute, out int value, int size);

    public static string Caption(IntPtr window)
    {
        var title = new StringBuilder(512);
        GetWindowText(window, title, title.Capacity);
        return title.ToString();
    }

    public static int CornerPreference(IntPtr window)
    {
        int value;
        return DwmGetWindowAttribute(window, 33, out value, sizeof(int)) >= 0 ? value : -1;
    }

    public static IntPtr Send(IntPtr window, uint message, int w, int l)
    {
        IntPtr result;
        if (SendMessageTimeout(window, message, (IntPtr)w, (IntPtr)l, 2, 4000, out result) == IntPtr.Zero)
            throw new InvalidOperationException("Timeout or invalid HWND for message " + message);
        return result;
    }

    public static void Query(IntPtr window, string text)
    {
        IntPtr result;
        if (SendTextTimeout(window, 0x000C, IntPtr.Zero, text, 2, 4000, out result) == IntPtr.Zero)
            throw new InvalidOperationException("Cannot set search text");
    }

    public static int XY(int x, int y) { return (x & 0xffff) | ((y & 0xffff) << 16); }

    public static void Capture(IntPtr window, string path)
    {
        Rect rect;
        if (!GetWindowRect(window, out rect)) throw new InvalidOperationException("Cannot get window rectangle");
        using (var image = new Bitmap(rect.Right - rect.Left, rect.Bottom - rect.Top))
        using (var graphics = Graphics.FromImage(image))
        {
            var dc = graphics.GetHdc();
            try
            {
                if (!PrintWindow(window, dc, 2)) throw new InvalidOperationException("PrintWindow failed");
            }
            finally { graphics.ReleaseHdc(dc); }
            image.Save(path, ImageFormat.Png);
        }
    }
}
