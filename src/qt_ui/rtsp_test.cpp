/**
 * @file rtsp_test.cpp
 * @brief 单路 RTSP + FFmpeg 软解码 + RKNN 推理
 */

#include <QApplication>
#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QTimer>
#include <QPainter>
#include <QImage>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include <rknn_api.h>

#define OBJ_CLASS_NUM 2
#define MODEL_INPUT_SIZE 640
static const char* CLASS_NAMES[OBJ_CLASS_NUM] = {"cow", "person"};

typedef struct {
    int left, right, top, bottom;
    float prop;
    int class_id;
} detect_result_t;

class RTSPTestWindow : public QMainWindow {
    Q_OBJECT
public:
    RTSPTestWindow(QWidget *parent = nullptr) : QMainWindow(parent) {
        setWindowTitle("RTSP + RKNN Test (FFmpeg Soft Decode)");
        resize(800, 700);
        QWidget *central = new QWidget(this);
        setCentralWidget(central);
        QVBoxLayout *layout = new QVBoxLayout(central);

        QHBoxLayout *uLayout = new QHBoxLayout();
        uLayout->addWidget(new QLabel("RTSP:"));
        urlEdit_ = new QLineEdit("rtsp://192.168.137.251:8554/cow0");
        uLayout->addWidget(urlEdit_);
        layout->addLayout(uLayout);

        QHBoxLayout *pLayout = new QHBoxLayout();
        pLayout->addWidget(new QLabel("Transport:"));
        protoCombo_ = new QComboBox();
        protoCombo_->addItem("TCP"); protoCombo_->addItem("UDP");
        pLayout->addWidget(protoCombo_);
        layout->addLayout(pLayout);

        QHBoxLayout *mLayout = new QHBoxLayout();
        mLayout->addWidget(new QLabel("Model:"));
        modelEdit_ = new QLineEdit("/home/topeet/rknpu2/examples/rknn_yolov5_demo/model/RK3588/yolov5s-640-640.rknn");
        mLayout->addWidget(modelEdit_);
        layout->addLayout(mLayout);

        QHBoxLayout *btnLayout = new QHBoxLayout();
        startBtn_ = new QPushButton("Start"); stopBtn_ = new QPushButton("Stop");
        stopBtn_->setEnabled(false);
        btnLayout->addWidget(startBtn_); btnLayout->addWidget(stopBtn_);
        layout->addLayout(btnLayout);

        statusLabel_ = new QLabel("Ready"); layout->addWidget(statusLabel_);
        imageLabel_ = new QLabel(); imageLabel_->setAlignment(Qt::AlignCenter);
        imageLabel_->setMinimumSize(640, 480); layout->addWidget(imageLabel_);

        connect(startBtn_, &QPushButton::clicked, this, &RTSPTestWindow::onStart);
        connect(stopBtn_, &QPushButton::clicked, this, &RTSPTestWindow::onStop);
        running_ = false; rknn_ctx_ = 0;
    }
    ~RTSPTestWindow() { onStop(); }

private slots:
    void onStart() {
        if (running_) return;
        FILE *fp = fopen(modelEdit_->text().toUtf8().constData(), "rb");
        if (!fp) return;
        fseek(fp, 0, SEEK_END); size_t sz = ftell(fp); fseek(fp, 0, SEEK_SET);
        std::vector<char> data(sz); fread(data.data(), 1, sz, fp); fclose(fp);
        if (rknn_init(&rknn_ctx_, data.data(), sz, 0, NULL) < 0) return;
        running_ = true;
        startBtn_->setEnabled(false); stopBtn_->setEnabled(true);
        statusLabel_->setText("Connecting...");
        thread_ = std::thread(&RTSPTestWindow::rtspLoop, this);
        displayTimer_ = new QTimer(this);
        connect(displayTimer_, &QTimer::timeout, this, &RTSPTestWindow::updateDisplay);
        displayTimer_->start(40);
    }
    void onStop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        if (displayTimer_) { displayTimer_->stop(); delete displayTimer_; displayTimer_ = nullptr; }
        if (rknn_ctx_) { rknn_destroy(rknn_ctx_); rknn_ctx_ = 0; }
        startBtn_->setEnabled(true); stopBtn_->setEnabled(false);
        statusLabel_->setText("Stopped");
    }
    void updateDisplay() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!displayImage_.isNull())
            imageLabel_->setPixmap(QPixmap::fromImage(displayImage_).scaled(imageLabel_->size(), Qt::KeepAspectRatio));
    }

