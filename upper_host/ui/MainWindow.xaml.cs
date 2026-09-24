using System.ComponentModel;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace EventCameraViewer.UI;

public partial class MainWindow : Window
{
    private const int OverviewRowCount = 5;
    private const int OverviewColumnCount = 4;

    private sealed class CameraTile
    {
        public required int Slot { get; init; }
        public required string Name { get; init; }
        public required Border Border { get; init; }
        public required Image Image { get; init; }
        public required TextBlock SerialText { get; init; }
        public required TextBlock StatusText { get; init; }
        public CameraInfo? Camera { get; set; }
        public WriteableBitmap? Bitmap { get; set; }
        public ulong LastFrameId { get; set; }
        public int BiasOff { get; set; } = 90;
        public int BiasOn { get; set; } = 45;
        public bool Running { get; set; }
    }

    private readonly DispatcherTimer _frameTimer;
    private readonly CameraTile[] _tiles = new CameraTile[NativeMethods.SlotCount];
    private IReadOnlyList<CameraInfo> _cameras = [];
    private int _selectedSlot;
    private int? _focusedSlot;
    private bool _nativeInitialized;
    private bool _busy;
    private bool _updatingControls;

    public MainWindow()
    {
        InitializeComponent();
        CreateCameraTiles();
        SlotComboBox.ItemsSource = Enumerable.Range(0, NativeMethods.SlotCount)
            .Select(GetSlotName)
            .ToArray();
        SlotComboBox.SelectedIndex = 0;

        _frameTimer = new DispatcherTimer(DispatcherPriority.Render)
        {
            Interval = TimeSpan.FromMilliseconds(40)
        };
        _frameTimer.Tick += FrameTimer_Tick;
        Loaded += MainWindow_Loaded;
        Closing += MainWindow_Closing;
    }

    private static string GetSlotName(int slot) =>
        $"第 {slot / 2 + 1} 组 - {(slot % 2 == 0 ? "宽视场" : "窄视场")}";

    private void CreateCameraTiles()
    {
        for (int row = 0; row < OverviewRowCount; row++)
            OverviewGrid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        for (int column = 0; column < OverviewColumnCount; column++)
            OverviewGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        for (int slot = 0; slot < NativeMethods.SlotCount; slot++)
        {
            var image = new Image { Stretch = Stretch.Uniform };
            RenderOptions.SetBitmapScalingMode(image, BitmapScalingMode.NearestNeighbor);
            var title = new TextBlock
            {
                Text = GetSlotName(slot),
                Foreground = Brushes.White,
                FontWeight = FontWeights.SemiBold,
                Margin = new Thickness(5, 2, 5, 0)
            };
            var serial = new TextBlock
            {
                Text = "未分配",
                Foreground = new SolidColorBrush(Color.FromRgb(155, 164, 176)),
                FontSize = 11,
                HorizontalAlignment = HorizontalAlignment.Right,
                Margin = new Thickness(5, 3, 5, 0),
                TextTrimming = TextTrimming.CharacterEllipsis,
                MaxWidth = 240
            };
            var status = new TextBlock
            {
                Text = "● 未启动",
                Foreground = Brushes.Gold,
                FontSize = 11,
                Margin = new Thickness(5, 0, 5, 2),
                VerticalAlignment = VerticalAlignment.Bottom
            };
            var overlay = new Grid { Background = Brushes.Transparent };
            overlay.ColumnDefinitions.Add(new ColumnDefinition());
            overlay.ColumnDefinitions.Add(new ColumnDefinition());
            Grid.SetColumn(title, 0);
            Grid.SetColumn(serial, 1);
            overlay.Children.Add(title);
            overlay.Children.Add(serial);

            var content = new Grid { Background = new SolidColorBrush(Color.FromRgb(28, 28, 28)) };
            content.Children.Add(image);
            content.Children.Add(overlay);
            content.Children.Add(status);

            var border = new Border
            {
                Margin = new Thickness(2),
                BorderThickness = new Thickness(1),
                BorderBrush = new SolidColorBrush(Color.FromRgb(59, 65, 75)),
                Background = new SolidColorBrush(Color.FromRgb(28, 28, 28)),
                Child = content,
                Tag = slot,
                Cursor = Cursors.Hand
            };
            border.MouseLeftButtonDown += CameraTile_MouseLeftButtonDown;
            Grid.SetRow(border, slot / OverviewColumnCount);
            Grid.SetColumn(border, slot % OverviewColumnCount);
            OverviewGrid.Children.Add(border);

            _tiles[slot] = new CameraTile
            {
                Slot = slot,
                Name = GetSlotName(slot),
                Border = border,
                Image = image,
                SerialText = serial,
                StatusText = status
            };
        }
        UpdateSelectionVisuals();
    }

