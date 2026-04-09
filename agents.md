# AIControlSkynet3588 项目环境配置

## 开发板连接信息

- **设备**: RK3588 开发板 (Firefly AIO-3588Q)
- **IP**: 192.168.137.251
- **用户**: topeet
- **密码**: topeet
- **连接命令**: `sshpass -p 'topeet' ssh topeet@192.168.137.251`
- **同步命令**: `sshpass -p 'topeet' scp <file> topeet@192.168.137.251:/home/topeet/AIControlSkynet3588/`

## GitHub 仓库信息

- **仓库**: https://github.com/renvdong-beep/AIControlSkynet3588
- **用户**: renvdong-beep
- **Token**: 见本地环境变量 GITHUB_TOKEN

## 板子环境

- **系统**: Ubuntu 20.04 (Kernel 5.10.x)
- **架构**: aarch64
- **MPP 版本**: rockchip-mpp
- **RGA 版本**: 1.10.1
- **RKNN SDK**: rknpu2
- **Qt 版本**: Qt5

### 头文件路径

- **MPP**: `/usr/include/rockchip/`
- **RGA**: `/usr/include/rga/`
- **RKNN**: `/home/topeet/rknpu2/runtime/RK3588/Linux/librknn_api/include/`

### 库文件路径

- **MPP**: `/usr/lib/aarch64-linux-gnu/librockchip_mpp.so`
- **RGA**: `/usr/lib/aarch64-linux-gnu/librga.so`
- **RKNN**: `/usr/lib/librknnrt.so`

### RKNN 模型

- **2 类 (cow+person)**: `/home/topeet/rknpu2/examples/rknn_yolov5_demo/install/rknn_yolov5_demo_Linux/model/RK3588/yolov5s-640-640_rk3588.rknn`
- **80 类 (COCO)**: `/home/topeet/rknpu2/examples/rknn_yolov5_demo/model/RK3588/yolov5s-640-640.rknn.bak`

## 编译命令

```bash
cd /home/topeet/AIControlSkynet3588/build
cmake ..
make multi_rtsp_mpp_test -j4   # Qt GUI 多通道
make mpp_rtsp_cli_test -j4      # CLI 单通道测试
```

## 运行命令

```bash
# Qt GUI 模式
./multi_rtsp_mpp_test

# 命令行自动启动
./multi_rtsp_mpp_test --auto --channels 4 --url rtsp://192.168.137.251:8554/cow0

# CLI 测试
./mpp_rtsp_cli_test rtsp://192.168.137.251:8554/cow0 30
```

## RTSP 推流

```bash
# 启动 mediamtx
./mediamtx &

# 推流
ffmpeg -re -stream_loop -1 -i video.mp4 -c:v copy -f rtsp -rtsp_transport tcp rtsp://127.0.0.1:8554/cow0
```

## 项目结构

```
AIControlSkynet3588/
├── CMakeLists.txt
├── readme.md
├── agents.md
├── src/
│   ├── main.cpp                     # 原始主程序入口
│   ├── mpp_rtsp_cli_test.cpp        # CLI 单通道测试
│   ├── qt_ui/
│   │   ├── CMakeLists.txt
│   │   └── multi_rtsp_mpp_test.cpp  # Qt 多通道主程序
│   ├── decoder/
│   │   ├── mpp_decoder.cpp/h
│   │   └── mpp_encoder.cpp/h
│   ├── inference/
│   │   ├── postprocess.cpp/h
│   │   ├── preprocess.cpp/h
│   │   └── preprocess_rga.cpp/h
│   └── utils/
│       └── drawing.cpp/h
└── scripts/
    └── multi_stream.sh
```

## 当前进度

- [x] MPP 硬解码 H.264 (Legacy API)
- [x] RGA NV12→RGB 缩放转换
- [x] RKNN YOLOv5 int8 推理 (2类 cow+person)
- [x] 多通道并行 (1-4路, NPU 三核轮转)
- [x] Qt GUI 界面 (通道选择/地址输入/启停控制)
- [x] RTSP 断线重连
- [x] CLI 测试程序
- [ ] 80 类 COCO 模型适配
- [ ] 外部摄像头 RTSP 接入
