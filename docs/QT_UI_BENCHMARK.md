# Qt UI 版 4 通道性能基准测试

**程序：** `multi_rtsp_benchmark_ui`
**源码：** `src/qt_ui/multi_rtsp_benchmark_ui.cpp`
**对标：** `multi_rtsp_benchmark` (CLI 版)

---

## 概述

基于 CLI 版 `multi_rtsp_benchmark` 的 Qt GUI 版本，提供可视化性能基准测试界面。

### CLI vs UI 对比

| 特性 | CLI 版 (multi_rtsp_benchmark) | Qt UI 版 (multi_rtsp_benchmark_ui) |
|------|------|------|
| 界面 | 纯命令行输出 | Qt 可视化界面 |
| 视频显示 | 无 | 4 路实时画面 + 检测框 |
| FPS 显示 | 每 5 秒打印 | 实时仪表盘 |
| NPU 利用率 | 读取 sysfs 打印 | 实时显示 |
| 延迟统计 | 结束时汇总 | 实时 + 汇总 |
| 对比基线 | 内嵌 FFmpeg 软解数据 | 对比 RTSP_PERFORMANCE_ANALYSIS.md 基线 |
| CSV 导出 | 自动保存 | 按钮导出 |
| 通道选择 | 命令行参数 | 下拉框 |
| 测试时长 | 命令行参数 | 旋转框 |
| 自动停止 | 按时长 | 按时长 + 手动停止 |

---

## 功能特性

### 1. 实时视频显示

- 4 路视频 2×2 网格布局
- 每路画面叠加检测框（cow=绿色, person=橙色）
- 检测框标注类别和置信度
- 通道标签（CH0-CH3）

### 2. 性能仪表盘

- **Overview**: 总 FPS、总帧数、检测数、模型参数
- **NPU Utilization**: 3 核实时利用率
- **Channel Stats**: 每路独立统计
  - FPS
  - 平均推理延迟 / P99 / Min / Max
  - 平均解码延迟
  - 端到端延迟
  - 检测数量
- **CLI vs UI Comparison**: 与 RTSP_PERFORMANCE_ANALYSIS.md 基线对比

### 3. 控制面板

- ▶ Start: 启动测试
- ⏹ Stop: 停止测试
- 📊 Export CSV: 导出结果
- Duration: 测试时长 (10-600s)
- Channels: 通道数 (1-4)
- 运行计时器

### 4. 日志窗口

- 带时间戳的操作日志
- RKNN 初始化信息
- 通道启动/停止信息
- 最终测试结果

---

## 使用方法

### 编译

```bash
cd /home/topeet/AIControlSkynet3588/build
cmake .. && make multi_rtsp_benchmark_ui -j$(nproc)
```

### 运行

```bash
# 默认 4 通道，连接 rtsp://192.168.137.251:8554/cow0~3
./multi_rtsp_benchmark_ui

# 可选：命令行指定 RTSP URL
./multi_rtsp_benchmark_ui rtsp://... rtsp://... rtsp://... rtsp://...
```

### 操作步骤

1. 选择通道数（1-4）和测试时长
2. 点击 ▶ Start
3. 观察实时视频和性能数据
4. 测试结束或手动点击 ⏹ Stop
5. 点击 📊 Export CSV 保存结果

---

## 性能基线（参考 RTSP_PERFORMANCE_ANALYSIS.md）

### CLI 版基准数据

| 路数 | 每路 FPS | 总 FPS | NPU 利用率 | CPU 占用 |
|------|---------|--------|-----------|---------|
| 1 路 | 30 fps | 30 fps | ~30% | 低 |
| 2 路 | 28 fps | 56 fps | ~55% | 低 |
| 3 路 | 26 fps | 78 fps | ~70% | 低 |
| 4 路 | 26 fps | 104 fps | ~82% | 低 |

### UI 版预期

Qt UI 版因显示刷新和帧拷贝会引入少量开销：
- 预期 FPS 损失 < 5%
- 推理延迟基本不变
- NPU 利用率基本不变

---

## 技术细节

### 显示/推理分离

与 `multi_rtsp_mpp_test` 相同的策略：
- 推理线程：全速运行，不等待显示
- 显示定时器：25fps 采样最新帧
- 互斥锁保护帧数据交换

### 延迟统计

- 使用 `std::chrono::steady_clock` 高精度计时
- 推理延迟：`rknn_inputs_set` → `rknn_outputs_get`
- 解码延迟：`decode_put_packet` → `decode_get_frame`
- 端到端延迟：`decode_put_packet` → `rknn_outputs_get`
- P99 延迟：排序后取 99% 位置值
- 滑动窗口：保留最近 10000 个样本，防止内存增长

### CSV 输出格式

```csv
channel,npu_core,frames,fps,avg_infer_ms,p99_infer_ms,min_infer_ms,max_infer_ms,detections,e2e_avg_ms,avg_decode_ms
0,1,1560,26.0,9.52,12.30,7.80,15.60,3120,18.45,3.20
1,2,1560,26.0,9.48,12.10,7.90,15.40,3080,18.30,3.15
2,4,1560,26.0,9.55,12.50,7.70,15.80,3150,18.55,3.25
3,1,1560,26.0,9.50,12.20,7.85,15.50,3100,18.40,3.18
```

---

## 代码架构

```
BenchmarkWindow (QWidget)
├── UI 层
│   ├── VideoWidget[4] — 视频显示 + 检测框绘制
│   ├── 控制面板 — Start/Stop/Export/Duration/Channels
│   ├── 统计面板 — Overview/NPU/Channel Stats/Comparison
│   └── 日志窗口 — 操作日志
│
├── 业务层
│   ├── RKNNInferencer[4] — RKNN 推理（NPU Core 0/1/2 轮转）
│   ├── MppH264Decoder[4] — MPP H.264 硬解码
│   ├── ChannelStats[4] — 性能统计
│   └── channelLoop() — 通道处理线程
│
└── 定时器
    ├── display_timer_ — 25fps 显示刷新
    ├── stats_timer_ — 1s 统计刷新
    └── auto_stop_timer_ — 自动停止
```

---

## 与其他测试程序的关系

```
                    ┌── mpp_rtsp_cli_test (CLI, 单通道)
RTSP + MPP + RKNN ──┤── multi_rtsp_benchmark (CLI, 多通道, 无GUI)
                    └── multi_rtsp_benchmark_ui (Qt UI, 多通道, 可视化) ← NEW

                    ┌── multi_rtsp_test (Qt UI, FFmpeg 软解)
Qt UI + RTSP ───────┤── multi_rtsp_mpp_test (Qt UI, MPP 硬解, 实用版)
                    └── multi_rtsp_benchmark_ui (Qt UI, MPP 硬解, 基准测试版) ← NEW
```

---

**创建时间：** 2026-04-10
**状态：** 代码已编写，待编译测试
