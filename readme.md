# AIControlSkynet3588 — RK3588 多路 RTSP 硬解码 + NPU 推理

在 RK3588 开发板上实现多路 RTSP 视频流的 MPP 硬件解码 + RGA 缩放 + RKNN YOLOv5 推理，全程硬件加速。

## 硬件要求

- **开发板**: RK3588 (Firefly AIO-3588Q 或类似)
- **系统**: Ubuntu 20.04 (Kernel 5.10.x, aarch64)
- **NPU**: 3 核，支持 int8 量化推理

## 依赖

- rockchip-mpp — H.264 硬件解码
- librga (v1.10+) — 2D 加速（缩放、色彩转换）
- rknpu2 — NPU 推理
- FFmpeg (libavformat/libavcodec) — RTSP 客户端
- Qt5 — GUI 显示
- C++17, CMake 3.10+

## 架构

RTSP 流 → FFmpeg 读取 → MPP 硬解 H.264 → RGA NV12→RGB+缩放 → RKNN YOLOv5 推理 → Qt 显示

- **MPP Legacy API**: decode_put_packet / decode_get_frame，支持 Info Change 处理
- **RGA**: 一次 imresize 完成 NV12→RGB 色彩转换 + 缩放到模型输入尺寸 (640x640)
- **RKNN**: int8 量化 YOLOv5 模型，2 类 (cow + person)，3 个 NPU 核心并行
- **多通道**: 每通道独立线程，NPU 核心自动分配 (Core0/1/2 轮转)
- **断线重连**: RTSP 断开自动重连 (3 秒间隔)

## 程序

### 1. multi_rtsp_mpp_test (Qt GUI)

4 路 RTSP 多通道检测界面，支持：
- 1-4 通道选择
- 每通道独立 RTSP 地址输入
- TCP/UDP 传输选择
- 模型路径配置
- 启动/停止控制
- 检测框 + 类别 + 置信度显示

```bash
# GUI 模式
./multi_rtsp_mpp_test

# 命令行自动启动
./multi_rtsp_mpp_test --auto --channels 4 --url rtsp://192.168.137.251:8554/cow0
```

### 2. mpp_rtsp_cli_test (命令行)

单通道纯命令行测试，无需 Qt，用于快速验证解码和推理：

```bash
./mpp_rtsp_cli_test rtsp://192.168.137.251:8554/cow0 30
```

## 性能

| 通道数 | 解码 FPS | 推理成功率 | NPU 核心 |
|--------|---------|-----------|---------|
| 1 路   | 26 fps  | 100%      | Core 0  |
| 2 路   | 26 fps  | 100%      | Core 0+1|
| 4 路   | 26 fps  | 100%      | Core 0+1+2 轮转 |

- 解码分辨率: 960x544 (stride 对齐)
- 推理输入: 640x640 RGB
- 显示刷新: 25 fps (40ms 间隔)
- BOX_THRESH: 0.50, NMS_THRESH: 0.45

## 构建

```bash
mkdir build && cd build
cmake ..
make multi_rtsp_mpp_test -j4
make mpp_rtsp_cli_test -j4
```

## 项目结构

```
├── CMakeLists.txt
├── readme.md
├── agents.md                    # 开发环境配置
├── src/
│   ├── main.cpp                 # 原始主程序入口
│   ├── mpp_rtsp_cli_test.cpp    # CLI 测试程序
│   ├── qt_ui/
│   │   ├── CMakeLists.txt
│   │   └── multi_rtsp_mpp_test.cpp  # Qt 多通道主程序
│   ├── decoder/
│   │   ├── mpp_decoder.cpp/h        # MPP 解码器
│   │   └── mpp_encoder.cpp/h        # MPP 编码器
│   ├── inference/
│   │   ├── postprocess.cpp/h        # YOLOv5 后处理
│   │   ├── preprocess.cpp/h         # 预处理
│   │   └── preprocess_rga.cpp/h     # RGA 预处理
│   └── utils/
│       └── drawing.cpp/h            # 绘制工具
└── scripts/
    └── multi_stream.sh              # 多流启动脚本
```

## 自动化

项目由 OpenClaw 编排，支持飞书远程控制：
1. 飞书发送指令 → OpenClaw 解析
2. 自动 SSH 到开发板编译、运行、调试
3. 日志自动分析，代码自动修复
4. 进度实时汇报到飞书
