# 同机版事件相机上位机

目标部署方式：DVSLume 相机、C++ 数据核心和 C# WPF 界面运行在同一台 Windows x64
电脑上。该版本不经过 UEVT/UDP，C++ 直接调用已安装的 DVSense SDK。

## 架构

```text
DVSLume 相机
  ↓ DVSense SDK / Event2D 回调
C++ EventCore.dll
  ├─ 管理 20 个互相独立的相机会话
  ├─ 单路或批量打开/停止相机
  ├─ 分别设置并回读每路 Bias
  ├─ 使用 OpenCV cv::Mat 生成 1280×720 BGR24 事件画面
  ├─ 每约 16.667 ms 交换一次双缓冲画面（目标 60 FPS）
  └─ 分别维护事件率、累计事件数和画面编号
  ↓ P/Invoke，一次本机内存复制
C# WPF
  ├─ 10 组 × 宽/窄视场固定槽位
  ├─ 自动/手动分配相机序列号
  ├─ 单路或批量启停及参数输入
  ├─ 独立选择 320×180～1280×720 分辨率和 30～60 FPS
  ├─ 5 行 × 4 列的 20 路 WriteableBitmap 实时总览
  ├─ 默认适配 1280×720，并支持窗口自适应缩放
  └─ 单击画面放大、再次单击或 Esc 返回
```

ON 事件显示为青色，OFF 事件显示为橙色，未触发像素为深灰色。C# 不解析事件，也不
执行逐像素渲染，只负责 UI 和将 C++ 的最新 BGR24 帧复制到 `WriteableBitmap`。

## 编译条件

- Windows 10/11 x64；
- 已安装 DVSense SDK，默认目录为 `C:\Program Files (x86)\DvsenseDriver`；
- .NET 8 SDK 或更高版本；
- Visual Studio 2022/2026 或对应 Build Tools，并勾选 **Desktop development with C++**；
- Visual Studio Installer 中的 **vcpkg package manager** 组件；
- CMake 3.20 或更高版本。

构建脚本会通过 `vswhere` 自动找到已安装的 Visual Studio，然后加载 x64 MSVC 环境。
厂家 SDK 提供的是 MSVC `.lib`，因此不能用普通 MinGW 工具链替代。
OpenCV 4.12.0 通过 `native/vcpkg.json` 管理；首次构建会联网安装 x64 开发包，后续构建
会直接复用项目安装目录和本机二进制缓存。正式界面不使用 `cv::imshow()`，OpenCV 负责
事件成帧，WPF 继续负责 20 路布局、参数控制和单路放大。
由于当前工程路径包含中文，脚本会在构建期间自动映射一个临时英文盘符，避免 MSVC 链接器
写入 PDB 时发生路径编码错误；构建结束后会自动解除映射。

## 构建和启动

安装好 C++ Build Tools 后：

1. 双击 `构建_同机上位机.cmd`；
2. 构建显示成功后，关闭 DVSense Viewer 等占用相机的软件；
3. 双击 `启动_同机相机上位机.cmd`；
4. 单击“刷新相机并自动分配”；
5. 按实际接线核对第 1～10 组的宽/窄相机序列号，必要时手动调整；
6. 设置 Bias 后单路启动，或直接单击“全部启动”；
7. 单击任一画面可使其占满显示区，再次单击或按 Esc 返回总览。

左侧“显示设置”包含两个独立下拉栏，可在运行过程中选择 320×180、480×270、
640×360 或 1280×720，以及 30、45 或 60 FPS。任意组合均可使用，设置作用于全部槽位；
较低分辨率或帧率可显著降低多路同时显示时的内存带宽占用。

程序会在启动时先设置 Bias，再立即回读。设置失败或回读不一致时不会开始事件流。

## 源码目录

- `native/include/EventCoreApi.h`：C++ 与 C# 之间的稳定 C 接口；
- `native/src/EventFrameRenderer.*`：基于 OpenCV `cv::Mat` 的事件双缓冲成帧；
- `native/src/EventCore.cpp`：SDK、事件回调、成帧和参数控制；
- `native/vcpkg.json`：固定 OpenCV 依赖和 vcpkg 基线；
- `ui/NativeMethods.cs`：C# P/Invoke 封装；
- `ui/MainWindow.xaml`：WPF 界面；
- `ui/MainWindow.xaml.cs`：界面交互和画面刷新。

## 当前版本边界

- 最多 20 台相机，对应 10 组宽/窄视场；某一路启动失败不会中断其他路；
- 当前开放 `bias_diff_off` 和 `bias_diff_on` 两项参数；
- 目标 60 FPS、1280×720 的实时事件画面；常用 4 路同时显示，实际帧率取决于主机内存带宽、CPU 和 WPF 渲染负载；
- 总览采用 5 行 × 4 列布局，默认窗口为 1280×720，最小支持 1024×600；
- 相机首次按 SDK 检测顺序自动分配，实机使用前必须核对宽/窄序列号；
- 不包含 UAV 检测、跟踪、分类和 RAW 录制；
- 不经过网络，不依赖原有 UEVT UDP 发送程序。

20 台相机同时运行还取决于电脑 USB 控制器数量、总线带宽、供电和 SDK 的设备并发能力。
软件会逐路报告启动失败，便于区分软件配置问题和硬件带宽问题。
