/**
 * @file multi_camera_test.cpp
 * @brief 多路摄像头 + RKNN 推理测试程序
 * 
 * 架构：
 * - 采集/解码/RGA：串行进行（避免 MPP/RGA 线程安全问题）
 * - RKNN 推理：并行进行（利用 NPU 多核）
 */

#include <QApplication>
#include <QMainWindow>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QLineEdit>
#include <QTimer>
#include <QPainter>
#include <QImage>
#include <QMutex>
#include <QMessageBox>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <chrono>
#include <queue>
#include <condition_variable>

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

// MPP 对齐宏
#ifndef MPP_ALIGN
#define MPP_ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#endif

// ============================================================================
// COCO 80 类别
// ============================================================================
static const char* COCO_CLASSES[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
    "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
    "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
    "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
    "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone",
    "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush"
};

// YOLOv5 anchors
static const int ANCHORS[3][6] = {
    {10, 13, 16, 30, 33, 23},
    {30, 61, 62, 45, 59, 119},
    {116, 90, 156, 198, 373, 326}
};

// 检测结果
struct Detection {
    int x1, y1, x2, y2;
    float confidence;
    int class_id;
    const char* class_name;
};

// 帧数据
struct FrameData {
    QImage image;
    std::vector<Detection> detections;
    bool valid = false;
};

// ============================================================================
// V4L2 采集类
// ============================================================================
class V4L2Capture {
public:
    V4L2Capture(const std::string& device, uint32_t width, uint32_t height)
        : device_(device), width_(width), height_(height), fd_(-1) {}
    
    ~V4L2Capture() { close(); }
    
    bool init() {
        fd_ = open(device_.c_str(), O_RDWR | O_NONBLOCK);
        if (fd_ < 0) {
            printf("[V4L2] Failed to open %s\n", device_.c_str());
            return false;
        }
        
        v4l2_format fmt = {};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = width_;
        fmt.fmt.pix.height = height_;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;
        
        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
            printf("[V4L2] VIDIOC_S_FMT failed\n");
            return false;
        }
        
        v4l2_requestbuffers req = {};
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        
        if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
            printf("[V4L2] VIDIOC_REQBUFS failed\n");
            return false;
        }
        
        for (unsigned int i = 0; i < req.count; i++) {
            v4l2_buffer buf = {};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            
            if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
                return false;
            }
            
            void* ptr = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
            buffers_.push_back({ptr, buf.length});
        }
        
        for (unsigned int i = 0; i < buffers_.size(); i++) {
            v4l2_buffer buf = {};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            ioctl(fd_, VIDIOC_QBUF, &buf);
        }
        
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd_, VIDIOC_STREAMON, &type);
        
        printf("[V4L2] %s initialized (%ux%u)\n", device_.c_str(), width_, height_);
        return true;
    }
    
    bool read_frame(std::vector<uint8_t>& data) {
        pollfd pfd = {fd_, POLLIN, 0};
        if (poll(&pfd, 1, 100) <= 0) return false;
        
        v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        
        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) return false;
        
        data.resize(buf.bytesused);
        memcpy(data.data(), buffers_[buf.index].first, buf.bytesused);
        
        ioctl(fd_, VIDIOC_QBUF, &buf);
        return true;
    }
    
    void close() {
        if (fd_ >= 0) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(fd_, VIDIOC_STREAMOFF, &type);
            for (auto& b : buffers_) munmap(b.first, b.second);
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    std::string device_;
    uint32_t width_, height_;
    int fd_;
    std::vector<std::pair<void*, size_t>> buffers_;
};

// ============================================================================
// MPP 解码器类
// ============================================================================
class MPPDecoder {
public:
    MPPDecoder() : ctx_(nullptr), api_(nullptr), frame_(nullptr), 
                   last_frame_(nullptr), frm_buf_(nullptr), 
                   frm_grp_(nullptr), pkt_grp_(nullptr) {}
    
    ~MPPDecoder() { close(); }
    
