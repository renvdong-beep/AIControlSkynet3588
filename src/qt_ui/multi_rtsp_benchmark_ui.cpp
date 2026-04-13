/**
 * @file multi_rtsp_benchmark_ui.cpp
 * @brief Qt UI 版 4 通道 RTSP + MPP 硬解 + RKNN 推理性能基准测试
 * 
 * 对标 multi_rtsp_benchmark.cpp (CLI版)，增加 Qt 可视化界面：
 * - 实时显示 4 路视频画面 + 检测框
 * - 实时 FPS / 推理延迟 / NPU 利用率仪表盘
 * - 性能统计图表（每路独立统计）
 * - CSV 导出
 * 
 * 基于 RTSP_PERFORMANCE_ANALYSIS.md 中的数据对比验证
 * 
 * 使用方法：
 *   ./multi_rtsp_benchmark_ui
 *   ./multi_rtsp_benchmark_ui [rtsp_url0] [rtsp_url1] [rtsp_url2] [rtsp_url3]
 */

#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QGroupBox>
#include <QProgressBar>
#include <QTextEdit>
#include <QFileDialog>
#include <QMessageBox>
#include <QPainter>
#include <QImage>
#include <QPixmap>
#include <QDateTime>
#include <QElapsedTimer>
#include <QScrollArea>
#include <QSplitter>
#include <QFrame>
#include <QComboBox>
#include <QSpinBox>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <functional>
#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <set>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_meta.h>
#include <rockchip/rk_vdec_cmd.h>

#include <rga/RgaApi.h>
#include <rga/im2d.h>
#include <rga/drmrga.h>

#include <rknn_api.h>

// ========== 配置 ==========
#define MODEL_PATH "/home/topeet/rknpu2/examples/rknn_yolov5_demo/model/RK3588/yolov5s-640-640_rk3588.rknn"
#define MODEL_INPUT_SIZE 640
#define OBJ_CLASS_NUM 2
#define BOX_THRESH 0.50
#define PROP_BOX_SIZE (5 + OBJ_CLASS_NUM)  // 7
#define NMS_THRESH 0.45
#define MAX_CHANNELS 4
#define DISPLAY_FPS 25

static const char* CLASS_NAMES[OBJ_CLASS_NUM] = {"cow", "person"};
static const char* DEFAULT_RTSP_URLS[MAX_CHANNELS] = {
    "rtsp://192.168.137.251:8554/cow0",
    "rtsp://192.168.137.251:8554/cow1",
    "rtsp://192.168.137.251:8554/cow2",
    "rtsp://192.168.137.251:8554/cow3"
};

// YOLOv5 anchors
static const int anchor0[6] = {10, 13, 16, 30, 33, 23};
static const int anchor1[6] = {30, 61, 62, 45, 59, 119};
static const int anchor2[6] = {116, 90, 156, 198, 373, 326};

// ========== 数据结构 ==========
typedef struct {
    int left, right, top, bottom;
    float prop;
    int class_id;
} detect_result_t;

// 单通道性能统计
struct ChannelStats {
    int channel_id = 0;
    std::atomic<int> decode_count{0};
    std::atomic<int> infer_count{0};
    std::atomic<int> detect_count{0};
    double decode_ms_sum = 0;
    double infer_ms_sum = 0;
    double e2e_ms_sum = 0;
    std::mutex sum_mutex;  // protects decode_ms_sum, infer_ms_sum, e2e_ms_sum
    std::vector<double> infer_latencies;
    std::vector<double> e2e_latencies;
    std::mutex latency_mutex;
    rknn_core_mask core_mask = RKNN_NPU_CORE_AUTO;
    
    void addDecodeMs(double ms) { std::lock_guard<std::mutex> lk(sum_mutex); decode_ms_sum += ms; }
    void addInferMs(double ms) { std::lock_guard<std::mutex> lk(sum_mutex); infer_ms_sum += ms; }
    void addE2EMs(double ms) { std::lock_guard<std::mutex> lk(sum_mutex); e2e_ms_sum += ms; }
    
    double inferAvg() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(latency_mutex));
        if (infer_latencies.empty()) return 0;
        return std::accumulate(infer_latencies.begin(), infer_latencies.end(), 0.0) / infer_latencies.size();
    }
    double inferP99() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(latency_mutex));
        if (infer_latencies.empty()) return 0;
        auto v = infer_latencies;
        std::sort(v.begin(), v.end());
        return v[(int)(v.size() * 0.99)];
    }
    double inferMin() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(latency_mutex));
        if (infer_latencies.empty()) return 0;
        return *std::min_element(infer_latencies.begin(), infer_latencies.end());
    }
    double inferMax() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(latency_mutex));
        if (infer_latencies.empty()) return 0;
        return *std::max_element(infer_latencies.begin(), infer_latencies.end());
    }
    double e2eAvg() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(latency_mutex));
        if (e2e_latencies.empty()) return 0;
        return std::accumulate(e2e_latencies.begin(), e2e_latencies.end(), 0.0) / e2e_latencies.size();
    }
    void addInferLatency(double ms) {
        std::lock_guard<std::mutex> lock(latency_mutex);
        infer_latencies.push_back(ms);
        if (infer_latencies.size() > 10000) infer_latencies.erase(infer_latencies.begin(), infer_latencies.begin() + 1000);
    }
    void addE2ELatency(double ms) {
        std::lock_guard<std::mutex> lock(latency_mutex);
        e2e_latencies.push_back(ms);
        if (e2e_latencies.size() > 10000) e2e_latencies.erase(e2e_latencies.begin(), e2e_latencies.begin() + 1000);
    }
};

// ========== 工具函数 ==========
static inline int clamp_i(float val, int min, int max) {
    return val > min ? (val < max ? (int)val : max) : min;
}

static float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale) {
    return ((float)qnt - (float)zp) * scale;
}

static int8_t qnt_f32_to_affine(float f32, int32_t zp, float scale) {
    float dst_val = (f32 / scale) + zp;
    int val = (int)dst_val;
    if (val > 127) val = 127;
    if (val < -128) val = -128;
    return (int8_t)val;
}

