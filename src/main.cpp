/**
 * @file main.cpp
 * @brief V4L2 + Rockchip MPP/RGA/RKNN 多摄像头 AI 推理框架
 * 
 * 特性：
 * - 多路 USB 摄像头同时采集解码
 * - MPP 硬件 MJPEG 解码
 * - RGA 零拷贝格式转换和缩放
 * - RKNN NPU 推理
 * - 帧回调机制
 * - CPU 亲和性绑定大核
 */

#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <stdexcept>
#include <cstring>
#include <chrono>
#include <queue>
#include <memory>

// Linux 头文件
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <linux/videodev2.h>
#include <sched.h>

// Rockchip MPP 头文件
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_meta.h>
#include <rockchip/mpp_task.h>

// Rockchip RGA 头文件
#include <rga/RgaApi.h>
#include <rga/rga.h>

// Rockchip RKNN 头文件
#include <rknn_api.h>

// MPP 对齐宏
#ifndef MPP_ALIGN
#define MPP_ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#endif

// ============================================================================
// 错误处理
// ============================================================================

class V4L2Error : public std::runtime_error {
public:
    explicit V4L2Error(const std::string& msg) : std::runtime_error(msg) {}
};

class MPPError : public std::runtime_error {
public:
    MPPError(const std::string& msg, MPP_RET ret)
        : std::runtime_error(msg + ": " + std::to_string(ret)) {}
};

class RGAError : public std::runtime_error {
public:
    explicit RGAError(const std::string& msg) : std::runtime_error(msg) {}
};

class RKNNError : public std::runtime_error {
public:
    RKNNError(const std::string& msg, int ret)
        : std::runtime_error(msg + ": " + std::to_string(ret)) {}
};

// ============================================================================
// 帧结构
// ============================================================================

struct Frame {
    int camera_id = 0;
    void* data = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;  // MPP_FMT_YUV420SP (NV12)
    uint64_t pts = 0;
    MppFrame mpp_frame = nullptr;  // 用于释放
    MppBuffer mpp_buffer = nullptr;  // MPP buffer
    
    // RGA 处理后的数据
    void* rga_data = nullptr;
    uint32_t rga_width = 0;
    uint32_t rga_height = 0;
    int rga_fd = -1;  // DMABUF fd
    
    // RKNN 推理结果
    std::vector<float> inference_results;
};

// ============================================================================
// 帧回调类型
// ============================================================================

enum class FrameStage {
    DECODED,    // MPP 解码后
    SCALED,     // RGA 缩放后
    INFERENCE   // RKNN 推理后
};

using FrameCallback = std::function<void(const Frame&, FrameStage)>;

// ============================================================================
// V4L2 采集类（MMAP 模式）
// ============================================================================

class V4L2Capture {
public:
    struct Buffer {
        void* data;
        size_t capacity;
        uint32_t index;
    };

    V4L2Capture(const std::string& device, uint32_t width, uint32_t height,
                uint32_t fps, uint32_t buffer_count = 4)
        : device_(device), width_(width), height_(height), fps_(fps),
          buffer_count_(buffer_count), fd_(-1), streaming_(false) {}

    ~V4L2Capture() { stop(); close(); }

    void init() {
        printf("[V4L2] Opening device: %s\n", device_.c_str());
        fflush(stdout);
        
        fd_ = open(device_.c_str(), O_RDWR | O_NONBLOCK);
        if (fd_ < 0) throw V4L2Error("Failed to open device: " + device_);
        printf("[V4L2] Device opened (fd=%d)\n", fd_);

        struct v4l2_format fmt = {};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = width_;
        fmt.fmt.pix.height = height_;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) throw V4L2Error("VIDIOC_S_FMT failed");
        printf("[V4L2] Format set: %ux%u MJPEG\n", width_, height_);