    bool init(uint32_t width, uint32_t height) {
        width_ = width;
        height_ = height;
        
        MPP_RET ret = mpp_create(&ctx_, &api_);
        if (ret != MPP_OK) return false;
        
        ret = mpp_init(ctx_, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG);
        if (ret != MPP_OK) return false;
        
        MppFrameFormat output_fmt = MPP_FMT_YUV420SP;
        api_->control(ctx_, MPP_DEC_SET_OUTPUT_FORMAT, &output_fmt);
        
        mpp_frame_init(&frame_);
        
        RK_U32 hor_stride = MPP_ALIGN(width_, 16);
        RK_U32 ver_stride = MPP_ALIGN(height_, 16);
        RK_U32 frm_buf_size = hor_stride * ver_stride * 4;
        
        ret = mpp_buffer_group_get_internal(&frm_grp_, MPP_BUFFER_TYPE_ION);
        if (ret != MPP_OK) ret = mpp_buffer_group_get_internal(&frm_grp_, MPP_BUFFER_TYPE_DMA_HEAP);
        if (ret != MPP_OK) return false;
        
        mpp_buffer_get(frm_grp_, &frm_buf_, frm_buf_size);
        mpp_frame_set_buffer(frame_, frm_buf_);
        
        ret = mpp_buffer_group_get_internal(&pkt_grp_, MPP_BUFFER_TYPE_ION);
        if (ret != MPP_OK) ret = mpp_buffer_group_get_internal(&pkt_grp_, MPP_BUFFER_TYPE_DMA_HEAP);
        
        return true;
    }
    
    bool decode(const uint8_t* data, size_t size, uint8_t** nv12_out,
                uint32_t* width_out, uint32_t* height_out, uint32_t* stride_out) {
        if (!ctx_ || !data || size == 0) return false;
        
        MppBuffer pkt_buf = nullptr;
        MPP_RET ret = mpp_buffer_get(pkt_grp_, &pkt_buf, size);
        if (ret != MPP_OK || !pkt_buf) return false;
        
        void* pkt_ptr = mpp_buffer_get_ptr(pkt_buf);
        if (!pkt_ptr) { mpp_buffer_put(pkt_buf); return false; }
        memcpy(pkt_ptr, data, size);
        
        MppPacket packet = nullptr;
        ret = mpp_packet_init_with_buffer(&packet, pkt_buf);
        if (ret != MPP_OK || !packet) { mpp_buffer_put(pkt_buf); return false; }
        
        ret = api_->poll(ctx_, MPP_PORT_INPUT, MPP_POLL_BLOCK);
        if (ret != MPP_OK) { mpp_packet_deinit(&packet); mpp_buffer_put(pkt_buf); return false; }
        
        MppTask task = nullptr;
        ret = api_->dequeue(ctx_, MPP_PORT_INPUT, &task);
        if (ret != MPP_OK || !task) { mpp_packet_deinit(&packet); mpp_buffer_put(pkt_buf); return false; }
        
        mpp_task_meta_set_packet(task, KEY_INPUT_PACKET, packet);
        mpp_task_meta_set_frame(task, KEY_OUTPUT_FRAME, frame_);
        api_->enqueue(ctx_, MPP_PORT_INPUT, task);
        
        ret = api_->poll(ctx_, MPP_PORT_OUTPUT, MPP_POLL_BLOCK);
        if (ret != MPP_OK) { mpp_packet_deinit(&packet); mpp_buffer_put(pkt_buf); return false; }
        
        MppTask out_task = nullptr;
        ret = api_->dequeue(ctx_, MPP_PORT_OUTPUT, &out_task);
        if (ret != MPP_OK || !out_task) { mpp_packet_deinit(&packet); mpp_buffer_put(pkt_buf); return false; }
        
        MppFrame frame_out = nullptr;
        mpp_task_meta_get_frame(out_task, KEY_OUTPUT_FRAME, &frame_out);
        api_->enqueue(ctx_, MPP_PORT_OUTPUT, out_task);
        
        mpp_packet_deinit(&packet);
        mpp_buffer_put(pkt_buf);
        
        if (!frame_out) return false;
        
        RK_U32 err_info = mpp_frame_get_errinfo(frame_out);
        if (err_info) { mpp_frame_deinit(&frame_out); return false; }
        
        MppBuffer buffer = mpp_frame_get_buffer(frame_out);
        if (!buffer) { mpp_frame_deinit(&frame_out); return false; }
        
        *nv12_out = (uint8_t*)mpp_buffer_get_ptr(buffer);
        *width_out = mpp_frame_get_width(frame_out);
        *height_out = mpp_frame_get_height(frame_out);
        *stride_out = mpp_frame_get_hor_stride(frame_out);
        
        if (last_frame_) mpp_frame_deinit(&last_frame_);
        last_frame_ = frame_out;
        
        return true;
    }
    
