/**
 * @file multi_rtsp_mpp_test.cpp
 * @brief 多路 RTSP 拉流 + MPP 硬件解码 + RKNN 推理（v6 - 基于 CLI 验证版重写）
 * 
 * 基于 mpp_rtsp_cli_test v5 验证通过的完整流程：
 * - MPP Legacy API (decode_put_packet / decode_get_frame)
 * - RGA im2d++ API (wrapbuffer_virtualaddr 6参数 + imresize)
 * - RKNN int8 量化输出 + YOLOv5 2类后处理
 * - NPU 多核并行 (Core0/1/2)
 * - RTSP 自动重连
 * 
 * 编译：cd build && make multi_rtsp_mpp_test -j4
 */

#include <QApplication>
#include <QMainWindow>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QTimer>
#include <QPainter>
#include <QImage>
#include <QSpinBox>
#include <QComboBox>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <cstdio>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <algorithm>
#include <math.h>
#include <set>
#include <sys/time.h>
#include <signal.h>
#include <string.h>
#include <stdint.h>

extern "C" {
#include <libavformat/avformat.h>
}

// MPP
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_vdec_cmd.h>

// RGA
#include <rga/im2d.hpp>
#include <rga/RgaApi.h>

// RKNN
#include <rknn_api.h>

// ============================================================================
// YOLOv5 Post-process (2-class model, matching official demo)
// ============================================================================
#define OBJ_CLASS_NUM 2
#define PROP_BOX_SIZE (5 + OBJ_CLASS_NUM)  // 7
#define NMS_THRESH 0.45f
#define BOX_THRESH 0.50f

const int anchor0[6] = {10, 13, 16, 30, 33, 23};
const int anchor1[6] = {30, 61, 62, 45, 59, 119};
const int anchor2[6] = {116, 90, 156, 198, 373, 326};

