using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace EventCameraUdpViewer.UI;

public partial class MainWindow : Window
{
    private sealed record ResolutionOption(int Width, int Height)
    {
        public string DisplayName => $"{Width} × {Height}";
    }

    private sealed record FrameRateOption(int FramesPerSecond)
    {
        public string DisplayName => $"{FramesPerSecond} FPS";
    }

    private sealed record DisplayContentOption(int Mode, string DisplayName);

    private sealed class UdpTile
    {
        public required int Slot { get; init; }
        public required string Name { get; init; }
        public required Border Border { get; init; }
        public required Image Image { get; init; }
        public required TextBlock PortText { get; init; }
        public required TextBlock StatusText { get; init; }
        public WriteableBitmap? Bitmap { get; set; }
        public ulong LastFrameId { get; set; } = ulong.MaxValue;
        public bool Running { get; set; }
    }

    private const int OverviewRowCount = 5;
    private const int OverviewColumnCount = 4;
    private const int CameraCountPerGroup = 2;
    private const int GroupCount = NativeMethods.SlotCount / CameraCountPerGroup;
    private const int DefaultBasePort = 50000;
    private const int DefaultWidth = 1280;
    private const int DefaultHeight = 720;
    private const int DefaultFramesPerSecond = 60;

    private static readonly ResolutionOption[] ResolutionOptions =
    [
        new(320, 180),
        new(480, 270),
        new(640, 360),
        new(1280, 720)
    ];

    private static readonly FrameRateOption[] FrameRateOptions =
    [
        new(30),
        new(45),
        new(60)
    ];

    private static readonly DisplayContentOption[] DisplayContentOptions =
    [
        new(NativeMethods.DisplayModeRaw, "原始事件"),
        new(NativeMethods.DisplayModeFiltered, "滤波事件")
    ];

    private readonly DispatcherTimer _frameTimer;
    private readonly UdpTile[] _tiles = new UdpTile[NativeMethods.SlotCount];
    private readonly int[] _slotDisplayModes = new int[NativeMethods.SlotCount];
    private bool _nativeInitialized;
    private bool _busy;
    private bool _updatingDisplaySettings;
    private bool _updatingDisplayContent;
    private int _selectedSlot;
    private int? _focusedSlot;
    private string _bindAddress = "0.0.0.0";
    private int _basePort = DefaultBasePort;
    private int _previewWidth = DefaultWidth;
    private int _previewHeight = DefaultHeight;
    private int _targetFramesPerSecond = DefaultFramesPerSecond;

    public MainWindow()
    {
        InitializeComponent();
        CreateUdpTiles();

        ResolutionComboBox.ItemsSource = ResolutionOptions;
        FrameRateComboBox.ItemsSource = FrameRateOptions;
        UpdateDisplaySettingControls();

        SlotComboBox.ItemsSource = Enumerable.Range(0, NativeMethods.SlotCount)
            .Select(GetSlotName)
            .ToArray();
        DisplayContentComboBox.ItemsSource = DisplayContentOptions;
        SlotComboBox.SelectedIndex = 0;
        UpdateDisplayContentControl();
        UpdatePortLabels();

        _frameTimer = new DispatcherTimer(DispatcherPriority.Render)
        {
            Interval = TimeSpan.FromSeconds(1.0 / _targetFramesPerSecond)
        };
        _frameTimer.Tick += FrameTimer_Tick;
        Loaded += MainWindow_Loaded;
    }