private:
    void rtspLoop() {
        AVFormatContext *fmt_ctx = NULL;
        AVDictionary *opts = NULL;
        av_dict_set(&opts, "rtsp_transport", protoCombo_->currentIndex() == 0 ? "tcp" : "udp", 0);
        av_dict_set(&opts, "stimeout", "5000000", 0);
        av_dict_set(&opts, "fflags", "nobuffer", 0);

        if (avformat_open_input(&fmt_ctx, urlEdit_->text().toUtf8().constData(), NULL, &opts) < 0) {
            av_dict_free(&opts); return;
        }
        av_dict_free(&opts);
        avformat_find_stream_info(fmt_ctx, NULL);
        int vi = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        if (vi < 0) { avformat_close_input(&fmt_ctx); return; }

        const AVCodec *codec = avcodec_find_decoder(fmt_ctx->streams[vi]->codecpar->codec_id);
        AVCodecContext *cc = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(cc, fmt_ctx->streams[vi]->codecpar);
        avcodec_open2(cc, codec, NULL);

        SwsContext *sws = sws_getContext(cc->width, cc->height, cc->pix_fmt,
            MODEL_INPUT_SIZE, MODEL_INPUT_SIZE, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);

        uint8_t *rgb = (uint8_t*)malloc(MODEL_INPUT_SIZE * MODEL_INPUT_SIZE * 3);
        int stride = MODEL_INPUT_SIZE * 3;
        uint8_t *data[1] = { rgb }; int linesize[1] = { stride };

        AVFrame *frame = av_frame_alloc();
        AVPacket *pkt = av_packet_alloc();
        int count = 0;

        while (running_ && av_read_frame(fmt_ctx, pkt) >= 0) {
            if (pkt->stream_index != vi) { av_packet_unref(pkt); continue; }
            if (avcodec_send_packet(cc, pkt) == 0) {
                while (avcodec_receive_frame(cc, frame) == 0) {
                    sws_scale(sws, frame->data, frame->linesize, 0, frame->height, data, linesize);
                    rknn_input input = {};
                    input.index = 0; input.type = RKNN_TENSOR_UINT8;
                    input.size = MODEL_INPUT_SIZE * MODEL_INPUT_SIZE * 3; input.buf = rgb;
                    if (rknn_inputs_set(rknn_ctx_, 1, &input) == RKNN_SUCC && rknn_run(rknn_ctx_, NULL) == RKNN_SUCC) {
                        rknn_output outputs[3] = {};
                        for (int i = 0; i < 3; i++) outputs[i].want_float = 1;
                        if (rknn_outputs_get(rknn_ctx_, 3, outputs, NULL) == RKNN_SUCC) {
                            QImage qimg(rgb, MODEL_INPUT_SIZE, MODEL_INPUT_SIZE, MODEL_INPUT_SIZE * 3, QImage::Format_RGB888);
                            { std::lock_guard<std::mutex> lk(mutex_); displayImage_ = qimg.copy(); }
                            rknn_outputs_release(rknn_ctx_, 3, outputs);
                        }
                    }
                    count++;
                }
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt); av_frame_free(&frame);
        sws_freeContext(sws); avcodec_free_context(&cc); avformat_close_input(&fmt_ctx);
        free(rgb);
    }

private:
    QLineEdit *urlEdit_, *modelEdit_;
    QComboBox *protoCombo_;
    QPushButton *startBtn_, *stopBtn_;
    QLabel *statusLabel_, *imageLabel_;
    QTimer *displayTimer_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_;
    std::mutex mutex_;
    QImage displayImage_;
    rknn_context rknn_ctx_;
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    RTSPTestWindow win;
    win.show();
    return app.exec();
}
#include "rtsp_test.moc"