static const char* CLASS_NAMES[OBJ_CLASS_NUM] = {"cow", "person"};

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
// MPP H.264 Decoder (Legacy API - verified in CLI test)
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
        printf("[MPP#%d] H.264 decoder initialized (Legacy API)\n", id_);
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

        if (is_keyframe) {
            got_keyframe_ = true;
            if (frame_count_ < 10)
                printf("[MPP#%d] 关键帧 NALU=%d size=%zu\n", id_, nalu_type, size);
        }
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
        } else if (ret != MPP_OK) {
            return false;
        }
        send_count_++;
        return true;
    }

    int drain_frames() {
        int got = 0;
        while (true) {
            MppFrame frame = nullptr;
            MPP_RET ret = api_->decode_get_frame(ctx_, &frame);
            if (ret != MPP_OK || !frame) break;

            if (mpp_frame_get_info_change(frame)) {
                handle_info_change(frame);
                mpp_frame_deinit(&frame);
                continue;
            }

            MppBuffer frm_buf = mpp_frame_get_buffer(frame);
            if (frm_buf) {
                width_ = mpp_frame_get_width(frame);
                height_ = mpp_frame_get_height(frame);
                hor_stride_ = mpp_frame_get_hor_stride(frame);
                ver_stride_ = mpp_frame_get_ver_stride(frame);
                size_t buf_size = mpp_buffer_get_size(frm_buf);
                uint8_t* src = (uint8_t*)mpp_buffer_get_ptr(frm_buf);

                if (buf_size > last_frame_size_) {
                    last_frame_data_.resize(buf_size);
                    last_frame_size_ = buf_size;
                }
                memcpy(last_frame_data_.data(), src, buf_size);
                last_frame_buf_ = last_frame_data_.data();
                frame_count_++;
                got++;
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

        if (info_change_done_) {
            api_->control(ctx_, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
            return;
        }

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
// RKNN Inference (int8 output + YOLOv5 2-class post-process)
// ============================================================================
class RKNNInference {
public:
    RKNNInference(int id=0) : id_(id), ctx_(0), initialized_(false),
        model_width_(0), model_height_(0), model_channel_(3), n_output_(0) {}
    ~RKNNInference() { if (ctx_) rknn_destroy(ctx_); }

    bool init(const std::string& model_path, rknn_core_mask core_mask = RKNN_NPU_CORE_0) {
        FILE* fp = fopen(model_path.c_str(), "rb");
        if (!fp) { printf("[RKNN#%d] Failed to open model: %s\n", id_, model_path.c_str()); return false; }
        fseek(fp, 0, SEEK_END); size_t sz = ftell(fp); fseek(fp, 0, SEEK_SET);
        std::vector<uint8_t> data(sz);
        if (fread(data.data(), 1, sz, fp) != sz) { fclose(fp); return false; }
        fclose(fp);

        if (rknn_init(&ctx_, data.data(), sz, 0, nullptr) < 0) { printf("[RKNN#%d] rknn_init failed\n", id_); return false; }
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

        n_output_ = io.n_output;
        for (int i = 0; i < n_output_ && i < 3; i++) {
            output_attrs_[i].index = i;
            rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &output_attrs_[i], sizeof(rknn_tensor_attr));
            printf("[RKNN#%d] Output[%d]: dims=[%d,%d,%d,%d] qnt=%d zp=%d scale=%f\n",
                   id_, i, output_attrs_[i].dims[0], output_attrs_[i].dims[1],
                   output_attrs_[i].dims[2], output_attrs_[i].dims[3],
                   output_attrs_[i].qnt_type, output_attrs_[i].zp, output_attrs_[i].scale);
        }

        printf("[RKNN#%d] Core=0x%x, Model: %dx%dx%d, Outputs: %d\n",
               id_, (int)core_mask, model_width_, model_height_, model_channel_, n_output_);
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
        if (chk != IM_STATUS_NOERROR) {
            printf("[RKNN#%d] RGA check fail: %s\n", id_, imStrError(chk));
            return 0;
        }
        IM_STATUS st = imresize(src_img, dst_img);
        if (st != IM_STATUS_SUCCESS) {
            printf("[RKNN#%d] RGA resize fail: %s\n", id_, imStrError(st));
            return 0;
        }

        // RKNN input
        rknn_input input;
        memset(&input, 0, sizeof(input));
        input.index = 0;
        input.type = RKNN_TENSOR_UINT8;
        input.size = model_width_ * model_height_ * model_channel_;
        input.fmt = RKNN_TENSOR_NHWC;
        input.pass_through = 0;
        input.buf = (void*)rgb_buf.data();

        if (rknn_inputs_set(ctx_, 1, &input) < 0) { printf("[RKNN#%d] inputs_set failed\n", id_); return 0; }
        int ret = rknn_run(ctx_, nullptr);
        if (ret < 0) { printf("[RKNN#%d] run failed: %d\n", id_, ret); return 0; }
        if (ret != RKNN_SUCC) { printf("[RKNN#%d] run warning: %d\n", id_, ret); }

        // Get int8 outputs
        rknn_output outputs[3];
        memset(outputs, 0, sizeof(outputs));
        for (int i = 0; i < n_output_ && i < 3; i++) {
            outputs[i].want_float = 0;  // int8!
            outputs[i].is_prealloc = 0;
        }
        if (rknn_outputs_get(ctx_, n_output_, outputs, nullptr) < 0) return 0;

        // YOLOv5 post-process
        std::vector<float> filterBoxes, objProbs;
        std::vector<int> classId;

        int stride0 = 8, grid_h0 = model_height_ / stride0, grid_w0 = model_width_ / stride0;
        int stride1 = 16, grid_h1 = model_height_ / stride1, grid_w1 = model_width_ / stride1;
        int stride2 = 32, grid_h2 = model_height_ / stride2, grid_w2 = model_width_ / stride2;

        int validCount = 0;

        if (n_output_ >= 1 && outputs[0].size > 0)
            validCount += process_feature((int8_t*)outputs[0].buf, (int*)anchor0,
                grid_h0, grid_w0, model_height_, model_width_, stride0,
                filterBoxes, objProbs, classId, BOX_THRESH,
                output_attrs_[0].zp, output_attrs_[0].scale);

        if (n_output_ >= 2 && outputs[1].size > 0)
            validCount += process_feature((int8_t*)outputs[1].buf, (int*)anchor1,
                grid_h1, grid_w1, model_height_, model_width_, stride1,
                filterBoxes, objProbs, classId, BOX_THRESH,
                output_attrs_[1].zp, output_attrs_[1].scale);

        if (n_output_ >= 3 && outputs[2].size > 0)
            validCount += process_feature((int8_t*)outputs[2].buf, (int*)anchor2,
                grid_h2, grid_w2, model_height_, model_width_, stride2,
                filterBoxes, objProbs, classId, BOX_THRESH,
                output_attrs_[2].zp, output_attrs_[2].scale);

        if (validCount > 0) {
            std::vector<int> indexArray;
            for (int i = 0; i < validCount; i++) indexArray.push_back(i);
            for (int i = 0; i < validCount - 1; i++)
                for (int j = i + 1; j < validCount; j++)
                    if (objProbs[indexArray[i]] < objProbs[indexArray[j]])
                        std::swap(indexArray[i], indexArray[j]);

            std::set<int> class_set(classId.begin(), classId.end());
            for (auto c : class_set)
                nms(filterBoxes, classId, indexArray, c, NMS_THRESH);

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

    int get_model_width() const { return model_width_; }
    int get_model_height() const { return model_height_; }

private:
    int id_;
    rknn_context ctx_;
    bool initialized_;
    int model_width_, model_height_, model_channel_;
    int n_output_;
    rknn_tensor_attr output_attrs_[3];
};

// ============================================================================
// Single RTSP Channel (MPP + RKNN)
// ============================================================================
class RTSPChannelMPP : public QObject {
    Q_OBJECT

public:
    RTSPChannelMPP(int id) : id_(id), running_(false), fmt_ctx_(nullptr) {}
    ~RTSPChannelMPP() { stop(); }

    bool init(const std::string& rtsp_url, const std::string& model_path,
              rknn_core_mask core_mask, const std::string& transport = "tcp") {
        rtsp_url_ = rtsp_url;
        rtsp_transport_ = transport;

        decoder_ = std::make_unique<MPPH264Decoder>(id_);
        if (!decoder_->init()) {
            printf("[Channel#%d] MPP init failed\n", id_);
            return false;
        }

        rknn_ = std::make_unique<RKNNInference>(id_);
        if (!rknn_->init(model_path, core_mask)) {
            printf("[Channel#%d] RKNN init failed\n", id_);
            return false;
        }

        printf("[Channel#%d] Initialized (MPP Legacy + RKNN int8)\n", id_);
        return true;
    }

    void start() {
        running_ = true;
        thread_ = std::thread(&RTSPChannelMPP::runLoop, this);
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        cleanupFFmpeg();
    }

    QImage getFrame() {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        return current_frame_;
    }

    int getFps() { return fps_count_.exchange(0); }
    int getDetections() { return detection_count_.load(); }
    int getDecodeFps() { return decode_fps_.exchange(0); }
    int getInferCount() { return infer_count_.load(); }

signals:
    void frameUpdated(int id);

private:
    bool connectRTSP() {
        if (fmt_ctx_) avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;

        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "rtsp_transport", rtsp_transport_.c_str(), 0);
        av_dict_set(&opts, "stimeout", "10000000", 0);

        int r = avformat_open_input(&fmt_ctx_, rtsp_url_.c_str(), nullptr, &opts);
        av_dict_free(&opts);
        if (r < 0) { fmt_ctx_ = nullptr; return false; }
        return true;
    }

    int findVideoStream() {
        if (!fmt_ctx_) return -1;
        for (unsigned i = 0; i < fmt_ctx_->nb_streams; i++)
            if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) return (int)i;
        return -1;
    }

    void sendExtradata() {
        if (!fmt_ctx_) return;
        int vs = findVideoStream();
        if (vs < 0) return;
        AVStream* vst = fmt_ctx_->streams[vs];
        if (!vst->codecpar->extradata || vst->codecpar->extradata_size <= 0) return;

        int sz = vst->codecpar->extradata_size;
        uint8_t* ed = vst->codecpar->extradata;
        bool is_annexb = (sz >= 4 && ed[0]==0 && ed[1]==0 && ed[2]==0 && ed[3]==1);

        if (is_annexb) {
            decoder_->send_packet(ed, sz, false);
        } else {
            int pos = 6;
            while (pos + 2 < sz) {
                int nl = (ed[pos]<<8)|ed[pos+1]; pos += 2;
                if (pos + nl > sz) break;
                std::vector<uint8_t> sc(4+nl);
                sc[0]=0; sc[1]=0; sc[2]=0; sc[3]=1;
                memcpy(sc.data()+4, &ed[pos], nl);
                decoder_->send_packet(sc.data(), sc.size(), false);
                pos += nl;
            }
        }
    }

    void cleanupFFmpeg() {
        if (packet_) { av_packet_free(&packet_); packet_ = nullptr; }
        if (fmt_ctx_) { avformat_close_input(&fmt_ctx_); fmt_ctx_ = nullptr; }
    }

    void runLoop() {
        printf("[Channel#%d] === Thread started ===\n", id_);

        int video_packets = 0, keyframe_count = 0;
        int mpp_frames = 0, infer_count = 0, infer_success = 0;
        int64_t last_fps_time = getTimeMs();
        int last_fps_packets = 0;

        while (running_) {
            // Connect
            if (!connectRTSP()) {
                printf("[Channel#%d] RTSP connect failed, retry in 3s\n", id_);
                for (int i = 0; i < 30 && running_; i++) usleep(100000);
                continue;
            }

            int video_stream = findVideoStream();
            if (video_stream < 0) {
                printf("[Channel#%d] No video stream\n", id_);
                avformat_close_input(&fmt_ctx_); fmt_ctx_ = nullptr;
                continue;
            }

            sendExtradata();
            if (!packet_) packet_ = av_packet_alloc();

            printf("[Channel#%d] RTSP connected, streaming...\n", id_);

            while (running_) {
                int ret = av_read_frame(fmt_ctx_, packet_);
                if (ret < 0) {
                    char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
                    printf("[Channel#%d] Read failed: %s\n", id_, errbuf);
                    avformat_close_input(&fmt_ctx_); fmt_ctx_ = nullptr;
                    break;
                }

                if (packet_->stream_index != video_stream) {
                    av_packet_unref(packet_);
                    continue;
                }

                video_packets++;
                bool is_keyframe = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
                if (is_keyframe) keyframe_count++;

                // MPP decode
                decoder_->send_packet(packet_->data, packet_->size, is_keyframe);
                int got = decoder_->drain_frames();
                mpp_frames += got;

                // RKNN inference every 3 frames
                if (got > 0 && decoder_->get_last_frame() && (mpp_frames % 3 == 0)) {
                    infer_count++;
                    std::vector<detect_result_t> results;
                    int nd = rknn_->infer_nv12(decoder_->get_last_frame(),
                                                decoder_->get_width(), decoder_->get_height(),
                                                decoder_->get_hor_stride(), decoder_->get_ver_stride(),
                                                results);
                    if (nd >= 0) {
                        infer_success++;
                        detection_count_ += nd;

                        if (nd > 0 && (infer_success <= 5 || infer_success % 50 == 0)) {
                            printf("[Channel#%d] %d detections:\n", id_, nd);
                            for (auto& r : results)
                                printf("  %s conf=%.2f [%d,%d,%d,%d]\n",
                                       CLASS_NAMES[r.class_id], r.prop, r.left, r.top, r.right, r.bottom);
                        }

                        // Save results for display (no RGA here!)
                        {
                            std::lock_guard<std::mutex> lock(results_mutex_);
                            last_results_ = std::move(results);
                        }
                    }
                }

                // Update display at ~10fps (every 100ms) to avoid RGA bottleneck
                int64_t now_display = getTimeMs();
                if (got > 0 && decoder_->get_last_frame() && (now_display - last_display_time_ >= 40)) {
                    last_display_time_ = now_display;
                    std::vector<detect_result_t> results_copy;
                    {
                        std::lock_guard<std::mutex> lock(results_mutex_);
                        results_copy = last_results_;
                    }
                    updateDisplayWithDetections(results_copy);
                }

                // FPS reporting
                fps_count_++;
                decode_fps_ += got;
                infer_count_ = infer_count;

                int64_t now = getTimeMs();
                if (now - last_fps_time >= 5000) {
                    int fps = (video_packets - last_fps_packets) * 1000 / (now - last_fps_time);
                    printf("[Channel#%d] FPS: %d | 视频=%d, 解码=%d, 关键帧=%d, 推理=%d/%d, 检测=%d\n",
                           id_, fps, video_packets, mpp_frames, keyframe_count,
                           infer_success, infer_count, detection_count_.load());
                    last_fps_packets = video_packets;
                    last_fps_time = now;
                }

                av_packet_unref(packet_);
            }
        }

        cleanupFFmpeg();
        printf("[Channel#%d] === Thread stopped ===\n", id_);
    }

    void updateDisplayWithDetections(const std::vector<detect_result_t>& results) {
        if (!decoder_->get_last_frame()) return;

        int w = decoder_->get_width(), h = decoder_->get_height();
        int hs = decoder_->get_hor_stride(), vs = decoder_->get_ver_stride();
        // Display at half resolution for speed
        int dw = w / 2, dh = h / 2;
        size_t rgb_size = (size_t)dw * dh * 3;

        // Pre-allocate display buffer
        if (display_rgb_buf_.size() < rgb_size)
            display_rgb_buf_.resize(rgb_size);

        rga_buffer_t src_img, dst_img;
        im_rect src_rect, dst_rect;
        memset(&src_img, 0, sizeof(src_img)); memset(&dst_img, 0, sizeof(dst_img));
        memset(&src_rect, 0, sizeof(src_rect)); memset(&dst_rect, 0, sizeof(dst_rect));

        src_img = wrapbuffer_virtualaddr((void*)decoder_->get_last_frame(), w, h,
                                          RK_FORMAT_YCbCr_420_SP, hs, vs);
        dst_img = wrapbuffer_virtualaddr((void*)display_rgb_buf_.data(), dw, dh, RK_FORMAT_RGB_888);

        // Combine color conversion + resize in one RGA call
        IM_STATUS st = imresize(src_img, dst_img);
        if (st != IM_STATUS_SUCCESS) {
            // Fallback: just color convert at full res
            dw = w; dh = h;
            rgb_size = (size_t)dw * dh * 3;
            if (display_rgb_buf_.size() < rgb_size)
                display_rgb_buf_.resize(rgb_size);
            dst_img = wrapbuffer_virtualaddr((void*)display_rgb_buf_.data(), dw, dh, RK_FORMAT_RGB_888);
            st = imcvtcolor(src_img, dst_img, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_RGB_888);
            if (st != IM_STATUS_SUCCESS) return;
        }

        QImage image(display_rgb_buf_.data(), dw, dh, dw * 3, QImage::Format_RGB888);

        // Draw detections
        if (!results.empty()) {
            QPainter painter(&image);
            QFont font = painter.font();
            font.setPointSize(10);
            painter.setFont(font);

            float dsx = (float)dw / w, dsy = (float)dh / h;

            for (const auto& r : results) {
                int x1 = r.left * dsx, y1 = r.top * dsy, x2 = r.right * dsx, y2 = r.bottom * dsy;

                painter.setPen(QPen(Qt::green, 2));
                painter.drawRect(x1, y1, x2 - x1, y2 - y1);

                QString label = QString("%1:%2%")
                    .arg(CLASS_NAMES[r.class_id])
                    .arg(std::min(100, (int)(r.prop * 100)));

                painter.setPen(Qt::green);
                painter.fillRect(x1, y1 - 16, painter.fontMetrics().horizontalAdvance(label) + 6, 16, Qt::darkGreen);
                painter.setPen(Qt::white);
                painter.drawText(x1 + 3, y1 - 2, label);
            }
        }

        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            current_frame_ = image.copy();  // still need copy since display_rgb_buf_ will be overwritten
        }

        emit frameUpdated(id_);
    }

    static int64_t getTimeMs() {
        struct timeval tv; gettimeofday(&tv, NULL);
        return (int64_t)tv.tv_sec*1000 + tv.tv_usec/1000;
    }

    int id_;
    std::string rtsp_url_;
    std::string rtsp_transport_;
    std::atomic<bool> running_;

    std::unique_ptr<MPPH264Decoder> decoder_;
    std::unique_ptr<RKNNInference> rknn_;

    std::thread thread_;
    QImage current_frame_;
    std::mutex frame_mutex_;
    std::vector<detect_result_t> last_results_;
    std::mutex results_mutex_;
    int64_t last_display_time_ = 0;
    std::vector<uint8_t> display_rgb_buf_;  // pre-allocated display buffer

    std::atomic<int> fps_count_{0};
    std::atomic<int> decode_fps_{0};
    std::atomic<int> detection_count_{0};
    std::atomic<int> infer_count_{0};

    // FFmpeg
    AVFormatContext* fmt_ctx_;
    AVPacket* packet_ = nullptr;
};

// ============================================================================
// Video Display Widget
// ============================================================================
class VideoWidget : public QWidget {
    Q_OBJECT
public:
    VideoWidget(int id, QWidget* parent = nullptr) : QWidget(parent), id_(id) {
        setMinimumSize(320, 240);
        setStyleSheet("background-color: black;");
    }

    void updateFrame(const QImage& img) {
        std::lock_guard<std::mutex> lock(mutex_);
        image_ = img;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);

        if (!image_.isNull()) {
            QImage scaled = image_.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            int x = (width() - scaled.width()) / 2;
            int y = (height() - scaled.height()) / 2;
            painter.drawImage(x, y, scaled);
        } else {
            painter.setPen(Qt::white);
            painter.drawText(rect(), Qt::AlignCenter,
                QString("通道 %1\n等待视频...").arg(id_));
        }
    }

private:
    int id_;
    QImage image_;
    std::mutex mutex_;
};

// ============================================================================
// Main Window
// ============================================================================
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget* parent = nullptr) : QMainWindow(parent) {
        setWindowTitle("AIControlSkynet3588 - 多路 RTSP 推理 (MPP+RKNN v6)");
        resize(1400, 900);

        QWidget* central = new QWidget(this);
        setCentralWidget(central);
        QVBoxLayout* main_layout = new QVBoxLayout(central);

        // Control panel
        QHBoxLayout* control = new QHBoxLayout();

        control->addWidget(new QLabel("路数:"));
        channel_spin_ = new QSpinBox();
        channel_spin_->setRange(1, 4);
        channel_spin_->setValue(1);
        control->addWidget(channel_spin_);

        control->addSpacing(5);
        control->addWidget(new QLabel("传输:"));
        transport_combo_ = new QComboBox();
        transport_combo_->addItem("tcp");
        transport_combo_->addItem("udp");
        transport_combo_->setCurrentText("tcp");
        control->addWidget(transport_combo_);

        control->addSpacing(10);

        start_btn_ = new QPushButton("▶ 启动");
        start_btn_->setStyleSheet("background-color: #4CAF50; color: white; padding: 8px 20px; font-weight: bold;");
        control->addWidget(start_btn_);
        connect(start_btn_, &QPushButton::clicked, this, &MainWindow::onStart);

        stop_btn_ = new QPushButton("■ 停止");
        stop_btn_->setStyleSheet("background-color: #f44336; color: white; padding: 8px 20px; font-weight: bold;");
        stop_btn_->setEnabled(false);
        control->addWidget(stop_btn_);
        connect(stop_btn_, &QPushButton::clicked, this, &MainWindow::onStop);

        control->addStretch();

        stats_label_ = new QLabel("FPS: -- | 检测: -- | 推理: --");
        stats_label_->setStyleSheet("font-weight: bold; font-size: 13px;");
        control->addWidget(stats_label_);

        main_layout->addLayout(control);

        // RTSP URLs
        QHBoxLayout* url_layout = new QHBoxLayout();
        url_layout->addWidget(new QLabel("RTSP:"));
        for (int i = 0; i < 4; i++) {
            rtsp_edits_[i] = new QLineEdit();
            rtsp_edits_[i]->setText(QString("rtsp://192.168.137.251:8554/cow%1").arg(i));
            url_layout->addWidget(rtsp_edits_[i]);
        }
        main_layout->addLayout(url_layout);

        // Model path
        QHBoxLayout* model_layout = new QHBoxLayout();
        model_layout->addWidget(new QLabel("模型:"));
        model_edit_ = new QLineEdit("/home/topeet/rknpu2/examples/rknn_yolov5_demo/install/rknn_yolov5_demo_Linux/model/RK3588/yolov5s-640-640_rk3588.rknn");
        model_layout->addWidget(model_edit_);
        main_layout->addLayout(model_layout);

        // Video display
        video_container_ = new QWidget();
        video_layout_ = new QGridLayout(video_container_);
        video_layout_->setSpacing(2);
        main_layout->addWidget(video_container_, 1);

        for (int i = 0; i < 4; i++) {
            video_widgets_[i] = new VideoWidget(i);
            video_layout_->addWidget(video_widgets_[i], i / 2, i % 2);
        }

        // Timers
        display_timer_ = new QTimer(this);
        connect(display_timer_, &QTimer::timeout, this, &MainWindow::updateDisplay);

        stats_timer_ = new QTimer(this);
        connect(stats_timer_, &QTimer::timeout, this, &MainWindow::updateStats);

        avformat_network_init();
    }

    ~MainWindow() { onStop(); }

    void setAutoConfig(int channels, const std::string& url) {
        channel_spin_->setValue(channels);
        if (!url.empty()) {
            for (int i = 0; i < channels && i < 4; i++)
                rtsp_edits_[i]->setText(QString::fromStdString(url));
        }
    }