    void close() {
        if (last_frame_) { mpp_frame_deinit(&last_frame_); last_frame_ = nullptr; }
        if (frm_buf_) { mpp_buffer_put(frm_buf_); frm_buf_ = nullptr; }
        if (frm_grp_) { mpp_buffer_group_put(frm_grp_); frm_grp_ = nullptr; }
        if (pkt_grp_) { mpp_buffer_group_put(pkt_grp_); pkt_grp_ = nullptr; }
        if (frame_) { mpp_frame_deinit(&frame_); frame_ = nullptr; }
        if (ctx_) { mpp_destroy(ctx_); ctx_ = nullptr; }
    }

private:
    MppCtx ctx_;
    MppApi* api_;
    MppFrame frame_;
    MppFrame last_frame_;
    MppBuffer frm_buf_;
    MppBufferGroup frm_grp_;
    MppBufferGroup pkt_grp_;
    uint32_t width_, height_;
};

// ============================================================================
// RKNN 推理类（支持 NPU 核心绑定）
// ============================================================================
class RKNNInference {
public:
    RKNNInference(int id) : id_(id), ctx_(0), initialized_(false) {}
    
    ~RKNNInference() {
        if (ctx_) rknn_destroy(ctx_);
    }
    
    bool init(const std::string& model_path, uint32_t input_w, uint32_t input_h, 
              rknn_core_mask core_mask = RKNN_NPU_CORE_AUTO) {
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
            return false;
        }
        
        if (core_mask != RKNN_NPU_CORE_AUTO) {
            ret = rknn_set_core_mask(ctx_, core_mask);
            if (ret == RKNN_SUCC) {
                printf("[RKNN#%d] Core mask: 0x%x\n", id_, core_mask);
            }
        }
        
        rknn_input_output_num io_num;
        rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
        
