# VideoControl 代码移植进度

**源项目：** `~/videocontrol` (RK3588 板子)
**本地备份：** `/home/nando/videocontrol_backup/videocontrol/`
**目标项目：** `AIControlSkynet3588`
**移植策略：** ✅ 保留 Qt 界面代码

---

## 阶段 1：代码梳理和文档 ✅

**完成时间：** 2026-04-07 21:00

**输出文档：**
- `VIDEOCONTROL_MIGRATION.md` - 移植规划
- `docs/videocontrol_analysis.md` - 代码详细分析

---

## 阶段 2：后处理模块移植 ✅

**完成时间：** 2026-04-07 21:35

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

**关键函数：**
```cpp
int post_process(int8_t *input0, int8_t *input1, int8_t *input2, 
                 int model_in_h, int model_in_w,
                 float conf_threshold, float nms_threshold,
                 BOX_RECT pads, float scale_w, float scale_h,
                 std::vector<int32_t> &qnt_zps, 
                 std::vector<float> &qnt_scales,
                 detect_result_group_t *group);

void draw_rectangle_yuv420sp(unsigned char* yuv420sp, int w, int h, 
                              int rx, int ry, int rw, int rh, 
                              unsigned int color, int thickness);
```

---

## 阶段 3：Qt 界面代码移植 ✅

**完成时间：** 2026-04-07 21:35

**移植文件：**
| 源文件 | 目标位置 | 功能 |
|--------|---------|------|
| `videocontrol.cpp/h` | `src/qt_ui/` | Qt 视频控制窗口 |
| `frmvideocontrol.cpp/h/ui` | `src/qt_ui/` | Qt 主窗口 UI |
| `mpprknnvideo.cpp/h` | `src/qt_ui/` | MPP + RKNN 视频处理 |
| `videocontrol.pro` | `src/qt_ui/` | Qt 项目文件 |

**Qt 界面功能：**
- 多画面布局（4/9/16/25/36/64）
- 视频显示
- 检测结果可视化
- 用户交互控制

---

## 阶段 4：MPP 解码器移植 ✅

**完成时间：** 2026-04-07 21:35

**移植文件：**
| 源文件 | 目标位置 | 功能 |
|--------|---------|------|
| `mpp_decoder.cpp/h` | `src/decoder/` | MPP H.264/H.265 解码器 |
| `mpp_encoder.cpp/h` | `src/decoder/` | MPP H.264 编码器 |

**解码器功能：**
- 支持 H.264/H.265 解码
- 帧回调机制
- FPS 控制

---

## 当前文件结构

```
src/
├── main.cpp                    # 主程序（多路解码）
├── stress_test.cpp             # 压力测试
├── rknn_optimized.cpp          # RKNN 优化引擎（独立）
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
│   ├── videocontrol.cpp/h      # 视频控制窗口
│   ├── frmvideocontrol.cpp/h/ui # 主窗口 UI
│   ├── mpprknnvideo.cpp/h      # MPP + RKNN 处理
│   └── videocontrol.pro        # Qt 项目文件
│
└── utils/                      # 工具模块
    └── drawing.cpp/h           # YUV 绘制（移植）
```

---

## 待完成任务

### 阶段 5：代码适配

- [ ] 修改 `postprocess.cpp` 标签路径
- [ ] 去除 Qt 依赖的回调函数
- [ ] 统一 RKNN 接口
- [ ] 创建 CMakeLists.txt（Qt 项目）

### 阶段 6：集成测试

- [ ] 编译测试
- [ ] 单路视频解码测试
- [ ] 多路视频显示测试
- [ ] RKNN 推理集成测试

---

## 技术要点

### 后处理参数

```cpp
#define OBJ_CLASS_NUM 2         // 类别数（需修改）
#define NMS_THRESH 0.45         // NMS 阈值
#define BOX_THRESH 0.25         // 置信度阈值
```

### 标签文件

```cpp
#define LABEL_NALE_TXT_PATH "./model/coco_80_labels_list.txt"
```

需要修改为实际标签文件路径。

### 绘制颜色

```cpp
// YUV420SP 格式绘制
draw_rectangle_yuv420sp(yuv_data, width, height, 
                        x, y, w, h, 
                        0x00FF0000,  // 红色
                        2);          // 线宽
```

---

**更新时间：** 2026-04-07 21:35
**状态：** 阶段 2-4 完成，待代码适配