public slots:
    void onStart() {

        int num_channels = channel_spin_->value();
        std::string model_path = model_edit_->text().toStdString();
        std::string transport = transport_combo_->currentText().toStdString();

        printf("\n=== 启动 %d 路 RTSP (MPP+RKNN v6) ===\n", num_channels);

        rknn_core_mask core_masks[4] = {
            RKNN_NPU_CORE_0, RKNN_NPU_CORE_1, RKNN_NPU_CORE_2, RKNN_NPU_CORE_0
        };

        int success = 0;
        for (int i = 0; i < num_channels; i++) {
            std::string rtsp_url = rtsp_edits_[i]->text().toStdString();
            printf("[Main] Channel %d: %s (%s)\n", i, rtsp_url.c_str(), transport.c_str());

            channels_[i] = std::make_unique<RTSPChannelMPP>(i);
            if (!channels_[i]->init(rtsp_url, model_path, core_masks[i], transport)) {
                printf("[Main] Channel %d init failed\n", i);
                continue;
            }

            // Connect signal
            connect(channels_[i].get(), &RTSPChannelMPP::frameUpdated,
                    this, [this, i](int id) { video_widgets_[id]->updateFrame(channels_[id]->getFrame()); });

            channels_[i]->start();
            success++;
        }

        printf("[Main] 成功启动 %d/%d 路\n\n", success, num_channels);

        display_timer_->start(33);  // 30fps display
        stats_timer_->start(1000);

        start_btn_->setEnabled(false);
        stop_btn_->setEnabled(true);
    }

    void onStop() {
        display_timer_->stop();
        stats_timer_->stop();

        for (int i = 0; i < 4; i++) {
            if (channels_[i]) {
                channels_[i]->stop();
                channels_[i].reset();
            }
        }

        start_btn_->setEnabled(true);
        stop_btn_->setEnabled(false);
    }

    void updateDisplay() {
        for (int i = 0; i < 4; i++) {
            if (channels_[i]) {
                QImage frame = channels_[i]->getFrame();
                if (!frame.isNull()) {
                    video_widgets_[i]->updateFrame(frame);
                }
            }
        }
    }

    void updateStats() {
        int total_fps = 0, total_det = 0, total_infer = 0;
        for (int i = 0; i < 4; i++) {
            if (channels_[i]) {
                total_fps += channels_[i]->getFps();
                total_det += channels_[i]->getDetections();
                total_infer += channels_[i]->getInferCount();
            }
        }
        stats_label_->setText(QString("FPS: %1 | 检测: %2 | 推理: %3")
            .arg(total_fps).arg(total_det).arg(total_infer));
    }