        input_attrs_.resize(io_num.n_input);
        rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, input_attrs_.data(), sizeof(rknn_tensor_attr) * io_num.n_input);
        
        output_attrs_.resize(io_num.n_output);
        rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, output_attrs_.data(), sizeof(rknn_tensor_attr) * io_num.n_output);
        
        input_w_ = input_w;
        input_h_ = input_h;
        initialized_ = true;
        
        int num_classes = (output_attrs_[0].dims[1] > 100) ? 80 : 2;
        printf("[RKNN#%d] Model loaded, classes: %d\n", id_, num_classes);
        
        return true;
    }
    
    bool inference(uint8_t* bgr_data, std::vector<Detection>& detections, float conf_thresh = 0.5f) {
        if (!initialized_) return false;
        
        rknn_input inputs[1];
        memset(inputs, 0, sizeof(inputs));
        inputs[0].index = 0;
        inputs[0].type = RKNN_TENSOR_UINT8;
        inputs[0].size = input_w_ * input_h_ * 3;
        inputs[0].fmt = RKNN_TENSOR_NHWC;
        inputs[0].buf = bgr_data;
        
        if (rknn_inputs_set(ctx_, 1, inputs) != RKNN_SUCC) return false;
        if (rknn_run(ctx_, NULL) != RKNN_SUCC) return false;
        
        rknn_output outputs[3];
        memset(outputs, 0, sizeof(outputs));
        for (int i = 0; i < 3; i++) outputs[i].want_float = 1;
        
        if (rknn_outputs_get(ctx_, 3, outputs, NULL) != RKNN_SUCC) return false;
        
        int num_classes = (output_attrs_[0].dims[1] > 100) ? 80 : 2;
        int prop_size = 5 + num_classes;
        int strides[3] = {8, 16, 32};
        int grids[3] = {80, 40, 20};
        
        for (int layer = 0; layer < 3; layer++) {
            float* data = (float*)outputs[layer].buf;
            int stride = strides[layer];
            int grid = grids[layer];
            int grid_len = grid * grid;
            
            for (int a = 0; a < 3; a++) {
                for (int y = 0; y < grid; y++) {
                    for (int x = 0; x < grid; x++) {
                        int base = (prop_size * a) * grid_len + y * grid + x;
                        float obj = data[(prop_size * a + 4) * grid_len + y * grid + x];
                        if (obj < conf_thresh) continue;
                        
                        float bx = data[base];
                        float by = data[base + grid_len];
                        float bw = data[base + 2 * grid_len];
                        float bh = data[base + 3 * grid_len];
                        
                        float max_conf = 0;
                        int max_cls = 0;
                        for (int c = 0; c < num_classes; c++) {
                            float cconf = data[(prop_size * a + 5 + c) * grid_len + y * grid + x];
                            if (cconf > max_conf) { max_conf = cconf; max_cls = c; }
                        }
                        
                        float final_conf = obj * max_conf;
                        if (final_conf < conf_thresh) continue;
                        
                        float cx = (bx * 2 - 0.5 + x) * stride;
                        float cy = (by * 2 - 0.5 + y) * stride;
                        float w = bw * bw * 4 * ANCHORS[layer][a * 2];
                        float h = bh * bh * 4 * ANCHORS[layer][a * 2 + 1];
                        
                        Detection d;
                        d.x1 = std::max(0, (int)(cx - w/2));
                        d.y1 = std::max(0, (int)(cy - h/2));
                        d.x2 = std::min((int)input_w_, (int)(cx + w/2));
                        d.y2 = std::min((int)input_h_, (int)(cy + h/2));
                        d.confidence = final_conf;
                        d.class_id = max_cls;
                        d.class_name = COCO_CLASSES[max_cls];
                        detections.push_back(d);
                    }
                }
            }
        }
        
        rknn_outputs_release(ctx_, 3, outputs);
        return true;
    }
    
    uint32_t input_width() const { return input_w_; }
    uint32_t input_height() const { return input_h_; }

private:
    int id_;
    rknn_context ctx_;
    bool initialized_;
    std::vector<rknn_tensor_attr> input_attrs_, output_attrs_;
    uint32_t input_w_, input_h_;
};

// ============================================================================
// 视频显示控件
// ============================================================================
class VideoWidget : public QWidget {
    Q_OBJECT
public:
    VideoWidget(int id, QWidget* parent = nullptr) : QWidget(parent), id_(id), no_signal_(false) {
        setMinimumSize(320, 240);
        setStyleSheet("background-color: black;");
    }
    
    void updateFrame(const QImage& img) {
        std::lock_guard<std::mutex> lock(mutex_);
        image_ = img;
        no_signal_ = false;
        update();
    }
    
    void setNoSignal(bool no_signal) {
        no_signal_ = no_signal;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
        
        if (no_signal_) {
            painter.setPen(Qt::gray);
            QFont font("Arial", 16);
            painter.setFont(font);
            painter.drawText(rect(), Qt::AlignCenter, QString("Camera %1\n无信号").arg(id_));
        } else if (!image_.isNull()) {
            QImage scaled = image_.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            int x = (width() - scaled.width()) / 2;
            int y = (height() - scaled.height()) / 2;
            painter.drawImage(x, y, scaled);
        } else {
            painter.setPen(Qt::white);
            painter.drawText(rect(), Qt::AlignCenter, QString("Camera %1\n等待启动...").arg(id_));
        }
    }

private:
    int id_;
    QImage image_;
    bool no_signal_;
    std::mutex mutex_;
};

