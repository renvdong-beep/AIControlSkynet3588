# AIControlSkynet3588 项目架构

## 项目概述

RK3588 平台多路视频解码 + RKNN 推理系统，支持 USB 摄像头和 RTSP 流。

## 目录结构

```
AIControlSkynet3588/
├── CMakeLists.txt                  # 编译配置
├── agents.md                       # Agent 协作文档
├── src/
│   ├── main.cpp                    # 命令行多路解码（无 GUI）
│   ├── stress_test.cpp             # NPU 多核压力测试（16路 105fps）
│   ├── rknn_optimized.cpp          # RKNN 优化测试
│   ├── h264_decode_test_main.cpp   # H.264 解码测试入口
│   ├── decoder/                    # 解码器模块
│   │   ├── mpp_decoder.cpp/h       # MPP 解码器封装
│   │   └── mpp_encoder.cpp/h       # MPP 编码器封装
│   ├── inference/                   # 推理模块
│   │   ├── postprocess.cpp/h       # YOLOv5 后处理（NMS）
│   │   └── preprocess.cpp/h        # 预处理
│   ├── utils/                       # 工具模块
│   │   └── drawing.cpp/h           # YUV 绘制工具
│   └── qt_ui/                       # Qt 界面程序
│       ├── multi_rtsp_mpp_test.cpp # ⭐ 多路 RTSP + MPP 硬解 + RKNN（主力程序）
│       ├── multi_camera_test.cpp   # 多路 USB 摄像头 + RKNN
│       ├── multi_rtsp_test.cpp     # 多路 RTSP + FFmpeg 软解
│       ├── rtsp_video_test.cpp     # 单路 RTSP + MPP 硬解
│       ├── rtsp_test.cpp           # 单路 RTSP + FFmpeg 软解
│       ├── video_test.cpp          # 视频文件 + MPP 硬解
│       ├── ffmpeg_video_test.cpp   # 视频文件 + FFmpeg 软解
│       ├── real_camera_test.cpp    # 单路真实摄像头 + RKNN
│       ├── simple_test.cpp         # 模拟数据推理测试
│       ├── frmvideocontrol.*       # Qt 视频控制窗口（移植自 videocontrol）
│       ├── frmwidget.*             # Qt 显示组件
│       ├── mpprknnvideo.*          # MPP + RKNN 视频处理（移植）
│       ├── videocontrol.*          # 视频控制逻辑（移植）
│       ├── mousemenu.*             # 鼠标菜单
│       └── main_qt.cpp             # Qt 主入口
├── scripts/
│   └── multi_stream.sh             # 4 路 RTSP 推流脚本
├── docs/
│   ├── ARCHITECTURE.md             # 本文档
│   ├── RTSP_PERFORMANCE_ANALYSIS.md # RTSP 性能分析
│   ├── PERFORMANCE_OPTIMIZATION.md # 性能优化历程
│   ├── INTEGRATION_TEST.md         # 集成测试指南
│   ├── MIGRATION_PROGRESS.md       # VideoControl 移植进度
│   ├── VIDEOCONTROL_MIGRATION.md   # VideoControl 移植规划
│   └── videocontrol_analysis.md    # VideoControl 代码分析
└── include/                         # 头文件
```

## 核心程序

### 1. multi_rtsp_mpp_test — 多路 RTSP + MPP 硬解（⭐ 主力）

**功能：** 1-4 路 RTSP 流 MPP 硬解码 + RKNN 推理 + Qt 显示

**技术路径：**
```
RTSP → FFmpeg 解封装 → H.264 NALU → MPP 硬解 → NV12 → RGA 缩放+转换 → RKNN → Qt 显示
```

**性能：**
- 4 路 26fps，推理成功率 100%
- NPU 三核轮转（Core0/1/2）
- BOX_THRESH=0.50 消除误检

**编译运行：**
```bash
make multi_rtsp_mpp_test -j4
./multi_rtsp_mpp_test
```

### 2. mpp_rtsp_cli_test — 命令行单通道

**功能：** 单路 RTSP + MPP 硬解码 + RKNN 推理（无 GUI）

**编译运行：**
```bash
make mpp_rtsp_cli_test -j4
./mpp_rtsp_cli_test rtsp://192.168.137.251:8554/cow0
```

### 3. stress_test — NPU 压力测试

**功能：** 16 路 NPU 多核并行推理极限测试

**性能：** 105 FPS @ 16 路，延迟 9.5ms

**编译运行：**
```bash
make stress_test -j4
./stress_test
```

### 4. multi_camera_test — 多路 USB 摄像头

**功能：** 4 路 USB 摄像头实时解码 + RKNN 推理

**技术路径：**
```
V4L2 MJPEG → MPP 解码 → NV12 → RGA → RKNN → Qt 显示
```

### 5. 其他测试程序

| 程序 | 输入源 | 解码方式 | 说明 |
|------|--------|---------|------|
| rtsp_video_test | 单路 RTSP | MPP 硬解 | 单路 MPP 测试 |
| rtsp_test | 单路 RTSP | FFmpeg 软解 | 单路软解测试 |
| video_test | 视频文件 | MPP 硬解 | 文件解码测试 |
| ffmpeg_video_test | 视频文件 | FFmpeg 软解 | 软解对比测试 |
| real_camera_test | USB 摄像头 | MPP MJPEG | 真实摄像头测试 |
| simple_test | 模拟数据 | 无 | RKNN 推理验证 |
| rknn_optimized | 模拟数据 | 无 | RKNN 优化引擎测试 |

## 技术栈

### 硬件平台
- **SoC:** RK3588 (4xCortex-A76 + 4xCortex-A55)
- **NPU:** 6 TOPS (3 核心 × 2 TOPS)
- **VPU:** 支持 8K H.264/H.265 解码

### 软件库
| 库 | 版本 | 用途 |
|---|---|---|
| MPP | 1.0.x | 视频硬件编解码 |
| RGA | 2.x | 图像硬件转换 |
| RKNN | 2.x | NPU 推理 |
| Qt5 | 5.x | GUI 界面 |
| FFmpeg | 4.x | RTSP 解封装 |

## NPU 多核并行

```cpp
// 绑定 NPU 核心
rknn_core_mask core_masks[4] = {
    RKNN_NPU_CORE_0,
    RKNN_NPU_CORE_1,
    RKNN_NPU_CORE_2,
    RKNN_NPU_CORE_0  // 循环
};
rknn_set_core_mask(ctx, core_mask);
```

## RTSP 推流

### 推流脚本
```bash
./scripts/multi_stream.sh
```

推流地址：
- `rtsp://192.168.137.251:8554/cow0`
- `rtsp://192.168.137.251:8554/cow1`
- `rtsp://192.168.137.251:8554/cow2`
- `rtsp://192.168.137.251:8554/cow3`

## 开发板环境

- **IP:** 192.168.137.251
- **用户:** topeet/topeet
- **系统:** Ubuntu 20.04 (Kernel 5.10.x)

### 编译
```bash
cd /home/topeet/AiControlSkynet3588
mkdir -p build && cd build
cmake ..
make -j4
```

## 参考

- [MPP 开发指南](https://github.com/rockchip-linux/mpp)
- [RKNN API 文档](https://github.com/rockchip-linux/rknn-toolkit2)
- [RGA 用户指南](https://github.com/rockchip-linux/linux-rga)