    private void CreateUdpTiles()
    {
        for (int row = 0; row < OverviewRowCount; ++row)
            OverviewGrid.RowDefinitions.Add(new RowDefinition());
        for (int column = 0; column < OverviewColumnCount; ++column)
            OverviewGrid.ColumnDefinitions.Add(new ColumnDefinition());

        for (int slot = 0; slot < NativeMethods.SlotCount; ++slot)
        {
            var image = new Image
            {
                Stretch = Stretch.Uniform,
                SnapsToDevicePixels = true
            };
            RenderOptions.SetBitmapScalingMode(image, BitmapScalingMode.HighQuality);

            var name = new TextBlock
            {
                Text = GetSlotName(slot),
                FontWeight = FontWeights.SemiBold,
                Foreground = Brushes.White,
                Margin = new Thickness(5, 2, 5, 2)
            };
            var port = new TextBlock
            {
                Foreground = new SolidColorBrush(Color.FromRgb(145, 160, 180)),
                HorizontalAlignment = HorizontalAlignment.Right,
                Margin = new Thickness(5, 2, 5, 2)
            };
            var status = new TextBlock
            {
                Text = "● 未监听",
                Foreground = Brushes.Gray,
                Margin = new Thickness(5, 2, 5, 2),
                VerticalAlignment = VerticalAlignment.Bottom
            };

            var top = new Grid
            {
                Background = new SolidColorBrush(Color.FromArgb(180, 16, 19, 24)),
                VerticalAlignment = VerticalAlignment.Top
            };
            top.Children.Add(name);
            top.Children.Add(port);

            var content = new Grid { Background = new SolidColorBrush(Color.FromRgb(28, 28, 28)) };
            content.Children.Add(image);
            content.Children.Add(top);
            content.Children.Add(status);

            var border = new Border
            {
                Margin = new Thickness(2),
                BorderThickness = new Thickness(1),
                BorderBrush = new SolidColorBrush(Color.FromRgb(59, 65, 75)),
                Background = new SolidColorBrush(Color.FromRgb(28, 28, 28)),
                Child = content,
                Cursor = Cursors.Hand
            };
            int capturedSlot = slot;
            border.MouseLeftButtonUp += (_, _) => ToggleFocus(capturedSlot);
            Grid.SetRow(border, slot / OverviewColumnCount);
            Grid.SetColumn(border, slot % OverviewColumnCount);
            OverviewGrid.Children.Add(border);

            _tiles[slot] = new UdpTile
            {
                Slot = slot,
                Name = GetSlotName(slot),
                Border = border,
                Image = image,
                PortText = port,
                StatusText = status
            };
        }
    }

    private async void MainWindow_Loaded(object sender, RoutedEventArgs e)
    {
        try
        {
            NativeMethods.Initialize();
            NativeMethods.SetNetworkSettings(_bindAddress, _basePort);
            NativeMethods.SetDisplaySettings(
                _previewWidth, _previewHeight, _targetFramesPerSecond);
            _nativeInitialized = true;
            _frameTimer.Start();
            MessageTextBlock.Text =
                "UDP核心初始化成功。设置发送端目标IP后，单击“全部监听”即可接收。";
            UpdateControlAvailability();
            await Task.CompletedTask;
        }
        catch (DllNotFoundException)
        {
            ShowFatal("没有找到 EventCore.dll。请先运行“构建_UDP上位机.cmd”。");
        }
        catch (BadImageFormatException)
        {
            ShowFatal("C#界面与C++核心位数不一致，必须都使用x64版本。");
        }
        catch (Exception error)
        {
            ShowFatal(error.Message);
        }
    }

    private void ApplyNetworkButton_Click(object sender, RoutedEventArgs e)
    {
        if (!_nativeInitialized || _busy) return;
        if (_tiles.Any(tile => tile.Running))
        {
            ShowError("请先停止全部UDP槽位，再修改绑定地址或端口。");
            return;
        }
        string address = BindAddressTextBox.Text.Trim();
        if (!int.TryParse(BasePortTextBox.Text.Trim(), out int basePort) ||
            basePort <= 0 || basePort + GroupCount - 1 > 65535)
        {
            ShowError("起始端口必须保证10个连续分组端口都处于1～65535范围内。");
            return;
        }
        try
        {
            NativeMethods.SetNetworkSettings(address, basePort);
            _bindAddress = address;
            _basePort = basePort;
            UpdatePortLabels();
            MessageTextBlock.Text =
                $"UEVT V2映射已更新：{address}:{basePort}～{basePort + GroupCount - 1}，每端口按camera_id分两路。";
        }
        catch (Exception error)
        {
            ShowError(error.Message);
        }
    }

