/**
 * @file multi_rtsp_test.cpp
 * @brief 多路 RTSP 拉流解码 + RKNN 推理测试程序
 * 
 * 基于 stress_test.cpp 优化策略：
 * - NPU 多核并行：每路绑定独立 NPU 核心
 * - 独立管道架构：每路完全独立的解码/推理管道
 * 
 * 使用方法：
 *   ./multi_rtsp_test
 * 
 * 界面操作：
 *   1. 添加 RTSP 流地址（最多 4 路）
 *   2. 点击"启动"
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
#include <QComboBox>
#include <QSpinBox>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <cstdio>
#include <mutex>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

// RGA
#include <rga/RgaApi.h>
#include <rga/drmrga.h>

// RKNN
#include <rknn_api.h>

// COCO 80 类别
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

static const int ANCHORS[3][6] = {
    {10, 13, 16, 30, 33, 23},
    {30, 61, 62, 45, 59, 119},
    {116, 90, 156, 198, 373, 326}
};

struct Detection {
    int x1, y1, x2, y2;
    float confidence;
    int class_id;
    const char* class_name;
};

// ============================================================================
// RKNN 推理类（参考 stress_test.cpp）
// ============================================================================
class RKNNInference {
public:
    RKNNInference(int id) : id_(id), ctx_(0), initialized_(false) {}
    ~RKNNInference() { if (ctx_) rknn_destroy(ctx_); }
    
    bool init(const std::string& model_path, uint32_t input_w, uint32_t input_h, 
              rknn_core_mask core_mask = RKNN_NPU_CORE_AUTO) {
        FILE* fp = fopen(model_path.c_str(), "rb");
        if (!fp) { printf("[RKNN#%d] Failed to open model: %s\n", id_, model_path.c_str()); return false; }
        
        fseek(fp, 0, SEEK_END);
        size_t model_size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        
        std::vector<char> model_data(model_size);
        fread(model_data.data(), 1, model_size, fp);
        fclose(fp);
        
        int ret = rknn_init(&ctx_, model_data.data(), model_size, 0, NULL);
        if (ret != RKNN_SUCC) {
            printf("[RKNN#%d] rknn_init failed: %d\n", id_, ret);
            return false;
        }
        
        // 设置 NPU 核心掩码（多核并行关键）
        if (core_mask != RKNN_NPU_CORE_AUTO) {
            ret = rknn_set_core_mask(ctx_, core_mask);
            if (ret == RKNN_SUCC) {
                printf("[RKNN#%d] Core mask: 0x%x\n", id_, core_mask);
            }
        }
        
        rknn_input_output_num io_num;
        rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
        
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

private:
    int id_;
    rknn_context ctx_;
    bool initialized_;
    std::vector<rknn_tensor_attr> output_attrs_;
    uint32_t input_w_, input_h_;
};

// ============================================================================
// 单路 RTSP 通道（完全独立管道）
// ============================================================================
class RTSPChannel {
public:
    RTSPChannel(int id) : id_(id), running_(false), fmt_ctx_(nullptr), codec_ctx_(nullptr) {}
    ~RTSPChannel() { stop(); }
    
    bool init(const std::string& rtsp_url, const std::string& model_path, rknn_core_mask core_mask) {
        rtsp_url_ = rtsp_url;
        
        // 初始化 RKNN
        rknn_ = std::make_unique<RKNNInference>(id_);
        if (!rknn_->init(model_path, 640, 640, core_mask)) {
            printf("[Channel#%d] Failed to init RKNN\n", id_);
            return false;
        }
        
        // 分配缓冲区
        rgb_buffer_.resize(640 * 640 * 3);
        rknn_buffer_.resize(640 * 640 * 3);
        
        printf("[Channel#%d] Initialized, URL: %s\n", id_, rtsp_url.c_str());
        return true;
    }
    
    void start() {
        running_ = true;
        thread_ = std::thread(&RTSPChannel::decodeLoop, this);
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
    int getDetections() { return detection_count_.exchange(0); }

private:
    bool initFFmpeg() {
        fmt_ctx_ = avformat_alloc_context();
        AVDictionary* options = nullptr;
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "stimeout", "5000000", 0);
        av_dict_set(&options, "max_delay", "500000", 0);
        
        int ret = avformat_open_input(&fmt_ctx_, rtsp_url_.c_str(), nullptr, &options);
        av_dict_free(&options);
        
        if (ret != 0) {
            printf("[Channel#%d] Failed to open RTSP: %d\n", id_, ret);
            return false;
        }
        
        ret = avformat_find_stream_info(fmt_ctx_, nullptr);
        if (ret < 0) {
            printf("[Channel#%d] Failed to find stream info\n", id_);
            return false;
        }
        
        // 查找视频流
        video_stream_idx_ = -1;
        for (unsigned int i = 0; i < fmt_ctx_->nb_streams; i++) {
            if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                video_stream_idx_ = i;
                break;
            }
        }
        
        if (video_stream_idx_ < 0) {
            printf("[Channel#%d] No video stream\n", id_);
            return false;
        }
        
        AVCodecParameters* codecpar = fmt_ctx_->streams[video_stream_idx_]->codecpar;
        printf("[Channel#%d] Video: %dx%d\n", id_, codecpar->width, codecpar->height);
        
        // 初始化解码器
        const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
        codec_ctx_ = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(codec_ctx_, codecpar);
        avcodec_open2(codec_ctx_, codec, nullptr);
        
        video_width_ = codec_ctx_->width;
        video_height_ = codec_ctx_->height;
        
        // 初始化 SwsContext
        sws_ctx_ = sws_getContext(
            video_width_, video_height_, codec_ctx_->pix_fmt,
            video_width_, video_height_, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        
        // 分配帧
        frame_ = av_frame_alloc();
        frame_rgb_ = av_frame_alloc();
        
        int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, video_width_, video_height_, 1);
        rgb_buffer_.resize(num_bytes);
        av_image_fill_arrays(frame_rgb_->data, frame_rgb_->linesize, rgb_buffer_.data(), 
            AV_PIX_FMT_RGB24, video_width_, video_height_, 1);
        
        packet_ = av_packet_alloc();
        
        return true;
    }
    
    void cleanupFFmpeg() {
        if (packet_) { av_packet_free(&packet_); packet_ = nullptr; }
        if (frame_rgb_) { av_frame_free(&frame_rgb_); frame_rgb_ = nullptr; }
        if (frame_) { av_frame_free(&frame_); frame_ = nullptr; }
        if (sws_ctx_) { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }
        if (codec_ctx_) { avcodec_free_context(&codec_ctx_); codec_ctx_ = nullptr; }
        if (fmt_ctx_) { avformat_close_input(&fmt_ctx_); fmt_ctx_ = nullptr; }
    }
    
    void decodeLoop() {
        printf("[Channel#%d] Thread started\n", id_);
        
        while (running_) {
            if (!initFFmpeg()) {
                printf("[Channel#%d] FFmpeg init failed, retry in 3s...\n", id_);
                for (int i = 0; i < 30 && running_; i++) usleep(100000);  // 等待3秒
                continue;
            }
            
            printf("[Channel#%d] Decoding...\n", id_);
            
            while (running_) {
                int ret = av_read_frame(fmt_ctx_, packet_);
                if (ret < 0) {
                    printf("[Channel#%d] Stream ended/error: %d, reconnecting...\n", id_, ret);
                    cleanupFFmpeg();
                    break;  // 跳出内层循环，重新初始化
                }
                
                if (packet_->stream_index != video_stream_idx_) {
                    av_packet_unref(packet_);
                    continue;
                }
                
                ret = avcodec_send_packet(codec_ctx_, packet_);
                av_packet_unref(packet_);
                
                if (ret < 0) continue;
                
                while (avcodec_receive_frame(codec_ctx_, frame_) == 0) {
                    // RGB 转换
                    sws_scale(sws_ctx_, frame_->data, frame_->linesize, 0, video_height_,
                        frame_rgb_->data, frame_rgb_->linesize);
                    
                    // RGA 转换到 BGR (RKNN 输入)
                    rga_info_t src_info = {}, dst_info = {};
                    src_info.fd = -1;
                    src_info.virAddr = frame_rgb_->data[0];
                    src_info.mmuFlag = 1;
                    rga_set_rect(&src_info.rect, 0, 0, video_width_, video_height_, 
                        frame_rgb_->linesize[0], video_height_, RK_FORMAT_RGB_888);
                    
                    dst_info.fd = -1;
                    dst_info.virAddr = rknn_buffer_.data();
                    dst_info.mmuFlag = 1;
                    rga_set_rect(&dst_info.rect, 0, 0, 640, 640, 640, 640, RK_FORMAT_BGR_888);
                    
                    if (c_RkRgaBlit(&src_info, &dst_info, nullptr) == 0) {
                        // RKNN 推理
                        std::vector<Detection> detections;
                        if (rknn_->inference(rknn_buffer_.data(), detections, 0.5f)) {
                            // 创建图像
                            QImage image(frame_rgb_->data[0], video_width_, video_height_, 
                                frame_rgb_->linesize[0], QImage::Format_RGB888);
                            
                            // 绘制检测框
                            QPainter painter(&image);
                            painter.setPen(QPen(Qt::green, 2));
                            QFont font("Arial", 10, QFont::Bold);
                            painter.setFont(font);
                            
                            float sx = image.width() / 640.0f;
                            float sy = image.height() / 640.0f;
                            
                            for (const auto& d : detections) {
                                int x1 = d.x1 * sx;
                                int y1 = d.y1 * sy;
                                int x2 = d.x2 * sx;
                                int y2 = d.y2 * sy;
                                
                                painter.drawRect(x1, y1, x2 - x1, y2 - y1);
                                QString label = QString("%1:%2%")
                                    .arg(d.class_name)
                                    .arg(std::min(100, (int)(d.confidence * 100)));
                                painter.drawText(x1, y1 - 3, label);
                            }
                            
                            {
                                std::lock_guard<std::mutex> lock(frame_mutex_);
                                current_frame_ = image.copy();
                            }
                            
                            detection_count_ += detections.size();
                        }
                    }
                    
                    fps_count_++;
                }
            }
        }
        
        cleanupFFmpeg();
        printf("[Channel#%d] Thread stopped\n", id_);
    }

private:
    int id_;
    std::string rtsp_url_;
    std::atomic<bool> running_;
    std::thread thread_;
    
    std::unique_ptr<RKNNInference> rknn_;
    std::vector<uint8_t> rgb_buffer_;
    std::vector<uint8_t> rknn_buffer_;
    
    QImage current_frame_;
    std::mutex frame_mutex_;
    std::atomic<int> fps_count_{0};
    std::atomic<int> detection_count_{0};
    
    // FFmpeg
    AVFormatContext* fmt_ctx_;
    AVCodecContext* codec_ctx_;
    SwsContext* sws_ctx_;
    AVFrame* frame_ = nullptr;
    AVFrame* frame_rgb_ = nullptr;
    AVPacket* packet_ = nullptr;
    int video_stream_idx_ = -1;
    int video_width_ = 0;
    int video_height_ = 0;
};

// ============================================================================
// 视频显示控件
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
            painter.drawText(rect(), Qt::AlignCenter, QString("通道 %1\n等待视频...").arg(id_));
        }
    }

private:
    int id_;
    QImage image_;
    std::mutex mutex_;
};

// ============================================================================
// 主窗口
// ============================================================================
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget* parent = nullptr) : QMainWindow(parent) {
        setWindowTitle("AIControlSkynet3588 - 多路 RTSP 推理测试");
        resize(1400, 900);
        
        QWidget* central = new QWidget(this);
        setCentralWidget(central);
        
        QVBoxLayout* main_layout = new QVBoxLayout(central);
        
        // 控制面板
        QHBoxLayout* control = new QHBoxLayout();
        
        control->addWidget(new QLabel("路数:"));
        channel_spin_ = new QSpinBox();
        channel_spin_->setRange(1, 4);
        channel_spin_->setValue(1);
        control->addWidget(channel_spin_);
        
        control->addSpacing(10);
        
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
        
        fps_label_ = new QLabel("总 FPS: -- | 检测: --");
        fps_label_->setStyleSheet("font-weight: bold; font-size: 14px;");
        control->addWidget(fps_label_);
        
        main_layout->addLayout(control);
        
        // RTSP 地址输入
        QHBoxLayout* url_layout = new QHBoxLayout();
        url_layout->addWidget(new QLabel("RTSP 地址:"));
        
        for (int i = 0; i < 4; i++) {
            rtsp_edits_[i] = new QLineEdit();
            rtsp_edits_[i]->setPlaceholderText(QString("通道 %1 RTSP 地址").arg(i));
            rtsp_edits_[i]->setText(QString("rtsp://192.168.137.251:8554/cow%1").arg(i));
            url_layout->addWidget(rtsp_edits_[i]);
        }
        
        main_layout->addLayout(url_layout);
        
        // 模型路径
        QHBoxLayout* model_layout = new QHBoxLayout();
        model_layout->addWidget(new QLabel("模型:"));
        model_edit_ = new QLineEdit("/home/topeet/rknpu2/examples/rknn_yolov5_demo/install/rknn_yolov5_demo_Linux/model/RK3588/yolov5s-640-640_rk3588.rknn");
        model_layout->addWidget(model_edit_);
        main_layout->addLayout(model_layout);
        
        // 视频显示区域
        video_container_ = new QWidget();
        video_layout_ = new QGridLayout(video_container_);
        video_layout_->setSpacing(2);
        main_layout->addWidget(video_container_, 1);
        
        for (int i = 0; i < 4; i++) {
            video_widgets_[i] = new VideoWidget(i);
            video_layout_->addWidget(video_widgets_[i], i / 2, i % 2);
        }
        
        // FPS 定时器
        fps_timer_ = new QTimer(this);
        connect(fps_timer_, &QTimer::timeout, this, &MainWindow::updateFps);
        
        display_timer_ = new QTimer(this);
        connect(display_timer_, &QTimer::timeout, this, &MainWindow::updateDisplay);
        
        // 初始化
        c_RkRgaInit();
        avformat_network_init();
    }
    
    ~MainWindow() { onStop(); }

private slots:
    void onStart() {
        int num_channels = channel_spin_->value();
        std::string model_path = model_edit_->text().toStdString();
        
        printf("\n=== 启动 %d 路 RTSP 解码 ===\n", num_channels);
        
        // NPU 核心分配策略（参考 stress_test.cpp）
        rknn_core_mask core_masks[4] = {
            RKNN_NPU_CORE_0,      // 核心 0 (2 TOPS)
            RKNN_NPU_CORE_1,      // 核心 1 (2 TOPS)
            RKNN_NPU_CORE_2,      // 核心 2 (2 TOPS)
            RKNN_NPU_CORE_0       // 循环使用核心 0
        };
        
        int success_count = 0;
        for (int i = 0; i < num_channels; i++) {
            std::string rtsp_url = rtsp_edits_[i]->text().toStdString();
            printf("[Main] Channel %d: %s\n", i, rtsp_url.c_str());
            
            channels_[i] = std::make_unique<RTSPChannel>(i);
            if (!channels_[i]->init(rtsp_url, model_path, core_masks[i])) {
                printf("[Main] Failed to init channel %d\n", i);
                continue;
            }
            channels_[i]->start();
            success_count++;
        }
        
        printf("[Main] 成功启动 %d/%d 路\n\n", success_count, num_channels);
        
        fps_timer_->start(1000);
        display_timer_->start(33);
        
        start_btn_->setEnabled(false);
        stop_btn_->setEnabled(true);
    }
    
    void onStop() {
        fps_timer_->stop();
        display_timer_->stop();
        
        for (int i = 0; i < 4; i++) {
            if (channels_[i]) {
                channels_[i]->stop();
                channels_[i].reset();
            }
        }
        
        start_btn_->setEnabled(true);
        stop_btn_->setEnabled(false);
    }
    
    void updateFps() {
        int total_fps = 0;
        int total_det = 0;
        
        for (int i = 0; i < 4; i++) {
            if (channels_[i]) {
                total_fps += channels_[i]->getFps();
                total_det += channels_[i]->getDetections();
            }
        }
        
        fps_label_->setText(QString("总 FPS: %1 | 检测: %2").arg(total_fps).arg(total_det));
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

private:
    QSpinBox* channel_spin_;
    QPushButton* start_btn_;
    QPushButton* stop_btn_;
    QLabel* fps_label_;
    QLineEdit* rtsp_edits_[4];
    QLineEdit* model_edit_;
    
    VideoWidget* video_widgets_[4];
    QWidget* video_container_;
    QGridLayout* video_layout_;
    QTimer* fps_timer_;
    QTimer* display_timer_;
    
    std::unique_ptr<RTSPChannel> channels_[4];
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    MainWindow win;
    win.show();
    return app.exec();
}

#include "multi_rtsp_test.moc"