        struct v4l2_requestbuffers req = {};
        req.count = buffer_count_;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) throw V4L2Error("VIDIOC_REQBUFS failed");

        buffers_.resize(req.count);
        for (uint32_t i = 0; i < req.count; ++i) {
            struct v4l2_buffer buf = {};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) throw V4L2Error("VIDIOC_QUERYBUF failed");
            buffers_[i].data = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
            if (buffers_[i].data == MAP_FAILED) throw V4L2Error("mmap failed");
            buffers_[i].capacity = buf.length;
            buffers_[i].index = i;
        }
        printf("[V4L2] %zu buffers allocated\n", buffers_.size());
    }

    void start() {
        printf("[V4L2] Starting capture...\n");
        for (uint32_t i = 0; i < buffers_.size(); ++i) {
            struct v4l2_buffer buf = {};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            ioctl(fd_, VIDIOC_QBUF, &buf);
        }
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) throw V4L2Error("VIDIOC_STREAMON failed");
        streaming_ = true;
        printf("[V4L2] Capture started\n");
    }

    void stop() {
        if (!streaming_) return;
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd_, VIDIOC_STREAMOFF, &type);
        streaming_ = false;
    }

    void close() {
        if (fd_ >= 0) {
            for (auto& buf : buffers_) {
                if (buf.data && buf.data != MAP_FAILED) munmap(buf.data, buf.capacity);
            }
            buffers_.clear();
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool capture_frame(void** out_data, size_t* out_size, uint32_t* out_index) {
        struct pollfd pfd = {fd_, POLLIN, 0};
        if (poll(&pfd, 1, 0) <= 0) return false;

        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) return false;

        *out_data = buffers_[buf.index].data;
        *out_size = buf.bytesused;
        *out_index = buf.index;
        return true;
    }

    void release_frame(uint32_t index) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = index;
        ioctl(fd_, VIDIOC_QBUF, &buf);
    }

    const std::string& device() const { return device_; }

private:
    std::string device_;
    uint32_t width_, height_, fps_, buffer_count_;
    int fd_;
    bool streaming_;
    std::vector<Buffer> buffers_;
};

// ============================================================================
// MPP 解码器类（带 buffer pool 优化）
// ============================================================================

class MPPDecoder {
public:
    MPPDecoder(int camera_id) : camera_id_(camera_id), ctx_(nullptr), api_(nullptr), 
                                 frm_grp_(nullptr), frame_(nullptr), frm_buf_(nullptr),
                                 pkt_grp_(nullptr), initialized_(false) {}

    ~MPPDecoder() { close(); }

    void init(uint32_t width, uint32_t height, MppCodingType codec_type = MPP_VIDEO_CodingMJPEG) {
        printf("[MPP#%d] Initializing decoder...\n", camera_id_);

        width_ = width;
        height_ = height;

        MPP_RET ret = mpp_create(&ctx_, &api_);
        if (ret != MPP_OK) throw MPPError("mpp_create failed", ret);

        ret = mpp_init(ctx_, MPP_CTX_DEC, codec_type);
        if (ret != MPP_OK) throw MPPError("mpp_init failed", ret);

        MppFrameFormat output_fmt = MPP_FMT_YUV420SP;
        api_->control(ctx_, MPP_DEC_SET_OUTPUT_FORMAT, &output_fmt);

        ret = mpp_frame_init(&frame_);
        if (ret != MPP_OK) throw MPPError("mpp_frame_init failed", ret);

        RK_U32 hor_stride = MPP_ALIGN(width_, 16);
        RK_U32 ver_stride = MPP_ALIGN(height_, 16);
        RK_U32 frm_buf_size = hor_stride * ver_stride * 4;

        ret = mpp_buffer_group_get_internal(&frm_grp_, MPP_BUFFER_TYPE_ION);
        if (ret != MPP_OK) ret = mpp_buffer_group_get_internal(&frm_grp_, MPP_BUFFER_TYPE_DMA_HEAP);
        if (ret != MPP_OK) throw MPPError("frame buffer group failed", ret);

        ret = mpp_buffer_get(frm_grp_, &frm_buf_, frm_buf_size);
        if (ret != MPP_OK) throw MPPError("mpp_buffer_get failed", ret);
        mpp_frame_set_buffer(frame_, frm_buf_);

        // 创建 buffer pool 用于 packet（减少 malloc/free）
        ret = mpp_buffer_group_get_internal(&pkt_grp_, MPP_BUFFER_TYPE_ION);
        if (ret != MPP_OK) ret = mpp_buffer_group_get_internal(&pkt_grp_, MPP_BUFFER_TYPE_DMA_HEAP);
        if (ret != MPP_OK) throw MPPError("packet buffer group failed", ret);

        initialized_ = true;
        printf("[MPP#%d] Decoder initialized\n", camera_id_);
    }

