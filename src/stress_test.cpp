/**
 * RKNN 极限压力测试
 * 
 * 从两路摄像头采集帧，复制成多路进行 RKNN 推理
 * 测试 RK3588 NPU 最大推理路数
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <memory>

// V4L2
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <unistd.h>

// MPP
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_meta.h>
#include <rockchip/mpp_task.h>
#include <rockchip/rk_vdec_cmd.h>

// RGA
#include <rga/RgaApi.h>
#include <rga/drmrga.h>

// RKNN
#include <rknn_api.h>

// ============================================================================
// 配置
// ============================================================================
#define MAX_CHANNELS 16  // 最大测试路数
#define TEST_DURATION 30 // 测试时长（秒）

// MPP 对齐宏
#define MPP_ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

// ============================================================================
// Buffer 结构
// ============================================================================
struct BufferInfo {
    void* ptr;
    size_t size;
};

// ============================================================================
// V4L2 采集
// ============================================================================
class V4L2Capture {
public:
    V4L2Capture(const std::string& device, uint32_t width, uint32_t height, uint32_t fps)
        : device_(device), width_(width), height_(height), fps_(fps), fd_(-1), running_(false) {}
    
    ~V4L2Capture() { stop(); }
    
    bool init() {
        fd_ = open(device_.c_str(), O_RDWR | O_NONBLOCK);
        if (fd_ < 0) {
            perror("open");
            return false;
        }
        
        // 设置格式
        v4l2_format fmt = {};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = width_;
        fmt.fmt.pix.height = height_;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;
        
        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
            perror("VIDIOC_S_FMT");
            return false;
        }
        
        // 请求缓冲区
        v4l2_requestbuffers req = {};
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        
        if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
            perror("VIDIOC_REQBUFS");
            return false;
        }
        
        // 映射缓冲区
        for (uint32_t i = 0; i < req.count; i++) {
            v4l2_buffer buf = {};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            
            if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
                perror("VIDIOC_QUERYBUF");
                return false;
            }
            
            void* ptr = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
            if (ptr == MAP_FAILED) {
                perror("mmap");
                return false;
            }
            
            BufferInfo info = {ptr, buf.length};
            buffers_.push_back(info);
            
            if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
                perror("VIDIOC_QBUF");
                return false;
            }
        }
        
        // 开始采集
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
            perror("VIDIOC_STREAMON");
            return false;
        }
        
        return true;
    }
    
    bool read_frame(std::vector<uint8_t>& data) {
        pollfd pfd = {fd_, POLLIN, 0};
        if (poll(&pfd, 1, 1000) <= 0) return false;
        
        v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        
        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) return false;
        
        data.assign((uint8_t*)buffers_[buf.index].ptr, 
                    (uint8_t*)buffers_[buf.index].ptr + buf.bytesused);
        
        ioctl(fd_, VIDIOC_QBUF, &buf);
        return true;
    }
    
    void stop() {
        if (fd_ >= 0) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(fd_, VIDIOC_STREAMOFF, &type);
            close(fd_);
            fd_ = -1;
        }
        for (auto& b : buffers_) {
            if (b.ptr) munmap(b.ptr, b.size);
        }
        buffers_.clear();
    }
    
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    
private:
    std::string device_;
    uint32_t width_, height_, fps_;
    int fd_;
    bool running_;
    std::vector<BufferInfo> buffers_;
};

// ============================================================================
// MPP 解码器（高级模式 - MJPEG 必需）
// ============================================================================
class MPPDecoder {
public:
    MPPDecoder() : ctx_(nullptr), api_(nullptr), frm_grp_(nullptr), frm_buf_(nullptr), 
                   pkt_grp_(nullptr), frame_(nullptr), initialized_(false) {}
    
    ~MPPDecoder() {
        if (frame_) mpp_frame_deinit(&frame_);
        if (frm_buf_) mpp_buffer_put(frm_buf_);
        if (frm_grp_) mpp_buffer_group_put(frm_grp_);
        if (pkt_grp_) mpp_buffer_group_put(pkt_grp_);
        if (ctx_) {
            if (api_) api_->reset(ctx_);
            mpp_destroy(ctx_);
        }
    }
    
    bool init(uint32_t width, uint32_t height) {
        MPP_RET ret = mpp_create(&ctx_, &api_);
        if (ret != MPP_OK) return false;
        
        ret = mpp_init(ctx_, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG);
        if (ret != MPP_OK) return false;
        
        // 设置输出格式为 NV12
        MppFrameFormat output_fmt = MPP_FMT_YUV420SP;
        api_->control(ctx_, MPP_DEC_SET_OUTPUT_FORMAT, &output_fmt);
        
        // 初始化 frame
        ret = mpp_frame_init(&frame_);
        if (ret != MPP_OK) return false;
        
        // 创建 frame buffer group
        ret = mpp_buffer_group_get_internal(&frm_grp_, MPP_BUFFER_TYPE_ION);
        if (ret != MPP_OK) {
            ret = mpp_buffer_group_get_internal(&frm_grp_, MPP_BUFFER_TYPE_DMA_HEAP);
        }
        if (ret != MPP_OK) return false;
        
        // 分配 NV12 缓冲区
        RK_U32 hor_stride = MPP_ALIGN(width, 16);
        RK_U32 ver_stride = MPP_ALIGN(height, 16);
        size_t frame_size = hor_stride * ver_stride * 4;
        
        ret = mpp_buffer_get(frm_grp_, &frm_buf_, frame_size);
        if (ret != MPP_OK) return false;
        mpp_frame_set_buffer(frame_, frm_buf_);
        
        // 创建 packet buffer group
        ret = mpp_buffer_group_get_internal(&pkt_grp_, MPP_BUFFER_TYPE_ION);
        if (ret != MPP_OK) {
            ret = mpp_buffer_group_get_internal(&pkt_grp_, MPP_BUFFER_TYPE_DMA_HEAP);
        }
        if (ret != MPP_OK) return false;
        
        initialized_ = true;
        return true;
    }
    
    bool decode(const uint8_t* data, size_t size, uint8_t** nv12_data, uint32_t* width, uint32_t* height) {
        if (!initialized_ || !data || size == 0) return false;
        
        // 获取 packet buffer
        MppBuffer pkt_buf = nullptr;
        MPP_RET ret = mpp_buffer_get(pkt_grp_, &pkt_buf, size);
        if (ret != MPP_OK || !pkt_buf) return false;
        
        void* pkt_ptr = mpp_buffer_get_ptr(pkt_buf);
        if (!pkt_ptr) {
            mpp_buffer_put(pkt_buf);
            return false;
        }
        memcpy(pkt_ptr, data, size);
        
        // 创建 packet
        MppPacket packet = nullptr;
        ret = mpp_packet_init_with_buffer(&packet, pkt_buf);
        if (ret != MPP_OK || !packet) {
            mpp_buffer_put(pkt_buf);
            return false;
        }
        
        // 高级模式：poll input
        ret = api_->poll(ctx_, MPP_PORT_INPUT, MPP_POLL_BLOCK);
        if (ret != MPP_OK) {
            mpp_packet_deinit(&packet);
            mpp_buffer_put(pkt_buf);
            return false;
        }
        
        // dequeue input task
        MppTask task = nullptr;
        ret = api_->dequeue(ctx_, MPP_PORT_INPUT, &task);
        if (ret != MPP_OK || !task) {
            mpp_packet_deinit(&packet);
            mpp_buffer_put(pkt_buf);
            return false;
        }
        
        // 设置 meta
        mpp_task_meta_set_packet(task, KEY_INPUT_PACKET, packet);
        mpp_task_meta_set_frame(task, KEY_OUTPUT_FRAME, frame_);
        
        // enqueue input task
        ret = api_->enqueue(ctx_, MPP_PORT_INPUT, task);
        if (ret != MPP_OK) {
            mpp_packet_deinit(&packet);
            mpp_buffer_put(pkt_buf);
            return false;
        }
        
        // poll output
        ret = api_->poll(ctx_, MPP_PORT_OUTPUT, MPP_POLL_BLOCK);
        if (ret != MPP_OK) {
            mpp_packet_deinit(&packet);
            mpp_buffer_put(pkt_buf);
            return false;
        }
        
        // dequeue output task
        MppTask out_task = nullptr;
        ret = api_->dequeue(ctx_, MPP_PORT_OUTPUT, &out_task);
        if (ret != MPP_OK || !out_task) {
            mpp_packet_deinit(&packet);
            mpp_buffer_put(pkt_buf);
            return false;
        }
        
        // 获取输出 frame
        MppFrame frame_out = nullptr;
        mpp_task_meta_get_frame(out_task, KEY_OUTPUT_FRAME, &frame_out);
        
        // enqueue output task
        api_->enqueue(ctx_, MPP_PORT_OUTPUT, out_task);
        
        // 清理 packet
        mpp_packet_deinit(&packet);
        
        if (!frame_out) {
            return false;
        }
        
        // 检查 frame 状态
        if (mpp_frame_get_eos(frame_out) || mpp_frame_get_info_change(frame_out)) {
            return false;
        }
        
        // 获取 frame 信息
        *width = mpp_frame_get_width(frame_out);
        *height = mpp_frame_get_height(frame_out);
        *nv12_data = (uint8_t*)mpp_buffer_get_ptr(frm_buf_);
        
        return true;
    }
    
private:
    MppCtx ctx_;
    MppApi* api_;
    MppBufferGroup frm_grp_;
    MppBuffer frm_buf_;
    MppBufferGroup pkt_grp_;
    MppFrame frame_;
    bool initialized_;
};

// ============================================================================
// RGA 缩放
// ============================================================================
class RGAProcessor {
public:
    bool scale_nv12_to_rgb(uint8_t* nv12, uint32_t src_w, uint32_t src_h,
                           uint8_t* rgb, uint32_t dst_w, uint32_t dst_h) {
        rga_info_t src_info = {}, dst_info = {};
        
        // 源：NV12
        src_info.fd = -1;
        src_info.virAddr = nv12;
        src_info.mmuFlag = 1;
        
        rga_set_rect(&src_info.rect, 0, 0, src_w, src_h, 
                     MPP_ALIGN(src_w, 16), MPP_ALIGN(src_h, 16), 
                     RK_FORMAT_YCbCr_420_SP);
        
        // 目标：RGB888
        dst_info.fd = -1;
        dst_info.virAddr = rgb;
        dst_info.mmuFlag = 1;
        
        rga_set_rect(&dst_info.rect, 0, 0, dst_w, dst_h, 
                     dst_w, dst_h, 
                     RK_FORMAT_RGB_888);
        
        return c_RkRgaBlit(&src_info, &dst_info, nullptr) == 0;
    }
};

// ============================================================================
// RKNN 推理
// ============================================================================
class RKNNInference {
public:
    RKNNInference(int id) : id_(id), ctx_(0), initialized_(false) {}
    
    ~RKNNInference() {
        if (ctx_) rknn_destroy(ctx_);
    }
    
    bool init(const std::string& model_path, uint32_t input_w, uint32_t input_h, rknn_core_mask core_mask = RKNN_NPU_CORE_AUTO) {
        FILE* fp = fopen(model_path.c_str(), "rb");
        if (!fp) {
            printf("[RKNN#%d] Failed to open model: %s\n", id_, model_path.c_str());
            return false;
        }
        
        fseek(fp, 0, SEEK_END);
        size_t model_size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        
        std::vector<char> model_data(model_size);
        if (fread(model_data.data(), 1, model_size, fp) != model_size) {
            fclose(fp);
            return false;
        }
        fclose(fp);
        
        int ret = rknn_init(&ctx_, model_data.data(), model_size, 0, NULL);
        if (ret != RKNN_SUCC) {
            printf("[RKNN#%d] rknn_init failed: %d\n", id_, ret);
            ctx_ = 0;
            return false;
        }
        
        // 设置 NPU 核心掩码（多核并行）
        if (core_mask != RKNN_NPU_CORE_AUTO) {
            ret = rknn_set_core_mask(ctx_, core_mask);
            if (ret != RKNN_SUCC) {
                printf("[RKNN#%d] rknn_set_core_mask failed: %d\n", id_, ret);
            } else {
                printf("[RKNN#%d] Set core mask: 0x%x\n", id_, core_mask);
            }
        }
        
        // 获取输入输出信息
        rknn_input_output_num io_num;
        ret = rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
        if (ret != RKNN_SUCC) {
            printf("[RKNN#%d] rknn_query failed: %d\n", id_, ret);
            return false;
        }
        
        input_attrs_.resize(io_num.n_input);
        rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, input_attrs_.data(), sizeof(rknn_tensor_attr) * io_num.n_input);
        
        output_attrs_.resize(io_num.n_output);
        rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, output_attrs_.data(), sizeof(rknn_tensor_attr) * io_num.n_output);
        
        input_w_ = input_w;
        input_h_ = input_h;
        initialized_ = true;
        
        printf("[RKNN#%d] Model loaded: %u inputs, %u outputs, input size: %u bytes\n",
               id_, io_num.n_input, io_num.n_output, input_attrs_[0].size);
        
        return true;
    }
    
    bool inference(uint8_t* rgb_data) {
        if (!initialized_) {
            static bool first = false;
            if (!first) {
                printf("[RKNN#%d] Not initialized\n", id_);
                first = true;
            }
            return false;
        }
        
        rknn_input inputs[1];
        memset(inputs, 0, sizeof(inputs));
        inputs[0].index = 0;
        inputs[0].type = RKNN_TENSOR_UINT8;
        inputs[0].size = input_w_ * input_h_ * 3;
        inputs[0].fmt = RKNN_TENSOR_NHWC;
        inputs[0].buf = rgb_data;
        
        int ret = rknn_inputs_set(ctx_, 1, inputs);
        if (ret != RKNN_SUCC) {
            static bool first_err[16] = {false};
            if (!first_err[id_]) {
                printf("[RKNN#%d] rknn_inputs_set failed: %d\n", id_, ret);
                first_err[id_] = true;
            }
            return false;
        }
        
        ret = rknn_run(ctx_, NULL);
        if (ret != RKNN_SUCC) {
            static bool first_err2[16] = {false};
            if (!first_err2[id_]) {
                printf("[RKNN#%d] rknn_run failed: %d\n", id_, ret);
                first_err2[id_] = true;
            }
            return false;
        }
        
        rknn_output outputs[3];
        memset(outputs, 0, sizeof(outputs));
        for (int i = 0; i < 3; i++) {
            outputs[i].want_float = 1;
        }
        
        ret = rknn_outputs_get(ctx_, 3, outputs, NULL);
        if (ret != RKNN_SUCC) {
            static bool first_err3[16] = {false};
            if (!first_err3[id_]) {
                printf("[RKNN#%d] rknn_outputs_get failed: %d\n", id_, ret);
                first_err3[id_] = true;
            }
            return false;
        }
        
        rknn_outputs_release(ctx_, 3, outputs);
        return true;
    }
    
private:
    int id_;
    rknn_context ctx_;
    bool initialized_;
    std::vector<rknn_tensor_attr> input_attrs_, output_attrs_;
    uint32_t input_w_, input_h_;
};

// ============================================================================
// 压力测试管理器
// ============================================================================
class StressTestManager {
public:
    StressTestManager(int num_channels) : num_channels_(num_channels), running_(false) {}
    
    bool init() {
        // 初始化两路摄像头
        printf("=== 初始化摄像头 ===\n");
        
        cameras_[0] = std::make_unique<V4L2Capture>("/dev/video21", 1920, 1080, 25);
        cameras_[1] = std::make_unique<V4L2Capture>("/dev/video23", 1920, 1080, 25);
        
        if (!cameras_[0]->init()) {
            printf("Failed to init camera 0\n");
            return false;
        }
        printf("Camera #0: /dev/video21 initialized\n");
        
        if (!cameras_[1]->init()) {
            printf("Failed to init camera 1\n");
            return false;
        }
        printf("Camera #1: /dev/video23 initialized\n");
        
        // 初始化解码器
        printf("\n=== 初始化 MPP 解码器 ===\n");
        decoders_[0] = std::make_unique<MPPDecoder>();
        decoders_[1] = std::make_unique<MPPDecoder>();
        
        if (!decoders_[0]->init(1920, 1080) || !decoders_[1]->init(1920, 1080)) {
            printf("Failed to init MPP decoders\n");
            return false;
        }
        printf("MPP decoders initialized\n");
        
        // 初始化 RGA
        printf("\n=== 初始化 RGA ===\n");
        c_RkRgaInit();
        printf("RGA initialized\n");
        
        // 初始化 RKNN 模型（多路共享）
        printf("\n=== 初始化 RKNN 模型 (%d 路) ===\n", num_channels_);
        std::string model_path = "/home/topeet/RKNN-YOLOV5-BatchInference-MultiThreading/model/RK3588/yolov5s-640-640.rknn";
        
        // NPU 核心分配策略：
        // - RKNN_NPU_CORE_0: 核心 0 (2 TOPS)
        // - RKNN_NPU_CORE_1: 核心 1 (2 TOPS)
        // - RKNN_NPU_CORE_2: 核心 2 (2 TOPS)
        // - RKNN_NPU_CORE_0_1_2: 三核并行（单模型加速）
        // - RKNN_NPU_CORE_AUTO: 自动调度
        
        // 策略：将实例均匀分配到 3 个核心
        rknn_core_mask core_masks[] = {
            RKNN_NPU_CORE_0,      // 实例 0, 3, 6, 9, 12, 15
            RKNN_NPU_CORE_1,      // 实例 1, 4, 7, 10, 13
            RKNN_NPU_CORE_2       // 实例 2, 5, 8, 11, 14
        };
        
        for (int i = 0; i < num_channels_; i++) {
            rknn_core_mask core = core_masks[i % 3];
            rknn_instances_[i] = std::make_unique<RKNNInference>(i);
            if (!rknn_instances_[i]->init(model_path, 640, 640, core)) {
                printf("Failed to init RKNN instance %d\n", i);
                return false;
            }
        }
        printf("%d RKNN instances initialized (3-core distribution)\n", num_channels_);
        
        // 分配 RGB 缓冲区
        for (int i = 0; i < num_channels_; i++) {
            rgb_buffers_[i].resize(640 * 640 * 3);
        }
        
        return true;
    }
    
    void run() {
        running_ = true;
        
        printf("\n=== 开始压力测试 ===\n");
        printf("路数: %d\n", num_channels_);
        printf("时长: %d 秒\n", TEST_DURATION);
        printf("\n");
        
        auto start_time = std::chrono::steady_clock::now();
        
        std::vector<uint8_t> mjpeg_data;
        uint8_t* nv12_data = nullptr;
        uint32_t frame_w, frame_h;
        
        int frame_count = 0;
        int inference_count = 0;
        int failed_count = 0;
        
        while (running_) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            
            if (elapsed >= TEST_DURATION) {
                running_ = false;
                break;
            }
            
            // 每 5 秒打印一次进度
            if (elapsed > 0 && elapsed % 5 == 0 && !printed_[elapsed]) {
                printed_[elapsed] = true;
                double fps = frame_count / (double)elapsed;
                double inf_fps = inference_count / (double)elapsed;
                printf("[%lds] Frames: %d, Inference: %d, FPS: %.1f, Inf FPS: %.1f (x%d = %.1f)\n",
                       elapsed, frame_count, inference_count, fps, inf_fps, 
                       num_channels_, inf_fps * num_channels_);
            }
            
            // 从摄像头 0 采集
            if (!cameras_[0]->read_frame(mjpeg_data)) {
                continue;
            }
            
            frame_count++;
            
            // 解码
            if (!decoders_[0]->decode(mjpeg_data.data(), mjpeg_data.size(), &nv12_data, &frame_w, &frame_h)) {
                static int decode_fail_count = 0;
                if (decode_fail_count++ < 3) {
                    printf("Decode failed (frame %d)\n", frame_count);
                }
                continue;
            }
            
            // RGA 缩放（只做一次）
            if (!rga_.scale_nv12_to_rgb(nv12_data, frame_w, frame_h, 
                                   rgb_buffers_[0].data(), 640, 640)) {
                static bool first_rga_err = false;
                if (!first_rga_err) {
                    printf("RGA scale failed (frame %d, %ux%u -> 640x640)\n", frame_count, frame_w, frame_h);
                    first_rga_err = true;
                }
                continue;
            }
            
            static bool first_frame = true;
            if (first_frame) {
                printf("First frame decoded and scaled: %ux%u -> 640x640\n", frame_w, frame_h);
                first_frame = false;
            }
            
            // 复制到所有路
            for (int i = 1; i < num_channels_; i++) {
                memcpy(rgb_buffers_[i].data(), rgb_buffers_[0].data(), 640 * 640 * 3);
            }
            
            // 多路 RKNN 推理（多线程并行）
            auto inf_start = std::chrono::high_resolution_clock::now();
            
            std::atomic<int> success_count{0};
            std::vector<std::thread> threads;
            
            for (int i = 0; i < num_channels_; i++) {
                threads.emplace_back([&, i]() {
                    if (rknn_instances_[i]->inference(rgb_buffers_[i].data())) {
                        success_count++;
                    }
                });
            }
            
            for (auto& t : threads) {
                t.join();
            }
            
            if (success_count == num_channels_) {
                inference_count++;
            }
            
            auto inf_end = std::chrono::high_resolution_clock::now();
            double inf_time = std::chrono::duration<double, std::milli>(inf_end - inf_start).count();
        }
        
        auto end_time = std::chrono::steady_clock::now();
        double total_seconds = std::chrono::duration<double>(end_time - start_time).count();
        
        printf("\n=== 测试结果 ===\n");
        printf("测试时长: %.1f 秒\n", total_seconds);
        printf("采集帧数: %d\n", frame_count);
        printf("推理次数: %d (x%d 路 = %d 次推理)\n", 
               inference_count, num_channels_, inference_count * num_channels_);
        printf("失败次数: %d\n", failed_count);
        printf("采集帧率: %.1f fps\n", frame_count / total_seconds);
        printf("推理帧率: %.1f fps\n", inference_count / total_seconds);
        printf("等效总帧率: %.1f fps (%d 路并行)\n", 
               inference_count * num_channels_ / total_seconds, num_channels_);
        printf("平均推理延迟: %.2f ms\n", total_seconds * 1000 / (inference_count * num_channels_));
    }
    
private:
    int num_channels_;
    std::atomic<bool> running_;
    
    std::unique_ptr<V4L2Capture> cameras_[2];
    std::unique_ptr<MPPDecoder> decoders_[2];
    std::unique_ptr<RKNNInference> rknn_instances_[MAX_CHANNELS];
    
    RGAProcessor rga_;
    std::vector<uint8_t> rgb_buffers_[MAX_CHANNELS];
    
    bool printed_[TEST_DURATION + 1] = {false};
};

// ============================================================================
// 主函数
// ============================================================================
int main(int argc, char* argv[]) {
    int num_channels = 4;  // 默认 4 路
    
    if (argc > 1) {
        num_channels = atoi(argv[1]);
        if (num_channels < 1 || num_channels > MAX_CHANNELS) {
            printf("Usage: %s [num_channels 1-%d]\n", argv[0], MAX_CHANNELS);
            return 1;
        }
    }
    
    printf("╔════════════════════════════════════════════════════════╗\n");
    printf("║     RKNN 极限压力测试 - RK3588 NPU 性能测试           ║\n");
    printf("╚════════════════════════════════════════════════════════╝\n\n");
    
    StressTestManager test(num_channels);
    
    if (!test.init()) {
        printf("初始化失败\n");
        return 1;
    }
    
    test.run();
    
    printf("\n测试完成！\n");
    return 0;
}
