using System.Runtime.InteropServices;

namespace EventCameraUdpViewer.UI;

[StructLayout(LayoutKind.Sequential, Pack = 8)]
internal struct EventCoreStatus
{
    public int State;
    public int Width;
    public int Height;
    public int ListenPort;
    public ulong TotalEvents;
    public ulong ReceivedPackets;
    public ulong LostPackets;
    public ulong OutOfOrderPackets;
    public ulong InvalidPackets;
    public ulong IncompleteWindows;
    public double EventsPerSecond;
    public ulong FrameId;
}

internal static class NativeMethods
{
    public const int SlotCount = 20;
    public const int DisplayModeRaw = 0;
    public const int DisplayModeFiltered = 1;
    private const string LibraryName = "EventCore.dll";

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    private static extern int ec_initialize();

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    private static extern void ec_shutdown();

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    private static extern int ec_set_network_settings(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string bindAddress,
        int basePort);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    private static extern int ec_set_display_settings(
        int width,
        int height,
        int framesPerSecond);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    private static extern int ec_set_slot_display_mode(int slot, int mode);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    private static extern int ec_start_udp_slot(int slot);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    private static extern void ec_stop_udp_slot(int slot);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    private static extern void ec_stop_all_udp_slots();

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
        if (Marshal.SizeOf<EventCoreStatus>() != 80)
            throw new InvalidOperationException("C# 与 C++ 状态结构体布局不一致。");
        if (ec_initialize() == 0)
            throw new InvalidOperationException(GetLastError());
    }

    public static void Shutdown() => ec_shutdown();

    public static void SetNetworkSettings(string bindAddress, int basePort)
    {
        if (ec_set_network_settings(bindAddress, basePort) == 0)
            throw new InvalidOperationException(GetLastError());
    }

    public static void SetDisplaySettings(int width, int height, int framesPerSecond)
    {
        if (ec_set_display_settings(width, height, framesPerSecond) == 0)
            throw new InvalidOperationException(GetLastError());
    }

    public static void SetSlotDisplayMode(int slot, int mode)
    {
        if (ec_set_slot_display_mode(slot, mode) == 0)
            throw new InvalidOperationException(GetLastError());
    }

    public static void StartSlot(int slot)
    {
        if (ec_start_udp_slot(slot) == 0)
            throw new InvalidOperationException(GetSlotError(slot));
    }

    public static void StopSlot(int slot) => ec_stop_udp_slot(slot);
    public static void StopAllSlots() => ec_stop_all_udp_slots();

    public static EventCoreStatus GetStatus(int slot)
    {
        if (ec_get_slot_status(slot, out EventCoreStatus status) == 0)
            throw new InvalidOperationException($"无法读取UDP槽位 {slot + 1} 的状态。");
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
        $"UDP槽位 {slot + 1} 返回了未知错误。");

    public static string GetLastError() => ReadNativeString(
        ec_get_last_error,
        "UDP核心返回了未知错误。");

    private static string ReadNativeString(
        Func<IntPtr, int, int> nativeCall,
        string fallback)
    {
        int required = nativeCall(IntPtr.Zero, 0);
        if (required <= 1) return fallback;
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
