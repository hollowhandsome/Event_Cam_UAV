# DVSLume UEVT V2 UDP 上位机

这是与原同机 SDK 版完全分离的 Windows UDP 接收版本。它与 Jetson 软件包
`event_cam_studio_developer_thor_arm64_v2_20260924` 使用的 **UEVT V2** 协议配套，
不再兼容 UEVT V1。原同机项目和 Jetson 软件包不会被本项目修改。

完整接线和操作流程见 [Jetson 到笔记本使用说明](Jetson到笔记本_UDP使用说明.md)。

## 数据路径

```text
Jetson：DVSense SDK -> event_cam_studio -> UEVT V2/UDP
Windows：UDP端口 -> V2校验/分流/重组 -> OpenCV -> WPF
```

Windows 接收端不调用 DVSense SDK。Jetson 软件包内的 `bin/event_cam_studio` 已经包含
双相机采集、检测、UEVT V2 编码和 UDP 发送功能，不需要另外编写发送程序。

## 10 组、20 路映射

每组只占用一个 UDP 端口，同一端口中的两路由 V2 包头的 `camera_id` 区分：

- 第 1 组：端口 50000；宽视场 `camera_id=1`，窄视场 `camera_id=2`；
- 第 2 组：端口 50001；宽视场 `camera_id=1`，窄视场 `camera_id=2`；
- 依此类推，第 10 组使用端口 50009。

一块 Jetson 上的左右两路可以填写同一个目标 IP 和端口。不同 Jetson/相机组使用不同
目标端口。不要让两块 Jetson 使用同一目标端口，否则相同 camera_id 的独立会话会冲突。

## 构建和启动

接收电脑重新构建时需要：

1. Visual Studio 的“使用 C++ 的桌面开发”和 vcpkg 组件；
2. .NET 8 SDK；
3. 首次构建时能够下载 OpenCV 4 依赖。

仅运行已经构建好的版本时，只需要 .NET 8 Desktop Runtime x64，不需要 Visual Studio、
vcpkg 或 DVSense SDK。

双击 `构建_UDP上位机.cmd`，成功后双击 `启动_UDP上位机.cmd`。

## 连接 Jetson

1. 查询 Windows 笔记本有线网卡 IPv4，例如 `192.168.10.3`；
2. 启动 Windows 上位机，绑定地址保持 `0.0.0.0`，起始端口保持 `50000`；
3. 单击“全部监听”；
4. 在 Jetson 运行 `./tools/start_studio.sh`；
5. 在左右两路分别填写笔记本 IP，并为同一组填写相同端口；
6. 选择“原始事件 + 已分类检测框”或“滤波事件 + drone 框”；
7. 点击 Jetson 每一路的“启动网口转发”，再启动相机采集。

上位机会同时解析事件、检测框和状态消息。drone 框显示为红色，Non-drone 框显示为灰色；
历史补发框不会覆盖当前画面，无新完整实时结果超过 200 ms 会清除旧框。

Windows 左侧先用“选择窗口”指定一路，再用“画面内容”分别选择“原始事件”或“滤波事件”。
该设置只改变当前所选窗口的本机画面，不会修改 Jetson 的网口发送模式。若 Jetson 发送原始
事件，上位机可利用 drone 框生成滤波画面；若 Jetson 只发送滤波事件，未发送的原始事件
无法恢复。因此需要在 Windows 随时切换两种画面时，Jetson 应选择“原始事件 + 已分类检测框”。

## 无相机测试

1. 启动 UDP 上位机并单击“全部监听”；
2. 双击 `测试_UDP模拟数据.cmd`；
3. 默认向端口 50000 和 50001 发送 4 路 V2 合成事件及检测框；
4. 端口内分别使用 `camera_id=1/2`；
5. 在测试窗口按 Ctrl+C 停止。

## V2 接收保护

- 验证 72 字节包头、消息类型、字段范围、记录长度和整个数据报 CRC32；
- 验证事件坐标/时间/极性、检测框坐标/概率/类别和状态记录；
- 按 `camera_id/session_id/generation/window_id/update_id/flags` 分流与重组；
- 事件消息和框消息都收齐后才交付画面；
- 支持 UDP 乱序和重复，2 秒仍未收齐的窗口会丢弃；
- 新 session 或 generation 会清理旧重组状态，旧代次迟到包不会混入新画面；
- UDP socket 接收缓冲区请求设置为 8 MB。

UEVT V2 没有认证、加密或重传，只应在隔离或可信局域网内使用。