static int process_feature(int8_t *input, int *anchor, int grid_h, int grid_w,
                           int model_h, int model_w, int stride,
                           std::vector<float> &boxes, std::vector<float> &objProbs,
                           std::vector<int> &classId, float threshold,
                           int32_t zp, float scale) {
    int validCount = 0;
    int grid_len = grid_h * grid_w;
    int8_t thres_i8 = qnt_f32_to_affine(threshold, zp, scale);

    for (int a = 0; a < 3; a++) {
        for (int i = 0; i < grid_h; i++) {
            for (int j = 0; j < grid_w; j++) {
                int8_t box_confidence = input[(PROP_BOX_SIZE * a + 4) * grid_len + i * grid_w + j];
                if (box_confidence >= thres_i8) {
                    int offset = (PROP_BOX_SIZE * a) * grid_len + i * grid_w + j;
                    int8_t *in_ptr = input + offset;
                    float box_x = (deqnt_affine_to_f32(*in_ptr, zp, scale)) * 2.0f - 0.5f;
                    float box_y = (deqnt_affine_to_f32(in_ptr[grid_len], zp, scale)) * 2.0f - 0.5f;
                    float box_w = (deqnt_affine_to_f32(in_ptr[2 * grid_len], zp, scale)) * 2.0f;
                    float box_h = (deqnt_affine_to_f32(in_ptr[3 * grid_len], zp, scale)) * 2.0f;
                    box_x = (box_x + j) * (float)stride;
                    box_y = (box_y + i) * (float)stride;
                    box_w = box_w * box_w * (float)anchor[a * 2];
                    box_h = box_h * box_h * (float)anchor[a * 2 + 1];
                    box_x -= (box_w / 2.0f);
                    box_y -= (box_h / 2.0f);

                    int8_t maxClassProbs = in_ptr[5 * grid_len];
                    int maxClassId = 0;
                    for (int k = 1; k < OBJ_CLASS_NUM; k++) {
                        int8_t prob = in_ptr[(5 + k) * grid_len];
                        if (prob > maxClassProbs) {
                            maxClassId = k;
                            maxClassProbs = prob;
                        }
                    }
                    if (maxClassProbs > thres_i8) {
                        objProbs.push_back(deqnt_affine_to_f32(maxClassProbs, zp, scale) *
                                           deqnt_affine_to_f32(box_confidence, zp, scale));
                        classId.push_back(maxClassId);
                        boxes.push_back(box_x);
                        boxes.push_back(box_y);
                        boxes.push_back(box_w);
                        boxes.push_back(box_h);
                        validCount++;
                    }
                }
            }
        }
    }
    return validCount;
}

static float iou(float x1, float y1, float w1, float h1, float x2, float y2, float w2, float h2) {
    float lx = std::max(x1, x2), ly = std::max(y1, y2);
    float rx = std::min(x1 + w1, x2 + w2), ry = std::min(y1 + h1, y2 + h2);
    float inter = std::max(0.f, rx - lx) * std::max(0.f, ry - ly);
    float union_area = w1 * h1 + w2 * h2 - inter;
    return inter / (union_area + 1e-6f);
}

static void nms(std::vector<float> &boxes, std::vector<int> &classId,
                std::vector<int> &indexArray, int c, float threshold) {
    for (int i = 0; i < (int)indexArray.size(); i++) {
        if (indexArray[i] == -1 || classId[indexArray[i]] != c) continue;
        for (int j = i + 1; j < (int)indexArray.size(); j++) {
            if (indexArray[j] == -1 || classId[indexArray[j]] != c) continue;
            int n = indexArray[i], m = indexArray[j];
            if (iou(boxes[n*4], boxes[n*4+1], boxes[n*4+2], boxes[n*4+3],
                    boxes[m*4], boxes[m*4+1], boxes[m*4+2], boxes[m*4+3]) > threshold)
                indexArray[j] = -1;
        }
    }
}

// ========== MPP 解码器 ==========
class MPPH264Decoder {
public:
    MPPH264Decoder(int id=0) : id_(id), ctx_(nullptr), api_(nullptr),
        frm_grp_(nullptr), initialized_(false), width_(0), height_(0),
        hor_stride_(0), ver_stride_(0), got_keyframe_(false),
        send_count_(0), frame_count_(0), info_change_done_(false),
        last_frame_buf_(nullptr), last_frame_size_(0) {}

    ~MPPH264Decoder() {
        if (frm_grp_) mpp_buffer_group_put(frm_grp_);
        if (ctx_) mpp_destroy(ctx_);
    }

    bool init() {
        MPP_RET ret = mpp_create(&ctx_, &api_);
        if (ret != MPP_OK) return false;
        MppParam param = nullptr;
        api_->control(ctx_, MPP_DEC_SET_PARSER_SPLIT_MODE, &param);
        ret = mpp_init(ctx_, MPP_CTX_DEC, MPP_VIDEO_CodingAVC);
        if (ret != MPP_OK) return false;
        initialized_ = true;
        printf("[MPP#%d] H.264 decoder initialized\n", id_);
        return true;
    }

    bool send_packet(const uint8_t* data, size_t size, bool is_keyframe = false) {
        if (!initialized_ || !data || size == 0) return false;
        int nalu_type = -1;
        for (size_t i = 0; i + 4 < size; i++) {
            if (data[i]==0 && data[i+1]==0 && data[i+2]==0 && data[i+3]==1) {
                nalu_type = data[i+4] & 0x1F; break;
            }
        }
        bool is_config = (nalu_type == 7 || nalu_type == 8) && !is_keyframe;
        if (is_keyframe) { got_keyframe_ = true; printf("[MPP#%d] 关键帧 NALU=%d size=%zu\n", id_, nalu_type, size); }
        if (!got_keyframe_ && !is_config) return false;

        void* pkt_data = malloc(size);
        if (!pkt_data) return false;
        memcpy(pkt_data, data, size);
        MppPacket packet = nullptr;
        MPP_RET ret = mpp_packet_init(&packet, pkt_data, size);
        if (ret != MPP_OK) { free(pkt_data); return false; }
        mpp_packet_set_length(packet, size);
        ret = api_->decode_put_packet(ctx_, packet);
        mpp_packet_deinit(&packet);
        if (ret == -1012) {
            drain_frames();
            void* rdata = malloc(size); if (!rdata) return false;
            memcpy(rdata, data, size);
            MppPacket rpkt = nullptr;
            ret = mpp_packet_init(&rpkt, rdata, size);
            if (ret != MPP_OK) { free(rdata); return false; }
            mpp_packet_set_length(rpkt, size);
            ret = api_->decode_put_packet(ctx_, rpkt);
            mpp_packet_deinit(&rpkt);
            if (ret != MPP_OK) return false;
        } else if (ret != MPP_OK) { return false; }
        send_count_++;
        return true;
    }

