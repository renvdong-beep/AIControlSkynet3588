# VideoControl 代码移植规划

**源项目：** `~/videocontrol` (RK3588 板子)
**本地备份：** `/home/nando/videocontrol_backup/videocontrol/`
**目标项目：** `AIControlSkynet3588`
**目标：** 移植视频解码和 RKNN 推理功能，保留 Qt 界面用于多路视频显示

---

## ⚠️ 移植策略调整

**原策略：** 不移植 Qt GUI，只移植核心功能
**新策略：** ✅ **保留 Qt 代码**，用于多路视频显示界面

**原因：** 用户希望复用 Qt 界面程序对多路视频进行解码推理并显示

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

- **Qt5** - GUI 框架（不移植）
- **MPP** - Rockchip 多媒体处理
- **RKNN** - NPU 推理
- **RGA** - 图形加速
- **ZLMediaKit** - RTSP 流媒体（可选）

---

## 二、功能对比

### 2.1 videocontrol 功能

```
RTSP/文件 → MPP 解码 → RGA 缩放 → RKNN 推理 → 绘制检测框 → MPP 编码 → 输出 H.264
    ↓
Qt 显示（RGB888）
```

**特点：**
- 支持 RTSP 实时流
- 支持 H.264/H.265 文件解码
- 单路处理
- Qt GUI 集成
- NPU 核心绑定（core_num 参数）

### 2.2 AIControlSkynet3588 功能

```
USB 摄像头 → V4L2 MMAP → MPP MJPEG 解码 → RGA 缩放 → RKNN 推理
    ↓
多路并行（16 路）
NPU 3 核并行优化
```

**特点：**
- V4L2 直接采集
- MJPEG 硬件解码
- 多路并行
- NPU 多核优化（105 fps）

---

## 三、移植策略

### 3.1 保留 Qt 界面（策略调整）

**✅ 需要移植的 Qt 相关代码：**
- `videocontrol.cpp/h` - Qt 视频控制窗口（多画面布局）
- `frmvideocontrol.cpp/h` - Qt 主窗口 UI
- Qt 显示回调机制

**移植后用途：**
- 多路视频显示界面（4/9/16/25/36/64 画面布局）
- 检测结果可视化
- 用户交互控制

**不移植的部分：**
- RTSP 流媒体处理（ZLMediaKit 依赖）- 可选
- MPP 编码器（当前项目不需要编码）- 可选

### 3.2 需要移植的功能

#### 高优先级 ⭐⭐⭐

1. **RTSP 流支持**
   - 从 `mpprknnvideo.cpp` 提取 RTSP 处理逻辑
   - 添加到 `src/rtsp_stream.cpp`

2. **H.264/H.265 解码**
   - 从 `utils/mpp_decoder.cpp` 提取解码逻辑
   - 已有 MJPEG 解码，添加 H.264/H.265 支持

3. **后处理模块**
   - `postprocess.cpp/h` - YOLOv5 后处理
   - 统一后处理接口

#### 中优先级 ⭐⭐

4. **绘制检测框**
   - `drawing.cpp/h` - YUV 绘制矩形
   - 用于在解码帧上绘制检测结果

5. **MPP 编码器**
   - `utils/mpp_encoder.cpp/h`
   - 用于输出 H.264 视频（可选）

#### 低优先级 ⭐

6. **Qt 显示回调**
   - 保留回调机制，但去除 Qt 依赖
   - 改为通用回调接口

---

## 四、代码架构设计

### 4.1 新增文件结构

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
│   ├── mpp_decoder_mjpeg.cpp/h # MJPEG 解码（已有）
│   ├── mpp_decoder_h264.cpp/h  # H.264 解码（新增）
│   └── mpp_decoder_h265.cpp/h  # H.265 解码（新增）
│
├── inference/                  # 推理模块
│   ├── rknn_engine.cpp/h       # RKNN 引擎（统一接口）
│   ├── rknn_optimized.cpp/h    # 优化推理（独立）
│   └── postprocess.cpp/h       # 后处理（移植）
│
├── output/                     # 输出模块
│   ├── mpp_encoder.cpp/h       # MPP 编码器（移植）
│   └── drawing.cpp/h           # 绘制检测框（移植）
│
└── utils/                      # 工具模块
    ├── rga_utils.cpp/h         # RGA 工具
    └── common.h                # 通用定义