// ============================================================================
// 主窗口
// ============================================================================
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget* parent = nullptr) : QMainWindow(parent) {
        setWindowTitle("AIControlSkynet3588 - 多路摄像头测试 (NPU 并行推理)");
        resize(1200, 800);
        
        QWidget* central = new QWidget(this);
        setCentralWidget(central);
        
        QVBoxLayout* main_layout = new QVBoxLayout(central);
        
        // 控制面板
        QHBoxLayout* control = new QHBoxLayout();
        
        control->addWidget(new QLabel("布局:"));
        layout_combo_ = new QComboBox();
        layout_combo_->addItem("1x1", 1);
        layout_combo_->addItem("2x2", 4);
        layout_combo_->setCurrentIndex(1);
        control->addWidget(layout_combo_);
        
        control->addSpacing(20);
        
        control->addWidget(new QLabel("模型:"));
        model_edit_ = new QLineEdit("/home/topeet/RKNN-YOLOV5-BatchInference-MultiThreading/model/RK3588/yolov5s-640-640.rknn");
        model_edit_->setMinimumWidth(400);
        control->addWidget(model_edit_);
        
        control->addSpacing(20);
        
        start_btn_ = new QPushButton("▶ 启动");
        start_btn_->setStyleSheet("background-color: #4CAF50; color: white; padding: 8px 20px;");
        control->addWidget(start_btn_);
        connect(start_btn_, &QPushButton::clicked, this, &MainWindow::onStart);
        
        stop_btn_ = new QPushButton("■ 停止");
        stop_btn_->setStyleSheet("background-color: #f44336; color: white; padding: 8px 20px;");
        stop_btn_->setEnabled(false);
        control->addWidget(stop_btn_);
        connect(stop_btn_, &QPushButton::clicked, this, &MainWindow::onStop);
        
        control->addStretch();
        
        fps_label_ = new QLabel("FPS: --");
        fps_label_->setStyleSheet("font-weight: bold; font-size: 14px;");
        control->addWidget(fps_label_);
        
        main_layout->addLayout(control);
        
        // 视频显示区域
        video_container_ = new QWidget();
        video_layout_ = new QGridLayout(video_container_);
        video_layout_->setSpacing(2);
        main_layout->addWidget(video_container_, 1);
        
        updateLayout(4);
        
        // FPS 定时器
        fps_timer_ = new QTimer(this);
        connect(fps_timer_, &QTimer::timeout, this, &MainWindow::updateFps);
        
        // 显示定时器
        display_timer_ = new QTimer(this);
        connect(display_timer_, &QTimer::timeout, this, &MainWindow::updateDisplay);
        
        c_RkRgaInit();
    }
    
    ~MainWindow() {
        onStop();
    }

