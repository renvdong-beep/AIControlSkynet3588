# 集成测试指南

## 测试环境要求

- **硬件**: RK3588 开发板 (Firefly AIO-3588Q 或类似)
- **系统**: Ubuntu 20.04/22.04 (Kernel 5.10.x)
- **依赖库**: rockchip-mpp, librga, rknn-api, Qt5, FFmpeg

## 编译步骤

```bash
cd /home/topeet/AiControlSkynet3588
mkdir -p build && cd build
cmake ..
make -j4
```

## 测试用例

### 1. 多路 RTSP + MPP 硬解推理（主力程序）

```bash
# 终端1: 启动 4 路 RTSP 推流
cd /home/topeet/AiControlSkynet3588
./scripts/multi_stream.sh

# 终端2: 启动 Qt 界面推理
cd build
./multi_rtsp_mpp_test
```

**预期结果：**
- 4 路 RTSP 同时解码推理
- 每路 ~26 fps
- 检测框正确显示
- NPU 三核轮转

### 2. 命令行单通道推理

```bash
./mpp_rtsp_cli_test rtsp://192.168.137.251:8554/cow0
```

**预期结果：**
- 单路 RTSP 解码推理
- 终端输出检测结果和 FPS

### 3. NPU 压力测试

```bash
./stress_test
```

**预期结果：**
- 16 路 NPU 并行推理
- 总吞吐量 105 fps
- 延迟 ~9.5ms

### 4. USB 摄像头测试

```bash
# 检查摄像头设备
ls /dev/video*

# 运行多路摄像头测试
./multi_camera_test
```

### 5. 单路 RTSP 测试

```bash
# MPP 硬解
./rtsp_video_test

# FFmpeg 软解
./rtsp_test
```

### 6. 视频文件测试

```bash
# MPP 硬解
./video_test

# FFmpeg 软解
./ffmpeg_video_test
```

### 7. 简化推理测试（无需视频源）

```bash
./simple_qt_test
```

## 性能指标

| 测试项 | 目标值 | 实际值 |
|--------|--------|--------|
| 单路 RTSP + MPP 解码 | 30 fps | 30 fps |
| 4 路 RTSP + MPP 解码推理 | 25 fps/路 | 26 fps/路 |
| 16 路 NPU 并行推理 | 100 fps | 105 fps |
| RKNN 推理延迟 | <40ms | ~9.5ms |
| NPU 3核利用率 | >80% | ~82% |

## 常见问题

### 1. MPP 库找不到
```bash
sudo apt install rockchip-mpp
```

### 2. RGA 库找不到
```bash
sudo apt install librga
```

### 3. RKNN 库找不到
从 Rockchip 官网下载 RKNN-SDK2 并安装。

### 4. 摄像头权限问题
```bash
sudo chmod 666 /dev/video*
```

### 5. RTSP 连接失败
- 检查推流服务是否启动：`ps aux | grep mediamtx`
- 检查端口是否开放：`netstat -tlnp | grep 8554`
- 尝试 TCP 传输：确保 `rtsp_transport=tcp`

### 6. NPU 利用率低
- 检查核心绑定：`cat /sys/kernel/debug/rknpu/load`
- 确认使用 `rknn_set_core_mask()`

## 测试报告模板

```
测试日期: YYYY-MM-DD
测试人员: 
测试环境: 
测试结果:
- [ ] 编译通过
- [ ] RTSP 推流正常
- [ ] MPP 解码正常
- [ ] RKNN 推理正常
- [ ] Qt 显示正常
- [ ] 性能达标

问题记录:
1. 
2. 
```