```

### 4.2 统一接口设计

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

## 五、移植步骤

### 阶段 1：代码梳理和文档（当前）

- [x] 分析 videocontrol 代码结构
- [x] 创建移植规划文档
- [ ] 提取核心代码片段
- [ ] 设计统一接口

### 阶段 2：后处理模块移植

- [ ] 复制 `postprocess.cpp/h`
- [ ] 适配当前项目接口
- [ ] 测试后处理功能

### 阶段 3：H.264/H.265 解码支持

- [ ] 从 `mpp_decoder.cpp` 提取解码逻辑
- [ ] 创建 `mpp_decoder_h264.cpp`
- [ ] 测试 H.264 解码

### 阶段 4：RTSP 流支持

- [ ] 从 `mpprknnvideo.cpp` 提取 RTSP 逻辑
- [ ] 创建 `rtsp_capture.cpp`
- [ ] 测试 RTSP 采集

### 阶段 5：绘制和编码

- [x] 移植 `drawing.cpp`
- [x] 移植 `mpp_encoder.cpp`
- [x] 移除 Qt/OpenCV 依赖
- [x] 更新 CMakeLists.txt
- [x] 代码适配完成

### 阶段 6：集成测试

- [ ] 多路 RTSP + USB 摄像头混合
- [ ] 性能测试
- [ ] 文档更新

---

## 六、关键代码片段

### 6.1 MPP 解码器初始化（H.264）

```cpp
// 来自 videocontrol/utils/mpp_decoder.cpp
int MppDecoder::Init(int video_type, int fps, void* userdata) {
    MPP_RET ret = MPP_OK;
    
    if(video_type == 264) {
        mpp_type = MPP_VIDEO_CodingAVC;  // H.264
    } else if (video_type == 265) {
        mpp_type = MPP_VIDEO_CodingHEVC; // H.265
    }
    
    ret = mpp_create(&mpp_ctx, &mpp_mpi);
    ret = mpp_init(mpp_ctx, MPP_CTX_DEC, mpp_type);
    
    // 配置解码器
    MppDecCfg cfg;
    mpp_dec_cfg_init(&cfg);
    mpp_dec_cfg_set_u32(cfg, "base:split_parse", need_split);
    mpp_mpi->control(mpp_ctx, MPP_DEC_SET_CFG, cfg);
    
    return 0;
}
```

### 6.2 RKNN 推理（带核心绑定）

```cpp
// 来自 videocontrol/mpprknnvideo.cpp
static int init_model(const char *model_path, rknn_app_context_t *app_ctx, int core_num) {
    ret = rknn_init(&ctx, model_data, model_data_size, 0, NULL);
    
    // 核心绑定
    rknn_core_mask core_mask;
    switch (core_num) {
        case 0: core_mask = RKNN_NPU_CORE_0; break;
        case 1: core_mask = RKNN_NPU_CORE_1; break;
        case 2: core_mask = RKNN_NPU_CORE_2; break;
        case 3: core_mask = RKNN_NPU_CORE_0_1_2; break;
    }
    rknn_set_core_mask(ctx, core_mask);
    
    return 0;
}
```

### 6.3 RGA 格式转换

```cpp
// 来自 videocontrol/mpprknnvideo.cpp
rga_buffer_t src = wrapbuffer_virtualaddr((void *)img->virt_addr, 
                                           img->width, img->height, 
                                           RK_FORMAT_YCbCr_420_SP);
rga_buffer_t dst = wrapbuffer_virtualaddr((void *)resize_buf, 
                                           model_width, model_height, 
                                           RK_FORMAT_RGB_888);
imresize(src, dst);
```

### 6.4 YUV 绘制矩形

```cpp
// 来自 videocontrol/drawing.cpp
void draw_rectangle_yuv420sp(unsigned char *buffer, 
                              int width, int height,
                              int x, int y, int w, int h, 
                              unsigned int color, int thickness);
```

---

## 七、性能对比

| 功能 | videocontrol | AIControlSkynet3588 | 移植后 |
|------|-------------|---------------------|--------|
| 输入源 | RTSP/文件 | USB 摄像头 | RTSP + USB |
| 解码格式 | H.264/H.265 | MJPEG | 全支持 |
| 并行路数 | 1 路 | 16 路 | 16 路 |
| NPU 优化 | 单核 | 3 核并行 | 3 核并行 |
| 推理性能 | ~30 fps | 105 fps | 105 fps |
| 输出 | H.264 文件 | - | H.264 文件 |

---

## 八、风险和注意事项

### 8.1 依赖冲突

- **Qt 依赖**：videocontrol 大量使用 Qt，需要剥离
- **ZLMediaKit**：RTSP 依赖，可选安装
- **MPP 版本**：确保版本兼容

### 8.2 性能影响

- RTSP 流可能引入网络延迟
- H.264 解码比 MJPEG 慢
- 多路 RTSP 需要更多带宽

### 8.3 代码独立性

- `rknn_optimized.cpp` 保持独立
- 不修改已优化的多核并行逻辑
- 新增功能通过接口集成

---

## 九、测试计划

### 9.1 单元测试

- [ ] H.264 解码测试
- [ ] H.265 解码测试
- [ ] RTSP 采集测试
- [ ] 后处理测试

### 9.2 集成测试

- [ ] USB + RTSP 混合测试
- [ ] 多路并行测试
- [ ] 长时间稳定性测试

### 9.3 性能测试

- [ ] 单路 RTSP 性能
- [ ] 多路 RTSP 性能
- [ ] 混合输入性能

---

## 十、时间规划

| 阶段 | 任务 | 预计时间 |
|------|------|---------|
| 阶段 1 | 代码梳理和文档 | 1 天 |
| 阶段 2 | 后处理模块移植 | 1 天 |
| 阶段 3 | H.264/H.265 解码 | 2 天 |
| 阶段 4 | RTSP 流支持 | 2 天 |
| 阶段 5 | 绘制和编码 | 1 天 |
| 阶段 6 | 集成测试 | 2 天 |

**总计：** 9 天

---

**创建时间：** 2026-04-07 21:00
**更新时间：** 2026-04-07 21:00
**状态：** 规划中
