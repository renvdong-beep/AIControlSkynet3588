# VideoControl 移植与代码分析

**源项目：** `~/videocontrol` (RK3588 板子)
**本地备份：** `/home/nando/videocontrol_backup/videocontrol/`
**目标项目：** `AIControlSkynet3588`
**移植策略：** ✅ 保留 Qt 界面代码

---

## 一、源代码结构分析

### 1.1 核心文件

| 文件 | 大小 | 功能 | 移植优先级 |
|------|------|------|-----------|
| `mpprknnvideo.cpp/h` | 21KB | MPP + RKNN 视频处理主逻辑 | ⭐⭐⭐ |
| `utils/mpp_decoder.cpp/h` | - | MPP 解码器封装 | ⭐⭐⭐ |
| `utils/mpp_encoder.cpp/h` | - | MPP 编码器封装 | ⭐⭐ |
| `videocontrol.cpp/h` | 15KB | Qt 视频控制窗口 | ⭐ |
| `frmvideocontrol.cpp/h` | 39KB | Qt 主窗口 UI | ⭐ |
| `postprocess.cpp/h` | - | RKNN 后处理 | ⭐⭐⭐ |
| `preprocess.cpp/h` | - | RKNN 预处理 | ⭐⭐⭐ |
| `drawing.cpp/h` | - | YUV 绘制矩形 | ⭐⭐ |

### 1.2 依赖库

- **Qt5** - GUI 框架
- **MPP** - Rockchip 多媒体处理
- **RKNN** - NPU 推理
- **RGA** - 图形加速
- **ZLMediaKit** - RTSP 流媒体（可选）

---

## 二、核心模块详细分析

### 2.1 MppRknnVideo 类

**文件：** `mpprknnvideo.cpp/h`

**职责：** MPP 解码 + RKNN 推理的主流程控制、RTSP 流和视频文件处理、回调机制

**关键成员：**
```cpp
class MppRknnVideo : public QThread {
    uchar* m_framebuff;  // 帧缓冲
};
```

**核心函数：**

#### init_model()
```cpp
static int init_model(const char *model_path, rknn_app_context_t *app_ctx, int core_num)
```
- 加载 RKNN 模型
- 设置 NPU 核心绑定（0/1/2/3）
- 查询输入输出张量信息

#### inference_model()
```cpp
static int inference_model(rknn_app_context_t *app_ctx, image_frame_t *img, detect_result_group_t *detect_result)
```
- RGA 缩放（NV12 → RGB888）
- RKNN 推理
- 后处理（NMS）

#### mpp_decoder_frame_callback()
- 解码帧回调
- 执行推理
- 绘制检测框
- 编码输出
- Qt 显示回调

### 2.2 MppDecoder 类

**文件：** `utils/mpp_decoder.cpp/h`

**职责：** MPP 解码器封装，支持 H.264/H.265 解码，帧回调机制

**关键成员：**
```cpp
class MppDecoder {
    MppCtx mpp_ctx;
    MppApi *mpp_mpi;
    MppDecoderFrameCallback callback;
    void* userdata;
};
```

**解码流程：**
```
decode_put_packet() → decode_get_frame() → callback()
```

**FPS 控制：**
```cpp
unsigned long cur_time_ms = GetCurrentTimeMS();
long time_gap = 1000/this->fps - (cur_time_ms - this->last_frame_time_ms);
if (time_gap > 0) usleep(time_gap * 1000);
```

### 2.3 后处理模块

**文件：** `postprocess.cpp/h`

**关键函数：**
```cpp
void post_process(int8_t *input0, int8_t *input1, int8_t *input2,
                  int model_height, int model_width,
                  float box_conf_threshold, float nms_threshold,
                  BOX_RECT pads, float scale_w, float scale_h,
                  std::vector<int32_t> &qnt_zps, std::vector<float> &qnt_scales,
                  detect_result_group_t *group);
```

**输出结构：**
```cpp
typedef struct {
    detect_result_t results[OBJ_NUMB_MAX_SIZE];
    int count;
} detect_result_group_t;
```

### 2.4 绘制模块

**文件：** `drawing.cpp/h`

```cpp
void draw_rectangle_yuv420sp(unsigned char *buffer, int width, int height,
                              int x, int y, int w, int h,
                              unsigned int color, int thickness);
```

---

## 三、数据流分析

### 3.1 RTSP 流处理

```
RTSP URL → ZLMediaKit → on_track_frame_out() → MppDecoder::Decode() → callback()
```

### 3.2 文件处理

```
视频文件 → read_file_data() → process_video_file() → MppDecoder::Decode()
```

### 3.3 完整处理流程

```
输入源（RTSP/文件）
    ↓
MppDecoder::Decode()
    ↓
解码帧（NV12）
    ↓
mpp_decoder_frame_callback()
    ↓
inference_model()
    ├─ RGA 缩放（NV12 → RGB888 640x640）
    ├─ RKNN 推理
    └─ 后处理（NMS）
    ↓
draw_rectangle_yuv420sp()
    ↓
MppEncoder::Encode()
    ↓
输出 H.264 文件 + Qt 显示
```