    typedef std::function<void(const uint8_t* nv12, int w, int h, int hs, int vs)> FrameCallback;
    int drain_frames(FrameCallback cb = nullptr) {
        int got = 0;
        while (true) {
            MppFrame frame = nullptr;
            MPP_RET ret = api_->decode_get_frame(ctx_, &frame);
            if (ret != MPP_OK || !frame) break;
            if (mpp_frame_get_info_change(frame)) { handle_info_change(frame); mpp_frame_deinit(&frame); continue; }
            MppBuffer frm_buf = mpp_frame_get_buffer(frame);
            if (frm_buf) {
                width_ = mpp_frame_get_width(frame);
                height_ = mpp_frame_get_height(frame);
                hor_stride_ = mpp_frame_get_hor_stride(frame);
                ver_stride_ = mpp_frame_get_ver_stride(frame);
                size_t buf_size = mpp_buffer_get_size(frm_buf);
                uint8_t* src = (uint8_t*)mpp_buffer_get_ptr(frm_buf);
                if (buf_size > last_frame_size_) { last_frame_data_.resize(buf_size); last_frame_size_ = buf_size; }
                memcpy(last_frame_data_.data(), src, buf_size);
                last_frame_buf_ = last_frame_data_.data();
                frame_count_++;
                if (frame_count_ <= 5 || frame_count_ % 100 == 0)
                    printf("[MPP#%d] Frame #%d, %dx%d (stride %d:%d)\n", id_, frame_count_, width_, height_, hor_stride_, ver_stride_);
                got++;
                if (cb) cb(last_frame_buf_, width_, height_, hor_stride_, ver_stride_);
            }
            mpp_frame_deinit(&frame);
        }
        return got;
    }

    uint8_t* get_last_frame() const { return last_frame_buf_; }
    uint32_t get_width() const { return width_; }
    uint32_t get_height() const { return height_; }
    uint32_t get_hor_stride() const { return hor_stride_; }
    uint32_t get_ver_stride() const { return ver_stride_; }
    int get_frame_count() const { return frame_count_; }
    int get_send_count() const { return send_count_; }

private:
    void handle_info_change(MppFrame frame) {
        uint32_t w = mpp_frame_get_width(frame), h = mpp_frame_get_height(frame);
        uint32_t hs = mpp_frame_get_hor_stride(frame), vs = mpp_frame_get_ver_stride(frame);
        uint32_t bs = mpp_frame_get_buf_size(frame);
        printf("[MPP#%d] Info change: %dx%d, stride [%d:%d], buf_size=%d\n", id_, w, h, hs, vs, bs);
        if (info_change_done_) { api_->control(ctx_, MPP_DEC_SET_INFO_CHANGE_READY, NULL); return; }
        MppBufferGroup grp = nullptr;
        MPP_RET ret = mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_ION);
        if (ret != MPP_OK) ret = mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_DMA_HEAP);
        if (ret != MPP_OK) return;
        for (int i = 0; i < 24; i++) { MppBuffer fb; mpp_buffer_get(grp, &fb, bs); }
        ret = api_->control(ctx_, MPP_DEC_SET_EXT_BUF_GROUP, grp);
        if (ret != MPP_OK) { mpp_buffer_group_put(grp); return; }
        frm_grp_ = grp;
        api_->control(ctx_, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
        info_change_done_ = true;
        width_ = w; height_ = h; hor_stride_ = hs; ver_stride_ = vs;
        printf("[MPP#%d] Info change handled\n", id_);
    }

    int id_; MppCtx ctx_; MppApi* api_; MppBufferGroup frm_grp_;
    bool initialized_; uint32_t width_, height_, hor_stride_, ver_stride_;
    bool got_keyframe_; int send_count_, frame_count_; bool info_change_done_;
    std::vector<uint8_t> last_frame_data_; uint8_t* last_frame_buf_; size_t last_frame_size_;
};

// ========== RKNN 推理器 ==========
class RKNNInferencer {
public:
    RKNNInferencer() : ctx_(0), initialized_(false), id_(-1), n_output_(0) {}
    
    bool init(int id, rknn_core_mask core_mask) {
        id_ = id;
        FILE *fp = fopen(MODEL_PATH, "rb");
        if (!fp) return false;
        fseek(fp, 0, SEEK_END); size_t sz = ftell(fp); fseek(fp, 0, SEEK_SET);
        std::vector<char> data(sz);
        if (fread(data.data(), 1, sz, fp) != sz) { fclose(fp); return false; }
        fclose(fp);
        
        if (rknn_init(&ctx_, data.data(), sz, 0, NULL) < 0) return false;
        rknn_set_core_mask(ctx_, core_mask);
        
        rknn_input_output_num io;
        rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
        n_output_ = io.n_output;
        
        for (int i = 0; i < n_output_; i++) {
            output_attrs_[i].index = i;
            rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &output_attrs_[i], sizeof(output_attrs_[i]));
        }
        
