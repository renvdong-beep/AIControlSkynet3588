/**
 * @file main_qt.cpp
 * @brief Qt 多路视频解码推理测试程序
 * 
 * 功能：
 * - 多路 USB 摄像头采集
 * - MPP 硬件解码
 * - RKNN 推理
 * - Qt 界面显示
 */

#include <QApplication>
#include <QMainWindow>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QTimer>
#include <QDebug>
#include <vector>
#include <thread>
#include <atomic>

#include "videocontrol.h"
#include "frmwidget.h"

// RK3588 平台头文件
#include <rockchip/rk_mpi.h>
#include <rknn_api.h>
#include <rga/RgaApi.h>

// 全局运行标志
std::atomic<bool> g_running(true);

/**
 * @brief 单路摄像头处理线程
 */
class CameraThread {
public:
    CameraThread(int camera_id, frmwidget* widget)
        : camera_id_(camera_id), widget_(widget), running_(false) {}
    
    bool start(const std::string& device) {
        device_ = device;
        running_ = true;
        thread_ = std::thread(&CameraThread::run, this);
        return true;
    }
    
    void stop() {
        running_ = false;
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void run() {
        qDebug() << "Camera" << camera_id_ << "started:" << device_.c_str();
        
        // TODO: 实际的摄像头采集和解码逻辑
        // 这里需要调用 V4L2 采集 + MPP 解码 + RKNN 推理
        
        // 测试：生成模拟帧
        int frame_count = 0;
        while (running_ && g_running) {
            // 模拟帧更新
            QImage test_image(640, 480, QImage::Format_RGB888);
            test_image.fill(Qt::black);
            
            // 绘制帧计数
            QPainter painter(&test_image);
            painter.setPen(Qt::green);
            painter.setFont(QFont("Arial", 20));
            painter.drawText(test_image.rect(), Qt::AlignCenter, 
                           QString("Camera %1\nFrame %2").arg(camera_id_).arg(frame_count));
            painter.end();
            
            // 更新显示
            if (widget_) {
                QMetaObject::invokeMethod(widget_, "updateImage", 
                                         Qt::QueuedConnection, 
                                         Q_ARG(const QImage&, test_image));
            }
            
            frame_count++;
            std::this_thread::sleep_for(std::chrono::milliseconds(40)); // 25 fps
        }
        
        qDebug() << "Camera" << camera_id_ << "stopped";
    }

    int camera_id_;
    frmwidget* widget_;
    std::string device_;
    std::atomic<bool> running_;
    std::thread thread_;
};

/**
 * @brief 主窗口
 */
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget* parent = nullptr) : QMainWindow(parent) {
        setWindowTitle("AIControlSkynet3588 - 多路视频解码推理测试");
        resize(1280, 720);
        
        // 创建中央部件
        QWidget* central = new QWidget(this);
        setCentralWidget(central);
        
        // 创建布局
        QVBoxLayout* main_layout = new QVBoxLayout(central);
        
        // 控制面板
        QHBoxLayout* control_layout = new QHBoxLayout();
        
        // 画面布局选择
        control_layout->addWidget(new QLabel("画面布局:"));
        layout_combo_ = new QComboBox();
        layout_combo_->addItems({"1x1", "2x2", "3x3", "4x4", "5x5", "6x6", "8x8"});
        layout_combo_->setCurrentIndex(1); // 默认 2x2
        control_layout->addWidget(layout_combo_);
        connect(layout_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &MainWindow::onLayoutChanged);
        
        // 启动按钮
        start_btn_ = new QPushButton("启动");
        control_layout->addWidget(start_btn_);
        connect(start_btn_, &QPushButton::clicked, this, &MainWindow::onStart);
        
        // 停止按钮
        stop_btn_ = new QPushButton("停止");
        stop_btn_->setEnabled(false);
        control_layout->addWidget(stop_btn_);
        connect(stop_btn_, &QPushButton::clicked, this, &MainWindow::onStop);
        
        control_layout->addStretch();
        main_layout->addLayout(control_layout);
        
        // 视频显示区域
        video_control_ = new VideoControl(this);
        main_layout->addWidget(video_control_);
        
        // 状态栏
        statusBar()->showMessage("就绪");
    }
    
    ~MainWindow() {
        stopAllCameras();
    }

private slots:
    void onLayoutChanged(int index) {
        int counts[] = {1, 4, 9, 16, 25, 36, 64};
        int count = counts[index];
        
        // 切换画面布局
        switch (count) {
            case 1: video_control_->show_video_4(); break;  // TODO: 实现 show_video_1
            case 4: video_control_->show_video_4(); break;
            case 9: video_control_->show_video_9(); break;
            case 16: video_control_->show_video_16(); break;
            case 25: video_control_->show_video_25(); break;
            case 36: video_control_->show_video_36(); break;
            case 64: video_control_->show_video_64(); break;
        }
        
        statusBar()->showMessage(QString("画面布局: %1x%1").arg(sqrt(count)));
    }
    
    void onStart() {
        // 获取画面数量
        int counts[] = {1, 4, 9, 16, 25, 36, 64};
        int count = counts[layout_combo_->currentIndex()];
        
        // 获取视频组件列表
        auto widgets = video_control_->getFrmWidgets();
        
        // 启动摄像头线程
        for (int i = 0; i < count && i < widgets.size(); i++) {
            QString device = QString("/dev/video%1").arg(i * 2);  // 假设摄像头设备
            auto* thread = new CameraThread(i, widgets[i]);
            thread->start(device.toStdString());
            camera_threads_.push_back(thread);
        }
        
        start_btn_->setEnabled(false);
        stop_btn_->setEnabled(true);
        statusBar()->showMessage(QString("已启动 %1 路摄像头").arg(count));
    }
    
    void onStop() {
        stopAllCameras();
        start_btn_->setEnabled(true);
        stop_btn_->setEnabled(false);
        statusBar()->showMessage("已停止");
    }

private:
    void stopAllCameras() {
        g_running = false;
        for (auto* thread : camera_threads_) {
            thread->stop();
            delete thread;
        }
        camera_threads_.clear();
        g_running = true;
    }
    
    QComboBox* layout_combo_;
    QPushButton* start_btn_;
    QPushButton* stop_btn_;
    VideoControl* video_control_;
    std::vector<CameraThread*> camera_threads_;
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    
    MainWindow window;
    window.show();
    
    return app.exec();
}

#include "main_qt.moc"
