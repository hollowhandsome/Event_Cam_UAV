# UAV/原始事件网络接口（UEVT V1）

## 1. 用途和边界

该接口支持两种互斥模式：筛选模式发送检测程序判定为 `drone` 的目标框内事件；原始
模式发送 DVSense SDK 输出的全部 Event2D 事件。原始模式不表示厂商 RAW 文件字节流。
筛选框内可能包含少量背景事件，模型漏检时对应事件不会外发。

默认使用 UDP IPv4 单播，目标地址和端口由命令行指定，默认建议端口为 `50000`。
发送线程和事件采集线程相互独立。发送队列满时丢弃最旧窗口，并通过程序统计量报告。

## 2. 传输约定

- 多字节整数全部使用网络字节序（大端）。
- 时间戳单位为微秒。
- 坐标原点位于图像左上角，x 向右，y 向下。
- 普通以太网 MTU 1500 时，UDP 负载不超过 1472 字节。
- 每包固定头为 48 字节，每条事件为 12 字节，每包最多 118 条事件。
- 一个 20 ms 窗口事件过多时，使用 `fragment_index` 和 `fragment_count` 分片。
- `sequence` 按 UDP 包递增，uint32 溢出后自然回绕。
- UDP 允许丢失、乱序和重复；接收端必须按序号检测，不应把 UDP 当作可靠流。

双相机部署仍使用完全相同的 UEVT V1 数据格式，不在包头中新增相机编号。推荐按目标
端口区分来源：相机 1 使用 `50000`，相机 2 使用 `50001`。上位机同时绑定两个端口，
并将接收端口作为 `camera_id` 的外部映射。禁止两路数据发送到同一目标 IP/端口，
否则两路各自从零开始的 `sequence` 和 `window_id` 会混在一起，无法可靠重组。

## 3. 48 字节包头

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `UEVT` |
| 4 | 1 | version | 当前为 1 |
| 5 | 1 | message_type | 1=事件，2=状态（预留） |
| 6 | 2 | flags | bit0=前回溯数据；bit1=原始事件模式 |
| 8 | 4 | sequence | UDP 包序号 |
| 12 | 4 | window_id | 20 ms 窗口编号 |
| 16 | 2 | fragment_index | 当前分片，从 0 开始 |
| 18 | 2 | fragment_count | 该窗口总分片数 |
| 20 | 2 | sensor_width | 传感器宽度 |
| 22 | 2 | sensor_height | 传感器高度 |
| 24 | 2 | event_count | 本包事件数 |
| 26 | 2 | event_stride | V1 固定为 12 |
| 28 | 8 | window_start_us | 窗口起始时间戳 |
| 36 | 8 | window_end_us | 窗口结束时间戳 |
| 44 | 4 | payload_crc32 | 仅对事件负载计算的 IEEE CRC-32 |

包头自身不包含在 CRC 中。接收端必须验证 Magic、版本、分片范围、负载长度、传感器
尺寸和 CRC 后，才能访问事件记录。

## 4. 12 字节事件记录

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 2 | x | 横坐标 |
| 2 | 2 | y | 纵坐标 |
| 4 | 4 | timestamp_offset_us | 相对 `window_start_us` 的时间 |
| 8 | 1 | polarity | 0=OFF，1=ON |
| 9 | 3 | reserved | V1 必须置零 |

绝对事件时间为 `window_start_us + timestamp_offset_us`。

## 5. 前回溯规则

分类器需要一定轨迹历史。首次把某个 `track_id` 判定为 UAV 时，程序默认回溯最近
300 ms，并使用该轨迹在历史窗口中的检测框筛选事件。此类包的 `flags & 0x0001`
为真。同一轨迹继续保持 UAV 状态时不会重复补发历史窗口。

`--pre-roll-ms 0` 可关闭回溯。`--box-expand N` 可将检测框向四周扩展 N 像素，扩展
结果会限制在传感器边界内。多个 UAV 框重叠时，同一事件只发送一次。

原始事件模式下 `flags & 0x0002` 为真，bit0 必须为 0。接收端可据此区分两种数据源。

## 6. 示例数据

工程源码中的 `samples/uav_event_protocol_v1` 包含筛选模式样例。Windows 运行包内的
`integration/samples/filtered_event_protocol_v1` 和
`integration/samples/raw_event_protocol_v1` 分别包含筛选模式与原始事件模式样例：

- `packet_*.bin`：实际协议 UDP 负载；
- `manifest.csv`：包序号、窗口号、分片和事件数索引；
- `manifest.json`：样例生成条件；
- `decoded_events.csv`：Python 解码后的逐事件对照；
- `sample_packets.pcap`：合成的 Ethernet/IPv4/UDP 抓包，源/目的地址使用文档地址
  `192.0.2.1` 和 `192.0.2.2`，目的端口 50000。

PCAP 仅用于 Wireshark 解析练习，不代表实际设备 IP。

## 7. 接收端要求

1. UDP 接收缓冲区至少为 1472 字节，建议增大操作系统 socket 缓冲区。
2. 按 `sequence` 统计丢包和乱序。
3. 使用 `(window_id, fragment_count)` 重组同一窗口。
4. 超时未收齐的窗口应丢弃或标记不完整，不能无限等待。
5. CRC 错误、未知版本、长度不一致、越界坐标和非法极性的包必须拒绝。
6. 当前协议无认证和加密，只应在隔离或可信局域网使用。

工程源码中，C/C++ 字段偏移常量位于
`include/event_cam/uav_event_protocol.h`，C++ 编解码 API 位于
`include/event_cam/uav_event_protocol.hpp`，Python 参考解析器位于
`tools/uav_event_protocol.py`。Windows 运行包中的公开 C 头文件和 Python 参考实现分别位于
`integration/include/uav_event_protocol.h` 和
`integration/python/uav_event_protocol.py`。