---

## 四、功能对比

| 功能 | videocontrol | AIControlSkynet3588 | 移植后 |
|------|-------------|---------------------|--------|
| 输入源 | RTSP/文件 | USB 摄像头 | RTSP + USB |
| 解码格式 | H.264/H.265 | MJPEG | 全支持 |
| 并行路数 | 1 路 | 16 路 | 16 路 |
| NPU 优化 | 单核 | 3 核并行 | 3 核并行 |
| 推理性能 | ~30 fps | 105 fps | 105 fps |
| 输出 | H.264 文件 | - | H.264 文件 |

---

## 五、移植进度

### 阶段 1：代码梳理和文档 ✅ (2026-04-07)

### 阶段 2：后处理模块移植 ✅ (2026-04-07)

**移植文件：**
| 源文件 | 目标位置 | 功能 |
|--------|---------|------|
| `postprocess.cpp/h` | `src/inference/` | YOLOv5 后处理（NMS） |
| `preprocess.cpp/h` | `src/inference/` | RKNN 预处理 |
| `drawing.cpp/h` | `src/utils/` | YUV 绘制矩形 |

**关键数据结构：**
```cpp
typedef struct {
    char name[16];
    BOX_RECT box;
    float prop;
} detect_result_t;

typedef struct {
    int id;
    int count;
    detect_result_t results[64];
} detect_result_group_t;
```

### 阶段 3：Qt 界面代码移植 ✅ (2026-04-07)

**移植文件：**
| 源文件 | 目标位置 | 功能 |
|--------|---------|------|
| `videocontrol.cpp/h` | `src/qt_ui/` | Qt 视频控制窗口 |
| `frmvideocontrol.cpp/h/ui` | `src/qt_ui/` | Qt 主窗口 UI |
| `mpprknnvideo.cpp/h` | `src/qt_ui/` | MPP + RKNN 视频处理 |

### 阶段 4：MPP 解码器移植 ✅ (2026-04-07)

**移植文件：**
| 源文件 | 目标位置 | 功能 |
|--------|---------|------|
| `mpp_decoder.cpp/h` | `src/decoder/` | MPP H.264/H.265 解码器 |
| `mpp_encoder.cpp/h` | `src/decoder/` | MPP H.264 编码器 |

### 阶段 5：代码适配（待完成）

- [ ] 修改 `postprocess.cpp` 标签路径
- [ ] 去除 Qt 依赖的回调函数
- [ ] 统一 RKNN 接口
- [ ] 创建 CMakeLists.txt（Qt 项目）

### 阶段 6：集成测试（待完成）

- [ ] 编译测试
- [ ] 单路视频解码测试
- [ ] 多路视频显示测试
- [ ] RKNN 推理集成测试

---

## 六、移植策略

### 6.1 保留 Qt 界面

**✅ 需要移植的 Qt 相关代码：**
- `videocontrol.cpp/h` - Qt 视频控制窗口（多画面布局）
- `frmvideocontrol.cpp/h` - Qt 主窗口 UI
- Qt 显示回调机制

**移植后用途：**
- 多路视频显示界面（4/9/16/25/36/64 画面布局）
- 检测结果可视化
- 用户交互控制

### 6.2 可复用代码

| 模块 | 复用程度 | 说明 |
|------|---------|------|
| postprocess.cpp/h | 100% | 直接复用 |
| drawing.cpp/h | 100% | 直接复用 |
| mpp_decoder.cpp/h | 80% | 去除 Qt 依赖 |
| mpprknnvideo.cpp | 60% | 提取核心逻辑 |

### 6.3 需要适配的代码

1. **Qt 回调** → 通用回调接口
2. **单路处理** → 多路并行架构
3. **RTSP 依赖** → 可选模块

---

## 七、代码架构设计

### 7.1 新增文件结构

```
src/
├── main.cpp                    # 主程序（多路解码）
├── stress_test.cpp             # 压力测试
├── rknn_optimized.cpp          # RKNN 优化引擎（独立）
│
├── capture/                    # 采集模块
│   ├── v4l2_capture.cpp/h      # V4L2 采集（已有）
│   └── rtsp_capture.cpp/h      # RTSP 采集（新增）
│
├── decoder/                    # 解码模块
│   ├── mpp_decoder.cpp/h       # H.264/H.265 解码器（移植）
│   └── mpp_encoder.cpp/h       # H.264 编码器（移植）
│
├── inference/                  # 推理模块
│   ├── postprocess.cpp/h       # 后处理（移植）
│   └── preprocess.cpp/h        # 预处理（移植）
│
├── qt_ui/                      # Qt 界面（移植）
│   ├── multi_rtsp_mpp_test.cpp # Qt 多路 RTSP+MPP+RKNN
│   ├── multi_rtsp_benchmark_ui.cpp # Qt 多路性能基准测试
│   └── ...                     # 其他 Qt 测试程序
│
└── utils/                      # 工具模块
    └── drawing.cpp/h           # YUV 绘制（移植）
```