private:
    QSpinBox* channel_spin_;
    QComboBox* transport_combo_;
    QPushButton* start_btn_;
    QPushButton* stop_btn_;
    QLabel* stats_label_;
    QLineEdit* rtsp_edits_[4];
    QLineEdit* model_edit_;

    VideoWidget* video_widgets_[4];
    QWidget* video_container_;
    QGridLayout* video_layout_;
    QTimer* display_timer_;
    QTimer* stats_timer_;

    std::unique_ptr<RTSPChannelMPP> channels_[4];
};

int main(int argc, char* argv[]) {
    setbuf(stdout, NULL); setbuf(stderr, NULL);
    printf("=== MPP+RKNN Multi-RTSP v6 ===\n");
    QApplication app(argc, argv);
    MainWindow win;
    win.show();

    // Auto-start if --auto flag provided
    int auto_channels = 0;
    std::string auto_url;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--auto") == 0) {
            auto_channels = 1;  // default 1 channel
        } else if (strcmp(argv[i], "--channels") == 0 && i + 1 < argc) {
            auto_channels = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--url") == 0 && i + 1 < argc) {
            auto_url = argv[++i];
        }
    }

    if (auto_channels > 0) {
        win.setAutoConfig(auto_channels, auto_url);
        printf("[Main] Auto-starting %d channels...\n", auto_channels);
        QTimer::singleShot(1000, &win, &MainWindow::onStart);
    }

    return app.exec();
}

#include "multi_rtsp_mpp_test.moc"