    private async void MainWindow_Loaded(object sender, RoutedEventArgs e)
    {
        try
        {
            NativeMethods.Initialize();
            _nativeInitialized = true;
            MessageTextBlock.Text = "C++ 多相机数据核心初始化成功。";
            _frameTimer.Start();
            await RefreshCamerasAsync();
        }
        catch (DllNotFoundException)
        {
            ShowFatal("未找到 EventCore.dll 或 DVSense SDK 运行库。请先运行构建脚本，并确认 SDK 已安装。");
        }
        catch (BadImageFormatException)
        {
            ShowFatal("C# UI 与 C++ 核心位数不一致，必须都使用 x64 版本。");
        }
        catch (Exception error)
        {
            ShowFatal(error.Message);
        }
    }

    private async void RefreshButton_Click(object sender, RoutedEventArgs e) =>
        await RefreshCamerasAsync();

    private async Task RefreshCamerasAsync()
    {
        if (!_nativeInitialized || _busy) return;
        if (_tiles.Any(tile => tile.Running))
        {
            ShowError("请先停止全部相机，再刷新相机列表。");
            return;
        }

        SetBusy(true, "正在搜索相机并分配到 20 个槽位…");
        try
        {
            _cameras = await Task.Run(NativeMethods.RefreshCameras);
            CameraComboBox.ItemsSource = _cameras;

            var availableBySerial = _cameras.ToDictionary(camera => camera.Serial);
            var used = new HashSet<string>();
            foreach (CameraTile tile in _tiles)
            {
                if (tile.Camera is CameraInfo old &&
                    availableBySerial.TryGetValue(old.Serial, out CameraInfo current))
                {
                    tile.Camera = current;
                    used.Add(current.Serial);
                }
                else
                {
                    tile.Camera = null;
                }
            }
            foreach (CameraTile tile in _tiles.Where(tile => tile.Camera is null))
            {
                CameraInfo? next = _cameras
                    .Where(camera => !used.Contains(camera.Serial))
                    .Select(camera => (CameraInfo?)camera)
                    .FirstOrDefault();
                if (next.HasValue)
                {
                    tile.Camera = next.Value;
                    used.Add(next.Value.Serial);
                }
            }
            foreach (CameraTile tile in _tiles) UpdateTileAssignment(tile);
            LoadSelectedSlotIntoControls();
            CameraCountTextBlock.Text = $"已发现：{_cameras.Count} / 20";
            MessageTextBlock.Text = _cameras.Count == 0
                ? "未发现相机：检查连接，并关闭其他占用相机的软件。"
                : $"发现 {_cameras.Count} 台相机，已按检测顺序分配；请核对宽/窄视场对应关系。";
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

    private async void StartAllButton_Click(object sender, RoutedEventArgs e)
    {
        if (!ValidateUniqueAssignments()) return;
        CameraTile[] assigned = _tiles.Where(tile => tile.Camera.HasValue).ToArray();
        if (assigned.Length == 0)
        {
            ShowError("当前没有已分配的相机，请先刷新相机列表。");
            return;
        }
        SaveBiasForSelectedSlot();
        SetBusy(true, $"正在依次启动 {assigned.Length} 台相机…");
        try
        {
            List<string> errors = await Task.Run(() =>
            {
                var result = new List<string>();
                foreach (CameraTile tile in assigned)
                {
                    try
                    {
                        NativeMethods.StartCamera(
                            tile.Slot, tile.Camera!.Value.Serial, tile.BiasOff, tile.BiasOn);
                    }
                    catch (Exception error)
                    {
                        result.Add($"{tile.Name}: {error.Message}");
                    }
                }
                return result;
            });
            MessageTextBlock.Text = errors.Count == 0
                ? $"已启动 {assigned.Length} 台相机。"
                : $"已完成启动，其中 {errors.Count} 路失败；其他相机继续运行。";
            if (errors.Count > 0)
                MessageBox.Show(this, string.Join(Environment.NewLine, errors),
                    "部分相机启动失败", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
        finally
        {
            SetBusy(false);
        }
    }

    private async void StopAllButton_Click(object sender, RoutedEventArgs e)
    {
        if (!_nativeInitialized || _busy) return;
        SetBusy(true, "正在停止全部相机…");
        try
        {
            await Task.Run(NativeMethods.StopAllCameras);
            MessageTextBlock.Text = "全部相机已停止。";
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

    private async void StartSelectedButton_Click(object sender, RoutedEventArgs e)
    {
        CameraTile tile = _tiles[_selectedSlot];
        if (!tile.Camera.HasValue)
        {
            ShowError("当前槽位未分配相机。");
            return;
        }
        if (!TryReadBias(out int off, out int on)) return;
        CameraTile? conflict = _tiles.FirstOrDefault(other =>
            other.Slot != tile.Slot && other.Running && other.Camera?.Serial == tile.Camera?.Serial);
        if (conflict is not null)
        {
            ShowError($"该序列号正在 {conflict.Name} 中运行。");
            return;
        }
        tile.BiasOff = off;
        tile.BiasOn = on;
        SetBusy(true, $"正在启动 {tile.Name}…");
        try
        {
            await Task.Run(() => NativeMethods.StartCamera(
                tile.Slot, tile.Camera.Value.Serial, off, on));
            tile.LastFrameId = 0;
            MessageTextBlock.Text = $"{tile.Name} 已启动。";
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
        CameraTile tile = _tiles[_selectedSlot];
        SetBusy(true, $"正在停止 {tile.Name}…");
        try
        {
            await Task.Run(() => NativeMethods.StopCamera(tile.Slot));
            MessageTextBlock.Text = $"{tile.Name} 已停止。";
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

    private async void ApplyBiasButton_Click(object sender, RoutedEventArgs e)
    {
        if (!TryReadBias(out int off, out int on)) return;
        CameraTile tile = _tiles[_selectedSlot];
        tile.BiasOff = off;
        tile.BiasOn = on;
        if (!tile.Running)
        {
            MessageTextBlock.Text = $"{tile.Name} 的 Bias 已保存，将在启动时应用。";
            return;
        }
        SetBusy(true, $"正在应用 {tile.Name} 的 Bias…");
        try
        {
            await Task.Run(() => NativeMethods.ApplyBias(tile.Slot, off, on));
            EventCoreStatus status = NativeMethods.GetStatus(tile.Slot);
            tile.BiasOff = status.BiasDiffOff;
            tile.BiasOn = status.BiasDiffOn;
            LoadSelectedSlotIntoControls();
            MessageTextBlock.Text =
                $"{tile.Name} Bias 已应用并回读：off={status.BiasDiffOff}, on={status.BiasDiffOn}。";
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

    private void FrameTimer_Tick(object? sender, EventArgs e)
    {
        if (!_nativeInitialized || _busy) return;
        try
        {
            int runningCount = 0;
            foreach (CameraTile tile in _tiles)
            {
                EventCoreStatus status = NativeMethods.GetStatus(tile.Slot);
                tile.Running = status.State == 1;
                if (!tile.Running)
                {
                    tile.StatusText.Text = tile.Camera.HasValue ? "● 未启动" : "● 未分配";
                    tile.StatusText.Foreground = tile.Camera.HasValue ? Brushes.Gold : Brushes.Gray;
                    continue;
                }
                runningCount++;
                tile.StatusText.Text = $"● 运行  {status.EventsPerSecond / 1_000_000.0:F2} Mev/s";
                tile.StatusText.Foreground = Brushes.LightGreen;
                if (status.FrameId == tile.LastFrameId) continue;
                EnsureBitmap(tile, status.Width, status.Height);
                WriteableBitmap bitmap = tile.Bitmap!;
                bitmap.Lock();
                try
                {
                    int capacity = checked(bitmap.BackBufferStride * bitmap.PixelHeight);
                    if (NativeMethods.CopyLatestFrame(
                        tile.Slot, bitmap.BackBuffer, capacity, bitmap.BackBufferStride,
                        out ulong copiedFrameId))
                    {
                        bitmap.AddDirtyRect(new Int32Rect(
                            0, 0, bitmap.PixelWidth, bitmap.PixelHeight));
                        tile.LastFrameId = copiedFrameId;
                    }
                }
                finally
                {
                    bitmap.Unlock();
                }
            }
            RunningCountTextBlock.Text = $"运行：{runningCount} / 20";
            UpdateSelectedStatus();
            UpdateControlAvailability();
        }
        catch (Exception error)
        {
            _frameTimer.Stop();
            ShowError(error.Message);
        }
    }

    private void EnsureBitmap(CameraTile tile, int width, int height)
    {
        if (tile.Bitmap?.PixelWidth == width && tile.Bitmap.PixelHeight == height) return;
        tile.Bitmap = new WriteableBitmap(width, height, 96, 96, PixelFormats.Bgr24, null);
        tile.Image.Source = tile.Bitmap;
        if (_focusedSlot == tile.Slot) FocusImage.Source = tile.Bitmap;
    }

    private void CameraTile_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (sender is not Border { Tag: int slot }) return;
        SelectSlot(slot);
        ShowFocusedSlot(slot);
    }

    private void ShowFocusedSlot(int slot)
    {
        _focusedSlot = slot;
        CameraTile tile = _tiles[slot];
        FocusTitleTextBlock.Text = $"{tile.Name}    {tile.Camera?.Serial ?? "未分配"}";
        FocusImage.Source = tile.Bitmap;
        Grid.SetRowSpan(MainLayout, 2);
        StatusBar.Visibility = Visibility.Collapsed;
        FocusView.Visibility = Visibility.Visible;
    }

    private void ReturnToOverview()
    {
        _focusedSlot = null;
        FocusImage.Source = null;
        FocusView.Visibility = Visibility.Collapsed;
        Grid.SetRowSpan(MainLayout, 1);
        StatusBar.Visibility = Visibility.Visible;
    }

    private void FocusImage_MouseLeftButtonDown(object sender, MouseButtonEventArgs e) =>
        ReturnToOverview();
    private void BackToOverviewButton_Click(object sender, RoutedEventArgs e) =>
        ReturnToOverview();
    private void MainWindow_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Escape && _focusedSlot.HasValue)
        {
            ReturnToOverview();
            e.Handled = true;
        }
    }

    private void SlotComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_updatingControls || SlotComboBox.SelectedIndex < 0) return;
        SelectSlot(SlotComboBox.SelectedIndex);
    }

    private void SelectSlot(int slot)
    {
        SaveBiasForSelectedSlot();
        _selectedSlot = slot;
        _updatingControls = true;
        SlotComboBox.SelectedIndex = slot;
        _updatingControls = false;
        LoadSelectedSlotIntoControls();
        UpdateSelectionVisuals();
    }

    private void CameraComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_updatingControls) return;
        CameraTile tile = _tiles[_selectedSlot];
        if (tile.Running)
        {
            LoadSelectedSlotIntoControls();
            ShowError("请先停止当前槽位，再更改相机分配。");
            return;
        }
        tile.Camera = CameraComboBox.SelectedItem is CameraInfo camera ? camera : null;
        UpdateTileAssignment(tile);
    }

    private void ClearAssignmentButton_Click(object sender, RoutedEventArgs e)
    {
        CameraTile tile = _tiles[_selectedSlot];
        if (tile.Running)
        {
            ShowError("请先停止当前槽位，再清除分配。");
            return;
        }
        tile.Camera = null;
        UpdateTileAssignment(tile);
        LoadSelectedSlotIntoControls();
    }

    private void LoadSelectedSlotIntoControls()
    {
        CameraTile tile = _tiles[_selectedSlot];
        _updatingControls = true;
        CameraComboBox.SelectedItem = tile.Camera.HasValue
            ? _cameras.FirstOrDefault(camera => camera.Serial == tile.Camera.Value.Serial)
            : null;
        BiasOffTextBox.Text = tile.BiasOff.ToString(CultureInfo.InvariantCulture);
        BiasOnTextBox.Text = tile.BiasOn.ToString(CultureInfo.InvariantCulture);
        _updatingControls = false;
        UpdateSelectedStatus();
        UpdateControlAvailability();
    }

    private void SaveBiasForSelectedSlot()
    {
        if (_tiles[_selectedSlot] is not CameraTile tile) return;
        if (int.TryParse(BiasOffTextBox.Text, NumberStyles.Integer,
                CultureInfo.InvariantCulture, out int off) && off >= 0)
            tile.BiasOff = off;
        if (int.TryParse(BiasOnTextBox.Text, NumberStyles.Integer,
                CultureInfo.InvariantCulture, out int on) && on >= 0)
            tile.BiasOn = on;
    }

    private void UpdateTileAssignment(CameraTile tile)
    {
        tile.SerialText.Text = tile.Camera?.Serial ?? "未分配";
        if (!tile.Running)
        {
            tile.StatusText.Text = tile.Camera.HasValue ? "● 未启动" : "● 未分配";
            tile.StatusText.Foreground = tile.Camera.HasValue ? Brushes.Gold : Brushes.Gray;
        }
    }

    private void UpdateSelectionVisuals()
    {
        foreach (CameraTile tile in _tiles)
        {
            tile.Border.BorderBrush = tile.Slot == _selectedSlot
                ? new SolidColorBrush(Color.FromRgb(46, 120, 210))
                : new SolidColorBrush(Color.FromRgb(59, 65, 75));
            tile.Border.BorderThickness = tile.Slot == _selectedSlot
                ? new Thickness(2)
                : new Thickness(1);
        }
    }

    private void UpdateSelectedStatus()
    {
        if (!_nativeInitialized) return;
        CameraTile tile = _tiles[_selectedSlot];
        EventCoreStatus status = NativeMethods.GetStatus(tile.Slot);
        StateTextBlock.Text = status.State == 1 ? "运行中" : "未启动";
        StateTextBlock.Foreground = status.State == 1 ? Brushes.LightGreen : Brushes.Gold;
        SerialTextBlock.Text = $"序列号：{tile.Camera?.Serial ?? "未分配"}";
        EventRateTextBlock.Text = $"事件率：{status.EventsPerSecond:N0} events/s";
        TotalEventsTextBlock.Text = $"累计事件：{status.TotalEvents:N0}";
        FrameIdTextBlock.Text = $"画面编号：{status.FrameId:N0}";
    }

    private bool ValidateUniqueAssignments()
    {
        string[] duplicates = _tiles.Where(tile => tile.Camera.HasValue)
            .GroupBy(tile => tile.Camera!.Value.Serial)
            .Where(group => group.Count() > 1)
            .Select(group => group.Key)
            .ToArray();
        if (duplicates.Length == 0) return true;
        ShowError("以下相机被重复分配到多个槽位：" + string.Join("、", duplicates));
        return false;
    }

    private bool TryReadBias(out int biasOff, out int biasOn)
    {
        bool offValid = int.TryParse(BiasOffTextBox.Text, NumberStyles.Integer,
            CultureInfo.InvariantCulture, out biasOff);
        bool onValid = int.TryParse(BiasOnTextBox.Text, NumberStyles.Integer,
            CultureInfo.InvariantCulture, out biasOn);
        if (!offValid || biasOff < 0 || !onValid || biasOn < 0)
        {
            ShowError("Bias 必须是非负整数。");
            return false;
        }
        return true;
    }

    private void SetBusy(bool busy, string? message = null)
    {
        _busy = busy;
        if (message is not null) MessageTextBlock.Text = message;
        UpdateControlAvailability();
    }

    private void UpdateControlAvailability()
    {
        if (!_nativeInitialized || _busy)
        {
            RefreshButton.IsEnabled = false;
            StartAllButton.IsEnabled = false;
            StopAllButton.IsEnabled = false;
            StartSelectedButton.IsEnabled = false;
            StopSelectedButton.IsEnabled = false;
            ApplyBiasButton.IsEnabled = false;
            return;
        }
        CameraTile selected = _tiles[_selectedSlot];
        bool anyRunning = _tiles.Any(tile => tile.Running);
        RefreshButton.IsEnabled = !anyRunning;
        StartAllButton.IsEnabled = _tiles.Any(tile => tile.Camera.HasValue && !tile.Running);
        StopAllButton.IsEnabled = anyRunning;
        StartSelectedButton.IsEnabled = selected.Camera.HasValue && !selected.Running;
        StopSelectedButton.IsEnabled = selected.Running;
        ApplyBiasButton.IsEnabled = selected.Camera.HasValue;
        CameraComboBox.IsEnabled = !selected.Running;
    }

    private void ShowError(string message)
    {
        MessageTextBlock.Text = message;
        MessageBox.Show(this, message, "上位机错误", MessageBoxButton.OK, MessageBoxImage.Error);
    }

    private void ShowFatal(string message)
    {
        ShowError(message);
        _nativeInitialized = false;
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