### 7.2 统一接口设计

```cpp
// 采集接口
class CaptureInterface {
public:
    virtual int init(const std::string& source) = 0;
    virtual int getFrame(Frame& frame) = 0;
    virtual int release() = 0;
};

// 解码接口
class DecoderInterface {
public:
    virtual int init(int codec_type) = 0;
    virtual int decode(uint8_t* data, int size, Frame& out) = 0;
    virtual int release() = 0;
};

// 推理接口
class InferenceInterface {
public:
    virtual int init(const std::string& model, int core_mask) = 0;
    virtual int inference(const Frame& frame, DetectResult& result) = 0;
    virtual int release() = 0;
};
```

---

## 八、关键代码片段

### 8.1 MPP 解码器初始化（H.264）

```cpp
int MppDecoder::Init(int video_type, int fps, void* userdata) {
    if(video_type == 264) mpp_type = MPP_VIDEO_CodingAVC;
    else if (video_type == 265) mpp_type = MPP_VIDEO_CodingHEVC;
    
    mpp_create(&mpp_ctx, &mpp_mpi);
    mpp_init(mpp_ctx, MPP_CTX_DEC, mpp_type);
    
    MppDecCfg cfg;
    mpp_dec_cfg_init(&cfg);
    mpp_dec_cfg_set_u32(cfg, "base:split_parse", need_split);
    mpp_mpi->control(mpp_ctx, MPP_DEC_SET_CFG, cfg);
}
```

### 8.2 RKNN 推理（带核心绑定）

```cpp
rknn_core_mask core_mask;
switch (core_num) {
    case 0: core_mask = RKNN_NPU_CORE_0; break;
    case 1: core_mask = RKNN_NPU_CORE_1; break;
    case 2: core_mask = RKNN_NPU_CORE_2; break;
    case 3: core_mask = RKNN_NPU_CORE_0_1_2; break;
}
rknn_set_core_mask(ctx, core_mask);
```

### 8.3 RGA 格式转换

```cpp
rga_buffer_t src = wrapbuffer_virtualaddr((void *)img->virt_addr, 
                                           img->width, img->height, 
                                           RK_FORMAT_YCbCr_420_SP);
rga_buffer_t dst = wrapbuffer_virtualaddr((void *)resize_buf, 
                                           model_width, model_height, 
                                           RK_FORMAT_RGB_888);
imresize(src, dst);
```

---

## 九、性能分析

### 9.1 解码性能

| 分辨率 | 格式 | 帧率 | 延迟 |
|--------|------|------|------|
| 1080p | H.264 | 30 fps | ~33ms |
| 1080p | H.265 | 30 fps | ~35ms |
| 720p | H.264 | 60 fps | ~16ms |

### 9.2 推理性能

| 模型 | 输入尺寸 | 单核 | 三核 |
|------|---------|------|------|
| YOLOv5s | 640x640 | ~30 fps | ~90 fps |
| YOLOv5m | 640x640 | ~20 fps | ~60 fps |

### 9.3 瓶颈分析

1. **解码瓶颈**：H.264 解码比 MJPEG 慢
2. **网络瓶颈**：RTSP 流延迟
3. **单路限制**：未优化多路并行

---

## 十、风险和注意事项

### 10.1 依赖冲突

- **Qt 依赖**：videocontrol 大量使用 Qt，需要剥离
- **ZLMediaKit**：RTSP 依赖，可选安装
- **MPP 版本**：确保版本兼容

### 10.2 性能影响

- RTSP 流可能引入网络延迟
- H.264 解码比 MJPEG 慢
- 多路 RTSP 需要更多带宽

### 10.3 代码独立性

- `rknn_optimized.cpp` 保持独立
- 不修改已优化的多核并行逻辑
- 新增功能通过接口集成

---

## 十一、测试计划

### 11.1 单元测试

```bash
./test_decoder_h264 test.h264
./test_postprocess
./test_drawing
```

### 11.2 集成测试

```bash
./multi_cam_decoder --rtsp rtsp://192.168.0.177:8554/stream
./multi_cam_decoder --usb /dev/video21 --rtsp rtsp://...
```

### 11.3 性能测试

- [ ] 单路 RTSP 性能
- [ ] 多路 RTSP 性能
- [ ] 混合输入性能

---

## 十二、时间规划

| 阶段 | 任务 | 预计时间 | 状态 |
|------|------|---------|------|
| 阶段 1 | 代码梳理和文档 | 1 天 | ✅ |
| 阶段 2 | 后处理模块移植 | 1 天 | ✅ |
| 阶段 3 | Qt 界面代码移植 | 1 天 | ✅ |
| 阶段 4 | MPP 解码器移植 | 1 天 | ✅ |
| 阶段 5 | 代码适配 | 1 天 | 待完成 |
| 阶段 6 | 集成测试 | 2 天 | 待完成 |

---

**创建时间：** 2026-04-07 21:00
**更新时间：** 2026-04-10
**状态：** 阶段 1-4 完成，待代码适配和集成测试
