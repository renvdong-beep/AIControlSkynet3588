/**
 * @file rtsp_video_test.cpp
 * @brief RTSP + MPP 硬解码 + RKNN 推理（CPU 占用低）
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

#define OBJ_CLASS_NUM 2
#define MODEL_INPUT_SIZE 640
static const char* CLASS_NAMES[OBJ_CLASS_NUM] = {"cow", "person"};

typedef struct {
    int left, right, top, bottom;
    float prop;
    int class_id;
} detect_result_t;

class RTSPVideoTestWindow : public QMainWindow {
    Q_OBJECT
public:
    RTSPVideoTestWindow(QWidget *parent = nullptr) : QMainWindow(parent) {
        setWindowTitle("RTSP + MPP Decode + RKNN Test");
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

        connect(startBtn_, &QPushButton::clicked, this, &RTSPVideoTestWindow::onStart);
        connect(stopBtn_, &QPushButton::clicked, this, &RTSPVideoTestWindow::onStop);
        running_ = false; rknn_ctx_ = 0;
    }
    ~RTSPVideoTestWindow() { onStop(); }

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
        thread_ = std::thread(&RTSPVideoTestWindow::rtspMppLoop, this);
        displayTimer_ = new QTimer(this);
        connect(displayTimer_, &QTimer::timeout, this, &RTSPVideoTestWindow::updateDisplay);
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
    void rtspMppLoop() {
        // Placeholder: RTSP + MPP H.264 decode + RGA + RKNN
        // Full implementation in multi_rtsp_mpp_test.cpp
        printf("RTSP+MPP loop - placeholder. Use multi_rtsp_mpp_test for full implementation.\n");
        int count = 0;
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            count++;
        }
        printf("RTSP+MPP loop ended. Count: %d\n", count);
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
    RTSPVideoTestWindow win;
    win.show();
    return app.exec();
}
#include "rtsp_video_test.moc"