    private async void StartAllButton_Click(object sender, RoutedEventArgs e)
    {
        if (!_nativeInitialized || _busy) return;
        SetBusy(true, "正在绑定10个UEVT V2分组端口…");
        try
        {
            List<string> errors = await Task.Run(() =>
            {
                var result = new List<string>();
                for (int slot = 0; slot < NativeMethods.SlotCount; ++slot)
                {
                    try { NativeMethods.StartSlot(slot); }
                    catch (Exception error) { result.Add($"{GetSlotName(slot)}：{error.Message}"); }
                }
                return result;
            });
            RefreshRunningStates();
            MessageTextBlock.Text = errors.Count == 0
                ? $"已监听UEVT V2端口 {_basePort}～{_basePort + GroupCount - 1}。"
                : $"已启动部分UDP槽位；{errors.Count}路失败。";
            if (errors.Count > 0) ShowError(string.Join(Environment.NewLine, errors));
        }
        finally
        {
            SetBusy(false);
        }
    }

    private async void StopAllButton_Click(object sender, RoutedEventArgs e)
    {
        if (!_nativeInitialized || _busy) return;
        SetBusy(true, "正在停止全部UDP监听…");
        try
        {
            await Task.Run(NativeMethods.StopAllSlots);
            foreach (UdpTile tile in _tiles) SetTileStopped(tile);
            MessageTextBlock.Text = "全部UDP槽位已停止。";
        }
        finally
        {
            SetBusy(false);
        }
    }

    private async void StartSelectedButton_Click(object sender, RoutedEventArgs e)
    {
        if (!_nativeInitialized || _busy) return;
        UdpTile tile = _tiles[_selectedSlot];
        SetBusy(true, $"正在监听 {tile.Name} 的UDP端口…");
        try
        {
            await Task.Run(() => NativeMethods.StartSlot(tile.Slot));
            tile.Running = true;
            MessageTextBlock.Text =
                $"{tile.Name} 正在监听 UDP {_basePort + tile.Slot / CameraCountPerGroup}，camera_id={tile.Slot % CameraCountPerGroup + 1}。";
        }
        catch (Exception error)
        {
            ShowError(error.Message);
        }
        finally
        {
            SetBusy(false);
        }
    }

    private async void StopSelectedButton_Click(object sender, RoutedEventArgs e)
    {
        if (!_nativeInitialized || _busy) return;
        UdpTile tile = _tiles[_selectedSlot];
        SetBusy(true, $"正在停止 {tile.Name}…");
        try
        {
            await Task.Run(() => NativeMethods.StopSlot(tile.Slot));
            SetTileStopped(tile);
            MessageTextBlock.Text = $"{tile.Name} 已停止监听。";
        }
        finally
        {
            SetBusy(false);
        }
    }

    private void DisplaySettingsComboBox_SelectionChanged(
        object sender,
        SelectionChangedEventArgs e)
    {
        if (_updatingDisplaySettings || !_nativeInitialized || _busy) return;
        if (ResolutionComboBox.SelectedItem is not ResolutionOption resolution ||
            FrameRateComboBox.SelectedItem is not FrameRateOption frameRate) return;

        try
        {
            NativeMethods.SetDisplaySettings(
                resolution.Width, resolution.Height, frameRate.FramesPerSecond);
            _previewWidth = resolution.Width;
            _previewHeight = resolution.Height;
            _targetFramesPerSecond = frameRate.FramesPerSecond;
            _frameTimer.Interval = TimeSpan.FromSeconds(1.0 / _targetFramesPerSecond);
            foreach (UdpTile tile in _tiles) ClearTileFrame(tile);
            FocusImage.Source = null;
            MessageTextBlock.Text =
                $"显示设置已切换为 {_previewWidth}×{_previewHeight}，{_targetFramesPerSecond} FPS。";
        }
        catch (Exception error)
        {
            UpdateDisplaySettingControls();
            ShowError(error.Message);
        }
    }