private slots:
    void onStart() {
        int num_cameras = layout_combo_->currentData().toInt();
        updateLayout(num_cameras);
        
        // 摄像头设备（只有两路真实摄像头）
        std::string devices[2] = {
            "/dev/video21",  // 2303 webcam
            "/dev/video23"   // 2K USB Camera
        };
        
        std::string model_path = model_edit_->text().toStdString();
        
        // 初始化摄像头和解码器
        for (int i = 0; i < 2; i++) {
            cameras_[i] = std::make_unique<V4L2Capture>(devices[i], 1920, 1080);
            if (!cameras_[i]->init()) {
                printf("[Main] Failed to init camera %d\n", i);
                return;
            }
            
            decoders_[i] = std::make_unique<MPPDecoder>();
            if (!decoders_[i]->init(1920, 1080)) {
                printf("[Main] Failed to init decoder %d\n", i);
                return;
            }
        }
        
        // 初始化 RKNN（NPU 核心绑定）
        rknn_core_mask core_masks[2] = {RKNN_NPU_CORE_0, RKNN_NPU_CORE_1};
        for (int i = 0; i < 2; i++) {
            rknn_[i] = std::make_unique<RKNNInference>(i);
            if (!rknn_[i]->init(model_path, 640, 640, core_masks[i])) {
                printf("[Main] Failed to init RKNN %d\n", i);
                return;
            }
        }
        
        // 分配缓冲区
        for (int i = 0; i < 2; i++) {
            display_buf_[i].resize(1920 * 1080 * 3);
            rknn_buf_[i].resize(640 * 640 * 3);
        }
        
        running_ = true;
        
        // 启动采集线程（串行处理两个摄像头）
        capture_thread_ = std::thread(&MainWindow::captureLoop, this);
        
        // 启动推理线程（并行）
        for (int i = 0; i < 2; i++) {
            inference_threads_[i] = std::thread(&MainWindow::inferenceLoop, this, i);
        }
        
        fps_timer_->start(1000);
        display_timer_->start(33);
        
        // 第3、4路显示"无信号"
        for (int i = 2; i < video_widgets_.size(); i++) {
            video_widgets_[i]->setNoSignal(true);
        }
        
        start_btn_->setEnabled(false);
        stop_btn_->setEnabled(true);
    }
    
    void onStop() {
        running_ = false;
        
        fps_timer_->stop();
        display_timer_->stop();
        
        // 通知推理线程
        for (int i = 0; i < 2; i++) {
            inference_cv_[i].notify_all();
        }
        
        if (capture_thread_.joinable()) capture_thread_.join();
        for (int i = 0; i < 2; i++) {
            if (inference_threads_[i].joinable()) inference_threads_[i].join();
        }
        
        for (int i = 0; i < 2; i++) {
            rknn_[i].reset();
            decoders_[i].reset();
            cameras_[i].reset();
        }
        
        for (int i = 0; i < video_widgets_.size(); i++) {
            video_widgets_[i]->setNoSignal(false);
        }
        
        start_btn_->setEnabled(true);
        stop_btn_->setEnabled(false);
    }
    
    void updateLayout(int num) {
        QLayoutItem* item;
        while ((item = video_layout_->takeAt(0)) != nullptr) {
            delete item->widget();
            delete item;
        }
        video_widgets_.clear();
        
        int cols = (num <= 1) ? 1 : 2;
        
        for (int i = 0; i < num; i++) {
            VideoWidget* w = new VideoWidget(i);
            video_widgets_.append(w);
            video_layout_->addWidget(w, i / cols, i % cols);
        }
    }
    
    void updateFps() {
        int total_fps = 0;
        for (int i = 0; i < 2; i++) {
            total_fps += fps_count_[i].exchange(0);
        }
        fps_label_->setText(QString("FPS: %1").arg(total_fps));
    }
    
    void updateDisplay() {
        for (int i = 0; i < 2 && i < video_widgets_.size(); i++) {
            std::lock_guard<std::mutex> lock(frame_mutex_[i]);
            if (!frames_[i].image.isNull()) {
                video_widgets_[i]->updateFrame(frames_[i].image);
            }
        }
    }