    bool decode(const void* data, size_t size, uint64_t pts, Frame* out_frame) {
        if (!initialized_ || !data || size == 0) return false;

        MppBuffer pkt_buf = nullptr;
        MPP_RET ret = mpp_buffer_get(pkt_grp_, &pkt_buf, size);
        if (ret != MPP_OK || !pkt_buf) return false;

        void* pkt_ptr = mpp_buffer_get_ptr(pkt_buf);
        if (!pkt_ptr) {
            mpp_buffer_put(pkt_buf);
            return false;
        }
        memcpy(pkt_ptr, data, size);

        MppPacket packet = nullptr;
        ret = mpp_packet_init_with_buffer(&packet, pkt_buf);
        if (ret != MPP_OK || !packet) {
            mpp_buffer_put(pkt_buf);
            return false;
        }

        mpp_packet_set_pts(packet, pts);

        ret = api_->poll(ctx_, MPP_PORT_INPUT, MPP_POLL_BLOCK);
        if (ret != MPP_OK) {
            mpp_packet_deinit(&packet);
            mpp_buffer_put(pkt_buf);
            return false;
        }

        MppTask task = nullptr;
        ret = api_->dequeue(ctx_, MPP_PORT_INPUT, &task);
        if (ret != MPP_OK || !task) {
            mpp_packet_deinit(&packet);
            mpp_buffer_put(pkt_buf);
            return false;
        }

        mpp_task_meta_set_packet(task, KEY_INPUT_PACKET, packet);
        mpp_task_meta_set_frame(task, KEY_OUTPUT_FRAME, frame_);

        ret = api_->enqueue(ctx_, MPP_PORT_INPUT, task);
        if (ret != MPP_OK) {
            mpp_packet_deinit(&packet);
            mpp_buffer_put(pkt_buf);
            return false;
        }

        ret = api_->poll(ctx_, MPP_PORT_OUTPUT, MPP_POLL_BLOCK);
        if (ret != MPP_OK) {
            mpp_packet_deinit(&packet);
            mpp_buffer_put(pkt_buf);
            return false;
        }

        MppTask out_task = nullptr;
        ret = api_->dequeue(ctx_, MPP_PORT_OUTPUT, &out_task);
        if (ret != MPP_OK || !out_task) {
            mpp_packet_deinit(&packet);
            mpp_buffer_put(pkt_buf);
            return false;
        }

        MppFrame frame_out = nullptr;
        mpp_task_meta_get_frame(out_task, KEY_OUTPUT_FRAME, &frame_out);

        api_->enqueue(ctx_, MPP_PORT_OUTPUT, out_task);

        mpp_packet_deinit(&packet);
        mpp_buffer_put(pkt_buf);

        if (!frame_out) return false;

        RK_U32 err_info = mpp_frame_get_errinfo(frame_out);
        if (err_info) {
            mpp_frame_deinit(&frame_out);
            return false;
        }

        MppBuffer buffer = mpp_frame_get_buffer(frame_out);
        if (!buffer) {
            mpp_frame_deinit(&frame_out);
            return false;
        }

        out_frame->camera_id = camera_id_;
        out_frame->data = mpp_buffer_get_ptr(buffer);
        out_frame->width = mpp_frame_get_width(frame_out);
        out_frame->height = mpp_frame_get_height(frame_out);
        out_frame->format = mpp_frame_get_fmt(frame_out);
        out_frame->pts = mpp_frame_get_pts(frame_out);
        out_frame->mpp_frame = frame_out;
        out_frame->mpp_buffer = buffer;

        return true;
    }