    private void UpdateDisplaySettingControls()
    {
        _updatingDisplaySettings = true;
        ResolutionComboBox.SelectedItem = ResolutionOptions.First(option =>
            option.Width == _previewWidth && option.Height == _previewHeight);
        FrameRateComboBox.SelectedItem = FrameRateOptions.First(option =>
            option.FramesPerSecond == _targetFramesPerSecond);
        _updatingDisplaySettings = false;
    }

    private void FrameTimer_Tick(object? sender, EventArgs e)
    {
        if (!_nativeInitialized) return;
        int runningCount = 0;
        ulong totalPackets = 0;
        ulong totalLost = 0;

        foreach (UdpTile tile in _tiles)
        {
            try
            {
                EventCoreStatus status = NativeMethods.GetStatus(tile.Slot);
                tile.Running = status.State != 0;
                if (!tile.Running)
                {
                    tile.StatusText.Text = "● 未监听";
                    tile.StatusText.Foreground = Brushes.Gray;
                    continue;
                }

                ++runningCount;
                totalPackets += status.ReceivedPackets;
                totalLost += status.LostPackets;
                tile.StatusText.Text = status.ReceivedPackets == 0
                    ? "● 监听中 · 等待数据"
                    : $"● {status.EventsPerSecond / 1_000_000.0:F2} Mev/s  丢:{status.LostPackets}";
                tile.StatusText.Foreground = status.InvalidPackets == 0
                    ? Brushes.LightGreen
                    : Brushes.Orange;

                if (status.FrameId != tile.LastFrameId)
                    CopyFrame(tile, status);
            }
            catch (Exception error)
            {
                tile.StatusText.Text = "● 接收错误";
                tile.StatusText.Foreground = Brushes.OrangeRed;
                if (tile.Slot == _selectedSlot) MessageTextBlock.Text = error.Message;
            }
        }

        RunningCountTextBlock.Text = $"监听：{runningCount} / {NativeMethods.SlotCount}";
        HeaderStatusTextBlock.Text =
            $"运行 {runningCount}/20 · 收包 {totalPackets:N0} · 丢包 {totalLost:N0}";
        UpdateSelectedStatus();
        UpdateControlAvailability();
    }

    private void CopyFrame(UdpTile tile, EventCoreStatus status)
    {
        if (status.Width <= 0 || status.Height <= 0) return;
        if (tile.Bitmap is null || tile.Bitmap.PixelWidth != status.Width ||
            tile.Bitmap.PixelHeight != status.Height)
        {
            tile.Bitmap = new WriteableBitmap(
                status.Width, status.Height, 96, 96, PixelFormats.Bgr24, null);
            tile.Image.Source = tile.Bitmap;
        }

        tile.Bitmap.Lock();
        try
        {
            int capacity = tile.Bitmap.BackBufferStride * tile.Bitmap.PixelHeight;
            if (!NativeMethods.CopyLatestFrame(
                    tile.Slot,
                    tile.Bitmap.BackBuffer,
                    capacity,
                    tile.Bitmap.BackBufferStride,
                    out ulong frameId)) return;
            tile.Bitmap.AddDirtyRect(new Int32Rect(
                0, 0, tile.Bitmap.PixelWidth, tile.Bitmap.PixelHeight));
            tile.LastFrameId = frameId;
            if (_focusedSlot == tile.Slot) FocusImage.Source = tile.Bitmap;
        }
        finally
        {
            tile.Bitmap.Unlock();
        }
    }