        initialized_ = true;
        printf("[RKNN#%d] Initialized, core=%d, outputs=%d\n", id, (int)core_mask, n_output_);
        return true;
    }
    
    int infer(uint8_t *rgb_data, int src_w, int src_h, std::vector<detect_result_t> &results) {
        if (!initialized_) return -1;
        
        rknn_input input = {};
        input.index = 0; input.type = RKNN_TENSOR_UINT8;
        input.size = MODEL_INPUT_SIZE * MODEL_INPUT_SIZE * 3; input.buf = rgb_data;
        
        if (rknn_inputs_set(ctx_, 1, &input) != RKNN_SUCC) return -2;
        if (rknn_run(ctx_, NULL) != RKNN_SUCC) return -3;
        
        rknn_output outputs[3] = {};
        for (int i = 0; i < n_output_; i++) outputs[i].want_float = 1;
        if (rknn_outputs_get(ctx_, n_output_, outputs, NULL) != RKNN_SUCC) return -4;
        
        int grid_h0 = MODEL_INPUT_SIZE/8, grid_w0 = MODEL_INPUT_SIZE/8;
        int grid_h1 = MODEL_INPUT_SIZE/16, grid_w1 = MODEL_INPUT_SIZE/16;
        int grid_h2 = MODEL_INPUT_SIZE/32, grid_w2 = MODEL_INPUT_SIZE/32;
        
        std::vector<float> filterBoxes, objProbs;
        std::vector<int> classId;
        int validCount = 0;
        
        if (n_output_ >= 1 && outputs[0].size > 0)
            validCount += process_feature((int8_t*)outputs[0].buf, (int*)anchor0,
                grid_h0, grid_w0, MODEL_INPUT_SIZE, MODEL_INPUT_SIZE, 8,
                filterBoxes, objProbs, classId, BOX_THRESH, output_attrs_[0].zp, output_attrs_[0].scale);
        if (n_output_ >= 2 && outputs[1].size > 0)
            validCount += process_feature((int8_t*)outputs[1].buf, (int*)anchor1,
                grid_h1, grid_w1, MODEL_INPUT_SIZE, MODEL_INPUT_SIZE, 16,
                filterBoxes, objProbs, classId, BOX_THRESH, output_attrs_[1].zp, output_attrs_[1].scale);
        if (n_output_ >= 3 && outputs[2].size > 0)
            validCount += process_feature((int8_t*)outputs[2].buf, (int*)anchor2,
                grid_h2, grid_w2, MODEL_INPUT_SIZE, MODEL_INPUT_SIZE, 32,
                filterBoxes, objProbs, classId, BOX_THRESH, output_attrs_[2].zp, output_attrs_[2].scale);
        
        if (validCount > 0) {
            std::vector<int> indexArray(validCount);
            for (int i = 0; i < validCount; i++) indexArray[i] = i;
            for (int i = 0; i < validCount - 1; i++)
                for (int j = i + 1; j < validCount; j++)
                    if (objProbs[indexArray[i]] < objProbs[indexArray[j]])
                        std::swap(indexArray[i], indexArray[j]);
            
            std::set<int> class_set(classId.begin(), classId.end());
            for (auto c : class_set) nms(filterBoxes, classId, indexArray, c, NMS_THRESH);
            
            float scale_w = (float)src_w / MODEL_INPUT_SIZE;
            float scale_h = (float)src_h / MODEL_INPUT_SIZE;
            
            for (int i = 0; i < validCount; i++) {
                if (indexArray[i] == -1) continue;
                int n = indexArray[i];
                detect_result_t r;
                r.left   = clamp_i(filterBoxes[n*4+0], 0, MODEL_INPUT_SIZE) * scale_w;
                r.top    = clamp_i(filterBoxes[n*4+1], 0, MODEL_INPUT_SIZE) * scale_h;
                r.right  = clamp_i(filterBoxes[n*4+0] + filterBoxes[n*4+2], 0, MODEL_INPUT_SIZE) * scale_w;
                r.bottom = clamp_i(filterBoxes[n*4+1] + filterBoxes[n*4+3], 0, MODEL_INPUT_SIZE) * scale_h;
                r.prop = objProbs[n];
                r.class_id = classId[n];
                results.push_back(r);
            }
        }
        
        rknn_outputs_release(ctx_, n_output_, outputs);
        return (int)results.size();
    }
    int infer_nv12(const uint8_t* nv12_data, int src_w, int src_h, int hor_stride, int ver_stride,
                   std::vector<detect_result_t> &results) {
        if (!initialized_) return -1;
        
        // RGA: NV12 -> RGB888 640x640
        uint8_t *rgb_buf = (uint8_t*)malloc(MODEL_INPUT_SIZE * MODEL_INPUT_SIZE * 3);
        rga_buffer_t src_img = wrapbuffer_virtualaddr((void*)nv12_data, src_w, src_h, RK_FORMAT_YCbCr_420_SP, hor_stride, ver_stride);
        rga_buffer_t dst_img = wrapbuffer_virtualaddr(rgb_buf, MODEL_INPUT_SIZE, MODEL_INPUT_SIZE, RK_FORMAT_BGR_888);
        IM_STATUS st = imresize(src_img, dst_img);
        if (st != IM_STATUS_SUCCESS) { free(rgb_buf); return 0; }
        
        int ret = infer(rgb_buf, src_w, src_h, results);
        free(rgb_buf);
        return ret >= 0 ? results.size() : ret;
    }
    
    ~RKNNInferencer() { if (initialized_) rknn_destroy(ctx_); }

private:
    int id_;
    rknn_context ctx_;
    bool initialized_;
    int n_output_;
    rknn_tensor_attr output_attrs_[3];
};

// ========== 视频显示控件 ==========
class VideoWidget : public QWidget {
public:
    VideoWidget(int channelId, QWidget *parent = nullptr) : QWidget(parent), channel_id_(channelId) {
        setMinimumSize(320, 180);
        setStyleSheet("background-color: #1a1a2e; border: 1px solid #333;");
    }
    
    void updateFrame(const QImage &img, const std::vector<detect_result_t> &results) {
        std::lock_guard<std::mutex> lock(mutex_);
        frame_ = img.copy();
        results_ = results;
        update();
    }
    
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        
        if (frame_.isNull()) {
            p.fillRect(rect(), QColor(26, 26, 46));
            p.setPen(Qt::gray);
            p.setFont(QFont("Monospace", 14));
            p.drawText(rect(), Qt::AlignCenter, QString("CH%1 - No Signal").arg(channel_id_));
            return;
        }
        
        // 绘制视频帧
        QPixmap pixmap = QPixmap::fromImage(frame_).scaled(size(), Qt::KeepAspectRatio, Qt::FastTransformation);
        int x = (width() - pixmap.width()) / 2;
        int y = (height() - pixmap.height()) / 2;
        p.drawPixmap(x, y, pixmap);
        
        // 绘制检测框
        float sx = (float)pixmap.width() / frame_.width();
        float sy = (float)pixmap.height() / frame_.height();
        
        QPen pen;
        pen.setWidth(2);
        
