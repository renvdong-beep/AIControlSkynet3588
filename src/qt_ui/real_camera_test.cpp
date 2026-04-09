/**
 * @file real_camera_test.cpp
 * @brief 单路真实摄像头 + RKNN 推理测试程序
 * V4L2 采集 → MPP MJPEG 硬解码 → RGA 缩放 → RKNN YOLOv5 推理
 */

#include <QApplication>
#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QTimer>
#include <QPainter>
#include <QImage>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <unistd.h>

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>

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

class RealCameraTestWindow : public QMainWindow {
    Q_OBJECT
public:
    RealCameraTestWindow(QWidget *parent = nullptr) : QMainWindow(parent) {
        setWindowTitle("Real Camera + RKNN Test");
        resize(800, 700);
        QWidget *central = new QWidget(this);
        setCentralWidget(central);
        QVBoxLayout *layout = new QVBoxLayout(central);

        QHBoxLayout *devLayout = new QHBoxLayout();
        devLayout->addWidget(new QLabel("Camera:"));
        deviceCombo_ = new QComboBox();
        for (int i = 0; i < 10; i++) {
            QString dev = QString("/dev/video%1").arg(i);
            if (access(dev.toUtf8().constData(), F_OK) == 0) deviceCombo_->addItem(dev);
        }
        devLayout->addWidget(deviceCombo_);
        layout->addLayout(devLayout);

        QHBoxLayout *modelLayout = new QHBoxLayout();
        modelLayout->addWidget(new QLabel("Model:"));
        modelEdit_ = new QLineEdit("/home/topeet/rknpu2/examples/rknn_yolov5_demo/model/RK3588/yolov5s-640-640.rknn");
        modelLayout->addWidget(modelEdit_);
        layout->addLayout(modelLayout);

        QHBoxLayout *btnLayout = new QHBoxLayout();
        startBtn_ = new QPushButton("Start");
        stopBtn_ = new QPushButton("Stop");
        stopBtn_->setEnabled(false);
        btnLayout->addWidget(startBtn_);
        btnLayout->addWidget(stopBtn_);
        layout->addLayout(btnLayout);

        statusLabel_ = new QLabel("Ready");
        layout->addWidget(statusLabel_);
        imageLabel_ = new QLabel();
        imageLabel_->setAlignment(Qt::AlignCenter);
        imageLabel_->setMinimumSize(640, 480);
        layout->addWidget(imageLabel_);

        connect(startBtn_, &QPushButton::clicked, this, &RealCameraTestWindow::onStart);
        connect(stopBtn_, &QPushButton::clicked, this, &RealCameraTestWindow::onStop);
        running_ = false;
        rknn_ctx_ = 0;
    }
    ~RealCameraTestWindow() { onStop(); }

private slots:
    void onStart() {
        if (running_) return;
        FILE *fp = fopen(modelEdit_->text().toUtf8().constData(), "rb");
        if (!fp) { QMessageBox::critical(this, "Error", "Cannot open model"); return; }
        fseek(fp, 0, SEEK_END); size_t sz = ftell(fp); fseek(fp, 0, SEEK_SET);
        std::vector<char> data(sz); fread(data.data(), 1, sz, fp); fclose(fp);
        if (rknn_init(&rknn_ctx_, data.data(), sz, 0, NULL) < 0) {
            QMessageBox::critical(this, "Error", "rknn_init failed"); return;
        }
        running_ = true;
        startBtn_->setEnabled(false); stopBtn_->setEnabled(true);
        statusLabel_->setText("Running...");
        thread_ = std::thread(&RealCameraTestWindow::captureLoop, this);
        displayTimer_ = new QTimer(this);
        connect(displayTimer_, &QTimer::timeout, this, &RealCameraTestWindow::updateDisplay);
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
    void captureLoop() {
        // Placeholder: V4L2 + MPP MJPEG decode + RGA + RKNN
        // Full implementation similar to multi_rtsp_mpp_test.cpp but with V4L2 input
        printf("Real camera capture loop - placeholder\n");
        int frame_count = 0;
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            frame_count++;
        }
        printf("Capture loop ended. Frames: %d\n", frame_count);
    }

private:
    QComboBox *deviceCombo_;
    QLineEdit *modelEdit_;
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
    RealCameraTestWindow win;
    win.show();
    return app.exec();
}
#include "real_camera_test.moc"