    void release_frame(MppFrame frame) {
        if (frame) mpp_frame_deinit(&frame);
    }

    void close() {
        if (frame_) { mpp_frame_deinit(&frame_); frame_ = nullptr; }
        if (frm_buf_) { mpp_buffer_put(frm_buf_); frm_buf_ = nullptr; }
        if (ctx_) { mpp_destroy(ctx_); ctx_ = nullptr; api_ = nullptr; }
        if (frm_grp_) { mpp_buffer_group_put(frm_grp_); frm_grp_ = nullptr; }
        if (pkt_grp_) { mpp_buffer_group_put(pkt_grp_); pkt_grp_ = nullptr; }
        initialized_ = false;
    }

private:
    int camera_id_;
    MppCtx ctx_;
    MppApi* api_;
    MppBufferGroup frm_grp_;
    MppBufferGroup pkt_grp_;
    MppFrame frame_;
    MppBuffer frm_buf_;
    bool initialized_;
    uint32_t width_, height_;
};

// ============================================================================
// RGA 处理器类（零拷贝格式转换和缩放）
// ============================================================================

class RGAProcessor {
public:
    RGAProcessor(int camera_id) : camera_id_(camera_id), initialized_(false) {}

    ~RGAProcessor() { close(); }

    void init() {
        printf("[RGA#%d] Initializing...\n", camera_id_);
        
        // RGA 初始化
        if (c_RkRgaInit() < 0) {
            throw RGAError("RGA init failed");
        }
        
        initialized_ = true;
        printf("[RGA#%d] Initialized\n", camera_id_);
    }

    // NV12 → RGB888 缩放
    bool scale_nv12_to_rgb(void* src_data, uint32_t src_w, uint32_t src_h,
                           void* dst_data, uint32_t dst_w, uint32_t dst_h,
                           MppBuffer src_buffer = nullptr) {
        if (!initialized_) return false;

        rga_info_t src_info = {};
        rga_info_t dst_info = {};

        // 源：NV12
        src_info.fd = -1;  // 使用虚拟地址
        src_info.virAddr = src_data;
        src_info.mmuFlag = 1;
        
        rga_set_rect(&src_info.rect, 0, 0, src_w, src_h, 
                     MPP_ALIGN(src_w, 16), MPP_ALIGN(src_h, 16), 
                     RK_FORMAT_YCbCr_420_SP);

        // 目标：RGB888
        dst_info.fd = -1;
        dst_info.virAddr = dst_data;
        dst_info.mmuFlag = 1;
        
        rga_set_rect(&dst_info.rect, 0, 0, dst_w, dst_h, 
                     dst_w, dst_h, 
                     RK_FORMAT_RGB_888);

        int ret = c_RkRgaBlit(&src_info, &dst_info, nullptr);
        return ret == 0;
    }

    // NV12 → RGB888 缩放（带 DMABUF fd，零拷贝）
    bool scale_nv12_to_rgb_dmabuf(int src_fd, uint32_t src_w, uint32_t src_h,
                                   void* dst_data, uint32_t dst_w, uint32_t dst_h) {
        if (!initialized_) return false;

        rga_info_t src_info = {};
        rga_info_t dst_info = {};

        // 源：NV12 (DMABUF)
        src_info.fd = src_fd;
        src_info.mmuFlag = 1;
        
        rga_set_rect(&src_info.rect, 0, 0, src_w, src_h, 
                     MPP_ALIGN(src_w, 16), MPP_ALIGN(src_h, 16), 
                     RK_FORMAT_YCbCr_420_SP);

        // 目标：RGB888
        dst_info.fd = -1;
        dst_info.virAddr = dst_data;
        dst_info.mmuFlag = 1;
        
        rga_set_rect(&dst_info.rect, 0, 0, dst_w, dst_h, 
                     dst_w, dst_h, 
                     RK_FORMAT_RGB_888);

        int ret = c_RkRgaBlit(&src_info, &dst_info, nullptr);
        return ret == 0;
    }