        for (auto &r : results_) {
            QColor color = (r.class_id == 0) ? QColor(0, 255, 0) : QColor(255, 165, 0);
            pen.setColor(color);
            p.setPen(pen);
            
            int rx = x + r.left * sx;
            int ry = y + r.top * sy;
            int rw = (r.right - r.left) * sx;
            int rh = (r.bottom - r.top) * sy;
            p.drawRect(rx, ry, rw, rh);
            
            // 标签
            QString label = QString("%1 %2%").arg(CLASS_NAMES[r.class_id]).arg((int)(r.prop * 100));
            p.setFont(QFont("Monospace", 9, QFont::Bold));
            QFontMetrics fm(p.font());
            int tw = fm.horizontalAdvance(label) + 6;
            p.fillRect(rx, ry - fm.height() - 2, tw, fm.height() + 2, color);
            p.setPen(Qt::black);
            p.drawText(rx + 3, ry - 3, label);
        }
        
        // 通道标签
        p.setPen(Qt::white);
        p.setFont(QFont("Monospace", 11, QFont::Bold));
        p.fillRect(0, 0, 50, 22, QColor(0, 0, 0, 160));
        p.drawText(5, 16, QString("CH%1").arg(channel_id_));
    }

private:
    int channel_id_;
    QImage frame_;
    std::vector<detect_result_t> results_;
    std::mutex mutex_;
};

// ========== 主窗口 ==========
class BenchmarkWindow : public QWidget {
    Q_OBJECT

public:
    BenchmarkWindow(QWidget *parent = nullptr) : QWidget(parent), running_(false), elapsed_sec_(0) {
        setWindowTitle("RK3588 Multi-RTSP Benchmark (Qt UI)");
        setMinimumSize(1200, 800);
        
        // 初始化 RTSP URLs
        for (int i = 0; i < MAX_CHANNELS; i++)
            rtsp_urls_[i] = DEFAULT_RTSP_URLS[i];
        
        setupUI();
        
        // 显示刷新定时器
        display_timer_ = new QTimer(this);
        connect(display_timer_, &QTimer::timeout, this, &BenchmarkWindow::updateDisplay);
        display_timer_->start(1000 / DISPLAY_FPS);
        
        // 统计刷新定时器
        stats_timer_ = new QTimer(this);
        connect(stats_timer_, &QTimer::timeout, this, &BenchmarkWindow::updateStats);
        stats_timer_->start(1000);
    }
    
    ~BenchmarkWindow() {
        stopBenchmark();
    }

private:
    void setupUI() {
        QVBoxLayout *mainLayout = new QVBoxLayout(this);
        
        // === 顶部控制栏 ===
        QHBoxLayout *controlLayout = new QHBoxLayout();
        
        start_btn_ = new QPushButton("▶ Start");
        start_btn_->setStyleSheet("QPushButton { background-color: #4CAF50; color: white; font-weight: bold; padding: 8px 20px; font-size: 14px; }");
        connect(start_btn_, &QPushButton::clicked, this, &BenchmarkWindow::startBenchmark);
        
        stop_btn_ = new QPushButton("⏹ Stop");
        stop_btn_->setStyleSheet("QPushButton { background-color: #f44336; color: white; font-weight: bold; padding: 8px 20px; font-size: 14px; }");
        stop_btn_->setEnabled(false);
        connect(stop_btn_, &QPushButton::clicked, this, &BenchmarkWindow::stopBenchmark);
        
        export_btn_ = new QPushButton("📊 Export CSV");
        export_btn_->setStyleSheet("QPushButton { background-color: #2196F3; color: white; font-weight: bold; padding: 8px 20px; font-size: 14px; }");
        connect(export_btn_, &QPushButton::clicked, this, &BenchmarkWindow::exportCSV);
        
        duration_spin_ = new QSpinBox();
        duration_spin_->setRange(10, 600);
        duration_spin_->setValue(60);
        duration_spin_->setSuffix(" sec");
        duration_spin_->setToolTip("Test duration in seconds");
        
        channels_combo_ = new QComboBox();
        channels_combo_->addItems({"1 Channel", "2 Channels", "3 Channels", "4 Channels"});
        channels_combo_->setCurrentIndex(3);
        
        controlLayout->addWidget(start_btn_);
        controlLayout->addWidget(stop_btn_);
        controlLayout->addWidget(export_btn_);
        controlLayout->addWidget(new QLabel("Duration:"));
        controlLayout->addWidget(duration_spin_);
        controlLayout->addWidget(new QLabel("Channels:"));
        controlLayout->addWidget(channels_combo_);
        controlLayout->addStretch();
        
        // 运行时间标签
        time_label_ = new QLabel("⏱ 00:00");
        time_label_->setStyleSheet("font-size: 16px; font-weight: bold; color: #2196F3;");
        controlLayout->addWidget(time_label_);
        
        mainLayout->addLayout(controlLayout);
        
        // === 中间区域：视频 + 统计 ===
        QSplitter *splitter = new QSplitter(Qt::Horizontal);
        
        // 视频区域 (2x2 grid)
        QGroupBox *videoGroup = new QGroupBox("Video Channels");
        QGridLayout *videoGrid = new QGridLayout(videoGroup);
        for (int i = 0; i < MAX_CHANNELS; i++) {
            video_widgets_[i] = new VideoWidget(i);
            videoGrid->addWidget(video_widgets_[i], i / 2, i % 2);
        }
        splitter->addWidget(videoGroup);
        
        // 统计面板
        QScrollArea *scrollArea = new QScrollArea();
        QWidget *statsWidget = new QWidget();
        QVBoxLayout *statsLayout = new QVBoxLayout(statsWidget);
        
        // 总体概览
        QGroupBox *overviewGroup = new QGroupBox("📊 Overview");
        QVBoxLayout *overviewLayout = new QVBoxLayout(overviewGroup);
        overview_label_ = new QLabel("Waiting to start...");
        overview_label_->setStyleSheet("font-family: Monospace; font-size: 12px;");
        overview_label_->setWordWrap(true);
        overviewLayout->addWidget(overview_label_);
        statsLayout->addWidget(overviewGroup);
        
        // NPU 利用率
        QGroupBox *npuGroup = new QGroupBox("🧠 NPU Utilization");
        QVBoxLayout *npuLayout = new QVBoxLayout(npuGroup);
        npu_label_ = new QLabel("N/A");
        npu_label_->setStyleSheet("font-family: Monospace; font-size: 12px;");
        npuLayout->addWidget(npu_label_);
        statsLayout->addWidget(npuGroup);
        
        // 每通道统计
        for (int i = 0; i < MAX_CHANNELS; i++) {
            QGroupBox *chGroup = new QGroupBox(QString("📺 Channel %1").arg(i));
            QVBoxLayout *chLayout = new QVBoxLayout(chGroup);
            ch_labels_[i] = new QLabel("Idle");
            ch_labels_[i]->setStyleSheet("font-family: Monospace; font-size: 11px;");
            ch_labels_[i]->setWordWrap(true);
            chLayout->addWidget(ch_labels_[i]);
            statsLayout->addWidget(chGroup);
        }
        
        // 对比表格
        QGroupBox *compareGroup = new QGroupBox("📋 CLI vs UI Comparison");
        QVBoxLayout *compareLayout = new QVBoxLayout(compareGroup);
        compare_label_ = new QLabel("Run benchmark to see comparison");
        compare_label_->setStyleSheet("font-family: Monospace; font-size: 11px;");
        compare_label_->setWordWrap(true);
        compareLayout->addWidget(compare_label_);
        statsLayout->addWidget(compareGroup);
        
        statsLayout->addStretch();
        scrollArea->setWidget(statsWidget);
        scrollArea->setWidgetResizable(true);
        scrollArea->setMinimumWidth(350);
        splitter->addWidget(scrollArea);
        
        splitter->setSizes({800, 400});
        mainLayout->addWidget(splitter);
        
        // === 底部日志 ===
        QGroupBox *logGroup = new QGroupBox("📝 Log");
        QVBoxLayout *logLayout = new QVBoxLayout(logGroup);
        log_text_ = new QTextEdit();
        log_text_->setReadOnly(true);
        log_text_->setMaximumHeight(120);
        log_text_->setStyleSheet("font-family: Monospace; font-size: 11px; background-color: #1a1a2e; color: #00ff00;");
        logLayout->addWidget(log_text_);
        mainLayout->addWidget(logGroup);
    }
    
