using System.Runtime.InteropServices;

namespace EventCameraViewer.UI;

internal readonly record struct CameraInfo(
    string Serial,
    string Product,
    string Manufacturer)
{
    public string DisplayName => string.IsNullOrWhiteSpace(Product)
        ? Serial
        : $"{Product}  [{Serial}]";
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
internal struct EventCoreStatus
{
    public int State;
    public int Width;
    public int Height;
    public int BiasDiffOff;
    public int BiasDiffOn;
    public ulong TotalEvents;
    public double EventsPerSecond;
    public ulong FrameId;
}

internal static class NativeMethods
{
    public const int SlotCount = 20;
    private const string LibraryName = "EventCore.dll";

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    private static extern int ec_initialize();

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    private static extern void ec_shutdown();

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    private static extern int ec_refresh_cameras(IntPtr buffer, int bufferCapacity);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    private static extern int ec_start_camera_slot(
        int slot,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string serial,
        int biasDiffOff,
        int biasDiffOn);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    private static extern void ec_stop_camera_slot(int slot);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    private static extern void ec_stop_all_cameras();

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    private static extern int ec_set_display_settings(
        int width,
        int height,
        int framesPerSecond);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    private static extern int ec_apply_slot_bias(int slot, int biasDiffOff, int biasDiffOn);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    private static extern int ec_get_slot_status(int slot, out EventCoreStatus status);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    private static extern int ec_copy_slot_frame(
        int slot,
        IntPtr destination,
        int destinationCapacity,
        int destinationStride,
        out ulong frameId);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    private static extern int ec_get_slot_error(int slot, IntPtr buffer, int bufferCapacity);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    private static extern int ec_get_last_error(IntPtr buffer, int bufferCapacity);

    public static void Initialize()
    {
        if (Marshal.SizeOf<EventCoreStatus>() != 48)
            throw new InvalidOperationException("C# 与 C++ 状态结构体布局不一致。");
        if (ec_initialize() == 0)
            throw new InvalidOperationException(GetLastError());
    }

    public static void Shutdown() => ec_shutdown();

    public static IReadOnlyList<CameraInfo> RefreshCameras()
    {
        int required = ec_refresh_cameras(IntPtr.Zero, 0);
        if (required < 0)
            throw new InvalidOperationException(GetLastError());
        if (required <= 1)
            return [];

        IntPtr buffer = Marshal.AllocHGlobal(required);
        try
        {
            if (ec_refresh_cameras(buffer, required) < 0)
                throw new InvalidOperationException(GetLastError());
            string text = Marshal.PtrToStringUTF8(buffer) ?? string.Empty;
            return text.Split('\n', StringSplitOptions.RemoveEmptyEntries)
                .Select(line => line.Split('\t'))
                .Where(parts => parts.Length > 0 && !string.IsNullOrWhiteSpace(parts[0]))
                .Select(parts => new CameraInfo(
                    parts[0],
                    parts.Length > 1 ? parts[1] : string.Empty,
                    parts.Length > 2 ? parts[2] : string.Empty))
                .ToArray();
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    public static void StartCamera(int slot, string serial, int biasDiffOff, int biasDiffOn)
    {
        if (ec_start_camera_slot(slot, serial, biasDiffOff, biasDiffOn) == 0)
            throw new InvalidOperationException(GetSlotError(slot));
    }

    public static void StopCamera(int slot) => ec_stop_camera_slot(slot);
    public static void StopAllCameras() => ec_stop_all_cameras();

    public static void SetDisplaySettings(int width, int height, int framesPerSecond)
    {
        if (ec_set_display_settings(width, height, framesPerSecond) == 0)
            throw new InvalidOperationException(GetLastError());
    }

    public static void ApplyBias(int slot, int biasDiffOff, int biasDiffOn)
    {
        if (ec_apply_slot_bias(slot, biasDiffOff, biasDiffOn) == 0)
            throw new InvalidOperationException(GetSlotError(slot));
    }

    public static EventCoreStatus GetStatus(int slot)
    {
        if (ec_get_slot_status(slot, out EventCoreStatus status) == 0)
            throw new InvalidOperationException($"无法读取槽位 {slot + 1} 的状态。");
        return status;
    }

    public static bool CopyLatestFrame(
        int slot,
        IntPtr destination,
        int destinationCapacity,
        int destinationStride,
        out ulong frameId) =>
        ec_copy_slot_frame(
            slot, destination, destinationCapacity, destinationStride, out frameId) != 0;

    public static string GetSlotError(int slot) => ReadNativeString(
        (buffer, capacity) => ec_get_slot_error(slot, buffer, capacity),
        $"槽位 {slot + 1} 返回了未知错误。");

    public static string GetLastError() => ReadNativeString(
        ec_get_last_error,
        "C++ 核心返回了未知错误。");

    private static string ReadNativeString(
        Func<IntPtr, int, int> nativeCall,
        string fallback)
    {
        int required = nativeCall(IntPtr.Zero, 0);
        if (required <= 1)
            return fallback;
        IntPtr buffer = Marshal.AllocHGlobal(required);
        try
        {
            nativeCall(buffer, required);
            return Marshal.PtrToStringUTF8(buffer) ?? fallback;
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }
}
