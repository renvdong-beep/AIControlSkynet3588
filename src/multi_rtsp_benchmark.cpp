/**
 * @file mpp_rtsp_cli_test.cpp
 * @brief MPP+RGA+RKNN RTSP CLI Test v5
 * 
 * 关键修复：
 * - 模型是 2 类 YOLOv5（不是 80 类 COCO）
 * - 输出格式: [1, 21, 80, 80] NHWC，21 = 3*(5+2)
 * - 使用 int8 量化输出 + 反量化
 * - 正确的 YOLOv5 后处理（anchor-based grid decode + NMS）
 * 
 * 编译：cd build && make mpp_rtsp_cli_test -j4
 * 运行：./mpp_rtsp_cli_test [rtsp_url] [duration_seconds]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include <math.h>

#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include <chrono>
#include <set>
#include <algorithm>
#include <mutex>
#include <numeric>
#include <functional>

extern "C" {
#include <libavformat/avformat.h>
}

#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_vdec_cmd.h>

#include <rga/im2d.hpp>
#include <rga/RgaApi.h>

#include <rknn_api.h>

static std::atomic<bool> g_running{true};
static int64_t get_time_ms() { struct timeval tv; gettimeofday(&tv, NULL); return (int64_t)tv.tv_sec*1000 + tv.tv_usec/1000; }

// ============================================================================

// ========== Multi-channel Benchmark Extensions ==========
#define MODEL_PATH "/home/topeet/rknpu2/examples/rknn_yolov5_demo/model/RK3588/yolov5s-640-640_rk3588.rknn"
#define MAX_CHANNELS 4
#define BENCH_DURATION 60

static const char* DEFAULT_RTSP_URLS[MAX_CHANNELS] = {
    "rtsp://192.168.137.251:8554/cow0",
    "rtsp://192.168.137.251:8554/cow1",
    "rtsp://192.168.137.251:8554/cow2",
    "rtsp://192.168.137.251:8554/cow3"
};

// Per-channel stats
struct ChannelStats {
    int channel_id = 0;
    std::atomic<int> decode_count{0};
    std::atomic<int> infer_count{0};
    std::atomic<int> detect_count{0};
    double decode_ms_sum = 0;
    double infer_ms_sum = 0;
    double e2e_ms_sum = 0;
    std::mutex sum_mutex;
    std::vector<double> infer_latencies;
    std::mutex latency_mutex;
    rknn_core_mask core_mask = RKNN_NPU_CORE_AUTO;
    
    void addDecodeMs(double ms) { std::lock_guard<std::mutex> lk(sum_mutex); decode_ms_sum += ms; }
    void addInferMs(double ms) { std::lock_guard<std::mutex> lk(sum_mutex); infer_ms_sum += ms; }
    void addE2EMs(double ms) { std::lock_guard<std::mutex> lk(sum_mutex); e2e_ms_sum += ms; }
    void addInferLatency(double ms) {
        std::lock_guard<std::mutex> lk(latency_mutex);
        infer_latencies.push_back(ms);
        if (infer_latencies.size() > 10000) infer_latencies.erase(infer_latencies.begin(), infer_latencies.begin() + 1000);
    }
    double inferAvg() const {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(latency_mutex));
        if (infer_latencies.empty()) return 0;
        return std::accumulate(infer_latencies.begin(), infer_latencies.end(), 0.0) / infer_latencies.size();
    }
    double inferP99() const {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(latency_mutex));
        if (infer_latencies.empty()) return 0;
        auto v = infer_latencies;
        std::sort(v.begin(), v.end());
        return v[(int)(v.size() * 0.99)];
    }
    double inferMin() const {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(latency_mutex));
        if (infer_latencies.empty()) return 0;
        return *std::min_element(infer_latencies.begin(), infer_latencies.end());
    }
    double inferMax() const {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(latency_mutex));
        if (infer_latencies.empty()) return 0;
        return *std::max_element(infer_latencies.begin(), infer_latencies.end());
    }
};

// YOLOv5 Post-process (2-class model)
// ============================================================================
#define OBJ_CLASS_NUM 2
#define PROP_BOX_SIZE (5 + OBJ_CLASS_NUM)  // 7
#define NMS_THRESH 0.45f
#define BOX_THRESH 0.25f

const int anchor0[6] = {10, 13, 16, 30, 33, 23};
const int anchor1[6] = {30, 61, 62, 45, 59, 119};
const int anchor2[6] = {116, 90, 156, 198, 373, 326};

typedef struct {
    int left, right, top, bottom;
    float prop;
    int class_id;
} detect_result_t;

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

static void nms(std::vector<float> &boxes, std::vector<int> &classId,
                std::vector<int> &indexArray, int targetClass, float nms_thresh) {
    for (int i = 0; i < (int)indexArray.size(); i++) {
        if (indexArray[i] == -1 || classId[indexArray[i]] != targetClass) continue;
        for (int j = i + 1; j < (int)indexArray.size(); j++) {
            if (indexArray[j] == -1 || classId[indexArray[j]] != targetClass) continue;
            int n = indexArray[i], m = indexArray[j];
            float x1 = std::max(boxes[n*4], boxes[m*4]);
            float y1 = std::max(boxes[n*4+1], boxes[m*4+1]);
            float x2 = std::min(boxes[n*4]+boxes[n*4+2], boxes[m*4]+boxes[m*4+2]);
            float y2 = std::min(boxes[n*4+1]+boxes[n*4+3], boxes[m*4+1]+boxes[m*4+3]);
            float w = std::max(0.0f, x2 - x1);
            float h = std::max(0.0f, y2 - y1);
            float inter = w * h;
            float area1 = boxes[n*4+2] * boxes[n*4+3];
            float area2 = boxes[m*4+2] * boxes[m*4+3];
            float iou = inter / (area1 + area2 - inter + 1e-5f);
            if (iou > nms_thresh) indexArray[j] = -1;
        }
    }
}

// ============================================================================
// MPP H.264 Decoder (legacy API)
// ============================================================================

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

// ============================================================================

// RKNN Inference (correct YOLOv5 2-class post-process)
// ============================================================================
class RKNNInference {
public:
    RKNNInference() : ctx_(0), initialized_(false), model_width_(0), model_height_(0), model_channel_(3) {}
    ~RKNNInference() { if (ctx_) rknn_destroy(ctx_); }

    bool init(const std::string& model_path, rknn_core_mask core_mask = RKNN_NPU_CORE_0) {
        FILE* fp = fopen(model_path.c_str(), "rb");
        if (!fp) return false;
        fseek(fp, 0, SEEK_END); size_t sz = ftell(fp); fseek(fp, 0, SEEK_SET);
        std::vector<uint8_t> data(sz);
        if (fread(data.data(), 1, sz, fp) != sz) { fclose(fp); return false; }
        fclose(fp);
        if (rknn_init(&ctx_, data.data(), sz, 0, nullptr) < 0) return false;
        rknn_set_core_mask(ctx_, core_mask);

        rknn_input_output_num io;
        rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));

        rknn_tensor_attr input_attrs[1];
        memset(input_attrs, 0, sizeof(input_attrs));
        input_attrs[0].index = 0;
        rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, input_attrs, sizeof(input_attrs));

        if (input_attrs[0].fmt == RKNN_TENSOR_NHWC) {
            model_height_ = input_attrs[0].dims[1];
            model_width_ = input_attrs[0].dims[2];
            model_channel_ = input_attrs[0].dims[3];
        } else {
            model_channel_ = input_attrs[0].dims[1];
            model_height_ = input_attrs[0].dims[2];
            model_width_ = input_attrs[0].dims[3];
        }

        // Query output attrs for zp and scale
        n_output_ = io.n_output;
        for (int i = 0; i < n_output_ && i < 3; i++) {
            output_attrs_[i].index = i;
            rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &output_attrs_[i], sizeof(rknn_tensor_attr));
            printf("[RKNN] Output[%d]: dims=[%d,%d,%d,%d] size=%d type=%d qnt=%d zp=%d scale=%f\n",
                   i, output_attrs_[i].dims[0], output_attrs_[i].dims[1], output_attrs_[i].dims[2],
                   output_attrs_[i].dims[3], output_attrs_[i].size, output_attrs_[i].type,
                   output_attrs_[i].qnt_type, output_attrs_[i].zp, output_attrs_[i].scale);
        }

        printf("[RKNN] Core=0x%x, Model: %dx%dx%d, Outputs: %d\n",
               core_mask, model_width_, model_height_, model_channel_, n_output_);
        initialized_ = true;
        return true;
    }

    int infer_nv12(const uint8_t* nv12_data, int src_w, int src_h,
                   int hor_stride, int ver_stride,
                   std::vector<detect_result_t>& results) {
        if (!initialized_) return 0;

        // RGA: NV12 → RGB resize
        rga_buffer_t src_img, dst_img;
        im_rect src_rect, dst_rect;
        memset(&src_img, 0, sizeof(src_img)); memset(&dst_img, 0, sizeof(dst_img));
        memset(&src_rect, 0, sizeof(src_rect)); memset(&dst_rect, 0, sizeof(dst_rect));

        src_img = wrapbuffer_virtualaddr((void*)nv12_data, src_w, src_h,
                                          RK_FORMAT_YCbCr_420_SP, hor_stride, ver_stride);
        std::vector<uint8_t> rgb_buf(model_width_ * model_height_ * model_channel_);
        dst_img = wrapbuffer_virtualaddr((void*)rgb_buf.data(), model_width_, model_height_, RK_FORMAT_RGB_888);

        IM_STATUS chk = imcheck(src_img, dst_img, src_rect, dst_rect);
        if (chk != IM_STATUS_NOERROR) { printf("[Infer] RGA check fail: %s\n", imStrError(chk)); return 0; }
        IM_STATUS st = imresize(src_img, dst_img);
        if (st != IM_STATUS_SUCCESS) { printf("[Infer] RGA resize fail: %s\n", imStrError(st)); return 0; }

        // RKNN input
        rknn_input input;
        memset(&input, 0, sizeof(input));
        input.index = 0;
        input.type = RKNN_TENSOR_UINT8;
        input.size = model_width_ * model_height_ * model_channel_;
        input.fmt = RKNN_TENSOR_NHWC;
        input.pass_through = 0;
        input.buf = (void*)rgb_buf.data();

        if (rknn_inputs_set(ctx_, 1, &input) < 0) return 0;
        if (rknn_run(ctx_, nullptr) < 0) return 0;

        // Get int8 outputs (NOT float!)
        rknn_output outputs[3];
        memset(outputs, 0, sizeof(outputs));
        for (int i = 0; i < n_output_ && i < 3; i++) {
            outputs[i].want_float = 0;  // int8 output!
            outputs[i].is_prealloc = 0;
        }
        if (rknn_outputs_get(ctx_, n_output_, outputs, nullptr) < 0) return 0;

        // YOLOv5 post-process with int8 dequantization
        std::vector<float> filterBoxes, objProbs;
        std::vector<int> classId;

        int stride0 = 8, grid_h0 = model_height_ / stride0, grid_w0 = model_width_ / stride0;
        int stride1 = 16, grid_h1 = model_height_ / stride1, grid_w1 = model_width_ / stride1;
        int stride2 = 32, grid_h2 = model_height_ / stride2, grid_w2 = model_width_ / stride2;

        int validCount0 = 0, validCount1 = 0, validCount2 = 0;

        if (n_output_ >= 1 && outputs[0].size > 0)
            validCount0 = process_feature((int8_t*)outputs[0].buf, (int*)anchor0,
                grid_h0, grid_w0, model_height_, model_width_, stride0,
                filterBoxes, objProbs, classId, BOX_THRESH,
                output_attrs_[0].zp, output_attrs_[0].scale);

        if (n_output_ >= 2 && outputs[1].size > 0)
            validCount1 = process_feature((int8_t*)outputs[1].buf, (int*)anchor1,
                grid_h1, grid_w1, model_height_, model_width_, stride1,
                filterBoxes, objProbs, classId, BOX_THRESH,
                output_attrs_[1].zp, output_attrs_[1].scale);

        if (n_output_ >= 3 && outputs[2].size > 0)
            validCount2 = process_feature((int8_t*)outputs[2].buf, (int*)anchor2,
                grid_h2, grid_w2, model_height_, model_width_, stride2,
                filterBoxes, objProbs, classId, BOX_THRESH,
                output_attrs_[2].zp, output_attrs_[2].scale);

        int validCount = validCount0 + validCount1 + validCount2;

        if (validCount > 0) {
            // NMS
            std::vector<int> indexArray;
            for (int i = 0; i < validCount; i++) indexArray.push_back(i);
            // Sort by confidence (descending)
            for (int i = 0; i < validCount - 1; i++)
                for (int j = i + 1; j < validCount; j++)
                    if (objProbs[indexArray[i]] < objProbs[indexArray[j]])
                        std::swap(indexArray[i], indexArray[j]);

            std::set<int> class_set(classId.begin(), classId.end());
            for (auto c : class_set)
                nms(filterBoxes, classId, indexArray, c, NMS_THRESH);

            // Scale from model coords to source image coords
            float scale_w = (float)src_w / model_width_;
            float scale_h = (float)src_h / model_height_;

            for (int i = 0; i < validCount; i++) {
                if (indexArray[i] == -1) continue;
                int n = indexArray[i];
                detect_result_t r;
                r.left   = clamp_i(filterBoxes[n*4+0], 0, model_width_) * scale_w;
                r.top    = clamp_i(filterBoxes[n*4+1], 0, model_height_) * scale_h;
                r.right  = clamp_i(filterBoxes[n*4+0] + filterBoxes[n*4+2], 0, model_width_) * scale_w;
                r.bottom = clamp_i(filterBoxes[n*4+1] + filterBoxes[n*4+3], 0, model_height_) * scale_h;
                r.prop = objProbs[n];
                r.class_id = classId[n];
                results.push_back(r);
            }
        }

        rknn_outputs_release(ctx_, n_output_, outputs);
        return (int)results.size();
    }

private:
    rknn_context ctx_;
    bool initialized_;
    int model_width_, model_height_, model_channel_;
    int n_output_;
    rknn_tensor_attr output_attrs_[3];
};

// ============================================================================
// Main
// ============================================================================


// ========== Multi-channel Benchmark ==========
static std::atomic<bool> g_bench_running{false};
static ChannelStats* g_stats[MAX_CHANNELS] = {nullptr};

void channel_loop(int ch, const char* rtsp_url, RKNNInference* rknn, int duration_sec) {
    ChannelStats& stats = *g_stats[ch];
    printf("[CH%d] Starting, RTSP: %s\n", ch, rtsp_url);
    
    MPPH264Decoder decoder(ch);
    if (!decoder.init()) { printf("[CH%d] MPP init failed\n", ch); return; }
    
    avformat_network_init();
    AVFormatContext* fmt_ctx = nullptr;
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "stimeout", "5000000", 0);
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    av_dict_set(&opts, "max_delay", "500000", 0);
    
    if (avformat_open_input(&fmt_ctx, rtsp_url, nullptr, &opts) < 0) {
        av_dict_free(&opts);
        printf("[CH%d] Failed to open RTSP\n", ch); return;
    }
    av_dict_free(&opts);
    
    int video_stream = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++)
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { video_stream = (int)i; break; }
    if (video_stream < 0) { avformat_close_input(&fmt_ctx); return; }
    
    AVStream* vst = fmt_ctx->streams[video_stream];
    printf("[CH%d] Video: %dx%d, codec=%d\n", ch, vst->codecpar->width, vst->codecpar->height, vst->codecpar->codec_id);
    
    // Send extradata
    if (vst->codecpar->extradata && vst->codecpar->extradata_size > 0) {
        int sz = vst->codecpar->extradata_size;
        uint8_t* ed = vst->codecpar->extradata;
        bool is_annexb = (sz >= 4 && ed[0]==0 && ed[1]==0 && ed[2]==0 && ed[3]==1);
        if (is_annexb) { decoder.send_packet(ed, sz, false); }
        else {
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
    
    AVPacket* pkt = av_packet_alloc();
    int64_t start_time = get_time_ms();
    
    while (g_bench_running && (get_time_ms() - start_time) < duration_sec * 1000) {
        int ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0) {
            printf("[CH%d] RTSP read failed, reconnecting...\n", ch);
            avformat_close_input(&fmt_ctx); fmt_ctx = nullptr;
            AVDictionary* opts2 = nullptr;
            av_dict_set(&opts2, "rtsp_transport", "tcp", 0);
            av_dict_set(&opts2, "stimeout", "5000000", 0);
            if (avformat_open_input(&fmt_ctx, rtsp_url, nullptr, &opts2) < 0) {
                av_dict_free(&opts2);
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            av_dict_free(&opts2);
            video_stream = -1;
            for (unsigned i = 0; i < fmt_ctx->nb_streams; i++)
                if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { video_stream = (int)i; break; }
            if (video_stream < 0) { avformat_close_input(&fmt_ctx); fmt_ctx = nullptr; continue; }
            vst = fmt_ctx->streams[video_stream];
            if (vst->codecpar->extradata && vst->codecpar->extradata_size > 0)
                decoder.send_packet(vst->codecpar->extradata, vst->codecpar->extradata_size, false);
            continue;
        }
        
        if (pkt->stream_index != video_stream) { av_packet_unref(pkt); continue; }
        bool is_keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        
        auto t_decode_start = std::chrono::steady_clock::now();
        decoder.send_packet(pkt->data, pkt->size, is_keyframe);
        
        decoder.drain_frames([&](const uint8_t* nv12, int w, int h, int hs, int vs) {
            auto t_decode_end = std::chrono::steady_clock::now();
            double decode_ms = std::chrono::duration<double, std::milli>(t_decode_end - t_decode_start).count();
            stats.decode_count.fetch_add(1);
            stats.addDecodeMs(decode_ms);
            
            auto t_infer_start = std::chrono::steady_clock::now();
            std::vector<detect_result_t> results;
            int det_count = rknn->infer_nv12(nv12, w, h, hs, vs, results);
            auto t_infer_end = std::chrono::steady_clock::now();
            
            double infer_ms = std::chrono::duration<double, std::milli>(t_infer_end - t_infer_start).count();
            double e2e_ms = std::chrono::duration<double, std::milli>(t_infer_end - t_decode_start).count();
            
            stats.infer_count.fetch_add(1);
            stats.addInferMs(infer_ms);
            stats.addE2EMs(e2e_ms);
            stats.detect_count.fetch_add(det_count);
            stats.addInferLatency(infer_ms);
        });
        av_packet_unref(pkt);
    }
    
    av_packet_free(&pkt);
    avformat_close_input(&fmt_ctx);
    printf("[CH%d] Loop ended. Decoded: %d, Inferred: %d\n", ch, stats.decode_count.load(), stats.infer_count.load());
}

int main(int argc, char* argv[]) {
    int num_channels = 4, duration_sec = BENCH_DURATION;
    if (argc >= 2) num_channels = atoi(argv[1]);
    if (argc >= 3) duration_sec = atoi(argv[2]);
    if (num_channels < 1 || num_channels > MAX_CHANNELS) {
        printf("Usage: %s [channels 1-%d] [duration_seconds]\n", argv[0], MAX_CHANNELS);
        return 1;
    }
    
    printf("\n========================================================\n");
    printf("  Multi-RTSP + MPP + RKNN Performance Benchmark\n");
    printf("========================================================\n");
    printf("  Channels: %d  |  Duration: %ds\n", num_channels, duration_sec);
    printf("  Model:    %s\n", MODEL_PATH);
    printf("  BOX_THRESH: %.2f  NMS_THRESH: %.2f\n", BOX_THRESH, NMS_THRESH);
    printf("========================================================\n\n");
    
    signal(SIGINT, [](int){ g_bench_running = false; });
    g_bench_running = true;
    
    // Init RKNN instances
    rknn_core_mask core_masks[] = { RKNN_NPU_CORE_0, RKNN_NPU_CORE_1, RKNN_NPU_CORE_2 };
    RKNNInference rknn[MAX_CHANNELS];
    for (int i = 0; i < num_channels; i++) {
        rknn_core_mask core = core_masks[i % 3];
        if (!rknn[i].init(std::string(MODEL_PATH), core)) {
            printf("[FATAL] RKNN#%d init failed\n", i); return 1;
        }
        printf("[RKNN#%d] Initialized, core=%d, outputs=3\n", i, (int)core);
    }
    
    // Init stats
    ChannelStats stats[MAX_CHANNELS];
    for (int i = 0; i < num_channels; i++) {
        stats[i].channel_id = i;
        stats[i].core_mask = core_masks[i % 3];
        g_stats[i] = &stats[i];
    }
    
    // Launch channel threads
    std::vector<std::thread> threads;
    for (int i = 0; i < num_channels; i++)
        threads.emplace_back(channel_loop, i, DEFAULT_RTSP_URLS[i], &rknn[i], duration_sec);
    
    // Live stats every 5 seconds
    int64_t start_time = get_time_ms();
    while (g_bench_running && (get_time_ms() - start_time) < duration_sec * 1000) {
        int elapsed = (get_time_ms() - start_time) / 1000;
        if (elapsed > 0 && elapsed % 5 == 0) {
            int next_report = (elapsed / 5 + 1) * 5;
            while (g_bench_running && (get_time_ms() - start_time) / 1000 < next_report && (get_time_ms() - start_time) < duration_sec * 1000)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!g_bench_running) break;
            int sec = (get_time_ms() - start_time) / 1000;
            printf("\n--- [%ds] Live Stats ---\n", sec);
            for (int i = 0; i < num_channels; i++) {
                double fps = sec > 0 ? (double)stats[i].infer_count / sec : 0;
                printf("  CH%d: %d frames, %.1f fps, avg_infer=%.1fms, detections=%d\n",
                    i, stats[i].infer_count.load(), fps, stats[i].inferAvg(), stats[i].detect_count.load());
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    g_bench_running = false;
    for (auto& t : threads) t.join();
    
    // Print results
    int total_sec = duration_sec;
    int total_infer = 0, total_detect = 0;
    for (int i = 0; i < num_channels; i++) { total_infer += stats[i].infer_count; total_detect += stats[i].detect_count; }
    double total_fps = total_sec > 0 ? (double)total_infer / total_sec : 0;
    
    printf("\n========================================================\n");
    printf("  BENCHMARK RESULTS SUMMARY\n");
    printf("========================================================\n");
    printf("  Channels: %d  |  Duration: %ds  |  Model: YOLOv5s\n", num_channels, total_sec);
    printf("  BOX_THRESH: %.2f  |  NMS_THRESH: %.2f\n", BOX_THRESH, NMS_THRESH);
    printf("========================================================\n\n");
    
    printf("Channel | NPU Core | Frames |   FPS  | Avg Infer | P99 Infer | Min Infer | Max Infer | Detections\n");
    printf("--------|----------|--------|--------|-----------|-----------|-----------|-----------|----------\n");
    for (int i = 0; i < num_channels; i++) {
        double fps = total_sec > 0 ? (double)stats[i].infer_count / total_sec : 0;
        printf("  CH%d   |  Core %d  | %6d | %6.1f |  %6.2fms |  %6.2fms |  %6.2fms |  %6.2fms | %8d\n",
            i, (int)stats[i].core_mask, stats[i].infer_count.load(), fps,
            stats[i].inferAvg(), stats[i].inferP99(), stats[i].inferMin(), stats[i].inferMax(),
            stats[i].detect_count.load());
    }
    printf("--------|----------|--------|--------|-----------|-----------|-----------|-----------|----------\n");
    printf("  TOTAL |    -     | %6d | %6.1f |     -     |     -     |     -     |     -     | %8d\n", total_infer, total_detect);
    
    // NPU load
    printf("\n--- NPU Load ---\n");
    FILE* npu_fp = fopen("/sys/kernel/debug/rknpu/load", "r");
    if (npu_fp) { char buf[256]; while (fgets(buf, sizeof(buf), npu_fp)) printf("  %s", buf); fclose(npu_fp); }
    
    // CSV export
    char csv_name[128];
    snprintf(csv_name, sizeof(csv_name), "benchmark_%dch_%ds.csv", num_channels, total_sec);
    FILE* csv = fopen(csv_name, "w");
    if (csv) {
        fprintf(csv, "channel,npu_core,frames,fps,avg_infer_ms,p99_infer_ms,min_infer_ms,max_infer_ms,detections\n");
        for (int i = 0; i < num_channels; i++) {
            double fps = total_sec > 0 ? (double)stats[i].infer_count / total_sec : 0;
            fprintf(csv, "%d,%d,%d,%.1f,%.2f,%.2f,%.2f,%.2f,%d\n",
                i, (int)stats[i].core_mask, stats[i].infer_count.load(), fps,
                stats[i].inferAvg(), stats[i].inferP99(), stats[i].inferMin(), stats[i].inferMax(),
                stats[i].detect_count.load());
        }
        fclose(csv);
        printf("\nResults saved to: %s\n", csv_name);
    }
    
    printf("\nBenchmark complete.\n");
    return 0;
}