    void close() {
        if (initialized_) {
            c_RkRgaDeInit();
            initialized_ = false;
        }
    }

private:
    int camera_id_;
    bool initialized_;
};

// ============================================================================
// RKNN 推理类
// ============================================================================

class RKNNInference {
public:
    RKNNInference(int camera_id) : camera_id_(camera_id), ctx_(0), initialized_(false) {}

    ~RKNNInference() { close(); }

    void init(const std::string& model_path) {
        printf("[RKNN#%d] Loading model: %s\n", camera_id_, model_path.c_str());

        // 加载模型
        FILE* fp = fopen(model_path.c_str(), "rb");
        if (!fp) {
            throw RKNNError("Failed to open model file", -1);
        }

        fseek(fp, 0, SEEK_END);
        size_t model_size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        std::vector<char> model_data(model_size);
        fread(model_data.data(), 1, model_size, fp);
        fclose(fp);

        // 初始化 RKNN
        int ret = rknn_init(&ctx_, model_data.data(), model_size, 0, nullptr);
        if (ret != RKNN_SUCC) {
            throw RKNNError("rknn_init failed", ret);
        }

        // 查询输入输出信息
        rknn_input_output_num io_num;
        ret = rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
        if (ret != RKNN_SUCC) {
            throw RKNNError("rknn_query failed", ret);
        }

        printf("[RKNN#%d] Model loaded: %d inputs, %d outputs\n", 
               camera_id_, io_num.n_input, io_num.n_output);

        // 获取输入属性
        input_attrs_.resize(io_num.n_input);
        for (uint32_t i = 0; i < io_num.n_input; ++i) {
            input_attrs_[i].index = i;
            ret = rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &input_attrs_[i], sizeof(rknn_tensor_attr));
            if (ret != RKNN_SUCC) {
                throw RKNNError("rknn_query input attr failed", ret);
            }
        }

        // 获取输出属性
        output_attrs_.resize(io_num.n_output);
        for (uint32_t i = 0; i < io_num.n_output; ++i) {
            output_attrs_[i].index = i;
            ret = rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &output_attrs_[i], sizeof(rknn_tensor_attr));
            if (ret != RKNN_SUCC) {
                throw RKNNError("rknn_query output attr failed", ret);
            }
        }

        initialized_ = true;
        printf("[RKNN#%d] Initialized\n", camera_id_);
    }

    bool inference(void* input_data, uint32_t width, uint32_t height, 
                   std::vector<float>* output_data) {
        if (!initialized_ || !input_data) return false;

        // 设置输入
        rknn_input inputs[1];
        memset(inputs, 0, sizeof(inputs));
        inputs[0].index = 0;
        inputs[0].type = RKNN_TENSOR_UINT8;
        inputs[0].size = width * height * 3;
        inputs[0].fmt = RKNN_TENSOR_NHWC;
        inputs[0].buf = input_data;

        int ret = rknn_inputs_set(ctx_, 1, inputs);
        if (ret != RKNN_SUCC) {
            // 只在第一次失败时打印
            static bool first_error[2] = {false, false};
            if (!first_error[camera_id_]) {
                printf("[RKNN#%d] rknn_inputs_set failed: %d (input: %ux%u, expected: %u bytes)\n", 
                       camera_id_, ret, width, height, input_attrs_[0].size);
                first_error[camera_id_] = true;
            }
            return false;
        }

        // 推理
        ret = rknn_run(ctx_, nullptr);
        if (ret != RKNN_SUCC) {
            printf("[RKNN#%d] rknn_run failed: %d\n", camera_id_, ret);
            return false;
        }

        // 获取输出
        std::vector<rknn_output> outputs(output_attrs_.size());
        for (auto& out : outputs) {
            out.want_float = 1;
            out.is_prealloc = 0;
        }

        ret = rknn_outputs_get(ctx_, outputs.size(), outputs.data(), nullptr);
        if (ret != RKNN_SUCC) {
            printf("[RKNN#%d] rknn_outputs_get failed: %d\n", camera_id_, ret);
            return false;
        }

        // 复制输出数据
        output_data->clear();
        for (const auto& out : outputs) {
            if (out.size > 0 && out.buf) {
                float* data = (float*)out.buf;
                size_t count = out.size / sizeof(float);
                output_data->insert(output_data->end(), data, data + count);
            }
        }

        rknn_outputs_release(ctx_, outputs.size(), outputs.data());
        return true;
    }

    void close() {
        if (ctx_ != 0) {
            rknn_destroy(ctx_);
            ctx_ = 0;
        }
        initialized_ = false;
    }

    const std::vector<rknn_tensor_attr>& input_attrs() const { return input_attrs_; }
    const std::vector<rknn_tensor_attr>& output_attrs() const { return output_attrs_; }

