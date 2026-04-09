# 多路 RTSP 性能基准测试指南

## 概述

`multi_rtsp_benchmark` 是专门用于精确测量多路 RTSP + MPP 硬解 + RKNN 推理性能的命令行工具。

与 `multi_rtsp_mpp_test`（Qt GUI 版本）的区别：
- **无 GUI**：纯命令行，不消耗显示资源
- **精确计时**：每帧独立计时，统计 min/max/avg/p99
- **实时监控**：每 5 秒输出一次实时 FPS 和 NPU 负载
- **结果导出**：自动保存 CSV 格式测试结果
- **对比输出**：自动对比 FFmpeg 软解 vs MPP 硬解性能

## 编译

```bash
cd /home/topeet/AiControlSkynet3588/build
cmake .. && make multi_rtsp_benchmark -j4
```

## 使用方法

```bash
# 4 路测试 60 秒（默认）
./multi_rtsp_benchmark

# 指定路数和时长
./multi_rtsp_benchmark [路数] [秒数]

# 1 路测试 30 秒
./multi_rtsp_benchmark 1 30

# 2 路测试 60 秒
./multi_rtsp_benchmark 2 60

# 4 路测试 120 秒
./multi_rtsp_benchmark 4 120
```

## 前置条件

1. **RTSP 推流服务已启动**
   ```bash
   ./scripts/multi_stream.sh
   ```

2. **模型文件存在**
   ```
   /home/topeet/rknpu2/examples/rknn_yolov5_demo/model/RK3588/yolov5s-640-640.rknn
   ```

3. **NPU 负载可读**
   ```bash
   cat /sys/kernel/debug/rknpu/load
   ```

## 测试指标

| 指标 | 说明 |
|------|------|
| FPS | 每路每秒推理帧数 |
| Avg Infer | 平均推理延迟（ms） |
| P99 Infer | 99% 推理延迟（ms） |
| Min/Max Infer | 最小/最大推理延迟（ms） |
| E2E Latency | 端到端延迟：解码+RGA+推理（ms） |
| Decode Latency | MPP 解码延迟（ms） |
| NPU Load | NPU 各核心利用率 |
| Detections | 检测到的目标总数 |

## 输出示例

```
========================================================
  Multi-RTSP + MPP + RKNN Performance Benchmark
========================================================
  Channels: 4  |  Duration: 60s
  Model:    yolov5s-640-640.rknn
  BOX_THRESH: 0.50  NMS_THRESH: 0.45
========================================================

[RKNN#0] Initialized, core=0, outputs=3
[RKNN#1] Initialized, core=1, outputs=3
[RKNN#2] Initialized, core=2, outputs=3
[RKNN#3] Initialized, core=0, outputs=3
[CH0] Starting, RTSP: rtsp://192.168.137.251:8554/cow0
[CH1] Starting, RTSP: rtsp://192.168.137.251:8554/cow1
[CH2] Starting, RTSP: rtsp://192.168.137.251:8554/cow2
[CH3] Starting, RTSP: rtsp://192.168.137.251:8554/cow3

--- [5s] Live Stats ---
  CH0: 130 frames, 26.0 fps, avg_infer=9.2ms, detections=42
  CH1: 128 frames, 25.6 fps, avg_infer=9.3ms, detections=38
  CH2: 131 frames, 26.2 fps, avg_infer=9.1ms, detections=45
  CH3: 129 frames, 25.8 fps, avg_infer=9.4ms, detections=40
  NPU: Core0: 85%, Core1: 82%, Core2: 80%

...

Channel | NPU Core | Frames |   FPS  | Avg Infer | P99 Infer | Min Infer | Max Infer | Detections
--------|----------|--------|--------|-----------|-----------|-----------|-----------|----------
  CH0   |  Core 0  |   1560 |   26.0 |    9.21ms |   12.34ms |    7.89ms |   15.67ms |      502
  CH1   |  Core 1  |   1548 |   25.8 |    9.35ms |   12.56ms |    8.01ms |   16.23ms |      478
  CH2   |  Core 2  |   1572 |   26.2 |    9.12ms |   11.98ms |    7.76ms |   14.89ms |      523
  CH3   |  Core 0  |   1555 |   25.9 |    9.28ms |   12.45ms |    7.93ms |   15.34ms |      491
--------|----------|--------|--------|-----------|-----------|-----------|-----------|----------
  TOTAL |    -     |   6235 |  103.9 |     -     |     -     |     -     |     -     |     1994

--- Comparison: FFmpeg Soft Decode vs MPP Hardware Decode ---
Metric               | FFmpeg Soft  | MPP Hardware
---------------------|--------------|-------------
4-ch Total FPS       |    ~30       |   103.9
Per-channel FPS      |    <30       |    26.0
NPU Utilization      |    ~55%      |     ~82%
CPU Usage            |    High      |     Low

Results saved to: benchmark_4ch_60s.csv
```

## 结果文件

测试完成后自动生成 CSV 文件，命名格式：`benchmark_{路数}ch_{秒数}s.csv`

```csv
channel,npu_core,frames,fps,avg_infer_ms,p99_infer_ms,min_infer_ms,max_infer_ms,detections,e2e_avg_ms
0,0,1560,26.0,9.21,12.34,7.89,15.67,502,15.23
1,1,1548,25.8,9.35,12.56,8.01,16.23,478,15.45
2,2,1572,26.2,9.12,11.98,7.76,14.89,523,14.98
3,0,1555,25.9,9.28,12.45,7.93,15.34,491,15.12
```

## 与 RTSP_PERFORMANCE_ANALYSIS.md 的对应关系

基准测试结果可直接填入 `RTSP_PERFORMANCE_ANALYSIS.md` 的性能对比表：

| 路数 | FFmpeg 软解 | MPP 硬解（目标） | MPP 硬解（实测） |
|------|-----------|----------------|----------------|
| 1 路 | 30 fps | 30 fps | _(benchmark 1ch)_ |
| 2 路 | < 30 fps | 60 fps | _(benchmark 2ch)_ |
| 3 路 | < 30 fps | 90 fps | _(benchmark 3ch)_ |
| 4 路 | < 30 fps | 100+ fps | _(benchmark 4ch)_ |

## 推荐测试流程

1. 启动推流：`./scripts/multi_stream.sh`
2. 逐路测试：
   ```bash
   ./multi_rtsp_benchmark 1 30   # 1路基准
   ./multi_rtsp_benchmark 2 30   # 2路基准
   ./multi_rtsp_benchmark 3 30   # 3路基准
   ./multi_rtsp_benchmark 4 60   # 4路长时间测试
   ```
3. 收集 CSV 文件，更新 RTSP_PERFORMANCE_ANALYSIS.md

## 注意事项

- 测试期间不要运行其他 NPU 任务
- 确保网络稳定（使用 TCP 传输）
- 首次连接可能有几秒延迟（等待关键帧）
- NPU 负载读取需要 root 权限：`sudo cat /sys/kernel/debug/rknpu/load`
