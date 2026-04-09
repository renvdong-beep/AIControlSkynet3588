/**
 * @file simple_test.cpp
 * @brief 简化版 Qt 测试程序（模拟数据，无需摄像头/RTSP）
 * 验证 Qt + RKNN 推理流程，生成模拟图像送入 RKNN 推理。
 */

#include <QApplication>
#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
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
#include <vector>

// RKNN
#include <rknn_api.h>

// RGA
#include <rga/RgaApi.h>
#include <rga/im2d.h>

#define OBJ_CLASS_NUM 2
#define MODEL_INPUT_SIZE 640

static const char* CLASS_NAMES[OBJ_CLASS_NUM] = {"cow", "person"};

typedef struct {
    int left, right, top, bottom;
    float prop;
    int class_id;
} detect_result_t;

class SimpleTestWindow : public QMainWindow {
    Q_OBJECT
public:
    SimpleTestWindow(QWidget *parent = nullptr) : QMainWindow(parent) {
        setWindowTitle("RKNN Simple Test (Simulated Data)");
        resize(700, 700);
        QWidget *central = new QWidget(this);
        setCentralWidget(central);
        QVBoxLayout *layout = new QVBoxLayout(central);

        QHBoxLayout *modelLayout = new QHBoxLayout();
        modelLayout->addWidget(new QLabel("Model:"));
        modelEdit_ = new QLineEdit("/home/topeet/rknpu2/examples/rknn_yolov5_demo/model/RK3588/yolov5s-640-640.rknn");
        modelLayout->addWidget(modelEdit_);
        layout->addLayout(modelLayout);

        runBtn_ = new QPushButton("Run Inference");
        layout->addWidget(runBtn_);
        resultLabel_ = new QLabel("Ready");
        layout->addWidget(resultLabel_);
        imageLabel_ = new QLabel();
        imageLabel_->setAlignment(Qt::AlignCenter);
        imageLabel_->setMinimumSize(640, 640);
        layout->addWidget(imageLabel_);

        connect(runBtn_, &QPushButton::clicked, this, &SimpleTestWindow::onRun);
        rknn_ctx_ = 0;
    }

    ~SimpleTestWindow() { if (rknn_ctx_) rknn_destroy(rknn_ctx_); }

private slots:
    void onRun() {
        resultLabel_->setText("Loading model...");
        QApplication::processEvents();
        if (rknn_ctx_) { rknn_destroy(rknn_ctx_); rknn_ctx_ = 0; }

        // 读取模型文件
        FILE *fp = fopen(modelEdit_->text().toUtf8().constData(), "rb");
        if (!fp) { resultLabel_->setText("Cannot open model"); return; }
        fseek(fp, 0, SEEK_END);
        size_t sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        std::vector<char> data(sz);
        fread(data.data(), 1, sz, fp);
        fclose(fp);

        int ret = rknn_init(&rknn_ctx_, data.data(), sz, 0, NULL);
        if (ret < 0) { resultLabel_->setText(QString("rknn_init failed: %1").arg(ret)); return; }

        rknn_sdk_version ver;
        rknn_query(rknn_ctx_, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver));
        printf("SDK: %s\n", ver.api_version);

        rknn_input_output_num io;
        rknn_query(rknn_ctx_, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
        printf("Input: %d, Output: %d\n", io.n_input, io.n_output);

        // 生成模拟图像
        uint8_t *img = (uint8_t*)malloc(MODEL_INPUT_SIZE * MODEL_INPUT_SIZE * 3);
        for (int y = 0; y < MODEL_INPUT_SIZE; y++)
            for (int x = 0; x < MODEL_INPUT_SIZE; x++) {
                int i = (y * MODEL_INPUT_SIZE + x) * 3;
                img[i+0] = 100; img[i+1] = 180; img[i+2] = 60;
            }
        for (int o = 0; o < 3; o++) {
            int ox = 100 + o * 180, oy = 150 + o * 120;
            for (int y = oy; y < oy + 100 && y < MODEL_INPUT_SIZE; y++)
                for (int x = ox; x < ox + 120 && x < MODEL_INPUT_SIZE; x++) {
                    int i = (y * MODEL_INPUT_SIZE + x) * 3;
                    img[i+0] = 200; img[i+1] = 100; img[i+2] = 50;
                }
        }

        rknn_input input = {};
        input.index = 0;
        input.type = RKNN_TENSOR_UINT8;
        input.size = MODEL_INPUT_SIZE * MODEL_INPUT_SIZE * 3;
        input.buf = img;

        rknn_inputs_set(rknn_ctx_, 1, &input);
        resultLabel_->setText("Running inference...");
        QApplication::processEvents();

        ret = rknn_run(rknn_ctx_, NULL);
        if (ret < 0) { resultLabel_->setText(QString("rknn_run failed: %1").arg(ret)); free(img); return; }

        rknn_output outputs[3] = {};
        for (int i = 0; i < 3; i++) outputs[i].want_float = 1;
        rknn_outputs_get(rknn_ctx_, 3, outputs, NULL);

        for (int i = 0; i < 3; i++)
            printf("Output[%d]: size=%zu\n", i, outputs[i].size);

        resultLabel_->setText("Inference done!");
        QImage qimg(img, MODEL_INPUT_SIZE, MODEL_INPUT_SIZE, MODEL_INPUT_SIZE * 3, QImage::Format_RGB888);
        imageLabel_->setPixmap(QPixmap::fromImage(qimg).scaled(640, 640, Qt::KeepAspectRatio));

        rknn_outputs_release(rknn_ctx_, 3, outputs);
        free(img);
    }

private:
    QLineEdit *modelEdit_;
    QPushButton *runBtn_;
    QLabel *resultLabel_;
    QLabel *imageLabel_;
    rknn_context rknn_ctx_;
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    SimpleTestWindow win;
    win.show();
    return app.exec();
}
#include "simple_test.moc"