    private void SlotComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (SlotComboBox.SelectedIndex < 0) return;
        SelectSlot(SlotComboBox.SelectedIndex);
    }

    private void DisplayContentComboBox_SelectionChanged(
        object sender,
        SelectionChangedEventArgs e)
    {
        if (_updatingDisplayContent || !_nativeInitialized || _busy) return;
        if (DisplayContentComboBox.SelectedItem is not DisplayContentOption option) return;

        int previousMode = _slotDisplayModes[_selectedSlot];
        try
        {
            NativeMethods.SetSlotDisplayMode(_selectedSlot, option.Mode);
            _slotDisplayModes[_selectedSlot] = option.Mode;
            ClearTileFrame(_tiles[_selectedSlot]);
            if (_focusedSlot == _selectedSlot) FocusImage.Source = null;
            MessageTextBlock.Text = option.Mode == NativeMethods.DisplayModeRaw
                ? $"{_tiles[_selectedSlot].Name} 已切换为原始事件。若 Jetson 只发送滤波事件，原始事件无法在本机恢复。"
                : $"{_tiles[_selectedSlot].Name} 已切换为滤波事件；若收到原始流，本机仅显示 drone 框内事件。";
        }
        catch (Exception error)
        {
            _slotDisplayModes[_selectedSlot] = previousMode;
            UpdateDisplayContentControl();
            ShowError(error.Message);
        }
    }

    private void UpdateDisplayContentControl()
    {
        _updatingDisplayContent = true;
        DisplayContentComboBox.SelectedItem = DisplayContentOptions.First(option =>
            option.Mode == _slotDisplayModes[_selectedSlot]);
        _updatingDisplayContent = false;
    }

    private void SelectSlot(int slot)
    {
        _selectedSlot = slot;
        for (int index = 0; index < _tiles.Length; ++index)
        {
            _tiles[index].Border.BorderBrush = index == slot
                ? Brushes.DodgerBlue
                : new SolidColorBrush(Color.FromRgb(59, 65, 75));
        }
        if (SlotComboBox.SelectedIndex != slot) SlotComboBox.SelectedIndex = slot;
        UpdateDisplayContentControl();
        CurrentPortTextBlock.Text =
            $"UDP {_bindAddress}:{_basePort + _selectedSlot / CameraCountPerGroup} · camera_id={_selectedSlot % CameraCountPerGroup + 1}";
        UpdateSelectedStatus();
        UpdateControlAvailability();
    }

    private void ToggleFocus(int slot)
    {
        SelectSlot(slot);
        if (_focusedSlot == slot)
        {
            ExitFocusMode();
            return;
        }
        _focusedSlot = slot;
        OverviewGrid.Visibility = Visibility.Collapsed;
        FocusBorder.Visibility = Visibility.Visible;
        FocusTitleTextBlock.Text =
            $"{_tiles[slot].Name} · UDP {_basePort + slot / CameraCountPerGroup} · camera_id={slot % CameraCountPerGroup + 1}";
        FocusImage.Source = _tiles[slot].Bitmap;
    }

    private void FocusBorder_MouseLeftButtonUp(object sender, MouseButtonEventArgs e) =>
        ExitFocusMode();

    private void ExitFocusMode()
    {
        _focusedSlot = null;
        FocusImage.Source = null;
        FocusBorder.Visibility = Visibility.Collapsed;
        OverviewGrid.Visibility = Visibility.Visible;
    }

    private void MainWindow_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Escape && _focusedSlot.HasValue)
        {
            ExitFocusMode();
            e.Handled = true;
        }
    }

    private void UpdateSelectedStatus()
    {
        if (!_nativeInitialized)
        {
            SelectedStatusTextBlock.Text = "UDP核心尚未初始化";
            return;
        }
        try
        {
            EventCoreStatus status = NativeMethods.GetStatus(_selectedSlot);
            double denominator = status.ReceivedPackets + status.LostPackets;
            double lossPercent = denominator > 0
                ? status.LostPackets * 100.0 / denominator
                : 0.0;
            SelectedStatusTextBlock.Text =
                $"{(status.State != 0 ? "监听中" : "未监听")}\n" +
                $"端口: {status.ListenPort}\n" +
                $"事件率: {status.EventsPerSecond:N0} event/s\n" +
                $"累计事件: {status.TotalEvents:N0}\n" +
                $"接收包: {status.ReceivedPackets:N0}\n" +
                $"估算丢包: {status.LostPackets:N0} ({lossPercent:F2}%)\n" +
                $"乱序/重复: {status.OutOfOrderPackets:N0}\n" +
                $"无效包: {status.InvalidPackets:N0}\n" +
                $"未完成窗口: {status.IncompleteWindows:N0}\n" +
                $"画面编号: {status.FrameId:N0}";
        }
        catch (Exception error)
        {
            SelectedStatusTextBlock.Text = error.Message;
        }
    }

    private void UpdatePortLabels()
    {
        foreach (UdpTile tile in _tiles)
            tile.PortText.Text =
                $":{_basePort + tile.Slot / CameraCountPerGroup} · ID{tile.Slot % CameraCountPerGroup + 1}";
        CurrentPortTextBlock.Text =
            $"UDP {_bindAddress}:{_basePort + _selectedSlot / CameraCountPerGroup} · camera_id={_selectedSlot % CameraCountPerGroup + 1}";
    }

    private void RefreshRunningStates()
    {
        foreach (UdpTile tile in _tiles)
            tile.Running = NativeMethods.GetStatus(tile.Slot).State != 0;
    }

    private static string GetSlotName(int slot)
    {
        int group = slot / 2 + 1;
        string fieldOfView = slot % 2 == 0 ? "宽视场" : "窄视场";
        return $"第 {group} 组 - {fieldOfView}";
    }

    private static void ClearTileFrame(UdpTile tile)
    {
        tile.LastFrameId = ulong.MaxValue;
        tile.Bitmap = null;
        tile.Image.Source = null;
    }

    private static void SetTileStopped(UdpTile tile)
    {
        tile.Running = false;
        tile.StatusText.Text = "● 未监听";
        tile.StatusText.Foreground = Brushes.Gray;
        ClearTileFrame(tile);
    }

    private void SetBusy(bool busy, string? message = null)
    {
        _busy = busy;
        if (message is not null) MessageTextBlock.Text = message;
        UpdateControlAvailability();
    }

    private void UpdateControlAvailability()
    {
        bool anyRunning = _tiles.Any(tile => tile.Running);
        bool selectedRunning = _tiles[_selectedSlot].Running;
        bool ready = _nativeInitialized && !_busy;

        BindAddressTextBox.IsEnabled = ready && !anyRunning;
        BasePortTextBox.IsEnabled = ready && !anyRunning;
        ApplyNetworkButton.IsEnabled = ready && !anyRunning;
        StartAllButton.IsEnabled = ready;
        StopAllButton.IsEnabled = ready && anyRunning;
        StartSelectedButton.IsEnabled = ready && !selectedRunning;
        StopSelectedButton.IsEnabled = ready && selectedRunning;
        SlotComboBox.IsEnabled = !_busy;
        DisplayContentComboBox.IsEnabled = ready;
        ResolutionComboBox.IsEnabled = ready;
        FrameRateComboBox.IsEnabled = ready;
    }

    private void ShowError(string message)
    {
        MessageTextBlock.Text = message;
        MessageBox.Show(this, message, "UDP上位机", MessageBoxButton.OK, MessageBoxImage.Warning);
    }

    private void ShowFatal(string message)
    {
        MessageTextBlock.Text = message;
        MessageBox.Show(this, message, "UDP上位机启动失败", MessageBoxButton.OK, MessageBoxImage.Error);
        UpdateControlAvailability();
    }

    private void MainWindow_Closing(object? sender, CancelEventArgs e)
    {
        _frameTimer.Stop();
        if (_nativeInitialized)
        {
            NativeMethods.Shutdown();
            _nativeInitialized = false;
        }
    }
}