    void log(const QString &msg) {
        QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
        log_text_->append(QString("[%1] %2").arg(timestamp).arg(msg));
    }
    
    void startBenchmark() {
        if (running_) return;
        
        num_channels_ = channels_combo_->currentIndex() + 1;
        duration_sec_ = duration_spin_->value();
        running_ = true;
        elapsed_sec_ = 0;
        
        start_btn_->setEnabled(false);
        stop_btn_->setEnabled(true);
        
        // 重置统计
        for (int i = 0; i < MAX_CHANNELS; i++) {
            stats_ptr_[i].reset(new ChannelStats());
            stats_(i).channel_id = i;
        }
        
        log(QString("Starting benchmark: %1 channels, %2 sec").arg(num_channels_).arg(duration_sec_));
        
        // 初始化 RKNN
        rknn_core_mask core_masks[] = { RKNN_NPU_CORE_0, RKNN_NPU_CORE_1, RKNN_NPU_CORE_2 };
        for (int i = 0; i < num_channels_; i++) {
            rknn_core_mask core = core_masks[i % 3];
            if (!rknn_[i].init(i, core)) {
                log(QString("FATAL: RKNN#%1 init failed").arg(i));
                stopBenchmark();
                return;
            }
            stats_(i).core_mask = core;
            log(QString("RKNN#%1 initialized on Core %2").arg(i).arg((int)core));
        }
        
        // 启动通道线程
        for (int i = 0; i < num_channels_; i++) {
            channel_threads_[i] = new std::thread(&BenchmarkWindow::channelLoop, this, i);
        }
        
        // 启动计时器
        elapsed_timer_ = new QElapsedTimer();
        elapsed_timer_->start();
        
        // 自动停止定时器
        auto_stop_timer_ = new QTimer(this);
        connect(auto_stop_timer_, &QTimer::timeout, this, [this]() {
            elapsed_sec_ = elapsed_timer_->elapsed() / 1000;
            if (elapsed_sec_ >= duration_sec_) {
                stopBenchmark();
            }
        });
        auto_stop_timer_->start(1000);
    }
    
    void stopBenchmark() {
        if (!running_) return;
        running_ = false;
        
        if (auto_stop_timer_) { auto_stop_timer_->stop(); delete auto_stop_timer_; auto_stop_timer_ = nullptr; }
        
        for (int i = 0; i < num_channels_; i++) {
            if (channel_threads_[i] && channel_threads_[i]->joinable()) {
                channel_threads_[i]->join();
                delete channel_threads_[i];
                channel_threads_[i] = nullptr;
            }
        }
        
        start_btn_->setEnabled(true);
        stop_btn_->setEnabled(false);
        
        log("Benchmark stopped.");
        printResults();
    }
    