private:
    int camera_id_;
    rknn_context ctx_;
    bool initialized_;
    std::vector<rknn_tensor_attr> input_attrs_;
    std::vector<rknn_tensor_attr> output_attrs_;
};

// ============================================================================
// 摄像头通道（带完整流水线）
// ============================================================================

class CameraChannel {
public:
    CameraChannel(int camera_id, const std::string& device, 
                  uint32_t width, uint32_t height, uint32_t fps,
                  const std::string& rknn_model = "")
        : camera_id_(camera_id), capture_(device, width, height, fps), 
          decoder_(camera_id), rga_(camera_id), rknn_(camera_id),
          running_(false), frame_count_(0), decode_count_(0), 
          scale_count_(0), inference_count_(0),
          rknn_model_(rknn_model), scale_width_(640), scale_height_(640) {}

    ~CameraChannel() { stop(); }

    void init() {
        capture_.init();
        decoder_.init(1920, 1080, MPP_VIDEO_CodingMJPEG);
        rga_.init();
        
        if (!rknn_model_.empty()) {
            try {
                rknn_.init(rknn_model_);
                has_rknn_ = true;
            } catch (const std::exception& e) {
                printf("[RKNN#%d] Warning: %s\n", camera_id_, e.what());
                has_rknn_ = false;
            }
        }
    }

    void start(FrameCallback callback) {
        frame_callback_ = callback;
        running_ = true;
        capture_.start();
        
        capture_thread_ = std::thread(&CameraChannel::capture_loop, this);
        decode_thread_ = std::thread(&CameraChannel::decode_loop, this);
        process_thread_ = std::thread(&CameraChannel::process_loop, this);

        // CPU 亲和性（大核）
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        for (int i = 4; i < 8; ++i) CPU_SET(i, &cpuset);
        pthread_setaffinity_np(capture_thread_.native_handle(), sizeof(cpu_set_t), &cpuset);
        pthread_setaffinity_np(decode_thread_.native_handle(), sizeof(cpu_set_t), &cpuset);
        pthread_setaffinity_np(process_thread_.native_handle(), sizeof(cpu_set_t), &cpuset);
    }

    void stop() {
        running_ = false;
        decoded_cv_.notify_all();
        process_cv_.notify_all();
        
        if (capture_thread_.joinable()) capture_thread_.join();
        if (decode_thread_.joinable()) decode_thread_.join();
        if (process_thread_.joinable()) process_thread_.join();
        
        capture_.stop();
    }

    void set_scale_size(uint32_t width, uint32_t height) {
        scale_width_ = width;
        scale_height_ = height;
    }