private:
    // 采集线程（串行处理两个摄像头，避免 MPP/RGA 线程安全问题）
    void captureLoop() {
        printf("[Capture] Thread started\n");
        
        std::vector<uint8_t> mjpeg_data[2];
        
        while (running_) {
            for (int i = 0; i < 2; i++) {
                if (!cameras_[i] || !cameras_[i]->read_frame(mjpeg_data[i])) continue;
                
                uint8_t* nv12_data = nullptr;
                uint32_t width = 0, height = 0, stride = 0;
                
                if (!decoders_[i]->decode(mjpeg_data[i].data(), mjpeg_data[i].size(),
                                          &nv12_data, &width, &height, &stride)) continue;
                
                if (nv12_data) {
                    // RGA 转换到 RGB (显示)
                    rga_info_t src_info = {}, dst_info = {};
                    src_info.fd = -1;
                    src_info.virAddr = nv12_data;
                    src_info.mmuFlag = 1;
                    rga_set_rect(&src_info.rect, 0, 0, width, height, stride, height, RK_FORMAT_YCbCr_420_SP);
                    
                    dst_info.fd = -1;
                    dst_info.virAddr = display_buf_[i].data();
                    dst_info.mmuFlag = 1;
                    rga_set_rect(&dst_info.rect, 0, 0, width, height, width, height, RK_FORMAT_RGB_888);
                    
                    if (c_RkRgaBlit(&src_info, &dst_info, nullptr) == 0) {
                        // RGA 转换到 BGR (RKNN)
                        rga_info_t rknn_dst = {};
                        rknn_dst.fd = -1;
                        rknn_dst.virAddr = rknn_buf_[i].data();
                        rknn_dst.mmuFlag = 1;
                        rga_set_rect(&rknn_dst.rect, 0, 0, 640, 640, 640, 640, RK_FORMAT_BGR_888);
                        
                        if (c_RkRgaBlit(&src_info, &rknn_dst, nullptr) == 0) {
                            // 推送到推理队列
                            {
                                std::lock_guard<std::mutex> lock(inference_mutex_[i]);
                                inference_data_[i] = rknn_buf_[i];
                                inference_ready_[i] = true;
                            }
                            inference_cv_[i].notify_one();
                            
                            // 保存显示数据
                            {
                                std::lock_guard<std::mutex> lock(frame_mutex_[i]);
                                frames_[i].image = QImage(display_buf_[i].data(), width, height, 
                                                          width * 3, QImage::Format_RGB888).copy();
                            }
                        }
                    }
                }
            }
        }
        
        printf("[Capture] Thread stopped\n");
    }
    
    // 推理线程（并行，利用 NPU 多核）
    void inferenceLoop(int id) {
        printf("[Inference#%d] Thread started\n", id);
        
        while (running_) {
            std::vector<uint8_t> data;
            {
                std::unique_lock<std::mutex> lock(inference_mutex_[id]);
                inference_cv_[id].wait(lock, [&] { return inference_ready_[id] || !running_; });
                
                if (!running_) break;
                
                if (inference_ready_[id]) {
                    data = inference_data_[id];
                    inference_ready_[id] = false;
                }
            }
            
            if (!data.empty() && rknn_[id]) {
                std::vector<Detection> detections;
                if (rknn_[id]->inference(data.data(), detections, 0.5f)) {
                    // 绘制检测框
                    std::lock_guard<std::mutex> lock(frame_mutex_[id]);
                    if (!frames_[id].image.isNull()) {
                        QPainter painter(&frames_[id].image);
                        painter.setPen(QPen(Qt::red, 2));
                        QFont font("Arial", 10);
                        painter.setFont(font);
                        
                        float sx = frames_[id].image.width() / 640.0f;
                        float sy = frames_[id].image.height() / 640.0f;
                        
                        for (const auto& d : detections) {
                            int x1 = d.x1 * sx;
                            int y1 = d.y1 * sy;
                            int x2 = d.x2 * sx;
                            int y2 = d.y2 * sy;
                            
                            painter.drawRect(x1, y1, x2 - x1, y2 - y1);
                            QString label = QString("%1: %2%").arg(d.class_name)
                                .arg(std::min(100, (int)(d.confidence * 100)));
                            painter.drawText(x1, y1 - 5, label);
                        }
                    }
                    
                    fps_count_[id]++;
                }
            }
        }
        
        printf("[Inference#%d] Thread stopped\n", id);
    }

private:
    QComboBox* layout_combo_;
    QLineEdit* model_edit_;
    QPushButton* start_btn_;
    QPushButton* stop_btn_;
    QLabel* fps_label_;
    QWidget* video_container_;
    QGridLayout* video_layout_;
    QList<VideoWidget*> video_widgets_;
    QTimer* fps_timer_;
    QTimer* display_timer_;
    
    std::atomic<bool> running_{false};
    
    // 摄像头和解码器
    std::unique_ptr<V4L2Capture> cameras_[2];
    std::unique_ptr<MPPDecoder> decoders_[2];
    
    // RKNN 推理
    std::unique_ptr<RKNNInference> rknn_[2];
    
    // 缓冲区
    std::vector<uint8_t> display_buf_[2];
    std::vector<uint8_t> rknn_buf_[2];
    
    // 线程
    std::thread capture_thread_;
    std::thread inference_threads_[2];
    
    // 帧数据
    FrameData frames_[2];
    std::mutex frame_mutex_[2];
    std::atomic<int> fps_count_[2]{0, 0};
    
    // 推理队列
    std::vector<uint8_t> inference_data_[2];
    std::mutex inference_mutex_[2];
    std::condition_variable inference_cv_[2];
    bool inference_ready_[2] = {false, false};
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    MainWindow win;
    win.show();
    return app.exec();
}

#include "multi_camera_test.moc"