    void channelLoop(int ch) {
        log(QString("CH%1 starting, RTSP: %2").arg(ch).arg(rtsp_urls_[ch]));
        
        AVFormatContext *fmt_ctx = nullptr;
        AVDictionary *opts = nullptr;
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        av_dict_set(&opts, "stimeout", "5000000", 0);
        av_dict_set(&opts, "fflags", "nobuffer", 0);
        av_dict_set(&opts, "max_delay", "500000", 0);
        
        if (avformat_open_input(&fmt_ctx, rtsp_urls_[ch], nullptr, &opts) < 0) {
            av_dict_free(&opts);
            log(QString("CH%1: Failed to open RTSP").arg(ch));
            return;
        }
        av_dict_free(&opts);
        
        int video_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (video_idx < 0) { avformat_close_input(&fmt_ctx); return; }
        
        AVCodecParameters *codecpar = fmt_ctx->streams[video_idx]->codecpar;
        log(QString("CH%1: %2x%3, codec=%4").arg(ch).arg(codecpar->width).arg(codecpar->height).arg(codecpar->codec_id));
        
        MPPH264Decoder decoder(ch);
        if (!decoder.init()) { avformat_close_input(&fmt_ctx); return; }
        
        // Send extradata (SPS/PPS)
        if (codecpar->extradata && codecpar->extradata_size > 0) {
            int sz = codecpar->extradata_size;
            uint8_t* ed = codecpar->extradata;
            bool is_annexb = (sz >= 4 && ed[0]==0 && ed[1]==0 && ed[2]==0 && ed[3]==1);
            if (is_annexb) {
                decoder.send_packet(ed, sz, false);
            } else {
                int pos = 6;
                while (pos + 2 < sz) {
                    int nl = (ed[pos]<<8)|ed[pos+1]; pos += 2;
                    if (pos + nl > sz) break;
                    std::vector<uint8_t> sc(4+nl); sc[0]=0;sc[1]=0;sc[2]=0;sc[3]=1;
                    memcpy(sc.data()+4, &ed[pos], nl);
                    decoder.send_packet(sc.data(), sc.size(), false);
                    pos += nl;
                }
            }
        }
        
        AVPacket *avpkt = av_packet_alloc();
        int display_counter = 0;
        
        while (running_ && av_read_frame(fmt_ctx, avpkt) >= 0) {
            if (avpkt->stream_index != video_idx) { av_packet_unref(avpkt); continue; }
            
            bool is_keyframe = (avpkt->flags & AV_PKT_FLAG_KEY) != 0;
            auto t_decode_start = std::chrono::steady_clock::now();
            decoder.send_packet(avpkt->data, avpkt->size, is_keyframe);
            
            decoder.drain_frames([&](const uint8_t* nv12, int w, int h, int hs, int vs) {
                auto t_decode_end = std::chrono::steady_clock::now();
                double decode_ms = std::chrono::duration<double, std::milli>(t_decode_end - t_decode_start).count();
                stats_(ch).decode_count++;
                stats_(ch).addDecodeMs(decode_ms);
                
                // RKNN inference
                auto t_infer_start = std::chrono::steady_clock::now();
                std::vector<detect_result_t> results;
                int det_count = rknn_[ch].infer_nv12(nv12, w, h, hs, vs, results);
                auto t_infer_end = std::chrono::steady_clock::now();
                
                double infer_ms = std::chrono::duration<double, std::milli>(t_infer_end - t_infer_start).count();
                double e2e_ms = std::chrono::duration<double, std::milli>(t_infer_end - t_decode_start).count();
                
                stats_(ch).infer_count++;
                stats_(ch).addInferMs(infer_ms);
                stats_(ch).addE2EMs(e2e_ms);
                stats_(ch).detect_count += det_count;
                stats_(ch).addInferLatency(infer_ms);
                stats_(ch).addE2ELatency(e2e_ms);
                
                // Update display frame (every 5th frame)
                display_counter++;
                if (display_counter % 5 == 0) {
                    std::lock_guard<std::mutex> lock(display_mutex_[ch]);
                    uint8_t *display_rgb = (uint8_t*)malloc(w * h * 3);
                    rga_buffer_t disp_src = wrapbuffer_virtualaddr((void*)nv12, hs, vs, RK_FORMAT_YCbCr_420_SP, hs, vs);
                    rga_buffer_t disp_dst = wrapbuffer_virtualaddr(display_rgb, w, h, RK_FORMAT_BGR_888);
                    if (imcvtcolor(disp_src, disp_dst, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGR_888) == IM_STATUS_SUCCESS) {
                        display_image_[ch] = QImage(display_rgb, h, w, w * 3, QImage::Format_RGB888).copy();
                        display_results_[ch] = results;
                    }
                    free(display_rgb);
                }
            });
            av_packet_unref(avpkt);
        }
        
        av_packet_free(&avpkt);
        avformat_close_input(&fmt_ctx);
        log(QString("CH%1 loop ended. Decoded: %2, Inferred: %3")
            .arg(ch).arg(stats_(ch).decode_count.load()).arg(stats_(ch).infer_count.load()));
    }
    
    void updateDisplay() {
        if (!running_) return;
        for (int i = 0; i < num_channels_; i++) {
            std::lock_guard<std::mutex> lock(display_mutex_[i]);
            if (!display_image_[i].isNull()) {
                video_widgets_[i]->updateFrame(display_image_[i], display_results_[i]);
            }
        }
    }
    
    void updateStats() {
        if (!running_ && elapsed_sec_ == 0) return;
        
        if (running_) {
            elapsed_sec_ = elapsed_timer_->elapsed() / 1000;
        }
        if (elapsed_sec_ == 0) return;
        
        int mm = elapsed_sec_ / 60;
        int ss = elapsed_sec_ % 60;
        time_label_->setText(QString("⏱ %1:%2").arg(mm, 2, 10, QChar('0')).arg(ss, 2, 10, QChar('0')));
        
        // 总体概览
        int total_infer = 0, total_detect = 0;
        for (int i = 0; i < num_channels_; i++) {
            total_infer += stats_(i).infer_count;
            total_detect += stats_(i).detect_count;
        }
        double total_fps = (double)total_infer / elapsed_sec_;
        
        QString overview;
        overview += QString("Channels: %1  Duration: %2s\n").arg(num_channels_).arg(elapsed_sec_);
        overview += QString("Total FPS: %1  Frames: %2  Detections: %3\n").arg(total_fps, 0, 'f', 1).arg(total_infer).arg(total_detect);
        overview += QString("Model: YOLOv5s  BOX_THRESH: %1  NMS_THRESH: %2").arg(BOX_THRESH).arg(NMS_THRESH);
        overview_label_->setText(overview);
        
        // NPU 利用率
        FILE *npu_fp = fopen("/sys/kernel/debug/rknpu/load", "r");
        if (npu_fp) {
            char buf[256];
            QString npu_text;
            while (fgets(buf, sizeof(buf), npu_fp)) npu_text += buf;
            fclose(npu_fp);
            npu_label_->setText(npu_text.trimmed());
        }
        
        // 每通道统计
        for (int i = 0; i < num_channels_; i++) {
            double fps = (double)stats_(i).infer_count / elapsed_sec_;
            double avg_infer = stats_(i).infer_count > 0 ? stats_(i).infer_ms_sum / stats_(i).infer_count : 0;
            double avg_decode = stats_(i).decode_count > 0 ? stats_(i).decode_ms_sum / stats_(i).decode_count : 0;
            
            QString ch_text;
            ch_text += QString("NPU Core: %1\n").arg((int)stats_(i).core_mask);
            ch_text += QString("FPS: %1  Frames: %2\n").arg(fps, 0, 'f', 1).arg(stats_(i).infer_count.load());
            ch_text += QString("Avg Infer: %1ms  P99: %2ms\n").arg(avg_infer, 0, 'f', 1).arg(stats_(i).inferP99(), 0, 'f', 1);
            ch_text += QString("Min: %1ms  Max: %2ms\n").arg(stats_(i).inferMin(), 0, 'f', 1).arg(stats_(i).inferMax(), 0, 'f', 1);
            ch_text += QString("Avg Decode: %1ms  E2E: %2ms\n").arg(avg_decode, 0, 'f', 1).arg(stats_(i).e2eAvg(), 0, 'f', 1);
            ch_text += QString("Detections: %1").arg(stats_(i).detect_count.load());
            ch_labels_[i]->setText(ch_text);
        }
        
        // 对比表格
        QString compare;
        compare += "Metric         | CLI Benchmark | Qt UI Benchmark\n";
        compare += "---------------|---------------|---------------\n";
        double cli_fps_per_ch[4] = {26.0, 26.0, 26.0, 26.0}; // RTSP_PERFORMANCE_ANALYSIS.md baseline
        double ui_total = 0;
        for (int i = 0; i < num_channels_; i++) {
            double ui_fps = (double)stats_(i).infer_count / elapsed_sec_;
            ui_total += ui_fps;
            compare += QString("CH%1 FPS       | %2 fps      | %3 fps\n")
                .arg(i).arg(cli_fps_per_ch[i], 0, 'f', 1).arg(ui_fps, 0, 'f', 1);
        }
        compare += QString("---------------|---------------|---------------\n");
        compare += QString("Total FPS      | %1 fps     | %2 fps\n")
            .arg(num_channels_ * 26.0, 0, 'f', 1).arg(ui_total, 0, 'f', 1);
        compare += QString("NPU Util       | ~82%%         | see above");
        compare_label_->setText(compare);
    }
    