    int camera_id() const { return camera_id_; }
    int frame_count() const { return frame_count_.load(); }
    int decode_count() const { return decode_count_.load(); }
    int scale_count() const { return scale_count_.load(); }
    int inference_count() const { return inference_count_.load(); }
    const std::string& device() const { return capture_.device(); }

private:
    void capture_loop() {
        printf("[Capture#%d] Thread started for %s\n", camera_id_, device().c_str());
        while (running_) {
            void* data;
            size_t size;
            uint32_t index;
            if (capture_.capture_frame(&data, &size, &index)) {
                frame_count_++;
                std::vector<uint8_t> frame_data((uint8_t*)data, (uint8_t*)data + size);
                {
                    std::lock_guard<std::mutex> lock(capture_mutex_);
                    if (capture_queue_.size() < 10) {
                        capture_queue_.push(std::move(frame_data));
                    }
                }
                decoded_cv_.notify_one();
                capture_.release_frame(index);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        printf("[Capture#%d] Thread stopped\n", camera_id_);
    }

    void decode_loop() {
        printf("[Decode#%d] Thread started\n", camera_id_);
        while (running_) {
            std::vector<uint8_t> frame_data;
            {
                std::unique_lock<std::mutex> lock(capture_mutex_);
                decoded_cv_.wait_for(lock, std::chrono::milliseconds(100), 
                    [this]() -> bool { return !capture_queue_.empty() || !running_; });
                if (!running_ && capture_queue_.empty()) break;
                if (!capture_queue_.empty()) {
                    frame_data = std::move(capture_queue_.front());
                    capture_queue_.pop();
                }
            }

            if (!frame_data.empty()) {
                Frame frame;
                if (decoder_.decode(frame_data.data(), frame_data.size(), 0, &frame)) {
                    decode_count_++;
                    
                    // 回调：解码完成
                    if (frame_callback_) {
                        frame_callback_(frame, FrameStage::DECODED);
                    }
                    
                    {
                        std::lock_guard<std::mutex> lock(decoded_mutex_);
                        if (decoded_queue_.size() < 5) {
                            decoded_queue_.push(std::move(frame));
                        }
                    }
                    process_cv_.notify_one();
                }
            }
        }
        printf("[Decode#%d] Thread stopped\n", camera_id_);
    }

    void process_loop() {
        printf("[Process#%d] Thread started\n", camera_id_);
        
        // 分配 RGB buffer（640x480x3）
        std::vector<uint8_t> rgb_buffer(scale_width_ * scale_height_ * 3);
        
        while (running_) {
            Frame frame;
            {
                std::unique_lock<std::mutex> lock(decoded_mutex_);
                process_cv_.wait_for(lock, std::chrono::milliseconds(100), 
                    [this]() -> bool { return !decoded_queue_.empty() || !running_; });
                if (!running_ && decoded_queue_.empty()) break;
                if (!decoded_queue_.empty()) {
                    frame = std::move(decoded_queue_.front());
                    decoded_queue_.pop();
                }
            }

            if (frame.data) {
                // RGA 缩放：NV12 → RGB888
                if (rga_.scale_nv12_to_rgb(frame.data, frame.width, frame.height,
                                           rgb_buffer.data(), scale_width_, scale_height_,
                                           frame.mpp_buffer)) {
                    scale_count_++;
                    frame.rga_data = rgb_buffer.data();
                    frame.rga_width = scale_width_;
                    frame.rga_height = scale_height_;
                    
                    // 回调：缩放完成
                    if (frame_callback_) {
                        frame_callback_(frame, FrameStage::SCALED);
                    }
                    
                    // RKNN 推理
                    if (has_rknn_) {
                        std::vector<float> results;
                        if (rknn_.inference(rgb_buffer.data(), scale_width_, scale_height_, &results)) {
                            inference_count_++;
                            frame.inference_results = std::move(results);
                            
                            // 回调：推理完成
                            if (frame_callback_) {
                                frame_callback_(frame, FrameStage::INFERENCE);
                            }
                        }
                    }
                }
                
                // 释放 MPP frame
                if (frame.mpp_frame) {
                    decoder_.release_frame(frame.mpp_frame);
                }
            }
        }
        printf("[Process#%d] Thread stopped\n", camera_id_);
    }

    int camera_id_;
    V4L2Capture capture_;
    MPPDecoder decoder_;
    RGAProcessor rga_;
    RKNNInference rknn_;
    
    std::thread capture_thread_, decode_thread_, process_thread_;
    std::atomic<bool> running_;
    std::atomic<int> frame_count_, decode_count_, scale_count_, inference_count_;
    
    std::mutex capture_mutex_, decoded_mutex_;
    std::condition_variable decoded_cv_, process_cv_;
    std::queue<std::vector<uint8_t>> capture_queue_;
    std::queue<Frame> decoded_queue_;
    
    FrameCallback frame_callback_;
    std::string rknn_model_;
    bool has_rknn_ = false;
    uint32_t scale_width_, scale_height_;
};

// ============================================================================
// 多摄像头管理器
// ============================================================================

class MultiCameraManager {
public:
    void add_camera(const std::string& device, uint32_t width, uint32_t height, uint32_t fps,
                    const std::string& rknn_model = "") {
        int id = channels_.size();
        channels_.push_back(std::make_unique<CameraChannel>(id, device, width, height, fps, rknn_model));
    }

    void init() {
        for (auto& ch : channels_) {
            try {
                ch->init();
            } catch (const std::exception& e) {
                fprintf(stderr, "Failed to init camera %s: %s\n", ch->device().c_str(), e.what());
            }
        }
    }

    void start(FrameCallback callback) {
        for (auto& ch : channels_) {
            ch->start(callback);
        }
    }

    void stop() {
        for (auto& ch : channels_) {
            ch->stop();
        }
    }

    void print_stats() {
        printf("\n=== Camera Statistics ===\n");
        for (auto& ch : channels_) {
            printf("  Camera #%d (%s): captured=%d, decoded=%d, scaled=%d, inference=%d\n", 
                   ch->camera_id(), ch->device().c_str(), 
                   ch->frame_count(), ch->decode_count(), 
                   ch->scale_count(), ch->inference_count());
        }
        printf("=========================\n");
        fflush(stdout);
    }

private:
    std::vector<std::unique_ptr<CameraChannel>> channels_;
};

// ============================================================================
// 主函数
// ============================================================================

int main(int argc, char* argv[]) {
    printf("=== V4L2 + MPP/RGA/RKNN Multi-Camera AI Framework ===\n");
    fflush(stdout);

    // RKNN 模型路径（板子上已有的模型）
    std::string rknn_model = "/home/topeet/RKNN-YOLOV5-BatchInference-MultiThreading/model/RK3588/yolov5s-640-640.rknn";
    
    // 检查模型是否存在
    FILE* fp = fopen(rknn_model.c_str(), "r");
    if (fp) {
        fclose(fp);
        printf("Found RKNN model: %s\n", rknn_model.c_str());
    } else {
        printf("Warning: RKNN model not found, running without inference\n");
        rknn_model = "";
    }

    MultiCameraManager manager;

    // 添加两个 USB 摄像头（带 RKNN 模型）
    manager.add_camera("/dev/video21", 1920, 1080, 25, rknn_model);  // 2303 webcam
    manager.add_camera("/dev/video23", 1920, 1080, 25, rknn_model);  // 2K USB Camera

    printf("Initializing cameras...\n");
    fflush(stdout);
    manager.init();

    // 帧回调
    auto callback = [](const Frame& frame, FrameStage stage) {
        switch (stage) {
            case FrameStage::DECODED:
                // MPP 解码完成
                break;
            case FrameStage::SCALED:
                // RGA 缩放完成
                break;
            case FrameStage::INFERENCE:
                // RKNN 推理完成
                if (!frame.inference_results.empty()) {
                    printf("[Inference#%d] Results: %zu values\n", 
                           frame.camera_id, frame.inference_results.size());
                }
                break;
        }
    };

    printf("Starting cameras...\n");
    fflush(stdout);
    manager.start(callback);

    printf("Running for 30 seconds... Press Ctrl+C to exit\n");
    fflush(stdout);

    // 运行 30 秒
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (i % 5 == 4) {
            manager.print_stats();
        }
    }

    manager.stop();
    manager.print_stats();
    
    printf("Done!\n");
    return 0;
}