    void exportCSV() {
        QString filename = QString("benchmark_%1ch_%2s_ui.csv")
            .arg(num_channels_).arg(elapsed_sec_);
        
        std::string result_file = filename.toStdString();
        FILE *fp = fopen(result_file.c_str(), "w");
        if (fp) {
            fprintf(fp, "channel,npu_core,frames,fps,avg_infer_ms,p99_infer_ms,min_infer_ms,max_infer_ms,detections,e2e_avg_ms,avg_decode_ms\n");
            for (int i = 0; i < num_channels_; i++) {
                double fps = (double)stats_(i).infer_count / (elapsed_sec_ > 0 ? elapsed_sec_ : 1);
                double avg_decode = stats_(i).decode_count > 0 ? stats_(i).decode_ms_sum / stats_(i).decode_count : 0;
                fprintf(fp, "%d,%d,%d,%.1f,%.2f,%.2f,%.2f,%.2f,%d,%.2f,%.2f\n",
                       i, (int)stats_(i).core_mask, stats_(i).infer_count.load(), fps,
                       stats_(i).inferAvg(), stats_(i).inferP99(),
                       stats_(i).inferMin(), stats_(i).inferMax(),
                       stats_(i).detect_count.load(), stats_(i).e2eAvg(), avg_decode);
            }
            fclose(fp);
            log(QString("Results exported to: %1").arg(filename));
            QMessageBox::information(this, "Export", QString("Results saved to:\n%1").arg(filename));
        }
    }
    
    void printResults() {
        log("\n========== BENCHMARK RESULTS ==========");
        
        int total_infer = 0, total_detect = 0;
        for (int i = 0; i < num_channels_; i++) {
            total_infer += stats_(i).infer_count;
            total_detect += stats_(i).detect_count;
        }
        double total_fps = elapsed_sec_ > 0 ? (double)total_infer / elapsed_sec_ : 0;
        
        log(QString("Channels: %1  Duration: %2s  Total FPS: %3")
            .arg(num_channels_).arg(elapsed_sec_).arg(total_fps, 0, 'f', 1));
        
        for (int i = 0; i < num_channels_; i++) {
            double fps = elapsed_sec_ > 0 ? (double)stats_(i).infer_count / elapsed_sec_ : 0;
            log(QString("CH%1: %2 frames, %3 fps, avg_infer=%4ms, p99=%5ms, detections=%6")
                .arg(i).arg(stats_(i).infer_count.load()).arg(fps, 0, 'f', 1)
                .arg(stats_(i).inferAvg(), 0, 'f', 1).arg(stats_(i).inferP99(), 0, 'f', 1)
                .arg(stats_(i).detect_count.load()));
        }
        
        log("========================================");
    }

private:
    // UI 元素
    QPushButton *start_btn_, *stop_btn_, *export_btn_;
    QSpinBox *duration_spin_;
    QComboBox *channels_combo_;
    QLabel *time_label_, *overview_label_, *npu_label_, *compare_label_;
    QLabel *ch_labels_[MAX_CHANNELS];
    QTextEdit *log_text_;
    VideoWidget *video_widgets_[MAX_CHANNELS];
    
    // 定时器
    QTimer *display_timer_, *stats_timer_, *auto_stop_timer_;
    QElapsedTimer *elapsed_timer_;
    
    // 状态
    std::atomic<bool> running_;
    int num_channels_ = 4;
    int duration_sec_ = 60;
    int elapsed_sec_ = 0;
    
    // RTSP
    const char* rtsp_urls_[MAX_CHANNELS];
    
    // RKNN & Stats
    RKNNInferencer rknn_[MAX_CHANNELS];
    std::unique_ptr<ChannelStats> stats_ptr_[MAX_CHANNELS];
    ChannelStats& stats_(int i) { return *stats_ptr_[i]; }
    
    // 显示帧
    QImage display_image_[MAX_CHANNELS];
    std::vector<detect_result_t> display_results_[MAX_CHANNELS];
    std::mutex display_mutex_[MAX_CHANNELS];
    
    // 线程
    std::thread *channel_threads_[MAX_CHANNELS] = {};
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    
    // 可选：通过命令行参数覆盖 RTSP URLs
    // usage: ./multi_rtsp_benchmark_ui [url0] [url1] [url2] [url3]
    
    BenchmarkWindow window;
    window.show();
    
    return app.exec();
}

#include "multi_rtsp_benchmark_ui.moc"
